// SPDX-License-Identifier: Apache-2.0
// mkfs の共通部分: 計画 (書く領域の組み立てと検証) → 確認トークン → 適用 (書き込み・読み戻し・検査)。設計は docs/disk-tools.md。
#include "usbmem/mkfs.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <set>

#include "mkfs_internal.h"
#include "usbmem/fscheck.h"
#include "usbmem/sha256.h"

namespace usbmem {

namespace {

using mkfsimpl::Build;
using mkfsimpl::Params;

constexpr uint64_t kBackupRegionMax = 4ull << 20;    // 1 つの領域がこれ以下ならバックアップ・事前内容の確認に含める
constexpr uint64_t kBackupTotalMax = 64ull << 20;
constexpr uint64_t kChunk = 1ull << 20;
constexpr uint64_t kPrelude = 8192;                  // 旧ファイルシステムの識別子を先に消す範囲

std::mutex g_mu;
std::set<const void*> g_busy;

struct Flat {
  const MkfsRegion* r;
  bool prelude;
  bool backup;   // バックアップ / 事前内容の対象
};

Diagnostic D(Severity s, const char* code, std::string msg) { return Diagnostic{s, code, std::move(msg)}; }

// ステージの全領域を、順番を保ったまま一列にし、バックアップ対象を決める (計画と適用で同じ規則)。
std::vector<Flat> flatten(const MkfsPlan& p, uint64_t* skipped_bytes, size_t* skipped_count) {
  std::vector<Flat> v;
  uint64_t total = 0, sk = 0;
  size_t skn = 0;
  for (auto& st : p.stages)
    for (auto& r : st.regions) {
      bool bk = r.length <= kBackupRegionMax && total + r.length <= kBackupTotalMax;
      if (bk) total += r.length;
      else { sk += r.length; skn++; }
      v.push_back(Flat{&r, st.name == "invalidate-old", bk});
    }
  if (skipped_bytes) *skipped_bytes = sk;
  if (skipped_count) *skipped_count = skn;
  return v;
}

std::string upper6(const std::string& hex) {
  std::string s = hex.substr(0, 6);
  for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

std::string compute_hash(const MkfsPlan& p) {
  Sha256 h;
  std::string head = "usbmem-mkfs-plan-v1";
  h.update(head.data(), head.size() + 1);
  h.update(p.device_id.data(), p.device_id.size() + 1);
  uint8_t b[8];
  mkfsimpl::p32(b, static_cast<uint32_t>(p.fs));
  h.update(b, 4);
  mkfsimpl::p64(b, p.offset);
  h.update(b, 8);
  mkfsimpl::p64(b, p.length);
  h.update(b, 8);
  mkfsimpl::p32(b, p.requires_force ? 1 : 0);
  h.update(b, 4);
  h.update(p.preimage_digest.data(), p.preimage_digest.size() + 1);
  for (auto& st : p.stages) {
    h.update(st.name.data(), st.name.size() + 1);
    mkfsimpl::p32(b, static_cast<uint32_t>(st.regions.size()));
    h.update(b, 4);
    for (auto& r : st.regions) {
      mkfsimpl::p64(b, r.offset);
      h.update(b, 8);
      mkfsimpl::p64(b, r.length);
      h.update(b, 8);
      uint8_t kind = r.data.empty() ? 0 : 1;
      h.update(&kind, 1);
      if (kind) h.update(r.data.data(), r.data.size());
    }
  }
  return hex_encode(h.finish());
}

std::string phrase_for(const MkfsPlan& p, const std::string& hash) { return std::string(p.requires_force ? "ERASE " : "FORMAT ") + upper6(hash); }

std::string device_id_of(BlockDevice& dev, uint64_t off, uint64_t len) {
  return dev.describe() + "|bytes=" + std::to_string(dev.size()) + "|offset=" + std::to_string(off) + "|length=" + std::to_string(len);
}

MkfsResult fail(Errc e, const char* code, std::string msg) {
  MkfsResult r;
  r.status = Status::Make(e);
  r.code = code;
  r.message = std::move(msg);
  return r;
}
MkfsResult io_fail(const Status& st, const char* code, std::string msg) {
  MkfsResult r;
  r.status = st;
  r.code = code;
  r.message = std::move(msg);
  return r;
}

// 事前内容のダイジェスト (バックアップ対象の領域の、いまのディスク上の内容)
Status preimage_digest(BlockDevice& dev, const MkfsPlan& p, const std::vector<Flat>& flat, std::string* out) {
  Sha256 h;
  std::vector<uint8_t> buf;
  for (auto& f : flat) {
    if (!f.backup) continue;
    buf.resize(static_cast<size_t>(f.r->length));
    if (!buf.empty()) {
      Status st = dev.read_at(p.offset + f.r->offset, buf.data(), buf.size());
      if (!st.ok()) return st;
    }
    uint8_t b[16];
    mkfsimpl::p64(b, f.r->offset);
    mkfsimpl::p64(b + 8, f.r->length);
    h.update(b, 16);
    h.update(buf.data(), buf.size());
  }
  *out = hex_encode(h.finish());
  return Status::Ok();
}

std::string jesc(const std::string& s) {
  std::string o;
  o.reserve(s.size() + 2);
  for (unsigned char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20) { char t[8]; std::snprintf(t, sizeof t, "\\u%04x", c); o += t; }
        else o += static_cast<char>(c);
    }
  }
  return o;
}

const char* sev_name(Severity s) { return s == Severity::Error ? "error" : s == Severity::Warning ? "warning" : "info"; }

std::string diags_json(const std::vector<Diagnostic>& v) {
  std::string o = "[";
  for (size_t i = 0; i < v.size(); i++) {
    if (i) o += ",";
    o += "{\"severity\":\"" + std::string(sev_name(v[i].severity)) + "\",\"code\":\"" + jesc(v[i].code) + "\",\"message\":\"" + jesc(v[i].message) + "\"}";
  }
  return o + "]";
}

bool looks_like_partition_table(BlockDevice& dev, uint64_t off, uint32_t ss, Status* io) {
  if (off != 0) return false;
  std::vector<uint8_t> s0(512), s1(512);
  *io = dev.read_at(0, s0.data(), 512);
  if (!io->ok()) return false;
  if (s0[510] == 0x55 && s0[511] == 0xAA) {
    // パーティション表のエントリに使用中 (種別 != 0) があれば表とみなす。BPB を持つ FAT のブートセクタは probe 側で先に判定する
    for (int i = 0; i < 4; i++) if (s0[446 + 16 * i + 4] != 0 && s0[446 + 16 * i + 12 + 0] + s0[446 + 16 * i + 13] + s0[446 + 16 * i + 14] + s0[446 + 16 * i + 15] != 0) return true;
  }
  if (dev.size() >= 2ull * ss + 512) {
    *io = dev.read_at(ss, s1.data(), 512);
    if (!io->ok()) return false;
    if (std::memcmp(s1.data(), "EFI PART", 8) == 0) return true;
  }
  return false;
}

}  // namespace

