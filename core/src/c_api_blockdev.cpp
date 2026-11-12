// SPDX-License-Identifier: Apache-2.0
#include "usbmem/c_api_blockdev.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "usbmem/c_api_internal.h"
#include "usbmem/disk_image.h"

using namespace usbmem;

namespace {
int code(Status s) { return static_cast<int>(s.code); }
int wrap(BlockDevicePtr d, usbmem_device** out) {
  auto* h = new (std::nothrow) usbmem_device{std::move(d)};
  if (!h) return static_cast<int>(Errc::NoMemory);
  *out = h;
  return 0;
}
char* dup_str(const std::string& s) {
  char* p = static_cast<char*>(std::malloc(s.size() + 1));
  if (p) std::memcpy(p, s.c_str(), s.size() + 1);
  return p;
}
std::string jesc(const std::string& s) {
  std::string o = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += static_cast<char>(c); }
    else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
    else o += static_cast<char>(c);
  }
  return o + "\"";
}
std::string info_json(const ImageInfo& i) {
  std::string j = "{";
  j += "\"format\":" + jesc(image_format_name(i.format));
  j += ",\"subformat\":" + jesc(i.subformat);
  j += ",\"virtual_size\":" + std::to_string(i.virtual_size);
  j += ",\"logical_block_size\":" + std::to_string(i.logical_block_size);
  j += std::string(",\"has_parent\":") + (i.has_parent ? "true" : "false");
  j += ",\"parent_name\":" + jesc(i.parent_name);
  j += std::string(",\"compressed\":") + (i.compressed ? "true" : "false");
  j += ",\"content_id\":" + jesc(i.content_id);
  j += ",\"detail\":" + jesc(i.detail) + "}";
  return j;
}
void put_out(char** where, const std::string& s) { if (where) *where = dup_str(s); }
}  // namespace

