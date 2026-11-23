// SPDX-License-Identifier: Apache-2.0
// 削除ファイルの一覧と救出: 共通部 (FS の判別・割り当て地図・先頭の検査・確度の計算・書き出し・JSON)。FS ごとの解析は undelete_{fat,exfat,ext,ntfs}.cpp。
#include "usbmem/undelete.h"

#include <algorithm>
#include <cstdio>
#include <numeric>

#include "filetype.h"
#include "undelete_internal.h"
#include "usbmem/fs_probe.h"
#include "usbmem/sha256.h"

namespace usbmem {

const char* recover_level_name(RecoverLevel l) {
  switch (l) {
    case RecoverLevel::High: return "high";
    case RecoverLevel::Medium: return "medium";
    case RecoverLevel::Low: return "low";
    case RecoverLevel::None: return "none";
  }
  return "none";
}

namespace ud {

void append_utf8(std::string* out, uint32_t cp) {
  if (cp < 0x80) out->push_back(static_cast<char>(cp));
  else if (cp < 0x800) { out->push_back(static_cast<char>(0xC0 | (cp >> 6))); out->push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
  else if (cp < 0x10000) { out->push_back(static_cast<char>(0xE0 | (cp >> 12))); out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F))); out->push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
  else { out->push_back(static_cast<char>(0xF0 | (cp >> 18))); out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F))); out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F))); out->push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
}

std::string utf16_to_utf8(const uint16_t* u, size_t n) {
  std::string out;
  for (size_t i = 0; i < n; i++) {
    uint32_t c = u[i];
    if (c >= 0xD800 && c < 0xDC00 && i + 1 < n && u[i + 1] >= 0xDC00 && u[i + 1] < 0xE000) {
      c = 0x10000 + ((c - 0xD800) << 10) + (u[i + 1] - 0xDC00);
      i++;
    } else if (c >= 0xD800 && c < 0xE000) {
      c = 0xFFFD;
    }
    append_utf8(&out, c);
  }
  return out;
}

int64_t dos_to_unix(uint16_t date, uint16_t time) {
  int day = date & 31, mon = (date >> 5) & 15, year = 1980 + (date >> 9);
  int sec = (time & 31) * 2, min = (time >> 5) & 63, hour = time >> 11;
  if (mon < 1 || mon > 12 || day < 1 || day > 31 || hour > 23 || min > 59 || sec > 59) return 0;
  // 1970-01-01 からの日数 (グレゴリオ暦。Howard Hinnant の days_from_civil と同じ考え方)
  int y = year - (mon <= 2 ? 1 : 0);
  int era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = static_cast<unsigned>(y - era * 400);
  unsigned doy = static_cast<unsigned>((153 * (mon + (mon > 2 ? -3 : 9)) + 2) / 5 + day - 1);
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  int64_t days = static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
  return days * 86400 + hour * 3600 + min * 60 + sec;
}

uint64_t count_set_bits(const std::vector<uint8_t>& bits, uint64_t first, uint64_t n) {
  uint64_t total = 0, end = first + n;
  const uint64_t cap = static_cast<uint64_t>(bits.size()) * 8;
  if (end > cap) end = cap;
  uint64_t b = first;
  while (b < end && (b & 7)) { total += (bits[b >> 3] >> (b & 7)) & 1; b++; }
  while (b + 8 <= end) { total += static_cast<uint64_t>(__builtin_popcount(bits[b >> 3])); b += 8; }
  while (b < end) { total += (bits[b >> 3] >> (b & 7)) & 1; b++; }
  return total;
}

void push_extent(std::vector<FileExtent>* v, uint64_t off, uint64_t len, bool hole) {
  if (!len) return;
  if (!v->empty()) {
    FileExtent& l = v->back();
    if (l.hole == hole && (hole || l.offset + l.length == off)) { l.length += len; return; }
  }
  FileExtent e;
  e.offset = hole ? 0 : off;
  e.length = len;
  e.hole = hole;
  v->push_back(e);
}