bool Mkfs::supported(FsType fs) { return fs == FsType::Ext2 || fs == FsType::Exfat || fs == FsType::Fat32; }

uint64_t Mkfs::min_size(FsType fs, uint32_t ss) {
  if (ss < 512) ss = 512;
  switch (fs) {
    case FsType::Ext2: return 256ull << 10;
    case FsType::Exfat: return 1ull << 20;
    case FsType::Fat32: return (65525ull + 32 + 2 * mkfsimpl::ceil_div(65527ull * 4, ss)) * ss;
    default: return 0;
  }
}

MkfsResult Mkfs::plan(BlockDevice& dev, const MkfsOptions& opt, MkfsPlan* out) {
  if (!out) return fail(Errc::InvalidArgument, "bad-argument", "null plan");
  *out = MkfsPlan{};
  MkfsResult ok;
  ok.status = Status::Ok();
  ok.code = "ok";
  out->fs = opt.fs;
  if (!supported(opt.fs)) {
    out->problems.push_back(D(Severity::Error, "unsupported-fs", "only ext2, exFAT and FAT32 can be created"));
    return ok;
  }
  const uint64_t devsize = dev.size();
  if (opt.offset > devsize) { out->problems.push_back(D(Severity::Error, "bad-range", "the offset is beyond the end of the device")); return ok; }
  uint64_t length = opt.length ? opt.length : devsize - opt.offset;
  if (!range_ok(devsize, opt.offset, length)) { out->problems.push_back(D(Severity::Error, "bad-range", "the range does not fit in the device")); return ok; }
  out->offset = opt.offset;
  out->length = length;
  uint32_t ss = opt.sector_size ? opt.sector_size : dev.logical_block_size();
  if (ss < 512 || ss > 4096 || !mkfsimpl::pow2(ss)) { out->problems.push_back(D(Severity::Error, "bad-sector-size", "sector size must be 512, 1024, 2048 or 4096")); return ok; }
  if (opt.offset % ss != 0) { out->problems.push_back(D(Severity::Error, "misaligned-offset", "the start of the range is not a multiple of the sector size")); return ok; }
  if (length < min_size(opt.fs, ss)) {
    out->problems.push_back(D(Severity::Error, "too-small", "the range is smaller than the minimum size for this file system (" + std::to_string(min_size(opt.fs, ss)) + " bytes)"));
    return ok;
  }
  if (opt.in_use) out->problems.push_back(D(Severity::Error, "in-use", "the device is in use (mounted); unmount it before creating a file system"));
  if (dev.is_read_only()) out->problems.push_back(D(Severity::Warning, "device-read-only", "the device is opened read-only; this plan cannot be applied until it is opened writable"));

  // 既存の内容
  Status io;
  FsType existing = probe_filesystem(dev, opt.offset, length, &io);
  if (!io.ok()) return io_fail(io, "io-error", "failed to read the start of the range");
  if (existing != FsType::Unknown) {
    out->existing_fs = fs_type_name(existing);
    out->requires_force = true;
    out->problems.push_back(D(Severity::Warning, "overwrites-filesystem", std::string("the range contains a ") + out->existing_fs + " file system; creating a new one makes its files unreachable"));
  } else {
    std::vector<uint8_t> head(static_cast<size_t>(std::min<uint64_t>(length, 65536)));
    io = dev.read_at(opt.offset, head.data(), head.size());
    if (!io.ok()) return io_fail(io, "io-error", "failed to read the start of the range");
    bool nz = false;
    for (uint8_t c : head) if (c) { nz = true; break; }
    if (nz) {
      out->requires_force = true;
      out->problems.push_back(D(Severity::Warning, "nonzero-start", "the start of the range contains data that is not a recognised file system; it will be overwritten"));
    }
  }
  bool pt = looks_like_partition_table(dev, opt.offset, ss, &io);
  if (!io.ok()) return io_fail(io, "io-error", "failed to read the start of the device");
  if (pt) {
    out->requires_force = true;
    out->problems.push_back(D(Severity::Warning, "overwrites-partition-table", "the range starts at the beginning of a device that has a partition table; creating a file system here destroys the partition table"));
  }

  // 組み立て
  Params prm;
  prm.opt = opt;
  prm.length = length;
  prm.sector_size = ss;
  prm.now = opt.timestamp ? opt.timestamp : static_cast<uint64_t>(std::time(nullptr));
  Build b;
  switch (opt.fs) {
    case FsType::Fat32: mkfsimpl::build_fat32(prm, &b); break;
    case FsType::Exfat: mkfsimpl::build_exfat(prm, &b); break;
    default: mkfsimpl::build_ext2(prm, &b); break;
  }
  for (auto& d : b.problems) out->problems.push_back(d);
  out->changes = b.changes;
  out->layout = b.layout;
  bool has_err = false;
  for (auto& d : out->problems) if (d.severity == Severity::Error) has_err = true;
  if (has_err) return ok;

  // 旧ファイルシステムの識別子を先に消す
  MkfsStage pre;
  pre.name = "invalidate-old";
  MkfsRegion z;
  z.offset = 0;
  z.length = std::min<uint64_t>(length, kPrelude);
  z.what = "clear-old-signatures";
  pre.regions.push_back(z);
  out->stages.push_back(std::move(pre));
  for (auto& s : b.stages) out->stages.push_back(std::move(s));

  // 検証: 範囲内・重なり無し (invalidate-old は後続に上書きされる前提)
  {
    std::vector<std::pair<uint64_t, uint64_t>> iv;
    bool bad = false;
    for (auto& st : out->stages) {
      for (auto& r : st.regions) {
        if (r.length == 0 || r.offset > length || r.length > length - r.offset || (!r.data.empty() && r.data.size() != r.length)) bad = true;
        if (st.name != "invalidate-old") iv.push_back({r.offset, r.length});
      }
    }
    std::sort(iv.begin(), iv.end());
    for (size_t i = 1; i < iv.size(); i++) if (iv[i].first < iv[i - 1].first + iv[i - 1].second) bad = true;
    if (bad) {
      out->problems.push_back(D(Severity::Error, "internal-layout", "internal error: the planned regions are out of range or overlap"));
      return ok;
    }
  }
  uint64_t sk_bytes = 0;
  size_t sk_n = 0;
  auto flat = flatten(*out, &sk_bytes, &sk_n);
  for (auto& f : flat) out->bytes_to_write += f.r->length;
  if (sk_n) out->problems.push_back(D(Severity::Warning, "partly-unbacked", std::to_string(sk_n) + " large region(s), " + std::to_string(sk_bytes >> 10) + " KiB in total (such as inode tables or the FAT), are overwritten with zeros and are not included in the backup"));
  out->device_id = device_id_of(dev, opt.offset, length);
  Status ds = preimage_digest(dev, *out, flat, &out->preimage_digest);
  if (!ds.ok()) return io_fail(ds, "io-error", "failed to read the regions that will be overwritten");
  out->plan_hash = compute_hash(*out);
  out->confirm_phrase = phrase_for(*out, out->plan_hash);
  out->ok = true;
  out->destructive = true;
  return ok;
}

