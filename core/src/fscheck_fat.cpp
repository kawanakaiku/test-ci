// SPDX-License-Identifier: Apache-2.0
// FAT12/16/32 の読み取り専用の整合性検査 (fsck.fat -n 相当)。何も書かない。
#include <algorithm>
#include <unordered_set>

#include "fscheck_internal.h"

namespace usbmem {
namespace fsck {
namespace {

struct Geo {
  uint32_t bps = 0, spc = 0, rsvd = 0, nfats = 0, root_ent = 0;
  uint64_t tot_sec = 0, fat_sec = 0;   // fat_sec: 1 つの FAT のセクタ数
  uint64_t root_secs = 0, data_start = 0;  // セクタ
  uint64_t clusters = 0;              // データクラスタ数 (番号は 2 .. clusters+1)
  int bits = 0;                       // 12 / 16 / 32
  uint32_t root_clus = 0, fsinfo = 0, backup = 0;
  uint64_t cb = 0;                    // クラスタのバイト数
  uint32_t active_fat = 0;
  bool mirrored = true;
};

class FatChk {
 public:
  FatChk(Ctx& c, FsType t) : c_(c), v_(c.vol), want_(t) {}
  Status run();

 private:
  Ctx& c_;
  Vol& v_;
  FsType want_;
  Geo g_;
  Bits claimed_, walk_;
  uint64_t free_clusters_ = 0, used_clusters_ = 0;
  bool bad_geo_ = false;

  uint32_t eoc_min() const { return g_.bits == 12 ? 0xFF8u : g_.bits == 16 ? 0xFFF8u : 0x0FFFFFF8u; }
  uint32_t bad_val() const { return g_.bits == 12 ? 0xFF7u : g_.bits == 16 ? 0xFFF7u : 0x0FFFFFF7u; }
  uint64_t fat_off(uint32_t copy) const { return (static_cast<uint64_t>(g_.rsvd) + static_cast<uint64_t>(copy) * g_.fat_sec) * g_.bps; }
  uint64_t clus_off(uint32_t cl) const { return (g_.data_start + static_cast<uint64_t>(cl - 2) * g_.spc) * g_.bps; }
  bool valid_cluster(uint64_t cl) const { return cl >= 2 && cl < g_.clusters + 2; }

  Status fat_get(uint32_t cl, uint32_t* val) {
    uint64_t base = fat_off(g_.active_fat);
    if (g_.bits == 32) {
      uint8_t b[4];
      Status st = v_.read(base + 4ull * cl, b, 4);
      if (!st.ok()) return st;
      *val = le32(b) & 0x0FFFFFFFu;
    } else if (g_.bits == 16) {
      uint8_t b[2];
      Status st = v_.read(base + 2ull * cl, b, 2);
      if (!st.ok()) return st;
      *val = le16(b);
    } else {
      uint8_t b[2];
      Status st = v_.read(base + cl + cl / 2, b, 2);
      if (!st.ok()) return st;
      uint32_t w = le16(b);
      *val = (cl & 1) ? (w >> 4) : (w & 0xFFF);
    }
    return Status::Ok();
  }