namespace {

// 先頭 n バイトを (resident / extents から) 読む
size_t read_head(Ctx& c, const DeletedFile& f, uint8_t* buf, size_t n) {
  size_t got = 0;
  if (f.has_resident) {
    got = std::min<size_t>(n, f.resident.size());
    if (got) std::memcpy(buf, f.resident.data(), got);
    return got;
  }
  Bytes tmp;
  for (const FileExtent& e : f.extents) {
    if (got >= n || got >= f.size) break;
    size_t want = static_cast<size_t>(std::min<uint64_t>({n - got, e.length, f.size - got}));
    if (e.hole) { std::memset(buf + got, 0, want); got += want; continue; }
    if (!c.rd(e.offset, want, &tmp).ok()) break;
    std::memcpy(buf + got, tmp.data(), want);
    got += want;
  }
  return got;
}

void finalize_file(Ctx& c, DeletedFile& f) {
  // 割り当ての確認
  f.allocation_known = c.used_ok;
  f.block_bytes = c.used_ok ? c.used.block_size : 0;
  if (c.used_ok) {
    const uint64_t bs = c.used.block_size;
    for (const FileExtent& e : f.extents) {
      if (e.hole || !e.length) continue;
      uint64_t first = e.offset / bs, last = (e.offset + e.length - 1) / bs;
      f.total_blocks += last - first + 1;
      f.reused_blocks += count_set_bits(c.used.bits, first, last - first + 1);
    }
  }
  // 先頭の形式
  bool head_hole = !f.extents.empty() && f.extents[0].hole && !f.has_resident;  // 先頭がスパース (ゼロが正しい)
  if (head_hole) {
    f.signature_check = "unknown";
  } else if (c.opt.verify_head && f.size) {
    uint8_t head[4096];
    size_t n = read_head(c, f, head, sizeof head);
    if (n == 0) {
      f.signature_check = "unknown";
      f.notes.push_back("head-unreadable");
    } else {
      f.signature = sniff_type(head, n);
      SigCheck sc = check_head_for_name(f.name, head, n, f.size);
      f.signature_check = sig_check_name(sc);
    }
  } else {
    f.signature_check = "unknown";
  }
}

void score_file(DeletedFile& f) {
  if (f.level == RecoverLevel::None) { f.confidence = 0; return; }  // 解析側が復元不可と決めたもの
  int s = f.layout_exact ? 100 : 85;
  if (!f.layout_exact) f.notes.push_back("layout-assumed-contiguous");
  if (f.signature_check == "mismatch") { s -= 35; f.notes.push_back("signature-mismatch"); }
  else if (f.signature_check == "unknown" && !f.layout_exact) s -= 5;
  if (!f.allocation_known) { s = std::min(s, 70); f.notes.push_back("allocation-unknown"); }
  if (f.allocation_known && f.reused_blocks > 0) {
    // 1 ブロックでも再利用されていれば、内容は壊れている可能性が高い。割合が大きいほど低く、上限は 39 (= 低)
    uint64_t pct = f.total_blocks ? f.reused_blocks * 100 / f.total_blocks : 100;
    s = std::min<int>(s, 39 - static_cast<int>(std::min<uint64_t>(pct, 39) * 39 / 100));
    f.notes.push_back("clusters-reused");
  }
  if (f.overlaps_other_deleted) { s = std::min(s, 40); f.notes.push_back("overlaps-other-deleted"); }
  if (f.name_partial) f.notes.push_back("name-first-char-lost");
  if (f.in_deleted_dir) f.notes.push_back("parent-deleted");
  s = std::max(0, std::min(100, s));
  f.confidence = s;
  f.level = s >= 80 ? RecoverLevel::High : s >= 50 ? RecoverLevel::Medium : RecoverLevel::Low;
}

// 別の削除済みファイルとデータ領域が重なるものに印を付ける
void mark_overlaps(std::vector<DeletedFile>* files) {
  struct R { uint64_t s, e; size_t idx; };
  std::vector<R> rs;
  for (size_t i = 0; i < files->size(); i++) {
    const DeletedFile& f = (*files)[i];
    if (f.level == RecoverLevel::None) continue;
    for (const FileExtent& e : f.extents)
      if (!e.hole && e.length) rs.push_back({e.offset, e.offset + e.length, i});
  }
  std::sort(rs.begin(), rs.end(), [](const R& a, const R& b) { return a.s < b.s; });
  uint64_t max_end = 0;
  size_t max_idx = 0;
  for (size_t k = 0; k < rs.size(); k++) {
    if (k && rs[k].s < max_end && rs[k].idx != max_idx) {
      (*files)[rs[k].idx].overlaps_other_deleted = true;
      (*files)[max_idx].overlaps_other_deleted = true;
    }
    if (rs[k].e > max_end) { max_end = rs[k].e; max_idx = rs[k].idx; }
  }
}

}  // namespace
}  // namespace ud