extern "C" {

int usbmem_image_open(usbmem_device* file, usbmem_image_resolve_fn resolve, void* user, usbmem_device** out, char** out_info_json) {
  if (out_info_json) *out_info_json = nullptr;
  if (!file || !out) return static_cast<int>(Errc::InvalidArgument);
  *out = nullptr;
  try {
    ImageOpenOptions o;
    if (resolve) {
      o.resolver = [resolve, user](const ImageResolveRequest& rq, BlockDevicePtr* dev) -> Status {
        usbmem_device* h = nullptr;
        int rc = resolve(user, rq.name.c_str(), rq.kind == ResolveKind::Backing ? 0 : 1, rq.format_hint.c_str(), &h);
        if (rc != 0 || !h) return Status::Make(rc != 0 ? static_cast<Errc>(rc) : Errc::Io);
        *dev = h->dev;   // 共有し、ハンドルだけ解放 (デバイスは close しない)
        delete h;
        return Status::Ok();
      };
    }
    BlockDevicePtr d;
    ImageInfo info;
    Status st = open_disk_image(file->dev, o, &d, &info);
    put_out(out_info_json, info_json(info));
    return st.ok() ? wrap(std::move(d), out) : code(st);
  } catch (...) { return static_cast<int>(Errc::NoMemory); }
}

int usbmem_image_open_path(const char* path, int allow_unsafe_paths, usbmem_device** out, char** out_info_json) {
  if (out_info_json) *out_info_json = nullptr;
  if (!path || !out) return static_cast<int>(Errc::InvalidArgument);
  *out = nullptr;
  try {
    BlockDevicePtr d;
    ImageInfo info;
    Status st = open_disk_image_path(path, allow_unsafe_paths != 0, &d, &info);
    put_out(out_info_json, info_json(info));
    return st.ok() ? wrap(std::move(d), out) : code(st);
  } catch (...) { return static_cast<int>(Errc::NoMemory); }
}

int usbmem_image_detect(usbmem_device* file, char* buf, size_t len) {
  if (!file || !buf || !len) return static_cast<int>(Errc::InvalidArgument);
  try {
    ImageFormat f = ImageFormat::Raw;
    Status st = detect_image_format(*file->dev, &f);
    if (!st.ok()) return code(st);
    std::snprintf(buf, len, "%s", image_format_name(f));
    return 0;
  } catch (...) { return static_cast<int>(Errc::NoMemory); }
}

int usbmem_overlay_create(usbmem_device* base, const char* log_path, unsigned int block_size, unsigned long long max_bytes, usbmem_device** out) {
  if (!base || !out) return static_cast<int>(Errc::InvalidArgument);
  *out = nullptr;
  try {
    OverlayDevice::Options o;
    if (block_size) o.block_size = block_size;
    if (log_path && *log_path) o.file_path = log_path;
    o.max_overlay_bytes = max_bytes;
    std::shared_ptr<OverlayDevice> ov;
    Status st = OverlayDevice::create(base->dev, o, &ov);
    return st.ok() ? wrap(std::move(ov), out) : code(st);
  } catch (...) { return static_cast<int>(Errc::NoMemory); }
}

unsigned long long usbmem_overlay_dirty_bytes(usbmem_device* h) {
  if (!h) return 0;
  auto* ov = dynamic_cast<OverlayDevice*>(h->dev.get());
  return ov ? ov->dirty_bytes() : 0;
}

int usbmem_overlay_commit(usbmem_device* h, usbmem_device* target, int clear_after) {
  if (!h || !target) return static_cast<int>(Errc::InvalidArgument);
  auto* ov = dynamic_cast<OverlayDevice*>(h->dev.get());
  if (!ov) return static_cast<int>(Errc::InvalidArgument);
  try { return code(ov->commit(*target->dev, clear_after != 0)); } catch (...) { return static_cast<int>(Errc::NoMemory); }
}

int usbmem_overlay_discard(usbmem_device* h) {
  if (!h) return static_cast<int>(Errc::InvalidArgument);
  auto* ov = dynamic_cast<OverlayDevice*>(h->dev.get());
  if (!ov) return static_cast<int>(Errc::InvalidArgument);
  return code(ov->discard());
}

int usbmem_nbd_open(const char* host, int port, const char* unix_socket, const char* export_name, int read_only,
                    unsigned int connect_timeout_ms, unsigned int io_timeout_ms, usbmem_device** out, char** out_detail) {
  if (out_detail) *out_detail = nullptr;
  if (!out || port < 0 || port > 65535) return static_cast<int>(Errc::InvalidArgument);
  *out = nullptr;
  try {
    NbdOptions o;
    if (host) o.host = host;
    if (port) o.port = static_cast<uint16_t>(port);
    if (unix_socket) o.unix_socket = unix_socket;
    if (export_name) o.export_name = export_name;
    o.read_only = read_only != 0;
    if (connect_timeout_ms) o.connect_timeout_ms = connect_timeout_ms;
    if (io_timeout_ms) o.io_timeout_ms = io_timeout_ms;
    std::shared_ptr<NbdDevice> d;
    std::string detail;
    Status st = NbdDevice::connect(o, &d, &detail);
    if (!st.ok()) { put_out(out_detail, detail); return code(st); }
    return wrap(std::move(d), out);
  } catch (...) { return static_cast<int>(Errc::NoMemory); }
}

int usbmem_nbd_list_exports_json(const char* host, int port, const char* unix_socket, char** out_json, char** out_detail) {
  if (out_json) *out_json = nullptr;
  if (out_detail) *out_detail = nullptr;
  if (!out_json || port < 0 || port > 65535) return static_cast<int>(Errc::InvalidArgument);
  try {
    NbdOptions o;
    if (host) o.host = host;
    if (port) o.port = static_cast<uint16_t>(port);
    if (unix_socket) o.unix_socket = unix_socket;
    std::vector<std::string> names;
    std::string detail;
    Status st = NbdDevice::list_exports(o, &names, &detail);
    if (!st.ok()) { put_out(out_detail, detail); return code(st); }
    std::string j = "[";
    for (size_t i = 0; i < names.size(); i++) j += (i ? "," : "") + jesc(names[i]);
    j += "]";
    *out_json = dup_str(j);
    return *out_json ? 0 : static_cast<int>(Errc::NoMemory);
  } catch (...) { return static_cast<int>(Errc::NoMemory); }
}

int usbmem_nbd_info_json(usbmem_device* h, char** out_json) {
  if (!h || !out_json) return static_cast<int>(Errc::InvalidArgument);
  *out_json = nullptr;
  auto* n = dynamic_cast<NbdDevice*>(h->dev.get());
  if (!n) return static_cast<int>(Errc::Unsupported);
  try {
    std::string j = "{\"size\":" + std::to_string(n->size());
    j += std::string(",\"read_only\":") + (n->is_read_only() ? "true" : "false");
    j += std::string(",\"structured\":") + (n->structured_replies() ? "true" : "false");
    j += ",\"min_block\":" + std::to_string(n->min_block_size());
    j += ",\"max_block\":" + std::to_string(n->max_block_size());
    j += ",\"flags\":" + std::to_string(n->transmission_flags());
    j += ",\"reconnects\":" + std::to_string(n->reconnect_count()) + "}";
    *out_json = dup_str(j);
    return *out_json ? 0 : static_cast<int>(Errc::NoMemory);
  } catch (...) { return static_cast<int>(Errc::NoMemory); }
}

}  // extern "C"