std::string Mkfs::make_confirm_token(const MkfsPlan& plan, const std::string& typed) {
  Sha256 h;
  std::string head = "usbmem-confirm-v1";
  h.update(head.data(), head.size() + 1);
  h.update(plan.plan_hash.data(), plan.plan_hash.size() + 1);
  h.update(plan.device_id.data(), plan.device_id.size() + 1);
  h.update(typed.data(), typed.size());
  return hex_encode(h.finish());
}

namespace {

struct Saved {
  uint64_t offset;
  std::vector<uint8_t> data;
};

std::vector<uint8_t> serialize_backup(const MkfsPlan& p, uint64_t ts, const std::vector<Saved>& saved) {
  std::vector<uint8_t> o;
  auto put = [&](const void* d, size_t n) { const uint8_t* b = static_cast<const uint8_t*>(d); o.insert(o.end(), b, b + n); };
  uint8_t t[8];
  put("UMMKFSBK", 8);
  mkfsimpl::p32(t, 1); put(t, 4);
  mkfsimpl::p64(t, ts); put(t, 8);
  mkfsimpl::p64(t, p.offset); put(t, 8);
  mkfsimpl::p64(t, p.length); put(t, 8);
  mkfsimpl::p32(t, static_cast<uint32_t>(saved.size())); put(t, 4);
  for (auto& s : saved) {
    mkfsimpl::p64(t, s.offset); put(t, 8);
    mkfsimpl::p64(t, s.data.size()); put(t, 8);
    put(s.data.data(), s.data.size());
  }
  auto d = Sha256::hash(o.data(), o.size());
  put(d.data(), d.size());
  return o;
}

}  // namespace