Status list_deleted(BlockDevice& dev, const UndeleteOptions& opt, UndeleteResult* out) {
  if (!out) return Status::Make(Errc::InvalidArgument);
  *out = UndeleteResult();
  ud::Ctx c(dev, opt, *out);
  Status io;
  FsType t = probe_filesystem(dev, 0, dev.size(), &io);
  if (!io.ok()) return io;
  switch (t) {
    case FsType::Fat12: case FsType::Fat16: case FsType::Fat32: case FsType::Exfat:
    case FsType::Ext2: case FsType::Ext3: case FsType::Ext4: case FsType::Ntfs:
      out->fs = fs_type_name(t);
      break;
    default:
      return Status::Make(Errc::Unsupported);
  }
  // 割り当て情報。信用できない (未クリーン等) ときは全ブロック使用中の地図になるので、その場合は「不明」として扱う
  c.progress("alloc", 0, 0);
  ExtractOptions eo;
  eo.full_if_unclean = true;
  FsStatus fs = build_used_map(dev, eo, &c.used);
  c.used_ok = fs.ok() && !c.used.full_copy;
  if (!fs.ok()) c.warn("allocation map unavailable: " + fs.code);
  else if (c.used.full_copy) for (const std::string& w : c.used.warnings) c.warn(w);
  out->allocation_known = c.used_ok;
  if (c.cancelled()) { out->cancelled = true; return Status::Make(Errc::Cancelled); }

  Status st;
  switch (t) {
    case FsType::Fat12: case FsType::Fat16: case FsType::Fat32: st = ud::scan_fat(c); break;
    case FsType::Exfat: st = ud::scan_exfat(c); break;
    case FsType::Ntfs: st = ud::scan_ntfs(c); break;
    default: st = ud::scan_ext(c); break;
  }
  if (st.code == Errc::Cancelled) out->cancelled = true;
  else if (!st.ok() && (st.code == Errc::Disconnected || st.code == Errc::Closed)) return st;

  // 後処理: 割り当て・先頭の検査 → 重なり → 確度
  const uint64_t n = out->files.size();
  for (uint64_t i = 0; i < n; i++) {
    if (i % 64 == 0) c.progress("verify", i, n);
    ud::finalize_file(c, out->files[i]);
  }
  ud::mark_overlaps(&out->files);
  for (DeletedFile& f : out->files) ud::score_file(f);
  c.progress("verify", n, n);
  if (out->cancelled) return Status::Make(Errc::Cancelled);
  return st.ok() || st.code == Errc::Corrupt ? Status::Ok() : st;
}

