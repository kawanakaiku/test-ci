// SPDX-License-Identifier: Apache-2.0
// 読み取り専用の整合性検査: 共通部分 (キャッシュ付きリーダ・報告・振り分け・JSON)。設計は docs/disk-tools.md。
#include <cstdio>

#include "fscheck_internal.h"

namespace usbmem {

const char* fs_verdict_name(FsVerdict v) {
  switch (v) {
    case FsVerdict::Clean: return "clean";
    case FsVerdict::Warnings: return "warnings";
    case FsVerdict::Errors: return "errors";
    case FsVerdict::Unsupported: return "unsupported";
    case FsVerdict::Incomplete: return "incomplete";
  }
  return "unknown";
}

bool fscheck_supported(FsType fs) {
  switch (fs) {
    case FsType::Fat12: case FsType::Fat16: case FsType::Fat32: case FsType::Exfat:
    case FsType::Ext2: case FsType::Ext3: case FsType::Ext4: case FsType::Ntfs:
      return true;
    default:
      return false;
  }
}

namespace fsck {

std::string num(uint64_t v) { return std::to_string(v); }
std::string hex(uint64_t v) { char b[24]; std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v)); return b; }

Status Vol::chunk(uint64_t idx, const uint8_t** out) {
  if (slots_.empty()) slots_.resize(kSlots);
  Slot& s = slots_[idx % kSlots];
  if (s.idx != idx) {
    uint64_t start = idx * kChunk;
    if (start >= len_) return Status::Make(Errc::OutOfRange);
    size_t n = static_cast<size_t>(std::min<uint64_t>(kChunk, len_ - start));
    s.data.resize(n);
    s.idx = UINT64_MAX;
    Status st = dev_.read_at(off_ + start, s.data.data(), n);
    if (!st.ok()) {
      if (io_.ok()) io_ = st;
      return st;
    }
    s.idx = idx;
  }
  *out = s.data.data();
  return Status::Ok();
}

Status Vol::read(uint64_t pos, void* buf, size_t n) {
  if (!range_ok(len_, pos, n)) return Status::Make(Errc::OutOfRange);
  uint8_t* d = static_cast<uint8_t*>(buf);
  while (n) {
    const uint8_t* c = nullptr;
    Status st = chunk(pos / kChunk, &c);
    if (!st.ok()) return st;
    size_t in = static_cast<size_t>(pos % kChunk);
    size_t take = std::min<size_t>(n, static_cast<size_t>(kChunk) - in);
    std::memcpy(d, c + in, take);
    d += take; pos += take; n -= take;
  }
  return Status::Ok();
}

void Ctx::add(Severity s, const char* code, std::string msg, uint64_t offset, std::string object) {
  FsCheckReport& r = rep;
  if (s == Severity::Error) r.error_count++;
  else if (s == Severity::Warning) r.warning_count++;
  else r.info_count++;
  if (r.findings.size() >= opt.max_findings) { r.findings_truncated = true; return; }
  FsFinding f;
  f.severity = s; f.code = code; f.message = std::move(msg); f.offset = offset; f.object = std::move(object);
  r.findings.push_back(std::move(f));
}

bool Ctx::stop() {
  if (cancelled) return true;
  if (opt.cancel && opt.cancel->cancelled()) {
    cancelled = true;
    rep.incomplete = true;
    return true;
  }
  return false;
}

bool Ctx::count_object() {
  if (++objects > opt.max_objects) {
    if (!rep.incomplete) incomplete("object-limit", "too many objects; the check stopped early");
    cancelled = true;
    return false;
  }
  return !stop();
}

void Ctx::incomplete(const char* code, const char* why) {
  rep.incomplete = true;
  info(code, why);
}

}  // namespace fsck

