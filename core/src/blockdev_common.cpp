// SPDX-License-Identifier: Apache-2.0
#include <zlib.h>

#include <algorithm>
#include <array>

#include "blockdev_internal.h"

namespace usbmem {
namespace imgint {

uint32_t crc32c(const void* data, size_t len, uint32_t seed) {
  static const std::array<uint32_t, 256> tbl = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : c >> 1;
      t[i] = c;
    }
    return t;
  }();
  uint32_t c = ~seed;
  const uint8_t* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < len; i++) c = tbl[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  return ~c;
}

Status inflate_to(const uint8_t* in, size_t inlen, uint8_t* out, size_t outlen, bool zlib_header) {
  z_stream z;
  std::memset(&z, 0, sizeof z);
  if (inflateInit2(&z, zlib_header ? 15 : -15) != Z_OK) return Status::Make(Errc::NoMemory);
  z.next_in = const_cast<Bytef*>(in);
  z.avail_in = static_cast<uInt>(inlen);
  z.next_out = out;
  z.avail_out = static_cast<uInt>(outlen);
  int r = inflate(&z, Z_FINISH);
  size_t produced = outlen - z.avail_out;
  inflateEnd(&z);
  // Z_STREAM_END: 正常終了。Z_BUF_ERROR / Z_OK で出力が満杯: 入力に続きがあっても必要分は得られた。
  bool good = (r == Z_STREAM_END) || ((r == Z_BUF_ERROR || r == Z_OK) && produced == outlen);
  if (!good) return Status::Make(Errc::Corrupt);
  if (produced < outlen) std::memset(out + produced, 0, outlen - produced);
  return Status::Ok();
}

std::string utf16_to_utf8(const uint8_t* p, size_t bytes, bool be) {
  std::string s;
  auto u16 = [&](size_t i) -> uint32_t { return be ? be16(p + i) : le16(p + i); };
  for (size_t i = 0; i + 1 < bytes; i += 2) {
    uint32_t c = u16(i);
    if (c == 0) break;
    if (c >= 0xD800 && c < 0xDC00 && i + 3 < bytes) {
      uint32_t d = u16(i + 2);
      if (d >= 0xDC00 && d < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00); i += 2; }
      else c = 0xFFFD;
    } else if (c >= 0xD800 && c < 0xE000) c = 0xFFFD;
    if (c < 0x80) s += static_cast<char>(c);
    else if (c < 0x800) { s += static_cast<char>(0xC0 | (c >> 6)); s += static_cast<char>(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) { s += static_cast<char>(0xE0 | (c >> 12)); s += static_cast<char>(0x80 | ((c >> 6) & 0x3F)); s += static_cast<char>(0x80 | (c & 0x3F)); }
    else { s += static_cast<char>(0xF0 | (c >> 18)); s += static_cast<char>(0x80 | ((c >> 12) & 0x3F)); s += static_cast<char>(0x80 | ((c >> 6) & 0x3F)); s += static_cast<char>(0x80 | (c & 0x3F)); }
  }
  return s;
}

std::string norm_slashes(std::string s) {
  std::replace(s.begin(), s.end(), '\\', '/');
  return s;
}

Status read_zero_beyond(BlockDevice& dev, uint64_t off, void* buf, size_t len, const IoOptions& io) {
  uint64_t sz = dev.size();
  uint8_t* p = static_cast<uint8_t*>(buf);
  if (off >= sz) { std::memset(p, 0, len); return Status::Ok(); }
  size_t have = static_cast<size_t>(std::min<uint64_t>(len, sz - off));
  Status st = dev.read_at(off, p, have, io);
  if (!st.ok()) return st;
  if (have < len) std::memset(p + have, 0, len - have);
  return Status::Ok();
}

Status ImageDeviceBase::read_at(uint64_t off, void* buf, size_t len, const IoOptions& opt) {
  std::lock_guard<std::mutex> lk(mu_);
  if (closed_) return Status::Make(Errc::Closed);
  if (!range_ok(vsize_, off, len)) return Status::Make(Errc::OutOfRange);
  uint8_t* p = static_cast<uint8_t*>(buf);
  while (len > 0) {
    if (opt.cancel && opt.cancel->cancelled()) return Status::Make(Errc::Cancelled);
    uint64_t n = std::min<uint64_t>(len, unit_len(off));
    if (n == 0) return Status::Make(Errc::Corrupt);
    Status st;
    try { st = read_unit(off, p, static_cast<size_t>(n), opt); } catch (...) { return Status::Make(Errc::NoMemory); }   // BlockDevice は例外を投げない
    if (!st.ok()) return st;
    p += n; off += n; len -= static_cast<size_t>(n);
  }
  return Status::Ok();
}

Status ImageDeviceBase::close() {
  std::lock_guard<std::mutex> lk(mu_);
  if (closed_) return Status::Ok();
  closed_ = true;
  on_close();
  Status st = file_ ? file_->close() : Status::Ok();
  file_.reset();
  return st;
}

Status open_parent(const ImageOpenOptions& o, uint32_t depth, const ImageResolveRequest& req, BlockDevicePtr* out, ImageInfo* pinfo, ImageInfo* err) {
  if (!o.resolver) IMG_FAIL(err, Errc::Unsupported, "image needs another file ('" + req.name + "') but no resolver was provided");
  if (depth + 1 > o.max_backing_depth) IMG_FAIL(err, Errc::Corrupt, "backing/parent chain too deep (or circular)");
  BlockDevicePtr raw;
  Status st = o.resolver(req, &raw);
  if (!st.ok()) { if (err) err->detail = "cannot open '" + req.name + "'"; return st; }
  if (!raw) IMG_FAIL(err, Errc::Io, "resolver returned no device for '" + req.name + "'");
  const std::string& h = req.format_hint;
  ImageInfo local;
  ImageInfo* pi = pinfo ? pinfo : &local;
  if (h == "raw") { *out = raw; pi->format = ImageFormat::Raw; pi->virtual_size = raw->size(); return Status::Ok(); }
  if (h.empty() && !o.probe_backing_format) { *out = raw; pi->format = ImageFormat::Raw; pi->virtual_size = raw->size(); return Status::Ok(); }
  Status r;
  if (h == "qcow2") r = open_qcow2_depth(raw, o, depth + 1, out, pi);
  else if (h == "vpc" || h == "vhd") r = open_vhd_depth(raw, o, depth + 1, out, pi);
  else if (h == "vhdx") r = open_vhdx_depth(raw, o, depth + 1, out, pi);
  else if (h == "vmdk") r = open_vmdk_depth(raw, o, depth + 1, out, pi);
  else r = open_disk_image_depth(raw, o, depth + 1, out, pi);
  if (!r.ok() && err) err->detail = "parent '" + req.name + "': " + pi->detail;
  return r;
}

}  // namespace imgint
}  // namespace usbmem
