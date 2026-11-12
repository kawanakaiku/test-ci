// SPDX-License-Identifier: Apache-2.0
// NTFS の読み取り専用の整合性検査 (報告のみ。ntfsfix のような「修復」は一切しない)。
//
// 検査する内容:
//   ブートセクタとバックアップ、$MFT / $MFTMirr の一致、全 MFT レコードの構造 (マルチセクタ保護・ヘッダ・属性・ランリスト)、
//   MFT ビットマップと使用中フラグ、$Bitmap とクラスタ使用 (二重割り当て・未登録・取りこぼし)、親ディレクトリの整合、
//   ボリュームのダーティフラグ、$LogFile の状態 (未再生ジャーナル)。
// 検査しない内容 (限界): ディレクトリのインデックス (B+ ツリー) の中身・$Secure・$UpCase・$AttrDef・$LogFile の再生可否・圧縮データの解凍。
#include <algorithm>
#include <unordered_map>

#include "fscheck_internal.h"

namespace usbmem {
namespace fsck {
namespace {

struct Run { uint64_t vcn, lcn, len; bool sparse; };

constexpr uint32_t AT_STANDARD_INFORMATION = 0x10, AT_ATTRIBUTE_LIST = 0x20, AT_FILE_NAME = 0x30, AT_VOLUME_INFORMATION = 0x70,
                   AT_DATA = 0x80, AT_BITMAP = 0xB0;
constexpr uint64_t kNoRec = UINT64_MAX;

class NtfsChk {
 public:
  explicit NtfsChk(Ctx& c) : c_(c), v_(c.vol) {}
  Status run();

 private:
  Ctx& c_;
  Vol& v_;
  uint32_t bps_ = 0, spc_ = 0, rs_ = 0;
  uint64_t cs_ = 0, total_sectors_ = 0, total_clusters_ = 0, mft_lcn_ = 0, mirr_lcn_ = 0;
  std::vector<Run> mft_runs_;
  uint64_t mft_data_size_ = 0, mft_records_ = 0;
  Bits claimed_, in_use_, is_dir_;
  std::vector<uint32_t> parent_;
  std::vector<uint16_t> parent_seq_, seq_;
  bool have_parent_ = false;
  std::vector<Run> bitmap_runs_, logfile_runs_, mirr_runs_;
  uint64_t bitmap_size_ = 0, logfile_size_ = 0;
  std::vector<uint8_t> mft_bitmap_;
  bool have_mft_bitmap_ = false;
  std::vector<std::vector<uint8_t>> first4_;
  uint64_t in_use_count_ = 0, bad_records_ = 0;

