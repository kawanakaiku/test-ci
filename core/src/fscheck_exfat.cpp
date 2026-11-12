// SPDX-License-Identifier: Apache-2.0
// exFAT の読み取り専用の整合性検査 (fsck.exfat -n 相当)。何も書かない。
#include <algorithm>
#include <unordered_set>

#include "fscheck_internal.h"

namespace usbmem {
namespace fsck {
namespace {

uint32_t boot_checksum_step(uint32_t sum, uint8_t b) { return ((sum << 31) | (sum >> 1)) + b; }
uint16_t set_checksum_step(uint16_t sum, uint8_t b) { return static_cast<uint16_t>(((sum << 15) | (sum >> 1)) + b); }

constexpr uint32_t kEofMin = 0xFFFFFFF8u;
constexpr uint32_t kBad = 0xFFFFFFF7u;

void append_utf8(std::string& o, uint32_t c) {
  if (c < 0x80) o += static_cast<char>(c);
  else if (c < 0x800) { o += static_cast<char>(0xC0 | (c >> 6)); o += static_cast<char>(0x80 | (c & 0x3F)); }
  else if (c < 0x10000) { o += static_cast<char>(0xE0 | (c >> 12)); o += static_cast<char>(0x80 | ((c >> 6) & 0x3F)); o += static_cast<char>(0x80 | (c & 0x3F)); }
  else { o += static_cast<char>(0xF0 | (c >> 18)); o += static_cast<char>(0x80 | ((c >> 12) & 0x3F)); o += static_cast<char>(0x80 | ((c >> 6) & 0x3F)); o += static_cast<char>(0x80 | (c & 0x3F)); }
}
std::string utf16_to_utf8(const std::vector<uint16_t>& u) {
  std::string o;
  for (size_t i = 0; i < u.size(); i++) {
    uint32_t c = u[i];
    if (c >= 0xD800 && c < 0xDC00 && i + 1 < u.size() && u[i + 1] >= 0xDC00 && u[i + 1] < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (u[i + 1] - 0xDC00); i++; }
    else if (c >= 0xD800 && c < 0xE000) c = 0xFFFD;
    append_utf8(o, c);
  }
  return o;
}

struct Boot {
  uint64_t part_off = 0, vol_len = 0;
  uint32_t fat_off = 0, fat_len = 0, heap_off = 0, clus_count = 0, root = 0, serial = 0;
  uint16_t rev = 0, flags = 0;
  uint8_t bps_shift = 0, spc_shift = 0, nfats = 0;
  uint32_t bps = 0;
  uint64_t cb = 0;
};

class ExChk {
 public:
  explicit ExChk(Ctx& c) : c_(c), v_(c.vol) {}
  Status run();

 private:
  Ctx& c_;
  Vol& v_;
  Boot b_;
  Bits claimed_, walk_;
  std::vector<uint16_t> upcase_;
  bool have_upcase_ = false;
  uint32_t bitmap_first_ = 0;
  uint64_t bitmap_len_ = 0;
  bool bitmap_nofat_ = false;
  std::vector<uint32_t> bitmap_clusters_;
  bool have_bitmap_ = false;
  uint64_t used_ = 0;

  uint64_t sec_off(uint64_t sec) const { return sec << b_.bps_shift; }
  uint64_t clus_off(uint32_t cl) const { return sec_off(b_.heap_off) + (static_cast<uint64_t>(cl - 2) << b_.spc_shift << b_.bps_shift); }
  bool valid_cluster(uint64_t cl) const { return cl >= 2 && cl < static_cast<uint64_t>(b_.clus_count) + 2; }

  Status fat_get(uint32_t cl, uint32_t* val) {
    uint8_t x[4];
    Status st = v_.read(sec_off(b_.fat_off) + 4ull * cl, x, 4);
    if (!st.ok()) return st;
    *val = le32(x);
    return Status::Ok();
  }

