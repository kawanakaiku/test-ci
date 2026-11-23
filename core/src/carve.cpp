// SPDX-License-Identifier: Apache-2.0
// カービングの走査: デバイスを塊で読み、ブロック境界 (align) ごとに署名を調べ、合えば形式の検査 (carve_formats.cpp) で長さを確定する。
// 見つけたファイルの終端までは飛ばして次へ進む (入れ子のファイルを重ねて数えない)。進捗は CarveState (再開用)。
#include "usbmem/carve.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "carve_internal.h"
#include "usbmem/backup.h"
#include "usbmem/fs_probe.h"
#include "usbmem/sha256.h"

namespace usbmem {

std::vector<CarveTypeInfo> carve_types() {
  std::vector<CarveTypeInfo> v;
  for (const carve::FormatDef& f : carve::formats()) v.push_back({f.id, f.exts, f.desc});
  return v;
}

namespace {

uint64_t fnv(uint64_t h, const void* p, size_t n) {
  const uint8_t* b = static_cast<const uint8_t*>(p);
  for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
  return h;
}

// 割り当て済みの領域を飛ばすための地図 (UsedMap) の上の問い合わせ
struct FreeMap {
  const UsedMap* m = nullptr;
  uint64_t cache_lo = 1, cache_hi = 0;  // 直近に求めた空き領域 [lo, hi) (バイト)
  bool used_at(uint64_t off) const { return m->test(off / m->block_size); }
  // off (空きのブロック内) から続く空き領域の終端 (バイト)
  uint64_t free_end(uint64_t off, uint64_t dev_size) {
    if (off >= cache_lo && off < cache_hi) return cache_hi;
    uint64_t b = off / m->block_size;
    const uint64_t total = m->total_blocks;
    while (b < total) {
      if ((b & 7) == 0 && b + 8 <= total && m->bits[b >> 3] == 0) { b += 8; continue; }
      if ((m->bits[b >> 3] >> (b & 7)) & 1) break;
      b++;
    }
    uint64_t end = b >= total ? dev_size : std::min<uint64_t>(dev_size, b * m->block_size);
    cache_lo = off;
    cache_hi = end;
    return end;
  }
};

}  // namespace

Status carve_scan(BlockDevice& dev, const CarveOptions& opt, CarveState* state, CarveResult* res) {
  if (!state || !res) return Status::Make(Errc::InvalidArgument);
  *res = CarveResult();
  const std::vector<carve::FormatDef>& all = carve::formats();
  std::vector<const carve::FormatDef*> sel;
  for (const std::string& t : opt.types) {
    bool known = false;
    for (const carve::FormatDef& f : all) known = known || t == f.id;
    if (!known) return Status::Make(Errc::InvalidArgument);
  }
  for (const carve::FormatDef& f : all)
    if (opt.types.empty() || opt.types.count(f.id)) sel.push_back(&f);
  if (sel.empty()) return Status::Make(Errc::InvalidArgument);
  const uint32_t align = opt.align ? opt.align : 512;
  if (align & (align - 1)) return Status::Make(Errc::InvalidArgument);
  const uint64_t max_len = opt.max_size ? opt.max_size : (16ull << 30);
  const uint64_t size = dev.size();

  uint64_t h = 1469598103934665603ull;
  for (const carve::FormatDef* f : sel) h = fnv(h, f->id, std::strlen(f->id) + 1);
  h = fnv(h, &align, sizeof align);
  h = fnv(h, &opt.min_size, sizeof opt.min_size);
  h = fnv(h, &max_len, sizeof max_len);
  uint8_t un = opt.unallocated_only ? 1 : 0;
  h = fnv(h, &un, 1);
  if (state->dev_size == 0 && state->pos == 0 && state->hits.empty()) {
    state->dev_size = size;
    state->options_hash = h;
  } else if (state->dev_size != size || state->options_hash != h || state->pos > size) {
    return Status::Make(Errc::InvalidArgument);
  }
  if (state->finished) return Status::Ok();

  // 未割り当ての領域だけ
  UsedMap used;
  FreeMap fm;
  if (opt.unallocated_only) {
    if (opt.progress) opt.progress("alloc", 0, 0);
    Status io;
    FsType t = probe_filesystem(dev, 0, size, &io);
    if (!io.ok() && (io.code == Errc::Disconnected || io.code == Errc::Closed)) return io;
    switch (t) {
      case FsType::Fat12: case FsType::Fat16: case FsType::Fat32: case FsType::Exfat: case FsType::Ext2: case FsType::Ext3: case FsType::Ext4: case FsType::Ntfs: {
        res->fs = fs_type_name(t);
        ExtractOptions eo;
        eo.full_if_unclean = true;
        FsStatus fs = build_used_map(dev, eo, &used);
        if (fs.ok() && !used.full_copy) { res->allocation_known = true; fm.m = &used; }
        else res->warnings.push_back("allocation information is unavailable: scanning the whole device");
        break;
      }
      default:
        res->warnings.push_back("filesystem not recognized: scanning the whole device");
    }
  }

  carve::CarveReader reader(dev, size, opt.cancel);
  const size_t chunk = 8u << 20;
  std::vector<uint8_t> buf(chunk + 256, 0);
  uint64_t pos = (state->pos + align - 1) & ~static_cast<uint64_t>(align - 1);
  uint64_t since_ck = 0;
  uint64_t skip_until = pos;
  if (!state->hits.empty()) {  // 再開: 直前のヒットの中は飛ばす (取り消しで塊の途中から再開すると二重に数えるため)
    const CarveHit& lh = state->hits.back();
    skip_until = std::max(skip_until, (lh.offset + lh.length + align - 1) & ~static_cast<uint64_t>(align - 1));
  }
  Status ret;
  while (pos < size) {
    if (opt.cancel && opt.cancel->cancelled()) { res->cancelled = true; ret = Status::Make(Errc::Cancelled); break; }
    uint64_t n = std::min<uint64_t>(chunk, size - pos);
    uint64_t rl = std::min<uint64_t>(n + 64, size - pos);
    IoOptions io;
    io.cancel = opt.cancel;
    Status rs = dev.read_at(pos, buf.data(), static_cast<size_t>(rl), io);
    if (!rs.ok()) {
      if (rs.code == Errc::Cancelled) { res->cancelled = true; ret = rs; break; }
      if (rs.code == Errc::Disconnected || rs.code == Errc::Closed) { ret = rs; break; }
      res->read_errors++;
      pos += n;
      state->pos = pos;
      continue;
    }
    std::memset(buf.data() + rl, 0, 64);
    bool stop = false;
    for (uint64_t p = 0; p < n; p += align) {
      uint64_t abs = pos + p;
      if (abs < skip_until) continue;
      if (fm.m && fm.used_at(abs)) continue;
      const uint8_t* q = buf.data() + p;
      for (const carve::FormatDef* f : sel) {
        if (!f->quick(q, 16)) continue;
        uint64_t limit = fm.m ? fm.free_end(abs, size) : size;
        reader.set_limit(limit);
        reader.reset_error();
        carve::Match m;
        bool ok = f->validate(reader, abs, std::min<uint64_t>(max_len, limit - abs), &m);
        if (reader.cancelled()) { res->cancelled = true; ret = Status::Make(Errc::Cancelled); stop = true; break; }
        if (reader.io_error()) {
          Status ls = reader.last_status();
          if (ls.code == Errc::Disconnected || ls.code == Errc::Closed) { ret = ls; stop = true; break; }
          res->read_errors++;
        }
        if (!ok || m.length < f->min_len) { state->rejected++; continue; }
        if (m.length < opt.min_size) continue;
        CarveHit hit;
        hit.offset = abs;
        hit.length = m.length;
        hit.format = f->id;
        hit.ext = m.ext;
        hit.detail = m.detail;
        hit.confidence = m.confidence;
        if (opt.on_hit) {
          Status hs = opt.on_hit(hit);
          if (!hs.ok()) { ret = hs; stop = true; break; }
        }
        state->hits.push_back(hit);
        skip_until = (abs + m.length + align - 1) & ~static_cast<uint64_t>(align - 1);
        if (opt.checkpoint) opt.checkpoint(*state);
        if (state->hits.size() >= opt.max_hits) { res->warnings.push_back("max_hits reached"); stop = true; }
        break;
      }
      if (stop) break;
    }
    if (stop) {
      if (ret.code == Errc::Cancelled) { /* この塊は途中。pos は進めず、再開時に同じ塊をやり直す */ }
      else if (ret.ok()) { state->pos = std::max(pos + n, skip_until); res->bytes_scanned += n; }
      break;
    }
    pos = std::max(pos + n, skip_until);
    pos = (pos + align - 1) & ~static_cast<uint64_t>(align - 1);
    res->bytes_scanned += n;
    state->pos = std::min(pos, size);
    since_ck += n;
    if (opt.progress) opt.progress("scan", state->pos, size);
    if (opt.checkpoint && since_ck >= opt.checkpoint_bytes) { since_ck = 0; opt.checkpoint(*state); }
  }
  if (ret.ok() && !res->cancelled && state->pos >= size) {
    state->finished = true;
    state->pos = size;
  }
  if (opt.checkpoint) opt.checkpoint(*state);
  if (ret.ok() && res->cancelled) ret = Status::Make(Errc::Cancelled);
  return ret;
}

Status carve_extract(BlockDevice& dev, const CarveHit& hit, const ByteSinkFn& sink, const CancelToken* cancel, ExtractResult* res) {
  if (hit.length > dev.size() || hit.offset > dev.size() - hit.length) return Status::Make(Errc::OutOfRange);
  Sha256 sh;
  std::vector<uint8_t> buf;
  uint64_t done = 0;
  Status st;
  while (done < hit.length && st.ok()) {
    if (cancel && cancel->cancelled()) { st = Status::Make(Errc::Cancelled); break; }
    size_t n = static_cast<size_t>(std::min<uint64_t>(hit.length - done, 1u << 20));
    buf.resize(n);
    IoOptions io;
    io.cancel = cancel;
    st = dev.read_at(hit.offset + done, buf.data(), n, io);
    if (!st.ok()) break;
    st = sink(buf.data(), n);
    if (!st.ok()) break;
    sh.update(buf.data(), n);
    done += n;
  }
  if (res) { res->bytes = done; res->sha256 = hex_encode(sh.finish()); }
  return st;
}

std::string carve_hit_filename(const CarveHit& h) {
  char b[64];
  std::snprintf(b, sizeof b, "f%012" PRIx64 ".", h.offset);
  return std::string(b) + (h.ext.empty() ? "bin" : h.ext);
}

std::string carve_state_to_text(const CarveState& s) {
  std::string o = "usbmem-carve-state 1\n";
  char b[160];
  std::snprintf(b, sizeof b, "dev_size %" PRIu64 "\noptions %016" PRIx64 "\npos %" PRIu64 "\nrejected %" PRIu64 "\nfinished %d\n", s.dev_size, s.options_hash, s.pos, s.rejected, s.finished ? 1 : 0);
  o += b;
  for (const CarveHit& h : s.hits) {
    std::snprintf(b, sizeof b, "hit %" PRIu64 " %" PRIu64 " %d ", h.offset, h.length, h.confidence);
    std::string d = h.detail;
    for (char& c : d) if (c == '\n' || c == '\r') c = ' ';
    o += b + h.format + " " + (h.ext.empty() ? "-" : h.ext) + " " + d + "\n";
  }
  return o;
}

bool carve_state_parse(const std::string& text, CarveState* out) {
  CarveState s;
  size_t p = 0;
  auto line = [&](std::string* l) {
    if (p >= text.size()) return false;
    size_t e = text.find('\n', p);
    if (e == std::string::npos) return false;  // 最後の行が途切れているものは壊れているとみなす
    *l = text.substr(p, e - p);
    p = e + 1;
    return true;
  };
  std::string l;
  if (!line(&l) || l != "usbmem-carve-state 1") return false;
  unsigned long long a = 0;
  int f = 0;
  if (!line(&l) || std::sscanf(l.c_str(), "dev_size %llu", &a) != 1) return false;
  s.dev_size = a;
  if (!line(&l) || std::sscanf(l.c_str(), "options %llx", &a) != 1) return false;
  s.options_hash = a;
  if (!line(&l) || std::sscanf(l.c_str(), "pos %llu", &a) != 1) return false;
  s.pos = a;
  if (!line(&l) || std::sscanf(l.c_str(), "rejected %llu", &a) != 1) return false;
  s.rejected = a;
  if (!line(&l) || std::sscanf(l.c_str(), "finished %d", &f) != 1 || (f != 0 && f != 1)) return false;
  s.finished = f == 1;
  if (s.pos > s.dev_size || s.dev_size == 0) return false;
  while (p < text.size()) {
    if (!line(&l)) return false;
    unsigned long long off, len;
    int conf, used = 0;
    if (std::sscanf(l.c_str(), "hit %llu %llu %d %n", &off, &len, &conf, &used) < 3 || used <= 0) return false;
    std::string rest = l.substr(static_cast<size_t>(used));
    size_t s1 = rest.find(' ');
    if (s1 == std::string::npos) return false;
    size_t s2 = rest.find(' ', s1 + 1);
    if (s2 == std::string::npos) return false;
    CarveHit h;
    h.offset = off;
    h.length = len;
    h.confidence = conf;
    h.format = rest.substr(0, s1);
    h.ext = rest.substr(s1 + 1, s2 - s1 - 1);
    if (h.ext == "-") h.ext.clear();
    h.detail = rest.substr(s2 + 1);
    if (h.format.empty() || len == 0 || off > s.dev_size || len > s.dev_size - off) return false;
    s.hits.push_back(h);
  }
  *out = std::move(s);
  return true;
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

std::string carve_result_to_json(const CarveResult& r, const CarveState& s) {
  std::string o = "{\"fs\":";
  jstr(o, r.fs);
  o += std::string(",\"allocationKnown\":") + (r.allocation_known ? "true" : "false") + ",\"cancelled\":" + (r.cancelled ? "true" : "false") +
       ",\"finished\":" + (s.finished ? "true" : "false") + ",\"pos\":" + std::to_string(s.pos) + ",\"devSize\":" + std::to_string(s.dev_size) +
       ",\"bytesScanned\":" + std::to_string(r.bytes_scanned) + ",\"readErrors\":" + std::to_string(r.read_errors) + ",\"rejected\":" + std::to_string(s.rejected) + ",\"warnings\":[";
  for (size_t i = 0; i < r.warnings.size(); i++) { if (i) o += ','; jstr(o, r.warnings[i]); }
  o += "],\"hits\":[";
  for (size_t i = 0; i < s.hits.size(); i++) {
    const CarveHit& h = s.hits[i];
    if (i) o += ',';
    o += "{\"offset\":" + std::to_string(h.offset) + ",\"length\":" + std::to_string(h.length) + ",\"format\":";
    jstr(o, h.format);
    o += ",\"ext\":";
    jstr(o, h.ext);
    o += ",\"detail\":";
    jstr(o, h.detail);
    o += ",\"confidence\":" + std::to_string(h.confidence) + ",\"name\":";
    jstr(o, carve_hit_filename(h));
    o += "}";
  }
  o += "]}";
  return o;
}

}  // namespace usbmem