Status parse_mkfs_backup(const std::vector<uint8_t>& f, MkfsBackupInfo* info) {
  if (f.size() < 8 + 4 + 8 * 3 + 4 + 32 || std::memcmp(f.data(), "UMMKFSBK", 8) != 0) return Status::Make(Errc::Corrupt);
  auto d = Sha256::hash(f.data(), f.size() - 32);
  if (std::memcmp(d.data(), f.data() + f.size() - 32, 32) != 0) return Status::Make(Errc::Corrupt);
  MkfsBackupInfo r;
  size_t p = 8;
  r.version = mkfsimpl::g32(&f[p]); p += 4;
  if (r.version != 1) return Status::Make(Errc::Unsupported);
  auto g64 = [&](size_t q) { return static_cast<uint64_t>(mkfsimpl::g32(&f[q])) | (static_cast<uint64_t>(mkfsimpl::g32(&f[q + 4])) << 32); };
  r.timestamp = g64(p); p += 8;
  r.range_offset = g64(p); p += 8;
  r.range_length = g64(p); p += 8;
  uint32_t n = mkfsimpl::g32(&f[p]); p += 4;
  size_t end = f.size() - 32;
  for (uint32_t i = 0; i < n; i++) {
    if (p + 16 > end) return Status::Make(Errc::Corrupt);
    uint64_t off = g64(p), len = g64(p + 8);
    p += 16;
    if (len > end - p || off > r.range_length || len > r.range_length - off) return Status::Make(Errc::Corrupt);
    p += static_cast<size_t>(len);
    r.bytes += len;
  }
  if (p != end) return Status::Make(Errc::Corrupt);
  r.regions = n;
  if (info) *info = r;
  return Status::Ok();
}