Status check_filesystem(BlockDevice& dev, uint64_t offset, uint64_t length, FsType fs, const FsCheckOptions& opt, FsCheckReport* out) {
  if (!out) return Status::Make(Errc::InvalidArgument);
  *out = FsCheckReport{};
  if (!range_ok(dev.size(), offset, length)) return Status::Make(Errc::OutOfRange);
  if (fs == FsType::Unknown) {
    Status io;
    fs = probe_filesystem(dev, offset, length, &io);
    if (!io.ok()) return io;
  }
  out->fs = fs;
  if (!fscheck_supported(fs)) {
    out->verdict = FsVerdict::Unsupported;
    return Status::Ok();
  }
  fsck::Vol vol(dev, offset, length);
  fsck::Ctx ctx(vol, opt, *out);
  Status st;
  switch (fs) {
    case FsType::Fat12: case FsType::Fat16: case FsType::Fat32: st = fsck::check_fat(ctx, fs); break;
    case FsType::Exfat: st = fsck::check_exfat(ctx); break;
    case FsType::Ext2: case FsType::Ext3: case FsType::Ext4: st = fsck::check_ext(ctx, fs); break;
    case FsType::Ntfs: st = fsck::check_ntfs(ctx); break;
    default: break;
  }
  if (!st.ok() && st.code != Errc::Cancelled) {
    out->incomplete = true;
  }
  if (out->error_count) out->verdict = FsVerdict::Errors;
  else if (out->incomplete) out->verdict = FsVerdict::Incomplete;
  else if (out->warning_count) out->verdict = FsVerdict::Warnings;
  else out->verdict = FsVerdict::Clean;
  if (ctx.cancelled && st.ok() && opt.cancel && opt.cancel->cancelled()) return Status::Make(Errc::Cancelled);
  return st;
}

// ------------------------------------------------------------------ JSON

namespace {
void js(std::string& o, const std::string& s) {
  o += '"';
  for (size_t i = 0; i < s.size();) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c == '"') { o += "\\\""; i++; }
    else if (c == '\\') { o += "\\\\"; i++; }
    else if (c == '\n') { o += "\\n"; i++; }
    else if (c == '\r') { o += "\\r"; i++; }
    else if (c == '\t') { o += "\\t"; i++; }
    else if (c < 0x20 || c == 0x7F) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; i++; }
    else if (c < 0x80) { o += static_cast<char>(c); i++; }
    else {
      // UTF-8 として正しい列だけそのまま出す。不正なバイトは U+FFFD
      size_t extra = (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : 99;
      bool ok = extra != 99;
      for (size_t k = 1; ok && k <= extra; k++)
        if (i + k >= s.size() || (static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) ok = false;
      if (ok) { o.append(s, i, extra + 1); i += extra + 1; }
      else { o += "\xEF\xBF\xBD"; i++; }
    }
  }
  o += '"';
}
const char* sevname(Severity s) { return s == Severity::Error ? "error" : s == Severity::Warning ? "warning" : "info"; }
std::string u64s(uint64_t v) { return "\"" + std::to_string(v) + "\""; }
}  // namespace

std::string fscheck_report_to_json(const FsCheckReport& r) {
  std::string o = "{\"fs\":";
  js(o, fs_type_name(r.fs));
  o += ",\"verdict\":"; js(o, fs_verdict_name(r.verdict));
  o += std::string(",\"incomplete\":") + (r.incomplete ? "true" : "false");
  o += std::string(",\"unclean\":") + (r.unclean ? "true" : "false");
  o += std::string(",\"needsJournalRecovery\":") + (r.needs_journal_recovery ? "true" : "false");
  o += std::string(",\"truncated\":") + (r.findings_truncated ? "true" : "false");
  o += ",\"errors\":" + u64s(r.error_count) + ",\"warnings\":" + u64s(r.warning_count) + ",\"infos\":" + u64s(r.info_count);
  o += ",\"stats\":{\"files\":" + u64s(r.stats.files) + ",\"directories\":" + u64s(r.stats.directories) +
       ",\"symlinks\":" + u64s(r.stats.symlinks) + ",\"other\":" + u64s(r.stats.other) +
       ",\"unitsTotal\":" + u64s(r.stats.units_total) + ",\"unitsUsed\":" + u64s(r.stats.units_used) +
       ",\"bytesPerUnit\":" + u64s(r.stats.bytes_per_unit) + "}";
  o += ",\"findings\":[";
  for (size_t i = 0; i < r.findings.size(); i++) {
    const FsFinding& f = r.findings[i];
    if (i) o += ',';
    o += "{\"severity\":"; js(o, sevname(f.severity));
    o += ",\"code\":"; js(o, f.code);
    o += ",\"message\":"; js(o, f.message);
    if (f.offset != UINT64_MAX) o += ",\"offset\":" + u64s(f.offset);
    if (!f.object.empty()) { o += ",\"object\":"; js(o, f.object); }
    o += '}';
  }
  o += "]}";
  return o;
}

}  // namespace usbmem