  Status boot();
  Status fat_checks();
  Status scan_dirs();
  Status scan_dir(const std::vector<uint8_t>& data, uint32_t self, uint32_t parent, bool is_root, const std::string& path,
                  std::vector<std::pair<uint32_t, std::string>>& subdirs, std::vector<uint32_t>* pending_parents);
  // 連鎖をたどり、claimed に登録する。*len = 連鎖の長さ。問題は報告する。clusters_out は null 可
  Status walk_chain(uint32_t start, const std::string& obj, uint64_t* len, std::vector<uint32_t>* clusters_out, uint64_t cap, bool* ok);
  Status orphans();
};

uint8_t lfn_checksum(const uint8_t* name11) {
  uint8_t s = 0;
  for (int i = 0; i < 11; i++) s = static_cast<uint8_t>(((s & 1) ? 0x80 : 0) + (s >> 1) + name11[i]);
  return s;
}

std::string short_name(const uint8_t* e) {
  std::string n;
  for (int i = 0; i < 8; i++) n += static_cast<char>(e[i]);
  while (!n.empty() && n.back() == ' ') n.pop_back();
  std::string x;
  for (int i = 8; i < 11; i++) x += static_cast<char>(e[i]);
  while (!x.empty() && x.back() == ' ') x.pop_back();
  if (!n.empty() && static_cast<uint8_t>(n[0]) == 0x05) n[0] = static_cast<char>(0xE5);
  return x.empty() ? n : n + "." + x;
}

bool bad_short_name(const uint8_t* e) {
  if (!std::memcmp(e, "EA DATA  SF", 11) || !std::memcmp(e, "WP ROOT  SF", 11)) return false;  // OS/2 の拡張属性
  bool all_space = true;
  for (int i = 0; i < 11; i++) {
    uint8_t ch = e[i];
    if (ch != ' ') all_space = false;
    if (ch < 0x20 || ch == 0x7F) { if (!(i == 0 && ch == 0x05)) return true; }
    if (ch < 0x80 && std::strchr("*?<>|\"/", ch) && ch) return true;
  }
  if (all_space) return true;
  if (e[0] == ' ') return true;
  // 名前部の途中の空白 ("A B")
  bool sp = false;
  for (int i = 0; i < 8; i++) { if (e[i] == ' ') sp = true; else if (sp) return true; }
  sp = false;
  for (int i = 8; i < 11; i++) { if (e[i] == ' ') sp = true; else if (sp) return true; }
  return false;
}

Status FatChk::boot() {
  uint8_t s[512];
  Status st = v_.read(0, s, 512);
  if (!st.ok()) return st;
  if (s[510] != 0x55 || s[511] != 0xAA) c_.error("fat-boot-signature", "boot sector signature 0x55AA is missing", 510);
  g_.bps = le16(s + 11);
  g_.spc = s[13];
  g_.rsvd = le16(s + 14);
  g_.nfats = s[16];
  g_.root_ent = le16(s + 17);
  uint32_t tot16 = le16(s + 19), fat16 = le16(s + 22), tot32 = le32(s + 32);
  g_.tot_sec = tot16 ? tot16 : tot32;
  uint32_t fat32sz = le32(s + 36);
  g_.fat_sec = fat16 ? fat16 : fat32sz;
  if (g_.bps < 512 || g_.bps > 4096 || !is_pow2(g_.bps)) { c_.error("fat-bad-sector-size", "bytes per sector is not 512/1024/2048/4096: " + num(g_.bps), 11); bad_geo_ = true; }
  if (!g_.spc || !is_pow2(g_.spc) || g_.spc > 128) { c_.error("fat-bad-cluster-size", "sectors per cluster is not a power of two in 1..128: " + num(g_.spc), 13); bad_geo_ = true; }
  if (!g_.rsvd) { c_.error("fat-bad-reserved", "reserved sector count is zero", 14); bad_geo_ = true; }
  if (!g_.nfats) { c_.error("fat-bad-fat-count", "number of FATs is zero", 16); bad_geo_ = true; }
  else if (g_.nfats > 2) c_.warn("fat-fat-count", "number of FATs is " + num(g_.nfats) + " (1 or 2 expected)", 16);
  if (!g_.fat_sec) { c_.error("fat-bad-fat-size", "FAT size is zero", 22); bad_geo_ = true; }
  if (!g_.tot_sec) { c_.error("fat-bad-total", "total sector count is zero", 19); bad_geo_ = true; }
  if (bad_geo_) return Status::Ok();

  bool fat32 = (fat16 == 0);
  g_.root_secs = (static_cast<uint64_t>(g_.root_ent) * 32 + g_.bps - 1) / g_.bps;
  if (fat32 && g_.root_ent != 0) c_.warn("fat-root-entries", "FAT32 root entry count should be 0", 17);
  if (!fat32 && g_.root_ent == 0) { c_.error("fat-root-entries", "FAT12/16 root entry count is zero", 17); bad_geo_ = true; return Status::Ok(); }
  uint64_t fats_secs;
  if (!mul_ok(g_.nfats, g_.fat_sec, &fats_secs)) { c_.error("fat-bad-fat-size", "FAT size overflows", 22); bad_geo_ = true; return Status::Ok(); }
  uint64_t meta = static_cast<uint64_t>(g_.rsvd) + fats_secs + g_.root_secs;
  if (meta >= g_.tot_sec) { c_.error("fat-bad-geometry", "reserved + FAT + root area is larger than the volume", UINT64_MAX); bad_geo_ = true; return Status::Ok(); }
  g_.data_start = meta;
  g_.clusters = (g_.tot_sec - meta) / g_.spc;
  g_.bits = g_.clusters < 4085 ? 12 : g_.clusters < 65525 ? 16 : 32;
  g_.cb = static_cast<uint64_t>(g_.spc) * g_.bps;
  if (fat32 != (g_.bits == 32)) {
    c_.error("fat-type-mismatch", "the FAT size field says FAT" + std::string(fat32 ? "32" : "12/16") + " but the cluster count (" + num(g_.clusters) + ") says FAT" + num(g_.bits), UINT64_MAX);
    bad_geo_ = true;
    return Status::Ok();
  }
  if ((want_ == FsType::Fat12 && g_.bits != 12) || (want_ == FsType::Fat16 && g_.bits != 16) || (want_ == FsType::Fat32 && g_.bits != 32))
    c_.warn("fat-type-probe", "the cluster count says FAT" + num(g_.bits) + " which differs from the detected type");
  if (g_.clusters > 0x0FFFFFF5ull) c_.error("fat-too-many-clusters", "too many clusters", UINT64_MAX);

  // 大きさが FAT の記述に収まるか
  uint64_t need_bytes = g_.bits == 12 ? ((g_.clusters + 2) * 3 + 1) / 2 : (g_.clusters + 2) * (g_.bits / 8);
  if (need_bytes > g_.fat_sec * g_.bps) { c_.error("fat-too-small", "the FAT is too small for the number of clusters", UINT64_MAX); bad_geo_ = true; return Status::Ok(); }
  if (g_.tot_sec * g_.bps > v_.size()) c_.error("fat-larger-than-device", "the filesystem claims more sectors (" + num(g_.tot_sec) + ") than the device has", 19);
  uint8_t media = s[21];
  if (media < 0xF8 && media != 0xF0) c_.warn("fat-media-byte", "unusual media byte " + hex(media), 21);

  if (g_.bits == 32) {
    uint16_t ext = le16(s + 40), ver = le16(s + 42);
    g_.root_clus = le32(s + 44);
    g_.fsinfo = le16(s + 48);
    g_.backup = le16(s + 50);
    if (ver != 0) c_.error("fat32-version", "FAT32 version is not 0.0: " + hex(ver), 42);
    if (ext & 0x80) { g_.active_fat = ext & 0xF; g_.mirrored = false; if (g_.active_fat >= g_.nfats) { c_.error("fat32-active-fat", "active FAT index is out of range", 40); g_.active_fat = 0; } }
    if (!valid_cluster(g_.root_clus)) { c_.error("fat32-root-cluster", "root directory cluster " + num(g_.root_clus) + " is out of range", 44); bad_geo_ = true; return Status::Ok(); }
    if (s[66] != 0x29 && s[66] != 0x28) c_.warn("fat-ext-boot-sig", "extended boot signature is not 0x29", 66);
    if (s[65] & 1) { c_.warn("fat-dirty-boot", "the volume dirty flag is set in the boot sector", 65); c_.rep.unclean = true; }
    // FSInfo
    if (g_.fsinfo && g_.fsinfo < g_.rsvd) {
      uint8_t f[512];
      st = v_.read(static_cast<uint64_t>(g_.fsinfo) * g_.bps, f, 512);
      if (!st.ok()) return st;
      if (le32(f) != 0x41615252 || le32(f + 484) != 0x61417272 || le32(f + 508) != 0xAA550000)
        c_.warn("fat32-fsinfo-signature", "the FSInfo sector has a bad signature", static_cast<uint64_t>(g_.fsinfo) * g_.bps);
    } else if (g_.fsinfo) {
      c_.warn("fat32-fsinfo-location", "the FSInfo sector number is outside the reserved area", 48);
    }
    // バックアップブートセクタ
    if (g_.backup) {
      if (g_.backup >= g_.rsvd) c_.warn("fat32-backup-location", "the backup boot sector number is outside the reserved area", 50);
      else {
        uint8_t b[512];
        st = v_.read(static_cast<uint64_t>(g_.backup) * g_.bps, b, 512);
        if (!st.ok()) return st;
        // BPB の主要部分 (0x00-0x5A) を比べる。ダーティ / ヘッダ内の状態バイト (0x41) と、ブートコードは見ない
        bool diff = false;
        for (int i = 3; i < 0x5A; i++) {
          if (i == 0x41 || i == 0x25) continue;
          if (s[i] != b[i]) { diff = true; break; }
        }
        if (diff) c_.warn("fat32-backup-differs", "the backup boot sector differs from the boot sector", static_cast<uint64_t>(g_.backup) * g_.bps);
      }
    }
  } else {
    if (s[38] != 0x29 && s[38] != 0x28 && s[38] != 0) c_.warn("fat-ext-boot-sig", "extended boot signature is not 0x29", 38);
    if (s[37] & 1) { c_.warn("fat-dirty-boot", "the volume dirty flag is set in the boot sector", 37); c_.rep.unclean = true; }
  }
  return Status::Ok();
}

Status FatChk::fat_checks() {
  // 先頭 2 エントリ
  uint32_t f0 = 0, f1 = 0;
  Status st = fat_get(0, &f0);
  if (!st.ok()) return st;
  st = fat_get(1, &f1);
  if (!st.ok()) return st;
  uint8_t boot_media;
  st = v_.read(21, &boot_media, 1);
  if (!st.ok()) return st;
  if ((f0 & 0xFF) != boot_media) c_.info("fat-media-mismatch", "FAT[0] media byte " + hex(f0 & 0xFF) + " differs from the boot sector " + hex(boot_media), fat_off(g_.active_fat));
  if (g_.bits >= 16) {
    uint32_t clean_bit = g_.bits == 16 ? 0x8000u : 0x08000000u;
    uint32_t err_bit = g_.bits == 16 ? 0x4000u : 0x04000000u;
    if (f1 < eoc_min() - (g_.bits == 16 ? 0x8000u + 0x4000u : 0x0C000000u) ) { /* EOC でない FAT[1] */ }
    if (!(f1 & clean_bit)) { c_.warn("fat-dirty", "the volume was not cleanly unmounted (FAT[1] dirty bit)", fat_off(g_.active_fat)); c_.rep.unclean = true; }
    if (!(f1 & err_bit)) c_.warn("fat-io-error-flag", "the volume has the disk-error flag set (FAT[1])", fat_off(g_.active_fat));
  }
  // FAT のコピーが一致するか (ミラー時のみ)
  if (g_.nfats > 1 && g_.mirrored) {
    uint64_t total = g_.fat_sec * g_.bps;
    std::vector<uint8_t> a, b;
    const size_t step = 64 * 1024;
    bool diff = false;
    uint64_t first = 0;
    for (uint64_t off = 0; off < total && !diff; off += step) {
      size_t n = static_cast<size_t>(std::min<uint64_t>(step, total - off));
      a.resize(n); b.resize(n);
      st = v_.read(fat_off(0) + off, a.data(), n);
      if (!st.ok()) return st;
      for (uint32_t k = 1; k < g_.nfats && !diff; k++) {
        st = v_.read(fat_off(k) + off, b.data(), n);
        if (!st.ok()) return st;
        if (a != b) {
          diff = true;
          for (size_t i = 0; i < n; i++) if (a[i] != b[i]) { first = off + i; break; }
        }
      }
      if (c_.stop()) return Status::Ok();
    }
    if (diff) c_.error("fat-copies-differ", "the FAT copies differ (first difference at FAT offset " + num(first) + ")", fat_off(0) + first);
  }
  return Status::Ok();
}

Status FatChk::walk_chain(uint32_t start, const std::string& obj, uint64_t* len, std::vector<uint32_t>* out, uint64_t cap, bool* ok) {
  *len = 0;
  *ok = true;
  uint32_t cl = start;
  bool crossed = false;
  while (true) {
    if (!valid_cluster(cl)) {
      c_.error("fat-chain-invalid", "the chain contains an invalid cluster number " + num(cl), UINT64_MAX, obj);
      *ok = false;
      break;
    }
    if (claimed_.get(cl)) {
      if (walk_.get(cl)) c_.error("fat-chain-loop", "the cluster chain loops back to cluster " + num(cl), clus_off(cl), obj);
      else { c_.error("fat-crosslink", "cluster " + num(cl) + " is shared with another file or directory (cross-linked)", clus_off(cl), obj); crossed = true; }
      *ok = false;
      break;
    }
    claimed_.set(cl);
    walk_.set(cl);
    (*len)++;
    if (out && out->size() < cap) out->push_back(cl);
    uint32_t nx;
    Status st = fat_get(cl, &nx);
    if (!st.ok()) return st;
    if (nx == 0) { c_.error("fat-chain-free", "the chain runs into a free cluster after cluster " + num(cl), clus_off(cl), obj); *ok = false; break; }
    if (nx == bad_val()) { c_.error("fat-chain-bad", "the chain contains a bad cluster after cluster " + num(cl), clus_off(cl), obj); *ok = false; break; }
    if (nx >= eoc_min()) break;
    cl = nx;
    if ((*len & 0xFFFF) == 0 && c_.stop()) { *ok = false; break; }
  }
  (void)crossed;
  // walk_ の目印を消す (同じ連鎖をたどり直す)
  uint32_t k = start;
  for (uint64_t i = 0; i < *len; i++) {
    if (!valid_cluster(k)) break;
    walk_.clear(k);
    uint32_t nx;
    Status st = fat_get(k, &nx);
    if (!st.ok()) return st;
    if (nx >= eoc_min() || nx == 0 || nx == bad_val()) break;
    k = nx;
  }
  return Status::Ok();
}

Status FatChk::scan_dir(const std::vector<uint8_t>& data, uint32_t self, uint32_t parent, bool is_root, const std::string& path,
                        std::vector<std::pair<uint32_t, std::string>>& subdirs, std::vector<uint32_t>* pending_parents) {
  std::unordered_set<std::string> names;
  // LFN の状態
  int lfn_expect = 0;            // 次に来るはずの序数 (0 = LFN 中ではない)
  int lfn_total = 0;
  uint8_t lfn_sum = 0;
  bool lfn_seen = false;
  bool first_entry = true, second_entry = true;
  size_t n = data.size() / 32;
  for (size_t i = 0; i < n; i++) {
    const uint8_t* e = data.data() + i * 32;
    uint8_t first = e[0];
    if (first == 0) break;  // ここから先は未使用
    if (!c_.count_object()) return Status::Ok();
    std::string where = path.empty() ? "/" : path;
    if (first == 0xE5) { lfn_expect = 0; lfn_seen = false; first_entry = second_entry = false; continue; }
    uint8_t attr = e[11];
    if ((attr & 0x3F) == 0x0F) {  // 長いファイル名のエントリ
      uint8_t ord = first & 0x3F;
      bool last = (first & 0x40) != 0;
      if (last) {
        if (lfn_seen && lfn_expect) c_.error("fat-lfn-orphan", "an incomplete long file name sequence precedes this entry", UINT64_MAX, where);
        lfn_total = ord; lfn_expect = ord; lfn_sum = e[13]; lfn_seen = true;
        if (ord == 0 || ord > 20) { c_.error("fat-lfn-bad", "bad long file name sequence number " + num(ord), UINT64_MAX, where); lfn_expect = 0; lfn_seen = false; }
      } else if (!lfn_seen || ord != lfn_expect || e[13] != lfn_sum) {
        c_.error("fat-lfn-orphan", "a long file name entry is out of sequence", UINT64_MAX, where);
        lfn_expect = 0; lfn_seen = false;
        continue;
      }
      if (lfn_seen) lfn_expect--;
      first_entry = second_entry = false;
      continue;
    }
    (void)lfn_total;
    // 短い名前のエントリ
    std::string name = short_name(e);
    std::string obj = (path.empty() ? "" : path) + "/" + name;
    if (lfn_seen) {
      if (lfn_expect != 0) c_.error("fat-lfn-orphan", "an incomplete long file name sequence precedes this entry", UINT64_MAX, obj);
      else if (lfn_checksum(e) != lfn_sum) c_.error("fat-lfn-checksum", "the long file name checksum does not match the short name", UINT64_MAX, obj);
    }
    lfn_seen = false; lfn_expect = 0;
    bool dot = !std::memcmp(e, ".          ", 11), dotdot = !std::memcmp(e, "..         ", 11);
    if (attr & 0x08 && !(attr & 0x10)) {  // ボリュームラベル
      if (!is_root) c_.warn("fat-label-in-subdir", "a volume label entry is in a subdirectory", UINT64_MAX, obj);
      first_entry = second_entry = false;
      continue;
    }
    uint32_t cl = le16(e + 26) | (g_.bits == 32 ? (static_cast<uint32_t>(le16(e + 20)) << 16) : 0);
    if (g_.bits != 32 && le16(e + 20) != 0) c_.info("fat-high-cluster", "the high 16 bits of the start cluster are not zero", UINT64_MAX, obj);
    uint32_t size = le32(e + 28);
    if (dot || dotdot) {
      if (is_root) { c_.error("fat-dot-in-root", "the root directory has a '.' or '..' entry", UINT64_MAX, obj); first_entry = second_entry = false; continue; }
      bool expected_pos = dot ? first_entry : second_entry;
      if (!expected_pos) c_.error("fat-dot-position", "'.' / '..' is in the wrong position", UINT64_MAX, obj);
      if (!(attr & 0x10)) c_.error("fat-dot-not-dir", "'.' / '..' is not a directory", UINT64_MAX, obj);
      if (dot && cl != self) c_.error("fat-dot-target", "'.' does not point to the directory itself", UINT64_MAX, obj);
      if (dotdot && cl != parent) c_.error("fat-dotdot-target", "'..' does not point to the parent directory", UINT64_MAX, obj);
      if (dotdot) second_entry = false;
      if (dot) first_entry = false;
      continue;
    }
    first_entry = second_entry = false;
    if (bad_short_name(e)) c_.error("fat-bad-name", "the short file name has illegal characters", UINT64_MAX, obj);
    std::string key(reinterpret_cast<const char*>(e), 11);
    if (!names.insert(key).second) c_.error("fat-duplicate-name", "duplicate directory entry", UINT64_MAX, obj);
    bool is_dir = (attr & 0x10) != 0;
    if (is_dir) {
      c_.rep.stats.directories++;
      if (!cl) { c_.error("fat-dir-no-cluster", "the directory has no start cluster", UINT64_MAX, obj); continue; }
      if (!valid_cluster(cl)) { c_.error("fat-start-range", "the start cluster " + num(cl) + " is out of range", UINT64_MAX, obj); continue; }
      subdirs.push_back({cl, obj});
      pending_parents->push_back(self);
      continue;
    }
    c_.rep.stats.files++;
    uint64_t expect = (static_cast<uint64_t>(size) + g_.cb - 1) / g_.cb;
    if (!cl) {
      if (size) c_.error("fat-file-no-cluster", "the file has size " + num(size) + " but no start cluster", UINT64_MAX, obj);
      continue;
    }
    if (!valid_cluster(cl)) { c_.error("fat-start-range", "the start cluster " + num(cl) + " is out of range", UINT64_MAX, obj); continue; }
    uint64_t len = 0;
    bool ok = true;
    Status st = walk_chain(cl, obj, &len, nullptr, 0, &ok);
    if (!st.ok()) return st;
    if (ok && len != expect)
      c_.error("fat-size-mismatch", "the file size is " + num(size) + " bytes (" + num(expect) + " clusters) but the cluster chain has " + num(len) + " clusters", UINT64_MAX, obj);
  }
  return Status::Ok();
}

Status FatChk::scan_dirs() {
  struct Item { uint32_t clus; uint32_t parent; std::string path; };
  std::vector<Item> stack;
  uint64_t dirs_done = 0;
  // ルート
  if (g_.bits == 32) stack.push_back({g_.root_clus, 0, ""});
  else {
    std::vector<uint8_t> root(static_cast<size_t>(g_.root_secs * g_.bps));
    Status st = v_.read(static_cast<uint64_t>(g_.rsvd + g_.nfats * g_.fat_sec) * g_.bps, root.data(), root.size());
    if (!st.ok()) return st;
    std::vector<std::pair<uint32_t, std::string>> subs;
    std::vector<uint32_t> pp;
    st = scan_dir(root, 0, 0, true, "", subs, &pp);
    if (!st.ok()) return st;
    for (size_t i = subs.size(); i-- > 0;) stack.push_back({subs[i].first, 0, subs[i].second});
  }
  const uint64_t kDirCap = 4u << 20;  // 1 つのディレクトリで見る上限
  while (!stack.empty()) {
    if (c_.stop()) return Status::Ok();
    Item it = stack.back();
    stack.pop_back();
    bool is_root = g_.bits == 32 && it.path.empty() && it.parent == 0 && it.clus == g_.root_clus;
    std::vector<uint32_t> chain;
    uint64_t len = 0;
    bool ok = true;
    Status st = walk_chain(it.clus, it.path.empty() ? "/" : it.path, &len, &chain, kDirCap / g_.cb + 1, &ok);
    if (!st.ok()) return st;
    if (chain.empty()) continue;
    if (len * g_.cb > kDirCap) c_.warn("fat-dir-large", "the directory is larger than 4 MiB", UINT64_MAX, it.path);
    std::vector<uint8_t> data;
    data.resize(static_cast<size_t>(chain.size() * g_.cb));
    for (size_t i = 0; i < chain.size(); i++) {
      st = v_.read(clus_off(chain[i]), data.data() + i * g_.cb, static_cast<size_t>(g_.cb));
      if (!st.ok()) return st;
    }
    std::vector<std::pair<uint32_t, std::string>> subs;
    std::vector<uint32_t> pp;
    st = scan_dir(data, it.clus, it.parent, is_root, is_root ? "" : it.path, subs, &pp);
    if (!st.ok()) return st;
    for (size_t i = subs.size(); i-- > 0;) stack.push_back({subs[i].first, is_root ? 0u : it.clus, subs[i].second});
    if ((++dirs_done & 63) == 0) c_.progress("directories", dirs_done, 0);
  }
  return Status::Ok();
}

Status FatChk::orphans() {
  uint64_t orphan = 0, first = 0;
  for (uint32_t cl = 2; cl < g_.clusters + 2; cl++) {
    uint32_t val;
    Status st = fat_get(cl, &val);
    if (!st.ok()) return st;
    if (val == 0) { free_clusters_++; continue; }
    used_clusters_++;
    if (val == bad_val()) continue;
    if (!claimed_.get(cl)) { if (!orphan) first = cl; orphan++; }
    if ((cl & 0xFFFFF) == 0 && c_.stop()) return Status::Ok();
  }
  if (orphan) c_.warn("fat-orphan-clusters", num(orphan) + " cluster(s) are allocated in the FAT but belong to no file (first: cluster " + num(first) + ")", clus_off(static_cast<uint32_t>(first)));
  return Status::Ok();
}

Status FatChk::run() {
  Status st = boot();
  if (!st.ok() || bad_geo_) return st;
  c_.rep.stats.units_total = g_.clusters;
  c_.rep.stats.bytes_per_unit = g_.cb;
  if (c_.over_budget(Bits::bytes_for(g_.clusters) * 2)) { c_.incomplete("memory-limit", "the volume is too large for the memory budget; only the boot sector was checked"); return Status::Ok(); }
  if (!claimed_.init(g_.clusters + 2) || !walk_.init(g_.clusters + 2)) { c_.incomplete("memory-limit", "not enough memory"); return Status::Ok(); }
  c_.progress("fat", 0, 0);
  st = fat_checks();
  if (!st.ok() || c_.stop()) return st;
  c_.progress("directories", 0, 0);
  st = scan_dirs();
  if (!st.ok() || c_.stop()) return st;
  c_.progress("clusters", 0, 0);
  st = orphans();
  if (!st.ok() || c_.stop()) return st;
  c_.rep.stats.units_used = used_clusters_;
  // FSInfo の空き数
  if (g_.bits == 32 && g_.fsinfo && g_.fsinfo < g_.rsvd) {
    uint8_t f[512];
    st = v_.read(static_cast<uint64_t>(g_.fsinfo) * g_.bps, f, 512);
    if (!st.ok()) return st;
    if (le32(f) == 0x41615252) {
      uint32_t fc = le32(f + 488);
      if (fc != 0xFFFFFFFFu && fc != free_clusters_)
        c_.warn("fat32-fsinfo-free", "the FSInfo free cluster count (" + num(fc) + ") differs from the actual count (" + num(free_clusters_) + ")", static_cast<uint64_t>(g_.fsinfo) * g_.bps + 488);
    }
  }
  return Status::Ok();
}

}  // namespace

Status check_fat(Ctx& c, FsType fs) {
  FatChk k(c, fs);
  return k.run();
}

}  // namespace fsck
}  // namespace usbmem