Status mkfs_restore_backup(BlockDevice& dev, const std::vector<uint8_t>& f) {
  MkfsBackupInfo info;
  Status st = parse_mkfs_backup(f, &info);
  if (!st.ok()) return st;
  if (dev.is_read_only()) return Status::Make(Errc::ReadOnly);
  if (!range_ok(dev.size(), info.range_offset, info.range_length)) return Status::Make(Errc::OutOfRange);
  size_t p = 8 + 4 + 24 + 4;
  auto g64 = [&](size_t q) { return static_cast<uint64_t>(mkfsimpl::g32(&f[q])) | (static_cast<uint64_t>(mkfsimpl::g32(&f[q + 4])) << 32); };
  for (size_t i = 0; i < info.regions; i++) {
    uint64_t off = g64(p), len = g64(p + 8);
    p += 16;
    st = dev.write_at(info.range_offset + off, &f[p], static_cast<size_t>(len));
    if (!st.ok()) return st;
    p += static_cast<size_t>(len);
  }
  return dev.flush();
}

MkfsResult Mkfs::apply(BlockDevice& dev, const MkfsPlan& plan, const std::string& token, const MkfsApplyOptions& ao) {
  if (dev.is_read_only()) return fail(Errc::ReadOnly, "device-read-only", "the device is opened read-only");
  if (ao.in_use) return fail(Errc::InvalidArgument, "in-use", "the device is in use (mounted); unmount it before creating a file system");
  if (!plan.ok) return fail(Errc::InvalidArgument, "plan-invalid", "the plan has errors");
  if (!supported(plan.fs) || plan.stages.empty() || !range_ok(dev.size(), plan.offset, plan.length)) return fail(Errc::InvalidArgument, "plan-invalid", "malformed plan");
  // 内容から計算し直す (構造体が改ざんされていても検出する)
  for (auto& st : plan.stages)
    for (auto& r : st.regions)
      if (r.length == 0 || r.offset > plan.length || r.length > plan.length - r.offset || (!r.data.empty() && r.data.size() != r.length))
        return fail(Errc::InvalidArgument, "plan-invalid", "malformed region in the plan");
  std::string h = compute_hash(plan);
  if (h != plan.plan_hash) return fail(Errc::InvalidArgument, "plan-tampered", "the plan was modified after it was created");
  MkfsPlan canon = plan;
  std::string phrase = phrase_for(plan, h);
  canon.confirm_phrase = phrase;
  if (token != make_confirm_token(canon, phrase)) return fail(Errc::InvalidArgument, "bad-token", "the confirmation token does not match this plan");
  if (plan.device_id != device_id_of(dev, plan.offset, plan.length)) return fail(Errc::InvalidArgument, "wrong-device", "the plan was made for a different device or range");

  // 排他
  {
    std::lock_guard<std::mutex> l(g_mu);
    if (!g_busy.insert(&dev).second) return fail(Errc::InvalidArgument, "busy", "another file system is being created on this device");
  }
  struct Release {
    BlockDevice* d;
    ~Release() { std::lock_guard<std::mutex> l(g_mu); g_busy.erase(d); }
  } release{&dev};

  auto flat = flatten(plan, nullptr, nullptr);
  std::string dig;
  Status ds = preimage_digest(dev, plan, flat, &dig);
  if (!ds.ok()) return io_fail(ds, "io-error", "failed to re-read the regions that will be overwritten");
  if (dig != plan.preimage_digest) return fail(Errc::InvalidArgument, "device-changed", "the device changed after the plan was made; create a new plan");

  // バックアップ
  std::vector<Saved> saved;
  for (auto& f : flat) {
    if (!f.backup) continue;
    Saved s;
    s.offset = f.r->offset;
    s.data.resize(static_cast<size_t>(f.r->length));
    Status rs = dev.read_at(plan.offset + f.r->offset, s.data.data(), s.data.size());
    if (!rs.ok()) return io_fail(rs, "io-error", "failed to read a region for the backup");
    saved.push_back(std::move(s));
  }
  if (ao.require_backup && !ao.backup_sink) return fail(Errc::InvalidArgument, "backup-required", "a backup sink is required before writing");
  if (ao.backup_sink) {
    Status bs = ao.backup_sink(serialize_backup(plan, ao.timestamp, saved));
    if (!bs.ok()) return io_fail(bs, "backup-failed", "the backup could not be saved; nothing was written");
  }

  // 書き込み
  MkfsResult res;
  res.status = Status::Ok();
  res.code = "ok";
  uint64_t total = plan.bytes_to_write, done = 0;
  static const std::vector<uint8_t> zeros(kChunk, 0);
  Status fs = Status::Ok();
  const char* fcode = "write-failed";
  bool cancelled = false;
  for (auto& st : plan.stages) {
    for (auto& r : st.regions) {
      for (uint64_t pos = 0; pos < r.length;) {
        if (ao.cancel && ao.cancel->cancelled()) { cancelled = true; break; }
        size_t n = static_cast<size_t>(std::min<uint64_t>(kChunk, r.length - pos));
        Status ws = dev.write_at(plan.offset + r.offset + pos, r.data.empty() ? zeros.data() : r.data.data() + pos, n);
        if (!ws.ok()) { fs = ws; break; }
        pos += n;
        done += n;
        if (ao.progress) ao.progress("write", done, total);
      }
      if (cancelled || !fs.ok()) break;
    }
    if (cancelled || !fs.ok()) break;
    Status fl = dev.flush();
    if (!fl.ok()) { fs = fl; fcode = "flush-failed"; break; }
  }
  res.bytes_written = done;
  if (cancelled || !fs.ok()) {
    res.status = cancelled ? Status::Make(Errc::Cancelled) : fs;
    res.code = cancelled ? "cancelled" : fcode;
    res.message = cancelled ? "cancelled; the file system was not completed" : "write failed";
    if (ao.rollback_on_error) {
      bool all = true;
      for (auto it = saved.rbegin(); it != saved.rend(); ++it)
        if (!dev.write_at(plan.offset + it->offset, it->data.data(), it->data.size()).ok()) all = false;
      if (!dev.flush().ok()) all = false;
      res.rolled_back = all;
      res.message += all ? ": the backed-up regions were restored (large zero-filled regions cannot be restored)" : ": ROLLBACK INCOMPLETE; restore the backup";
    }
    return res;
  }

  // 読み戻し (invalidate-old 以外)
  {
    std::vector<uint8_t> buf;
    uint64_t vdone = 0;
    for (auto& st : plan.stages) {
      if (st.name == "invalidate-old") continue;
      for (auto& r : st.regions) {
        for (uint64_t pos = 0; pos < r.length;) {
          if (ao.cancel && ao.cancel->cancelled()) {
            res.status = Status::Make(Errc::Cancelled);
            res.code = "cancelled";
            res.message = "cancelled during verification; the file system was written but not verified";
            return res;
          }
          size_t n = static_cast<size_t>(std::min<uint64_t>(kChunk, r.length - pos));
          buf.resize(n);
          Status rs = dev.read_at(plan.offset + r.offset + pos, buf.data(), n);
          bool same = rs.ok() && (r.data.empty() ? std::all_of(buf.begin(), buf.end(), [](uint8_t c) { return c == 0; })
                                                  : std::memcmp(buf.data(), r.data.data() + pos, n) == 0);
          if (!same) {
            res.status = rs.ok() ? Status::Make(Errc::Io) : rs;
            res.code = "verify-failed";
            res.message = "read-back differs at offset " + std::to_string(r.offset + pos) + " (" + r.what + ")";
            return res;
          }
          pos += n;
          vdone += n;
          if (ao.progress) ao.progress("verify", vdone, total);
        }
      }
    }
  }
  // 自前の検査
  FsCheckOptions co;
  co.cancel = ao.cancel;
  FsCheckReport rep;
  Status cs = check_filesystem(dev, plan.offset, plan.length, plan.fs, co, &rep);
  if (!cs.ok()) {
    res.status = cs;
    res.code = "verify-failed";
    res.message = "the consistency check of the new file system could not be run";
    return res;
  }
  res.check_verdict = fs_verdict_name(rep.verdict);
  for (auto& f : rep.findings)
    if (f.severity != Severity::Info) res.verify_problems.push_back(Diagnostic{f.severity, f.code, f.message});
  if (rep.verdict != FsVerdict::Clean) {
    res.status = Status::Make(Errc::Corrupt);
    res.code = "verify-failed";
    res.message = std::string("the new file system did not pass the consistency check (") + res.check_verdict + ")";
    return res;
  }
  res.verified = true;
  res.message = "the file system was created and verified";
  return res;
}