Status extract_deleted(BlockDevice& dev, const DeletedFile& f, const ByteSinkFn& sink, const CancelToken* cancel, ExtractResult* res) {
  if (f.level == RecoverLevel::None) return Status::Make(Errc::Unsupported);
  Sha256 h;
  uint64_t done = 0;
  std::vector<uint8_t> buf;
  const size_t kChunk = 1u << 20;
  auto emit = [&](const void* p, size_t n) -> Status {
    Status s = sink(p, n);
    if (!s.ok()) return s;
    h.update(p, n);
    done += n;
    return Status::Ok();
  };
  Status st;
  if (f.has_resident) {
    size_t n = static_cast<size_t>(std::min<uint64_t>(f.size, f.resident.size()));
    st = n ? emit(f.resident.data(), n) : Status::Ok();
  } else {
    for (const FileExtent& e : f.extents) {
      uint64_t left = std::min<uint64_t>(e.length, f.size - done);
      uint64_t off = e.offset;
      while (left && st.ok()) {
        if (cancel && cancel->cancelled()) { st = Status::Make(Errc::Cancelled); break; }
        size_t n = static_cast<size_t>(std::min<uint64_t>(left, kChunk));
        buf.resize(n);
        if (e.hole) std::memset(buf.data(), 0, n);
        else {
          IoOptions io;
          io.cancel = cancel;
          st = dev.read_at(off, buf.data(), n, io);
          if (!st.ok()) break;
        }
        st = emit(buf.data(), n);
        left -= n;
        off += n;
      }
      if (!st.ok() || done >= f.size) break;
    }
  }
  // 記録された配置が size に満たなければ (スパース・NTFS の長さの不一致) ゼロで埋める
  if (st.ok() && done < f.size) {
    buf.assign(kChunk, 0);
    while (done < f.size && st.ok()) {
      size_t n = static_cast<size_t>(std::min<uint64_t>(f.size - done, kChunk));
      st = emit(buf.data(), n);
    }
  }
  if (res) {
    res->bytes = done;
    res->sha256 = hex_encode(h.finish());
  }
  return st;
}

namespace {
void jstr(std::string& o, const std::string& s) {
  o += '"';
  for (unsigned char ch : s) {
    if (ch == '"' || ch == '\\') { o += '\\'; o += static_cast<char>(ch); }
    else if (ch < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", ch); o += b; }
    else o += static_cast<char>(ch);
  }
  o += '"';
}
}  // namespace

std::string undelete_result_to_json(const UndeleteResult& r) {
  std::string o = "{\"fs\":";
  jstr(o, r.fs);
  o += ",\"allocationKnown\":" + std::string(r.allocation_known ? "true" : "false") + ",\"truncated\":" + (r.truncated ? "true" : "false") +
       ",\"cancelled\":" + (r.cancelled ? "true" : "false") + ",\"entriesSeen\":" + std::to_string(r.entries_seen) +
       ",\"skippedInvalid\":" + std::to_string(r.skipped_invalid) + ",\"warnings\":[";
  for (size_t i = 0; i < r.warnings.size(); i++) { if (i) o += ','; jstr(o, r.warnings[i]); }
  o += "],\"files\":[";
  for (size_t i = 0; i < r.files.size(); i++) {
    const DeletedFile& f = r.files[i];
    if (i) o += ',';
    o += "{\"id\":" + std::to_string(f.id) + ",\"name\":";
    jstr(o, f.name);
    o += ",\"path\":";
    jstr(o, f.path);
    o += ",\"size\":" + std::to_string(f.size) + ",\"mtime\":" + std::to_string(f.mtime) + ",\"namePartial\":" + (f.name_partial ? "true" : "false") +
         ",\"inDeletedDir\":" + (f.in_deleted_dir ? "true" : "false") + ",\"node\":" + std::to_string(f.node) +
         ",\"blockBytes\":" + std::to_string(f.block_bytes) + ",\"totalBlocks\":" + std::to_string(f.total_blocks) +
         ",\"reusedBlocks\":" + std::to_string(f.reused_blocks) + ",\"allocationKnown\":" + (f.allocation_known ? "true" : "false") +
         ",\"layoutExact\":" + (f.layout_exact ? "true" : "false") + ",\"overlapsOtherDeleted\":" + (f.overlaps_other_deleted ? "true" : "false") +
         ",\"resident\":" + (f.has_resident ? "true" : "false") + ",\"signature\":";
    jstr(o, f.signature);
    o += ",\"signatureCheck\":";
    jstr(o, f.signature_check);
    o += ",\"confidence\":" + std::to_string(f.confidence) + ",\"level\":";
    jstr(o, recover_level_name(f.level));
    o += ",\"recoverable\":" + std::string(f.level != RecoverLevel::None ? "true" : "false") + ",\"notes\":[";
    for (size_t k = 0; k < f.notes.size(); k++) { if (k) o += ','; jstr(o, f.notes[k]); }
    o += "]}";
  }
  o += "]}";
  return o;
}

}  // namespace usbmem