  Status boot_region();
  Status check_region(uint64_t first_sector, bool primary, bool* checksum_ok);
  Status load_upcase(uint32_t first, uint64_t len, uint32_t expect_sum, const std::string& obj);
  Status load_bitmap_chain();
  // 連鎖 (FAT / NoFatChain) を集める。claimed に登録して、問題を報告する。ok = 連鎖が正常に終端している
  Status chain_of(uint32_t first, uint64_t data_len, bool nofat, const std::string& obj, std::vector<uint32_t>* out, uint64_t cap, uint64_t* len, bool* ok);
  uint16_t up(uint16_t ch) const { return (have_upcase_ && ch < upcase_.size()) ? upcase_[ch] : (ch >= 'a' && ch <= 'z' ? static_cast<uint16_t>(ch - 32) : ch); }
  Status read_dir(const std::vector<uint32_t>& clusters, std::vector<uint8_t>* out);
  struct Sub { uint32_t first; uint64_t len; bool nofat; std::string path; };
  Status scan_dir(const std::vector<uint8_t>& data, bool is_root, const std::string& path, std::vector<Sub>* subs, bool sys_only);
  Status bitmap_compare();
};

Status ExChk::check_region(uint64_t first_sector, bool primary, bool* checksum_ok) {
  std::vector<uint8_t> r(static_cast<size_t>(b_.bps) * 12);
  Status st = v_.read(sec_off(first_sector), r.data(), r.size());
  if (!st.ok()) return st;
  uint32_t sum = 0;
  for (size_t i = 0; i < b_.bps * 11u; i++) {
    if (i == 106 || i == 107 || i == 112) continue;
    sum = boot_checksum_step(sum, r[i]);
  }
  const uint8_t* cs = r.data() + static_cast<size_t>(b_.bps) * 11;
  bool ok = true;
  for (uint32_t i = 0; i < b_.bps / 4; i++)
    if (le32(cs + 4ull * i) != sum) { ok = false; break; }
  *checksum_ok = ok;
  const char* which = primary ? "main" : "backup";
  if (!ok) c_.error(primary ? "exfat-boot-checksum" : "exfat-backup-checksum", std::string("the ") + which + " boot region checksum is wrong", sec_off(first_sector + 11));
  // 拡張ブートセクタの署名 (1..8)
  for (int s = 1; s <= 8; s++) {
    const uint8_t* p = r.data() + static_cast<size_t>(b_.bps) * s;
    if (le32(p + b_.bps - 4) != 0xAA550000u) {
      c_.warn(primary ? "exfat-ext-boot-signature" : "exfat-backup-ext-boot-signature", std::string("extended boot sector ") + num(static_cast<uint64_t>(s)) + " of the " + which + " region has a bad signature", sec_off(first_sector + static_cast<uint64_t>(s)));
      break;
    }
  }
  return Status::Ok();
}

Status ExChk::boot_region() {
  uint8_t s[512];
  Status st = v_.read(0, s, 512);
  if (!st.ok()) return st;
  if (std::memcmp(s + 3, "EXFAT   ", 8) != 0) { c_.error("exfat-fs-name", "the file system name is not EXFAT", 3); return Status::Make(Errc::Corrupt); }
  if (s[510] != 0x55 || s[511] != 0xAA) c_.error("exfat-boot-signature", "boot sector signature 0x55AA is missing", 510);
  if (!all_zero(s + 11, 53)) c_.warn("exfat-must-be-zero", "the reserved area of the boot sector is not zero", 11);
  b_.part_off = le64(s + 64); b_.vol_len = le64(s + 72);
  b_.fat_off = le32(s + 80); b_.fat_len = le32(s + 84); b_.heap_off = le32(s + 88);
  b_.clus_count = le32(s + 92); b_.root = le32(s + 96); b_.serial = le32(s + 100);
  b_.rev = le16(s + 104); b_.flags = le16(s + 106);
  b_.bps_shift = s[108]; b_.spc_shift = s[109]; b_.nfats = s[110];
  bool bad = false;
  if (b_.bps_shift < 9 || b_.bps_shift > 12) { c_.error("exfat-bad-sector-shift", "bytes per sector shift is out of range: " + num(b_.bps_shift), 108); bad = true; }
  if (b_.spc_shift > 25 || (!bad && b_.bps_shift + b_.spc_shift > 25)) { c_.error("exfat-bad-cluster-shift", "sectors per cluster shift is out of range: " + num(b_.spc_shift), 109); bad = true; }
  if (bad) return Status::Make(Errc::Corrupt);
  b_.bps = 1u << b_.bps_shift;
  b_.cb = static_cast<uint64_t>(b_.bps) << b_.spc_shift;
  if ((b_.rev >> 8) != 1) c_.error("exfat-revision", "unsupported file system revision " + hex(b_.rev), 104);
  if (b_.nfats != 1 && b_.nfats != 2) { c_.error("exfat-bad-fat-count", "number of FATs is " + num(b_.nfats), 110); bad = true; }
  if (b_.nfats == 2) c_.warn("exfat-two-fats", "TexFAT (two FATs) is not supported by this checker", 110);
  if (b_.vol_len < 24 || b_.vol_len < b_.heap_off) { c_.error("exfat-bad-volume-length", "the volume length is too small", 72); bad = true; }
  if (sec_off(b_.vol_len) > v_.size()) c_.error("exfat-larger-than-device", "the file system claims more sectors (" + num(b_.vol_len) + ") than the device has", 72);
  if (b_.fat_off < 24 || static_cast<uint64_t>(b_.fat_off) + b_.fat_len > b_.heap_off) { c_.error("exfat-bad-fat-location", "the FAT lies outside the area before the cluster heap", 80); bad = true; }
  if (b_.heap_off >= b_.vol_len) { c_.error("exfat-bad-heap", "the cluster heap offset is outside the volume", 88); bad = true; }
  if (!bad) {
    uint64_t max_clusters = (b_.vol_len - b_.heap_off) >> b_.spc_shift;
    if (b_.clus_count == 0 || b_.clus_count > max_clusters) { c_.error("exfat-bad-cluster-count", "the cluster count (" + num(b_.clus_count) + ") does not fit in the volume (max " + num(max_clusters) + ")", 92); bad = true; }
    else if (b_.clus_count > 0xFFFFFFF5u) { c_.error("exfat-bad-cluster-count", "too many clusters", 92); bad = true; }
    if (!bad && (static_cast<uint64_t>(b_.clus_count) + 2) * 4 > static_cast<uint64_t>(b_.fat_len) * b_.bps) { c_.error("exfat-fat-too-small", "the FAT is too small for the cluster count", 84); bad = true; }
    if (!bad && !valid_cluster(b_.root)) { c_.error("exfat-bad-root", "the root directory cluster " + num(b_.root) + " is out of range", 96); bad = true; }
  }
  if (b_.flags & 2) { c_.warn("exfat-dirty", "the volume was not cleanly unmounted (VolumeDirty flag)", 106); c_.rep.unclean = true; }
  if (b_.flags & 4) c_.warn("exfat-media-failure", "the MediaFailure flag is set", 106);
  if (bad) return Status::Make(Errc::Corrupt);

  bool main_ok = false, backup_ok = false;
  st = check_region(0, true, &main_ok);
  if (!st.ok()) return st;
  if (sec_off(24) <= v_.size()) {
    st = check_region(12, false, &backup_ok);
    if (!st.ok()) return st;
    if (main_ok && backup_ok) {
      std::vector<uint8_t> a(static_cast<size_t>(b_.bps) * 12), bk(a.size());
      st = v_.read(0, a.data(), a.size());
      if (!st.ok()) return st;
      st = v_.read(sec_off(12), bk.data(), bk.size());
      if (!st.ok()) return st;
      // VolumeFlags / PercentInUse はチェックサム対象外なので違っていてよい
      for (size_t i : {106u, 107u, 112u}) bk[i] = a[i];
      if (a != bk) c_.warn("exfat-backup-differs", "the backup boot region differs from the main boot region", sec_off(12));
    }
  }
  return Status::Ok();
}

Status ExChk::chain_of(uint32_t first, uint64_t data_len, bool nofat, const std::string& obj, std::vector<uint32_t>* out, uint64_t cap, uint64_t* len, bool* ok) {
  *len = 0;
  *ok = true;
  uint64_t expect = (data_len + b_.cb - 1) / b_.cb;
  if (nofat) {
    // 連続領域
    if (!valid_cluster(first) || first + expect - 1 >= static_cast<uint64_t>(b_.clus_count) + 2 || expect == 0) {
      if (expect) { c_.error("exfat-range", "the contiguous cluster range " + num(first) + "+" + num(expect) + " is outside the cluster heap", UINT64_MAX, obj); *ok = false; }
      return Status::Ok();
    }
    for (uint64_t i = 0; i < expect; i++) {
      uint32_t cl = static_cast<uint32_t>(first + i);
      if (claimed_.get(cl)) { c_.error("exfat-crosslink", "cluster " + num(cl) + " is shared with another file or directory (cross-linked)", clus_off(cl), obj); *ok = false; break; }
      claimed_.set(cl);
      (*len)++;
      if (out && out->size() < cap) out->push_back(cl);
      if ((i & 0xFFFFF) == 0xFFFFF && c_.stop()) { *ok = false; break; }
    }
    return Status::Ok();
  }
  uint32_t cl = first;
  while (true) {
    if (!valid_cluster(cl)) { c_.error("exfat-chain-invalid", "the chain contains an invalid cluster number " + num(cl), UINT64_MAX, obj); *ok = false; break; }
    if (claimed_.get(cl)) {
      if (walk_.get(cl)) c_.error("exfat-chain-loop", "the cluster chain loops back to cluster " + num(cl), clus_off(cl), obj);
      else c_.error("exfat-crosslink", "cluster " + num(cl) + " is shared with another file or directory (cross-linked)", clus_off(cl), obj);
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
    if (nx == 0) { c_.error("exfat-chain-free", "the chain runs into a free cluster after cluster " + num(cl), clus_off(cl), obj); *ok = false; break; }
    if (nx == kBad) { c_.error("exfat-chain-bad", "the chain contains a bad cluster after cluster " + num(cl), clus_off(cl), obj); *ok = false; break; }
    if (nx >= kEofMin) break;
    cl = nx;
    if ((*len & 0xFFFF) == 0 && c_.stop()) { *ok = false; break; }
  }
  uint32_t k = first;
  for (uint64_t i = 0; i < *len; i++) {
    if (!valid_cluster(k)) break;
    walk_.clear(k);
    uint32_t nx;
    Status st = fat_get(k, &nx);
    if (!st.ok()) return st;
    if (nx >= kEofMin || nx == 0 || nx == kBad) break;
    k = nx;
  }
  return Status::Ok();
}

Status ExChk::load_upcase(uint32_t first, uint64_t len, uint32_t expect_sum, const std::string& obj) {
  if (len == 0 || len > 0x20000 * 2 || (len & 1)) { c_.error("exfat-upcase-size", "the upcase table has a bad size " + num(len), UINT64_MAX, obj); return Status::Ok(); }
  if (!valid_cluster(first)) { c_.error("exfat-upcase-cluster", "the upcase table start cluster is out of range", UINT64_MAX, obj); return Status::Ok(); }
  std::vector<uint32_t> cl;
  uint64_t n = 0;
  bool ok = true;
  uint32_t nx0 = 0;
  Status st = fat_get(first, &nx0);
  if (!st.ok()) return st;
  st = chain_of(first, len, nx0 == 0, obj, &cl, 1u << 20, &n, &ok);  // ルートのエントリには NoFatChain が無い: FAT に連鎖があれば FAT、無ければ連続
  if (!st.ok()) return st;
  if (!ok) return Status::Ok();
  if (cl.size() != (len + b_.cb - 1) / b_.cb) { c_.error("exfat-upcase-chain", "the upcase table chain length does not match its size", UINT64_MAX, obj); return Status::Ok(); }
  if (cl.size() > 1) { for (size_t i = 1; i < cl.size(); i++) if (cl[i] != cl[i - 1] + 1) { c_.warn("exfat-upcase-fragmented", "the upcase table is not contiguous (not checked)", UINT64_MAX, obj); return Status::Ok(); } }
  std::vector<uint8_t> raw(static_cast<size_t>(len));
  st = v_.read(clus_off(first), raw.data(), raw.size());
  if (!st.ok()) return st;
  uint32_t sum = 0;
  for (uint8_t x : raw) sum = boot_checksum_step(sum, x);
  if (sum != expect_sum) { c_.error("exfat-upcase-checksum", "the upcase table checksum is wrong", clus_off(first), obj); return Status::Ok(); }
  // 圧縮テーブルを展開
  std::vector<uint16_t> t;
  size_t units = raw.size() / 2;
  for (size_t i = 0; i < units && t.size() < 0x10000; i++) {
    uint16_t v = le16(raw.data() + 2 * i);
    if (v == 0xFFFF && i + 1 < units) {
      uint16_t cnt = le16(raw.data() + 2 * (i + 1));
      i++;
      for (uint32_t k = 0; k < cnt && t.size() < 0x10000; k++) t.push_back(static_cast<uint16_t>(t.size()));
    } else t.push_back(v);
  }
  upcase_ = std::move(t);
  have_upcase_ = true;
  return Status::Ok();
}

Status ExChk::read_dir(const std::vector<uint32_t>& clusters, std::vector<uint8_t>* out) {
  out->assign(static_cast<size_t>(clusters.size() * b_.cb), 0);
  for (size_t i = 0; i < clusters.size(); i++) {
    Status st = v_.read(clus_off(clusters[i]), out->data() + i * b_.cb, static_cast<size_t>(b_.cb));
    if (!st.ok()) return st;
  }
  return Status::Ok();
}

Status ExChk::scan_dir(const std::vector<uint8_t>& data, bool is_root, const std::string& path, std::vector<Sub>* subs, bool sys_only) {
  size_t n = data.size() / 32;
  int bitmaps = 0, upcases = 0;
  std::unordered_set<std::string> names;
  for (size_t i = 0; i < n;) {
    const uint8_t* e = data.data() + i * 32;
    uint8_t type = e[0];
    if (type == 0) break;
    if (!(type & 0x80)) { i++; continue; }  // 削除済み
    if (!sys_only && !c_.count_object()) return Status::Ok();
    std::string where = path.empty() ? "/" : path;
    if (type == 0x81 || type == 0x82 || type == 0x83 || type == 0xA0) {
      if (!is_root) { if (!sys_only) c_.error("exfat-sysentry-outside-root", "a bitmap / upcase / label entry is outside the root directory", UINT64_MAX, where); i++; continue; }
      if (type == 0x81) {
        bitmaps++;
        if (sys_only) {
          if (bitmaps > 1) c_.warn("exfat-dup-bitmap", "the root directory has more than one allocation bitmap entry", UINT64_MAX, "/");
          else { bitmap_first_ = le32(e + 20); bitmap_len_ = le64(e + 24); have_bitmap_ = true; }
          if (e[1] & 1) c_.warn("exfat-second-bitmap", "the root directory has the second allocation bitmap flag (TexFAT)", UINT64_MAX, "/");
        }
      } else if (type == 0x82) {
        upcases++;
        if (sys_only) {
          if (upcases > 1) c_.warn("exfat-dup-upcase", "the root directory has more than one upcase table entry", UINT64_MAX, "/");
          else {
            Status st = load_upcase(le32(e + 20), le64(e + 24), le32(e + 4), "upcase table");
            if (!st.ok()) return st;
            upcases = 1;
          }
        }
      } else if (type == 0x83) {
        if (sys_only && e[1] > 11) c_.error("exfat-label-length", "the volume label is longer than 11 characters", UINT64_MAX, "/");
      }
      i++;
      continue;
    }
    if (sys_only) { i++; continue; }
    if (type == 0x85) {
      uint8_t sc = e[1];
      std::string obj = where;
      if (sc < 2 || sc > 18) { c_.error("exfat-secondary-count", "a file entry has a bad secondary count " + num(sc), UINT64_MAX, where); i++; continue; }
      if (i + sc >= n) { c_.error("exfat-set-truncated", "a file entry set runs past the end of the directory", UINT64_MAX, where); break; }
      bool broken = false;
      for (size_t k = 1; k <= sc; k++)
        if (data[(i + k) * 32] == 0 || !(data[(i + k) * 32] & 0x80)) { broken = true; break; }
      if (broken) { c_.error("exfat-set-incomplete", "a file entry set is incomplete (missing stream / name entries)", UINT64_MAX, where); i++; continue; }
      const uint8_t* st_e = e + 32;
      if (st_e[0] != 0xC0) { c_.error("exfat-no-stream", "a file entry is not followed by a stream extension entry", UINT64_MAX, where); i++; continue; }
      uint8_t name_len = st_e[3];
      size_t need_names = (static_cast<size_t>(name_len) + 14) / 15;
      std::vector<uint16_t> name;
      size_t names_found = 0;
      bool name_ok = true;
      for (size_t k = 2; k <= sc; k++) {
        const uint8_t* ne = data.data() + (i + k) * 32;
        if (ne[0] != 0xC1) { if (names_found < need_names) name_ok = false; break; }
        names_found++;
        for (int u = 0; u < 15; u++) name.push_back(le16(ne + 2 + 2 * u));
      }
      if (!name_ok || names_found != need_names) {
        c_.error("exfat-name-entries", "the number of file name entries (" + num(names_found) + ") does not match the name length " + num(name_len), UINT64_MAX, where);
        i += 1ull + sc;
        continue;
      }
      if (name_len == 0) { c_.error("exfat-empty-name", "a file has an empty name", UINT64_MAX, where); i += 1ull + sc; continue; }
      name.resize(name_len);
      std::string nm = utf16_to_utf8(name);
      obj = (path.empty() ? "" : path) + "/" + nm;
      // SetChecksum
      uint16_t sum = 0;
      for (size_t k = 0; k <= sc; k++) {
        const uint8_t* p = data.data() + (i + k) * 32;
        for (int b = 0; b < 32; b++) {
          if (k == 0 && (b == 2 || b == 3)) continue;
          sum = set_checksum_step(sum, p[b]);
        }
      }
      if (sum != le16(e + 2)) c_.error("exfat-set-checksum", "the file entry set checksum is wrong", UINT64_MAX, obj);
      // NameHash
      uint16_t h = 0;
      for (uint16_t ch : name) { uint16_t u = up(ch); h = set_checksum_step(h, static_cast<uint8_t>(u & 0xFF)); h = set_checksum_step(h, static_cast<uint8_t>(u >> 8)); }
      if (h != le16(st_e + 4)) c_.error("exfat-name-hash", "the file name hash does not match the name", UINT64_MAX, obj);
      // 名前に使えない文字
      for (uint16_t ch : name)
        if (ch < 0x20 || ch == '"' || ch == '*' || ch == '/' || ch == ':' || ch == '<' || ch == '>' || ch == '?' || ch == '\\' || ch == '|') {
          c_.error("exfat-bad-name-char", "the file name contains a character that is not allowed", UINT64_MAX, obj);
          break;
        }
      // 重複 (大文字小文字を同一視)
      {
        std::string key;
        for (uint16_t ch : name) { uint16_t u = up(ch); key.push_back(static_cast<char>(u & 0xFF)); key.push_back(static_cast<char>(u >> 8)); }
        if (!names.insert(key).second) c_.error("exfat-duplicate-name", "duplicate file name in the directory", UINT64_MAX, obj);
      }
      uint16_t attr = le16(e + 4);
      bool is_dir = (attr & 0x10) != 0;
      uint8_t sflags = st_e[1];
      bool nofat = (sflags & 2) != 0;
      uint64_t vdl = le64(st_e + 8), dlen = le64(st_e + 24);
      uint32_t first = le32(st_e + 20);
      if (!(sflags & 1) && (dlen || first)) c_.warn("exfat-alloc-flag", "the stream has data but AllocationPossible is not set", UINT64_MAX, obj);
      if (vdl > dlen) c_.error("exfat-vdl", "ValidDataLength (" + num(vdl) + ") exceeds DataLength (" + num(dlen) + ")", UINT64_MAX, obj);
      if (is_dir) c_.rep.stats.directories++; else c_.rep.stats.files++;
      if (dlen == 0) {
        if (first != 0) c_.error("exfat-empty-with-cluster", "an empty file / directory has a start cluster", UINT64_MAX, obj);
        if (is_dir) c_.error("exfat-dir-empty-stream", "a directory has no allocated clusters", UINT64_MAX, obj);
      } else {
        if (is_dir && (dlen % b_.cb)) c_.error("exfat-dir-size", "the directory size is not a multiple of the cluster size", UINT64_MAX, obj);
        if (!valid_cluster(first)) c_.error("exfat-start-range", "the start cluster " + num(first) + " is out of range", UINT64_MAX, obj);
        else if (dlen > static_cast<uint64_t>(b_.clus_count) * b_.cb) c_.error("exfat-size-range", "the size " + num(dlen) + " is larger than the volume", UINT64_MAX, obj);
        else {
          std::vector<uint32_t> cls;
          uint64_t len = 0;
          bool ok = true;
          uint64_t expect = (dlen + b_.cb - 1) / b_.cb;
          Status st = chain_of(first, dlen, nofat, obj, is_dir ? &cls : nullptr, (4u << 20) / static_cast<uint32_t>(b_.cb) + 1, &len, &ok);
          if (!st.ok()) return st;
          if (ok && len != expect)
            c_.error("exfat-size-mismatch", "the size is " + num(dlen) + " bytes (" + num(expect) + " clusters) but the cluster chain has " + num(len) + " clusters", UINT64_MAX, obj);
          if (ok && is_dir) subs->push_back({first, dlen, nofat, obj});
        }
      }
      i += 1ull + sc;
      continue;
    }
    if ((type & 0xC0) == 0x40) {  // 使われていない場所に現れた副エントリ (0xC0 / 0xC1 ...)
      c_.error("exfat-orphan-secondary", "a secondary entry (" + hex(type) + ") is not part of any entry set", UINT64_MAX, where);
      i++;
      continue;
    }
    if (!(type & 0x20) && (type & 0xC0) == 0x80) c_.warn("exfat-unknown-entry", "an unknown critical entry type " + hex(type), UINT64_MAX, where);
    i++;
  }
  if (is_root && !sys_only) {
    // ルートに必須のエントリ
    if (!have_bitmap_) c_.error("exfat-no-bitmap", "the root directory has no allocation bitmap entry", UINT64_MAX, "/");
    if (!have_upcase_) c_.error("exfat-no-upcase", "the root directory has no (valid) upcase table entry", UINT64_MAX, "/");
  }
  return Status::Ok();
}

Status ExChk::bitmap_compare() {
  if (!have_bitmap_ || bitmap_clusters_.empty()) return Status::Ok();
  uint64_t missing = 0, leaked = 0, first_missing = 0, first_leak = 0;
  // 1 クラスタ分ずつ読む
  std::vector<uint8_t> buf(static_cast<size_t>(b_.cb));
  uint64_t total_bytes = (static_cast<uint64_t>(b_.clus_count) + 7) / 8;
  for (size_t ci = 0; ci < bitmap_clusters_.size(); ci++) {
    uint64_t base_byte = static_cast<uint64_t>(ci) * b_.cb;
    if (base_byte >= total_bytes) break;
    Status st = v_.read(clus_off(bitmap_clusters_[ci]), buf.data(), buf.size());
    if (!st.ok()) return st;
    uint64_t lim = std::min<uint64_t>(b_.cb, total_bytes - base_byte);
    for (uint64_t k = 0; k < lim; k++) {
      uint8_t x = buf[static_cast<size_t>(k)];
      for (int bit = 0; bit < 8; bit++) {
        uint64_t idx = (base_byte + k) * 8 + static_cast<uint64_t>(bit);
        if (idx >= b_.clus_count) break;
        uint32_t cl = static_cast<uint32_t>(idx + 2);
        bool marked = (x >> bit) & 1, used = claimed_.get(cl);
        if (marked) used_++;
        if (used && !marked) { if (!missing) first_missing = cl; missing++; }
        else if (marked && !used) { if (!leaked) first_leak = cl; leaked++; }
      }
    }
    if (c_.stop()) return Status::Ok();
  }
  if (missing) c_.error("exfat-bitmap-missing", num(missing) + " cluster(s) are in use but not marked in the allocation bitmap (first: cluster " + num(first_missing) + ")", clus_off(static_cast<uint32_t>(first_missing)));
  if (leaked) c_.warn("exfat-bitmap-leak", num(leaked) + " cluster(s) are marked allocated but belong to nothing (first: cluster " + num(first_leak) + ")", clus_off(static_cast<uint32_t>(first_leak)));
  return Status::Ok();
}

Status ExChk::load_bitmap_chain() {
  // ビットマップ自身の領域 (クラスタ列)。先頭 2 エントリ以外の NoFatChain 連続を想定しつつ FAT 連鎖でも読む
  if (!have_bitmap_) return Status::Ok();
  uint64_t need = (static_cast<uint64_t>(b_.clus_count) + 7) / 8;
  if (bitmap_len_ < need) { c_.error("exfat-bitmap-size", "the allocation bitmap is smaller (" + num(bitmap_len_) + " bytes) than needed (" + num(need) + ")", UINT64_MAX, "allocation bitmap"); have_bitmap_ = false; return Status::Ok(); }
  if (!valid_cluster(bitmap_first_)) { c_.error("exfat-bitmap-cluster", "the allocation bitmap start cluster is out of range", UINT64_MAX, "allocation bitmap"); have_bitmap_ = false; return Status::Ok(); }
  // ルートのエントリには NoFatChain の情報が無い。FAT がビットマップの連鎖を持っていればそれを使い、無ければ連続とみなす
  uint64_t nclus = (bitmap_len_ + b_.cb - 1) / b_.cb;
  uint32_t nx = 0;
  Status st = fat_get(bitmap_first_, &nx);
  if (!st.ok()) return st;
  bool chained = nx != 0;
  bitmap_nofat_ = !chained;
  bool ok = true;
  uint64_t len = 0;
  std::vector<uint32_t> cls;
  st = chain_of(bitmap_first_, bitmap_len_, !chained, "allocation bitmap", &cls, 1u << 22, &len, &ok);
  if (!st.ok()) return st;
  if (!ok || len != nclus) { if (ok) c_.error("exfat-bitmap-chain", "the allocation bitmap chain length does not match its size", UINT64_MAX, "allocation bitmap"); have_bitmap_ = false; return Status::Ok(); }
  bitmap_clusters_ = std::move(cls);
  return Status::Ok();
}

Status ExChk::run() {
  Status st = boot_region();
  if (!st.ok()) return st.code == Errc::Corrupt ? Status::Ok() : st;
  c_.rep.stats.units_total = b_.clus_count;
  c_.rep.stats.bytes_per_unit = b_.cb;
  if (c_.over_budget(Bits::bytes_for(b_.clus_count + 2ull) * 2)) { c_.incomplete("memory-limit", "the volume is too large for the memory budget; only the boot region was checked"); return Status::Ok(); }
  if (!claimed_.init(b_.clus_count + 2ull) || !walk_.init(b_.clus_count + 2ull)) { c_.incomplete("memory-limit", "not enough memory"); return Status::Ok(); }
  // FAT の先頭 2 エントリ
  uint32_t f0, f1;
  st = fat_get(0, &f0);
  if (!st.ok()) return st;
  st = fat_get(1, &f1);
  if (!st.ok()) return st;
  if ((f0 & 0xFFFFFFF8u) != 0xFFFFFFF8u || f1 != 0xFFFFFFFFu) c_.info("exfat-fat-header", "the first two FAT entries are not the standard values (" + hex(f0) + ", " + hex(f1) + ")", sec_off(b_.fat_off));
  // ルートディレクトリの連鎖
  std::vector<uint32_t> root_cls;
  uint64_t rlen = 0;
  bool ok = true;
  st = chain_of(b_.root, 1, false, "/", &root_cls, (64u << 20) / static_cast<uint32_t>(b_.cb) + 1, &rlen, &ok);
  if (!st.ok()) return st;
  if (!ok || root_cls.empty()) return Status::Ok();
  if (rlen * b_.cb > (64u << 20)) { c_.incomplete("dir-too-large", "the root directory is larger than 64 MiB; the check stopped"); return Status::Ok(); }
  std::vector<uint8_t> rdata;
  st = read_dir(root_cls, &rdata);
  if (!st.ok()) return st;
  std::vector<Sub> subs;
  // 1 周目: ビットマップとアップケーステーブルを先に読む (名前ハッシュの検証に必要)
  st = scan_dir(rdata, true, "", &subs, true);
  if (!st.ok()) return st;
  st = load_bitmap_chain();
  if (!st.ok()) return st;
  // 2 周目
  subs.clear();
  c_.progress("directories", 0, 0);
  st = scan_dir(rdata, true, "", &subs, false);
  if (!st.ok() || c_.cancelled) return st;
  std::vector<Sub> stack(subs.rbegin(), subs.rend());
  uint64_t done = 0;
  while (!stack.empty()) {
    if (c_.stop()) return Status::Ok();
    Sub s = stack.back();
    stack.pop_back();
    // ディレクトリのクラスタ列 (claimed は chain_of 済みなので、ここでは再度集めるだけ)
    std::vector<uint32_t> cls;
    uint64_t n = (s.len + b_.cb - 1) / b_.cb;
    if (n * b_.cb > (64u << 20)) { c_.warn("exfat-dir-large", "a directory is larger than 64 MiB and was not checked", UINT64_MAX, s.path); continue; }
    if (s.nofat) for (uint64_t i = 0; i < n; i++) cls.push_back(static_cast<uint32_t>(s.first + i));
    else {
      uint32_t cl = s.first;
      for (uint64_t i = 0; i < n; i++) {
        if (!valid_cluster(cl)) break;
        cls.push_back(cl);
        uint32_t nx;
        st = fat_get(cl, &nx);
        if (!st.ok()) return st;
        if (nx >= kEofMin || nx == 0 || nx == kBad) break;
        cl = nx;
      }
    }
    std::vector<uint8_t> data;
    st = read_dir(cls, &data);
    if (!st.ok()) return st;
    std::vector<Sub> sub2;
    st = scan_dir(data, false, s.path, &sub2, false);
    if (!st.ok() || c_.cancelled) return st;
    for (size_t i = sub2.size(); i-- > 0;) stack.push_back(sub2[i]);
    if ((++done & 63) == 0) c_.progress("directories", done, 0);
  }
  c_.progress("bitmap", 0, 0);
  st = bitmap_compare();
  if (!st.ok()) return st;
  c_.rep.stats.units_used = used_;
  return Status::Ok();
}

}  // namespace

Status check_exfat(Ctx& c) {
  ExChk k(c);
  return k.run();
}

}  // namespace fsck
}  // namespace usbmem