std::string mkfs_plan_to_json(const MkfsPlan& p) {
  std::string o = "{";
  o += "\"ok\":" + std::string(p.ok ? "true" : "false");
  o += ",\"fs\":\"" + std::string(fs_type_name(p.fs)) + "\"";
  o += ",\"offset\":\"" + std::to_string(p.offset) + "\",\"length\":\"" + std::to_string(p.length) + "\"";
  o += ",\"existingFs\":\"" + jesc(p.existing_fs) + "\"";
  o += ",\"destructive\":" + std::string(p.destructive ? "true" : "false");
  o += ",\"requiresForce\":" + std::string(p.requires_force ? "true" : "false");
  o += ",\"bytesToWrite\":\"" + std::to_string(p.bytes_to_write) + "\"";
  o += ",\"problems\":" + diags_json(p.problems);
  o += ",\"changes\":[";
  for (size_t i = 0; i < p.changes.size(); i++) o += (i ? ",\"" : "\"") + jesc(p.changes[i]) + "\"";
  o += "],\"layout\":{";
  for (size_t i = 0; i < p.layout.size(); i++) o += (i ? ",\"" : "\"") + jesc(p.layout[i].first) + "\":\"" + jesc(p.layout[i].second) + "\"";
  o += "},\"stages\":[";
  for (size_t i = 0; i < p.stages.size(); i++) {
    if (i) o += ",";
    o += "{\"name\":\"" + jesc(p.stages[i].name) + "\",\"regions\":[";
    for (size_t k = 0; k < p.stages[i].regions.size(); k++) {
      auto& r = p.stages[i].regions[k];
      if (k) o += ",";
      o += "{\"what\":\"" + jesc(r.what) + "\",\"offset\":\"" + std::to_string(r.offset) + "\",\"length\":\"" + std::to_string(r.length) + "\",\"zero\":" + (r.data.empty() ? "true" : "false") + "}";
    }
    o += "]}";
  }
  o += "],\"deviceId\":\"" + jesc(p.device_id) + "\",\"planHash\":\"" + p.plan_hash + "\",\"confirmPhrase\":\"" + jesc(p.confirm_phrase) + "\"}";
  return o;
}

std::string mkfs_result_to_json(const MkfsResult& r) {
  std::string o = "{";
  o += "\"ok\":" + std::string(r.ok() ? "true" : "false");
  o += ",\"code\":\"" + jesc(r.code) + "\",\"message\":\"" + jesc(r.message) + "\"";
  o += ",\"bytesWritten\":\"" + std::to_string(r.bytes_written) + "\"";
  o += ",\"rolledBack\":" + std::string(r.rolled_back ? "true" : "false");
  o += ",\"verified\":" + std::string(r.verified ? "true" : "false");
  o += ",\"checkVerdict\":\"" + jesc(r.check_verdict) + "\"";
  o += ",\"verifyProblems\":" + diags_json(r.verify_problems) + "}";
  return o;
}

}  // namespace usbmem