  Status boot();
  Status map_read(const std::vector<Run>& runs, uint64_t pos, void* buf, size_t n, bool* hole);
  Status read_record(uint64_t n, std::vector<uint8_t>* out);
  bool fixups(uint8_t* rec, uint64_t n, bool report);
  bool decode_runs(const uint8_t* p, size_t avail, uint64_t start_vcn, std::vector<Run>* out, uint64_t* total, const std::string& obj);
  Status scan_records();
  Status handle_record(uint64_t n, std::vector<uint8_t>& rec);
  Status load_mft_runs();
  Status check_bitmap();
  Status check_mirror();
  Status check_logfile();
  void claim(const std::vector<Run>& runs, const std::string& obj);
  Status read_all(const std::vector<Run>& runs, uint64_t size, std::vector<uint8_t>* out, uint64_t cap);
};

// ---- ランリスト

bool NtfsChk::decode_runs(const uint8_t* p, size_t avail, uint64_t start_vcn, std::vector<Run>* out, uint64_t* total, const std::string& obj) {
  size_t i = 0;
  int64_t lcn = 0;
  uint64_t vcn = start_vcn;
  *total = 0;
  while (i < avail && p[i]) {
    unsigned ls = p[i] & 0xF, os = p[i] >> 4;
    i++;
    if (ls == 0 || ls > 8 || os > 8 || i + ls + os > avail) { c_.error("ntfs-runlist", "a run list is malformed", UINT64_MAX, obj); return false; }
    uint64_t len = 0;
    for (unsigned k = 0; k < ls; k++) len |= static_cast<uint64_t>(p[i + k]) << (8 * k);
    i += ls;
    if (len == 0) { c_.error("ntfs-runlist", "a run has zero length", UINT64_MAX, obj); return false; }
    Run r{vcn, 0, len, os == 0};
    if (os) {
      int64_t d = 0;
      for (unsigned k = 0; k < os; k++) d |= static_cast<int64_t>(p[i + k]) << (8 * k);
      if (p[i + os - 1] & 0x80 && os < 8) d |= -(int64_t(1) << (8 * os));
      i += os;
      lcn += d;
      if (lcn < 0) { c_.error("ntfs-runlist", "a run has a negative cluster number", UINT64_MAX, obj); return false; }
      r.lcn = static_cast<uint64_t>(lcn);
      if (r.lcn >= total_clusters_ || r.len > total_clusters_ - r.lcn) { c_.error("ntfs-run-range", "a run [" + num(r.lcn) + ", +" + num(r.len) + ") is outside the volume", UINT64_MAX, obj); return false; }
    }
    if (out) out->push_back(r);
    vcn += len;
    *total += len;
    if (out && out->size() > (1u << 22)) { c_.error("ntfs-runlist", "a run list has too many runs", UINT64_MAX, obj); return false; }
  }
  return true;
}

Status NtfsChk::map_read(const std::vector<Run>& runs, uint64_t pos, void* buf, size_t n, bool* hole) {
  uint8_t* d = static_cast<uint8_t*>(buf);
  *hole = false;
  while (n) {
    uint64_t vcn = pos / cs_, in = pos % cs_;
    // run を探す (線形でなく二分探索)
    size_t lo = 0, hi = runs.size();
    while (lo < hi) { size_t mid = (lo + hi) / 2; if (runs[mid].vcn + runs[mid].len <= vcn) lo = mid + 1; else hi = mid; }
    if (lo >= runs.size() || runs[lo].vcn > vcn) return Status::Make(Errc::OutOfRange);
    const Run& r = runs[lo];
    uint64_t left_in_run = (r.vcn + r.len - vcn) * cs_ - in;
    size_t take = static_cast<size_t>(std::min<uint64_t>(n, left_in_run));
    if (r.sparse) { std::memset(d, 0, take); *hole = true; }
    else {
      Status st = v_.read((r.lcn + (vcn - r.vcn)) * cs_ + in, d, take);
      if (!st.ok()) return st;
    }
    d += take; pos += take; n -= take;
  }
  return Status::Ok();
}

Status NtfsChk::read_all(const std::vector<Run>& runs, uint64_t size, std::vector<uint8_t>* out, uint64_t cap) {
  if (size > cap) return Status::Make(Errc::OutOfRange);
  out->assign(static_cast<size_t>(size), 0);
  bool h;
  return size ? map_read(runs, 0, out->data(), out->size(), &h) : Status::Ok();
}

// ---- ブートセクタ

Status NtfsChk::boot() {
  uint8_t s[512];
  Status st = v_.read(0, s, 512);
  if (!st.ok()) return st;
  if (std::memcmp(s + 3, "NTFS    ", 8) != 0) { c_.error("ntfs-oem-id", "the boot sector OEM ID is not \"NTFS    \"", 3); return Status::Make(Errc::Corrupt); }
  if (s[510] != 0x55 || s[511] != 0xAA) c_.error("ntfs-boot-signature", "boot sector signature 0x55AA is missing", 510);
  bps_ = le16(s + 11);
  uint8_t spc = s[13];
  bool bad = false;
  if (bps_ < 256 || bps_ > 4096 || !is_pow2(bps_)) { c_.error("ntfs-bad-sector-size", "bytes per sector " + num(bps_) + " is invalid", 11); bad = true; }
  if (spc == 0) { c_.error("ntfs-bad-cluster-size", "sectors per cluster is zero", 13); bad = true; }
  if (bad) return Status::Make(Errc::Corrupt);
  spc_ = spc <= 0x80 ? spc : (1u << (256 - spc));
  if (spc > 0x80 && (256 - spc) > 16) { c_.error("ntfs-bad-cluster-size", "bad sectors per cluster byte", 13); return Status::Make(Errc::Corrupt); }
  if (!is_pow2(spc_) || spc_ > (1u << 16)) { c_.error("ntfs-bad-cluster-size", "sectors per cluster " + num(spc_) + " is invalid", 13); return Status::Make(Errc::Corrupt); }
  cs_ = static_cast<uint64_t>(bps_) * spc_;
  if (s[14] || s[15] || s[16] || s[17] || s[18] || s[19] || s[20]) c_.warn("ntfs-bpb-zero-fields", "reserved BPB fields that must be zero are not", 14);
  total_sectors_ = le64(s + 0x28);
  mft_lcn_ = le64(s + 0x30);
  mirr_lcn_ = le64(s + 0x38);
  int8_t cpr = static_cast<int8_t>(s[0x40]);
  rs_ = cpr > 0 ? static_cast<uint32_t>(cpr) * static_cast<uint32_t>(cs_) : (1u << (-cpr));
  if (cpr < -16 || cpr == 0 || rs_ < 256 || rs_ > (1u << 16) || !is_pow2(rs_)) { c_.error("ntfs-bad-record-size", "MFT record size is invalid (clusters per record byte " + num(static_cast<uint8_t>(s[0x40])) + ")", 0x40); return Status::Make(Errc::Corrupt); }
  if (total_sectors_ == 0 || total_sectors_ > v_.size() / bps_) {
    if (total_sectors_ == 0) { c_.error("ntfs-bad-total", "total sector count is zero", 0x28); return Status::Make(Errc::Corrupt); }
    c_.error("ntfs-larger-than-device", "the file system claims " + num(total_sectors_) + " sectors but the device has " + num(v_.size() / bps_), 0x28);
  }
  total_clusters_ = total_sectors_ / spc_;
  if (mft_lcn_ >= total_clusters_ || mirr_lcn_ >= total_clusters_ || mft_lcn_ == 0) { c_.error("ntfs-bad-mft-location", "the MFT / MFTMirr location is outside the volume", 0x30); return Status::Make(Errc::Corrupt); }
  // バックアップブートセクタ: ボリューム末尾 (total_sectors 番目のセクタ)
  uint64_t bk = total_sectors_ * bps_;
  if (bk + 512 <= v_.size()) {
    uint8_t b[512];
    st = v_.read(bk, b, 512);
    if (!st.ok()) return st;
    if (std::memcmp(b + 3, "NTFS    ", 8) != 0) c_.warn("ntfs-no-backup-boot", "the backup boot sector (last sector) is missing or invalid", bk);
    else if (std::memcmp(b, s, 0x50) != 0) c_.warn("ntfs-backup-boot-differs", "the backup boot sector differs from the boot sector", bk);
  } else c_.warn("ntfs-no-backup-boot", "the device is too small to contain the backup boot sector", bk);
  c_.rep.stats.units_total = total_clusters_;
  c_.rep.stats.bytes_per_unit = cs_;
  return Status::Ok();
}

// ---- MFT レコード

bool NtfsChk::fixups(uint8_t* rec, uint64_t n, bool report) {
  std::string obj = "mft record " + num(n);
  uint16_t ufo = le16(rec + 4), ufc = le16(rec + 6);
  uint32_t blocks = rs_ / 512;
  if (ufo < 0x2A || ufo % 2 || static_cast<uint32_t>(ufo) + 2u * ufc > rs_ || ufc != blocks + 1u) {
    if (report) c_.error("ntfs-record-usa", "the update sequence array of the record is invalid", UINT64_MAX, obj);
    return false;
  }
  uint16_t usn = le16(rec + ufo);
  for (uint32_t i = 0; i < blocks; i++) {
    uint8_t* tail = rec + 512u * (i + 1) - 2;
    if (le16(tail) != usn) {
      if (report) c_.error("ntfs-record-fixup", "the multi-sector transfer check failed (torn write) at sector " + num(i) + " of the record", UINT64_MAX, obj);
      return false;
    }
  }
  for (uint32_t i = 0; i < blocks; i++) {
    uint8_t* tail = rec + 512u * (i + 1) - 2;
    tail[0] = rec[ufo + 2 + 2 * i];
    tail[1] = rec[ufo + 3 + 2 * i];
  }
  return true;
}

Status NtfsChk::read_record(uint64_t n, std::vector<uint8_t>* out) {
  out->resize(rs_);
  bool h;
  return map_read(mft_runs_, n * rs_, out->data(), rs_, &h);
}

void NtfsChk::claim(const std::vector<Run>& runs, const std::string& obj) {
  uint64_t dup = 0, first = 0;
  for (const Run& r : runs) {
    if (r.sparse) continue;
    for (uint64_t k = 0; k < r.len; k++) {
      uint64_t cl = r.lcn + k;
      if (claimed_.get(cl)) { if (!dup) first = cl; dup++; }
      else claimed_.set(cl);
    }
  }
  if (dup) c_.error("ntfs-crosslink", num(dup) + " cluster(s) are allocated to more than one file / stream (first: cluster " + num(first) + ")", first * cs_, obj);
}

Status NtfsChk::handle_record(uint64_t n, std::vector<uint8_t>& rec) {
  std::string obj = "mft record " + num(n);
  uint8_t* r = rec.data();
  if (std::memcmp(r, "FILE", 4) != 0) {
    if (!std::memcmp(r, "BAAD", 4)) { c_.error("ntfs-record-baad", "the record is marked BAAD (failed multi-sector transfer)", UINT64_MAX, obj); bad_records_++; return Status::Ok(); }
    if (all_zero(r, rs_)) {
      if (n < 16 && n != 15) c_.error("ntfs-system-record-empty", "a system file record is empty", UINT64_MAX, obj);
      return Status::Ok();
    }
    c_.error("ntfs-record-magic", "the record has a bad magic number", UINT64_MAX, obj);
    bad_records_++;
    return Status::Ok();
  }
  if (!fixups(r, n, true)) { bad_records_++; return Status::Ok(); }
  uint16_t flags = le16(r + 0x16);
  uint32_t used = le32(r + 0x18), alloc = le32(r + 0x1C);
  uint16_t aoff = le16(r + 0x14);
  uint64_t base = le64(r + 0x20);
  bool in_use = flags & 1, is_dir = flags & 2;
  if (le16(r + 4) >= 0x30) {
    uint32_t selfn = le32(r + 0x2C);
    if (selfn != 0 && selfn != n) c_.error("ntfs-record-number", "the record's own number field is " + num(selfn), UINT64_MAX, obj);
  }
  if (alloc != rs_ || used > alloc || used < 0x30 || (used & 7)) { c_.error("ntfs-record-size", "the record's used/allocated size (" + num(used) + "/" + num(alloc) + ") is invalid", UINT64_MAX, obj); bad_records_++; return Status::Ok(); }
  if (aoff < 0x30 || (aoff & 7) || aoff >= used) { c_.error("ntfs-record-attr-offset", "the first attribute offset " + num(aoff) + " is invalid", UINT64_MAX, obj); bad_records_++; return Status::Ok(); }
  if (n < 16 && !in_use && n != 15 && !(n >= 12)) c_.error("ntfs-system-record-unused", "a system file record is not marked in use", UINT64_MAX, obj);
  if (!in_use) return Status::Ok();
  in_use_.set(n);
  in_use_count_++;
  if (is_dir) is_dir_.set(n);
  if (have_parent_ && n < parent_.size()) seq_[n] = le16(r + 0x10);
  if (base != 0 && (base & 0xFFFFFFFFFFFFull) != 0) {
    uint64_t bn = base & 0xFFFFFFFFFFFFull;
    if (bn >= mft_records_) c_.error("ntfs-base-record", "the base record reference " + num(bn) + " is outside the MFT", UINT64_MAX, obj);
  }
  // 属性の走査
  uint32_t off = aoff;
  uint32_t prev_type = 0;
  bool ended = false;
  std::vector<Run> runs;
  while (off + 8 <= used) {
    uint32_t type = le32(r + off);
    if (type == 0xFFFFFFFFu) { ended = true; break; }
    uint32_t len = le32(r + off + 4);
    if (len < 0x18 || (len & 7) || off + len > used) { c_.error("ntfs-attr-length", "an attribute has an invalid length " + num(len), UINT64_MAX, obj); bad_records_++; return Status::Ok(); }
    if (type < prev_type) c_.error("ntfs-attr-order", "attributes are not in ascending type order", UINT64_MAX, obj);
    prev_type = type;
    if (type == 0 || (type & 0xF)) { c_.error("ntfs-attr-type", "an attribute has an invalid type code " + hex(type), UINT64_MAX, obj); bad_records_++; return Status::Ok(); }
    const uint8_t* a = r + off;
    bool nonres = a[8] != 0;
    uint8_t nl = a[9];
    uint16_t no = le16(a + 10);
    if (nl && (no < (nonres ? 0x40u : 0x18u) || static_cast<uint32_t>(no) + 2u * nl > len)) { c_.error("ntfs-attr-name", "an attribute name lies outside the attribute", UINT64_MAX, obj); bad_records_++; return Status::Ok(); }
    if (!nonres) {
      uint32_t vl = le32(a + 0x10);
      uint16_t vo = le16(a + 0x14);
      if (vo < 0x18 || static_cast<uint64_t>(vo) + vl > len) { c_.error("ntfs-attr-value", "a resident attribute value lies outside the attribute", UINT64_MAX, obj); bad_records_++; return Status::Ok(); }
      const uint8_t* v = a + vo;
      if (type == AT_FILE_NAME && vl >= 0x42 && base == 0 && have_parent_ && n < parent_.size() && parent_[n] == 0xFFFFFFFFu) {
        uint64_t pref = le64(v);
        parent_[n] = static_cast<uint32_t>(pref & 0xFFFFFFFFFFFFull);
        parent_seq_[n] = static_cast<uint16_t>(pref >> 48);
      }
      if (type == AT_VOLUME_INFORMATION && n == 3 && vl >= 12) {
        uint16_t vf = le16(v + 10);
        if (vf & 0x0001) { c_.warn("ntfs-dirty", "the volume dirty flag is set (the volume was not cleanly dismounted)", UINT64_MAX, "$Volume"); c_.rep.unclean = true; }
        if (vf & 0x0080) c_.warn("ntfs-chkdsk-scheduled", "a disk check is scheduled for the next mount", UINT64_MAX, "$Volume");
        if (vf & 0x0002) c_.info("ntfs-resize-logfile", "the volume has the 'resize $LogFile' flag", UINT64_MAX, "$Volume");
      }
      if (type == AT_BITMAP && n == 0 && nl == 0) { mft_bitmap_.assign(v, v + vl); have_mft_bitmap_ = true; }
    } else {
      if (len < 0x40) { c_.error("ntfs-attr-length", "a non-resident attribute header is too short", UINT64_MAX, obj); bad_records_++; return Status::Ok(); }
      uint64_t svcn = le64(a + 0x10), evcn = le64(a + 0x18);
      uint16_t mp = le16(a + 0x20);
      uint64_t alloc_sz = le64(a + 0x28), data_sz = le64(a + 0x30), init_sz = le64(a + 0x38);
      if (mp < 0x40 || mp > len) { c_.error("ntfs-attr-mapping", "the mapping pairs offset is invalid", UINT64_MAX, obj); bad_records_++; return Status::Ok(); }
      if (evcn + 1 < svcn) { c_.error("ntfs-attr-vcn", "an attribute's last VCN is below its first VCN", UINT64_MAX, obj); bad_records_++; return Status::Ok(); }
      std::string aobj = obj + " attribute " + hex(type);
      std::vector<Run> rl;
      uint64_t total = 0;
      bool ok = decode_runs(a + mp, len - mp, svcn, &rl, &total, aobj);
      if (ok) {
        uint64_t expect = evcn + 1 - svcn;
        if (!(evcn == UINT64_MAX && svcn == 0) && total != expect) c_.error("ntfs-runlist-length", "the run list covers " + num(total) + " clusters but the VCN range is " + num(expect), UINT64_MAX, aobj);
        else {
          if (svcn == 0 && !(le16(a + 0xC) & 0x8000)) {
            uint16_t aflags = le16(a + 0xC);
            bool compressed = (aflags & 0xFF) != 0 || a[0x22] != 0;
            if (!compressed && alloc_sz != total * cs_) c_.warn("ntfs-attr-alloc-size", "the allocated size " + num(alloc_sz) + " does not match the run list (" + num(total * cs_) + ")", UINT64_MAX, aobj);
            if (data_sz > alloc_sz && !compressed) c_.error("ntfs-attr-size", "the data size exceeds the allocated size", UINT64_MAX, aobj);
            if (init_sz > data_sz) c_.error("ntfs-attr-init-size", "the initialized size exceeds the data size", UINT64_MAX, aobj);
          }
          claim(rl, aobj);
          if (n == 6 && type == AT_DATA && nl == 0) { for (auto& x : rl) bitmap_runs_.push_back(x); if (svcn == 0) bitmap_size_ = data_sz; }
          if (n == 2 && type == AT_DATA && nl == 0) { for (auto& x : rl) logfile_runs_.push_back(x); if (svcn == 0) logfile_size_ = data_sz; }
          if (n == 1 && type == AT_DATA && nl == 0) for (auto& x : rl) mirr_runs_.push_back(x);
          if (n == 0 && type == AT_BITMAP && nl == 0 && svcn == 0) {
            // $MFT のビットマップ (非常駐)
            std::vector<Run> br = rl;
            std::vector<uint8_t> tmp;
            if (read_all(br, std::min<uint64_t>(data_sz, 64u << 20), &tmp, 64u << 20).ok()) { mft_bitmap_ = std::move(tmp); have_mft_bitmap_ = true; }
          }
        }
      }
    }
    off += len;
  }
  (void)runs;
  if (!ended) c_.error("ntfs-attr-end", "the attribute list of the record is not terminated", UINT64_MAX, obj);
  if (base == 0 && n >= 16) { if (is_dir) c_.rep.stats.directories++; else c_.rep.stats.files++; }
  return Status::Ok();
}

Status NtfsChk::load_mft_runs() {
  // レコード 0 を MFT の位置から直接読み、$DATA のランリストを得る
  std::vector<uint8_t> rec(rs_);
  Status st = v_.read(mft_lcn_ * cs_, rec.data(), rs_);
  if (!st.ok()) return st;
  uint8_t* r = rec.data();
  if (std::memcmp(r, "FILE", 4) != 0) { c_.error("ntfs-mft-magic", "the first MFT record ($MFT) has a bad magic number", mft_lcn_ * cs_); return Status::Make(Errc::Corrupt); }
  if (!fixups(r, 0, true)) return Status::Make(Errc::Corrupt);
  uint16_t aoff = le16(r + 0x14);
  uint32_t used = le32(r + 0x18);
  if (aoff < 0x30 || aoff >= used || used > rs_) { c_.error("ntfs-record-attr-offset", "the first attribute offset of $MFT is invalid", UINT64_MAX, "mft record 0"); return Status::Make(Errc::Corrupt); }
  uint32_t off = aoff;
  bool found = false;
  bool has_list = false;
  while (off + 8 <= used) {
    uint32_t type = le32(r + off);
    if (type == 0xFFFFFFFFu) break;
    uint32_t len = le32(r + off + 4);
    if (len < 0x18 || (len & 7) || off + len > used) { c_.error("ntfs-attr-length", "an attribute of $MFT has an invalid length", UINT64_MAX, "mft record 0"); return Status::Make(Errc::Corrupt); }
    const uint8_t* a = r + off;
    if (type == AT_ATTRIBUTE_LIST) has_list = true;
    if (type == AT_DATA && a[9] == 0) {
      if (!a[8]) { c_.error("ntfs-mft-resident", "the $MFT data attribute is resident", UINT64_MAX, "mft record 0"); return Status::Make(Errc::Corrupt); }
      uint64_t svcn = le64(a + 0x10);
      uint16_t mp = le16(a + 0x20);
      if (svcn == 0) mft_data_size_ = le64(a + 0x30);
      uint64_t total = 0;
      std::vector<Run> rl;
      if (mp < 0x40 || mp > len || !decode_runs(a + mp, len - mp, svcn, &rl, &total, "mft record 0 $DATA")) return Status::Make(Errc::Corrupt);
      for (auto& x : rl) mft_runs_.push_back(x);
      found = true;
    }
    off += len;
  }
  if (!found) { c_.error("ntfs-mft-no-data", "$MFT has no unnamed data attribute", UINT64_MAX, "mft record 0"); return Status::Make(Errc::Corrupt); }
  if (has_list) {
    // $MFT が複数のレコードにまたがる場合: 属性リストを読んで、拡張レコードの $DATA を足す
    off = aoff;
    std::vector<uint8_t> list;
    while (off + 8 <= used) {
      uint32_t type = le32(r + off);
      if (type == 0xFFFFFFFFu) break;
      uint32_t len = le32(r + off + 4);
      if (len < 0x18 || off + len > used) break;
      const uint8_t* a = r + off;
      if (type == AT_ATTRIBUTE_LIST) {
        if (!a[8]) { uint32_t vl = le32(a + 0x10); uint16_t vo = le16(a + 0x14); if (vo + vl <= len) list.assign(a + vo, a + vo + vl); }
        else {
          uint64_t total = 0; std::vector<Run> rl;
          uint16_t mp = le16(a + 0x20);
          if (mp >= 0x40 && mp <= len && decode_runs(a + mp, len - mp, le64(a + 0x10), &rl, &total, "$MFT attribute list")) {
            if (!read_all(rl, std::min<uint64_t>(le64(a + 0x30), 1u << 20), &list, 1u << 20).ok()) list.clear();
          }
        }
      }
      off += len;
    }
    std::vector<std::pair<uint64_t, uint64_t>> ext;  // (lowest_vcn, record)
    for (size_t p = 0; p + 0x1A <= list.size();) {
      uint16_t el = le16(list.data() + p + 4);
      if (el < 0x1A || p + el > list.size()) break;
      uint32_t t = le32(list.data() + p);
      uint8_t nl = list[p + 6];
      uint64_t lv = le64(list.data() + p + 8);
      uint64_t ref = le64(list.data() + p + 0x10) & 0xFFFFFFFFFFFFull;
      if (t == AT_DATA && nl == 0 && ref != 0) ext.push_back({lv, ref});
      p += el;
    }
    std::sort(ext.begin(), ext.end());
    for (auto& e : ext) {
      std::vector<uint8_t> er;
      bool hole = false;
      er.resize(rs_);
      Status s2 = map_read(mft_runs_, e.second * rs_, er.data(), rs_, &hole);
      if (!s2.ok()) { c_.error("ntfs-mft-extension", "an extension record of $MFT cannot be read", UINT64_MAX, "mft record " + num(e.second)); break; }
      if (std::memcmp(er.data(), "FILE", 4) != 0 || !fixups(er.data(), e.second, true)) break;
      uint32_t eo = le16(er.data() + 0x14), eu = le32(er.data() + 0x18);
      while (eo + 8 <= eu && eu <= rs_) {
        uint32_t t = le32(er.data() + eo);
        if (t == 0xFFFFFFFFu) break;
        uint32_t len = le32(er.data() + eo + 4);
        if (len < 0x18 || eo + len > eu) break;
        const uint8_t* a = er.data() + eo;
        if (t == AT_DATA && a[9] == 0 && a[8]) {
          uint64_t total = 0; std::vector<Run> rl;
          uint16_t mp = le16(a + 0x20);
          if (mp >= 0x40 && mp <= len && decode_runs(a + mp, len - mp, le64(a + 0x10), &rl, &total, "mft extension $DATA"))
            for (auto& x : rl) mft_runs_.push_back(x);
        }
        eo += len;
      }
    }
  }
  std::sort(mft_runs_.begin(), mft_runs_.end(), [](const Run& a, const Run& b) { return a.vcn < b.vcn; });
  if (mft_data_size_ < 16ull * rs_) { c_.error("ntfs-mft-size", "$MFT is smaller than the 16 reserved records", UINT64_MAX, "mft record 0"); return Status::Make(Errc::Corrupt); }
  mft_records_ = mft_data_size_ / rs_;
  uint64_t covered = 0;
  for (auto& x : mft_runs_) covered += x.len * cs_;
  if (covered < mft_data_size_) { c_.error("ntfs-mft-runs", "the $MFT run list covers fewer bytes than its size", UINT64_MAX, "mft record 0"); return Status::Make(Errc::Corrupt); }
  return Status::Ok();
}

Status NtfsChk::scan_records() {
  if (mft_records_ > c_.opt.max_objects) { c_.incomplete("object-limit", "the MFT has too many records; the check stopped"); return Status::Ok(); }
  uint64_t budget = Bits::bytes_for(mft_records_) * 2 + mft_records_ * 10;
  have_parent_ = !c_.over_budget(budget + Bits::bytes_for(total_clusters_));
  if (c_.over_budget(Bits::bytes_for(total_clusters_) + Bits::bytes_for(mft_records_) * 2)) { c_.incomplete("memory-limit", "the volume is too large for the memory budget; only the boot sector and $MFT were checked"); return Status::Ok(); }
  if (!claimed_.init(total_clusters_) || !in_use_.init(mft_records_) || !is_dir_.init(mft_records_)) { c_.incomplete("memory-limit", "not enough memory"); return Status::Ok(); }
  if (have_parent_) { parent_.assign(static_cast<size_t>(mft_records_), 0xFFFFFFFFu); parent_seq_.assign(static_cast<size_t>(mft_records_), 0); seq_.assign(static_cast<size_t>(mft_records_), 0); }
  std::vector<uint8_t> rec;
  c_.progress("mft", 0, mft_records_);
  for (uint64_t n = 0; n < mft_records_; n++) {
    if (!c_.count_object()) return Status::Ok();
    Status st = read_record(n, &rec);
    if (!st.ok()) return st;
    if (n < 4) first4_.push_back(rec);   // 修正前の生データ (MFTMirr との比較用)
    st = handle_record(n, rec);
    if (!st.ok()) return st;
    if ((n & 0xFFF) == 0xFFF) c_.progress("mft", n + 1, mft_records_);
  }
  return Status::Ok();
}

Status NtfsChk::check_mirror() {
  if (first4_.size() < 4) return Status::Ok();
  // $MFTMirr は MFT 先頭 4 レコードの写し。ブートセクタが示す位置から読む
  std::vector<uint8_t> m(4ull * rs_);
  if (mirr_lcn_ * cs_ + m.size() > v_.size()) { c_.error("ntfs-mirror-range", "$MFTMirr lies outside the device", UINT64_MAX, "$MFTMirr"); return Status::Ok(); }
  Status st = v_.read(mirr_lcn_ * cs_, m.data(), m.size());
  if (!st.ok()) return st;
  for (uint32_t i = 0; i < 4; i++) {
    const uint8_t* a = first4_[i].data();
    const uint8_t* b = m.data() + static_cast<size_t>(i) * rs_;
    // 使っている範囲 (bytes_in_use) だけ比べる。FILE ヘッダの USN 差を避けるため、fixup 後の内容で比べる
    std::vector<uint8_t> aa(a, a + rs_), bb(b, b + rs_);
    bool fa = std::memcmp(aa.data(), "FILE", 4) == 0 && fixups(aa.data(), i, false);
    bool fb = std::memcmp(bb.data(), "FILE", 4) == 0 && fixups(bb.data(), i, false);
    if (!fb) { c_.error("ntfs-mirror-bad", "record " + num(i) + " of $MFTMirr is not a valid MFT record", mirr_lcn_ * cs_ + static_cast<uint64_t>(i) * rs_, "$MFTMirr"); continue; }
    if (!fa) continue;
    uint32_t ua = le32(aa.data() + 0x18), ub = le32(bb.data() + 0x18);
    if (ua != ub || ua > rs_ || std::memcmp(aa.data() + 0x30, bb.data() + 0x30, ua > 0x30 ? ua - 0x30 : 0) != 0)
      c_.error("ntfs-mirror-differs", "record " + num(i) + " of $MFTMirr differs from $MFT", mirr_lcn_ * cs_ + static_cast<uint64_t>(i) * rs_, "$MFTMirr");
  }
  return Status::Ok();
}

Status NtfsChk::check_bitmap() {
  if (bitmap_runs_.empty()) { c_.error("ntfs-no-bitmap", "$Bitmap has no data runs", UINT64_MAX, "$Bitmap"); return Status::Ok(); }
  std::sort(bitmap_runs_.begin(), bitmap_runs_.end(), [](const Run& a, const Run& b) { return a.vcn < b.vcn; });
  uint64_t need = (total_clusters_ + 7) / 8;
  if (bitmap_size_ < need) { c_.error("ntfs-bitmap-size", "$Bitmap is smaller (" + num(bitmap_size_) + " bytes) than needed (" + num(need) + ")", UINT64_MAX, "$Bitmap"); return Status::Ok(); }
  uint64_t missing = 0, leaked = 0, first_m = 0, first_l = 0;
  std::vector<uint8_t> buf;
  const uint64_t step = 1u << 20;
  for (uint64_t pos = 0; pos < need; pos += step) {
    size_t n = static_cast<size_t>(std::min<uint64_t>(step, need - pos));
    buf.resize(n);
    bool hole;
    Status st = map_read(bitmap_runs_, pos, buf.data(), n, &hole);
    if (st.code == Errc::OutOfRange) { c_.error("ntfs-bitmap-runs", "the $Bitmap run list does not cover its size", UINT64_MAX, "$Bitmap"); return Status::Ok(); }
    if (!st.ok()) return st;
    for (size_t k = 0; k < n; k++) {
      uint8_t x = buf[k];
      for (int b = 0; b < 8; b++) {
        uint64_t cl = (pos + k) * 8 + static_cast<uint64_t>(b);
        if (cl >= total_clusters_) break;
        bool marked = (x >> b) & 1, used = claimed_.get(cl);
        if (used && !marked) { if (!missing) first_m = cl; missing++; }
        else if (marked && !used) { if (!leaked) first_l = cl; leaked++; }
      }
    }
    if (c_.stop()) return Status::Ok();
  }
  if (missing) c_.error("ntfs-bitmap-missing", num(missing) + " cluster(s) are in use by files but marked free in $Bitmap (first: cluster " + num(first_m) + ")", first_m * cs_, "$Bitmap");
  if (leaked) c_.warn("ntfs-bitmap-leak", num(leaked) + " cluster(s) are marked used in $Bitmap but belong to no file (first: cluster " + num(first_l) + ")", first_l * cs_, "$Bitmap");
  uint64_t used_clusters = claimed_.count();
  c_.rep.stats.units_used = used_clusters;
  return Status::Ok();
}

Status NtfsChk::check_logfile() {
  if (logfile_runs_.empty()) { c_.error("ntfs-no-logfile", "$LogFile has no data runs", UINT64_MAX, "$LogFile"); return Status::Ok(); }
  std::sort(logfile_runs_.begin(), logfile_runs_.end(), [](const Run& a, const Run& b) { return a.vcn < b.vcn; });
  if (logfile_size_ < 0x4000) { c_.warn("ntfs-logfile-size", "$LogFile is smaller than expected (" + num(logfile_size_) + " bytes)", UINT64_MAX, "$LogFile"); return Status::Ok(); }
  // 先頭 2 ページ (リスタートページ)。各ページは system_page_size (通常 4096)
  std::vector<uint8_t> pg(4096);
  bool hole;
  Status st = map_read(logfile_runs_, 0, pg.data(), pg.size(), &hole);
  if (!st.ok()) { if (st.code == Errc::OutOfRange) return Status::Ok(); return st; }
  auto page_state = [&](const std::vector<uint8_t>& p) -> int {   // 0 = 不明, 1 = クリーン, 2 = 未クリーン
    bool all_ff = true;
    for (size_t i = 0; i < p.size(); i++) if (p[i] != 0xFF) { all_ff = false; break; }
    if (all_ff) return 1;                               // 0xFF で埋まっている = ログは空 (リセット済み)
    if (!std::memcmp(p.data(), "CHKD", 4)) return 1;    // chkdsk がリセットした
    if (std::memcmp(p.data(), "RSTR", 4) != 0) return 0;
    uint16_t ra = le16(p.data() + 0x18);
    if (ra < 0x20 || ra + 0x30u > p.size()) return 0;
    uint16_t flags = le16(p.data() + ra + 0xE);
    return (flags & 0x0002) ? 1 : 2;                   // RESTART_VOLUME_IS_CLEAN
  };
  int s0 = page_state(pg);
  std::vector<uint8_t> pg2(4096);
  st = map_read(logfile_runs_, 4096, pg2.data(), pg2.size(), &hole);
  int s1 = st.ok() ? page_state(pg2) : 0;
  if (s0 == 0 && s1 == 0) { c_.warn("ntfs-logfile-unknown", "the $LogFile restart pages are not recognized", UINT64_MAX, "$LogFile"); return Status::Ok(); }
  if ((s0 == 0) != (s1 == 0)) c_.info("ntfs-logfile-one-page-invalid", "one of the two $LogFile restart pages is not valid (the other one is used)", UINT64_MAX, "$LogFile");
  if (s0 == 2 || s1 == 2) {
    // どちらかのリスタートページが「ダーティ」。NTFS は新しい方 (LSN が大きい方) を採る。ここでは保守的に、クリーンと確認できない場合に報告する
    if (!(s0 == 1 && s1 == 1)) {
      bool clean_newer = false;
      // 両ページに RSTR があるとき current_lsn を比べる
      if (s0 != 0 && s1 != 0 && !std::memcmp(pg.data(), "RSTR", 4) && !std::memcmp(pg2.data(), "RSTR", 4)) {
        uint16_t a0 = le16(pg.data() + 0x18), a1 = le16(pg2.data() + 0x18);
        uint64_t l0 = le64(pg.data() + a0), l1 = le64(pg2.data() + a1);
        const std::vector<uint8_t>& newer = l1 > l0 ? pg2 : pg;
        clean_newer = page_state(newer) == 1;
      }
      if (!clean_newer) {
        c_.warn("ntfs-logfile-unclean", "the $LogFile journal is not empty (the volume was not cleanly dismounted); Windows must replay it before writing", UINT64_MAX, "$LogFile");
        c_.rep.unclean = true;
      }
    }
  }
  return Status::Ok();
}

Status NtfsChk::run() {
  Status st = boot();
  if (!st.ok()) return st.code == Errc::Corrupt ? Status::Ok() : st;
  st = load_mft_runs();
  if (!st.ok()) return st.code == Errc::Corrupt ? Status::Ok() : st;
  st = scan_records();
  if (!st.ok() || c_.stop() || c_.rep.incomplete) return st;
  // $MFT ビットマップと使用中フラグ
  if (have_mft_bitmap_) {
    uint64_t missing = 0, leaked = 0, fm = 0, fl = 0;
    for (uint64_t n = 0; n < mft_records_; n++) {
      bool bit = (n / 8) < mft_bitmap_.size() ? ((mft_bitmap_[static_cast<size_t>(n / 8)] >> (n & 7)) & 1) : false;
      bool use = in_use_.get(n);
      if (use && !bit) { if (!missing) fm = n; missing++; }
      else if (bit && !use && n >= 16) { if (!leaked) fl = n; leaked++; }
    }
    if (missing) c_.error("ntfs-mftbitmap-missing", num(missing) + " MFT record(s) are in use but marked free in the MFT bitmap (first: record " + num(fm) + ")", UINT64_MAX, "$MFT bitmap");
    if (leaked) c_.warn("ntfs-mftbitmap-leak", num(leaked) + " MFT record(s) are marked in use in the MFT bitmap but are free (first: record " + num(fl) + ")", UINT64_MAX, "$MFT bitmap");
  } else c_.warn("ntfs-no-mft-bitmap", "the $MFT bitmap could not be read", UINT64_MAX, "$MFT");
  st = check_mirror();
  if (!st.ok() || c_.stop()) return st;
  // 親ディレクトリの整合
  if (have_parent_) {
    uint64_t bad_parent = 0, first = 0;
    for (uint64_t n = 0; n < mft_records_; n++) {
      if (!in_use_.get(n) || parent_[n] == 0xFFFFFFFFu) continue;
      uint32_t p = parent_[n];
      bool ok = p < mft_records_ && in_use_.get(p) && is_dir_.get(p) && seq_[p] == parent_seq_[n];
      if (!ok) { if (!bad_parent) first = n; bad_parent++; }
    }
    if (bad_parent) c_.error("ntfs-bad-parent", num(bad_parent) + " file record(s) refer to a parent directory that is not in use, not a directory, or has a different sequence number (first: record " + num(first) + ")", UINT64_MAX, "mft record " + num(first));
  }
  st = check_bitmap();
  if (!st.ok() || c_.stop()) return st;
  st = check_logfile();
  if (!st.ok()) return st;
  return Status::Ok();
}

}  // namespace

Status check_ntfs(Ctx& c) {
  NtfsChk k(c);
  return k.run();
}

}  // namespace fsck
}  // namespace usbmem
