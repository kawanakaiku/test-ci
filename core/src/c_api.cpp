// SPDX-License-Identifier: Apache-2.0
#include "usbmem/c_api.h"
#include "usbmem/c_api_internal.h"
#include "usbmem/usb_msc.h"
#include "usbmem/usbdevfs_transport.h"

#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <vector>
#include <chrono>
#include <new>

#include "usbmem/devices.h"
#include "usbmem/partition.h"


using namespace usbmem;

struct usbmem_usb_bus { std::shared_ptr<MscBus> bus; };

namespace {
void json_str(std::string& o, const std::string& v) {
  o += '"';
  for (char c : v) {
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E) o += ' ';
    else o += c;
  }
  o += '"';
}
int code(Status s) { return static_cast<int>(s.code); }
int wrap(usbmem::BlockDevicePtr d, usbmem_device** out) {
  auto* h = new (std::nothrow) usbmem_device{std::move(d)};
  if (!h) return static_cast<int>(Errc::NoMemory);
  *out = h;
  return 0;
}
}  // namespace

extern "C" {

int usbmem_open_fd(int fd, int read_only, int own, usbmem_device** out) {
  if (!out) return static_cast<int>(Errc::InvalidArgument);
  *out = nullptr;
  try {
    BlockDevicePtr d;
    Status st = RawImageDevice::open_fd(fd, read_only != 0, own != 0, 512, &d);
    return st.ok() ? wrap(std::move(d), out) : code(st);
  } catch (...) { return static_cast<int>(Errc::NoMemory); }
}

int usbmem_open_path(const char* path, int read_only, usbmem_device** out) {
  if (!path || !out) return static_cast<int>(Errc::InvalidArgument);
  *out = nullptr;
  try {
    BlockDevicePtr d;
    Status st = RawImageDevice::open_path(path, read_only != 0, 512, &d);
    return st.ok() ? wrap(std::move(d), out) : code(st);
  } catch (...) { return static_cast<int>(Errc::NoMemory); }
}

int usbmem_list_partitions_json(usbmem_device* h, char** out_json) {
  if (!h || !out_json) return static_cast<int>(Errc::InvalidArgument);
  *out_json = nullptr;
  try {
    PartitionTable t;
    Status st = read_partition_table(*h->dev, &t);
    if (!st.ok()) return code(st);
    std::string j = partition_table_to_json(t);
    char* p = static_cast<char*>(std::malloc(j.size() + 1));
    if (!p) return static_cast<int>(Errc::NoMemory);
    std::memcpy(p, j.c_str(), j.size() + 1);
    *out_json = p;
    return 0;
  } catch (...) { return static_cast<int>(Errc::NoMemory); }
}

/* 親デバイスの一区間をデバイスとして得る (パーティション)。親は子が生きている間 shared_ptr で保持される。 */
int usbmem_device_sub_range(usbmem_device* parent, unsigned long long offset, unsigned long long length, int read_only, usbmem_device** out) {
  if (!parent || !out) return static_cast<int>(Errc::InvalidArgument);
  BlockDevicePtr d;
  Status st = SubRangeDevice::create(parent->dev, offset, length, read_only != 0, &d);
  if (!st.ok()) return code(st);
  return wrap(std::move(d), out);
}

int usbmem_open_usb_msc(int fd, int ep_in, int ep_out, int interface_number, int lun, int read_only, usbmem_device** out) {
  if (!out || fd < 0 || ep_in < 0 || ep_in > 255 || ep_out < 0 || ep_out > 255 || !(ep_in & 0x80) || (ep_out & 0x80) ||
      interface_number < 0 || interface_number > 255 || lun < 0 || lun > 15)
    return static_cast<int>(Errc::InvalidArgument);
  UsbEndpoints eps;
  eps.ep_in = static_cast<uint8_t>(ep_in); eps.ep_out = static_cast<uint8_t>(ep_out); eps.interface = static_cast<uint8_t>(interface_number);
  auto t = make_usbdevfs_transport(fd, eps);
  if (!t) return static_cast<int>(Errc::NoMemory);
  MscOptions o;
  o.lun = static_cast<uint8_t>(lun);
  o.read_only = read_only != 0;
  BlockDevicePtr d;
  Status st = MscDevice::open(std::move(t), o, &d);
  if (!st.ok()) return code(st);
  return wrap(std::move(d), out);
}

int usbmem_usb_bus_open(int fd, int ep_in, int ep_out, int interface_number, int query_max_lun, usbmem_usb_bus** out) {
  if (!out) return static_cast<int>(Errc::InvalidArgument);
  *out = nullptr;
  if (fd < 0 || ep_in < 0 || ep_in > 255 || ep_out < 0 || ep_out > 255 || !(ep_in & 0x80) || (ep_out & 0x80) || interface_number < 0 || interface_number > 255)
    return static_cast<int>(Errc::InvalidArgument);
  UsbEndpoints eps;
  eps.ep_in = static_cast<uint8_t>(ep_in); eps.ep_out = static_cast<uint8_t>(ep_out); eps.interface = static_cast<uint8_t>(interface_number);
  auto t = make_usbdevfs_transport(fd, eps);
  if (!t) return static_cast<int>(Errc::NoMemory);
  std::shared_ptr<MscBus> bus;
  Status st = MscBus::create(std::move(t), query_max_lun != 0, &bus);
  if (!st.ok()) return code(st);
  auto* h = new (std::nothrow) usbmem_usb_bus{std::move(bus)};
  if (!h) return static_cast<int>(Errc::NoMemory);
  *out = h;
  return 0;
}

int usbmem_usb_bus_lun_count(usbmem_usb_bus* b) { return b && b->bus ? static_cast<int>(b->bus->lun_count()) : 0; }

void usbmem_usb_bus_max_lun_note(usbmem_usb_bus* b, char* buf, size_t len) {
  if (!buf || !len) return;
  snprintf(buf, len, "%s", b && b->bus ? b->bus->max_lun_note().c_str() : "");
}

int usbmem_usb_bus_scan_luns(usbmem_usb_bus* b, unsigned int skip_mask, int ready_wait_ms, char** out_json) {
  if (!b || !b->bus || !out_json) return static_cast<int>(Errc::InvalidArgument);
  *out_json = nullptr;
  try {
    MscOptions o;
    o.ready_wait_ms = ready_wait_ms < 0 ? 0 : static_cast<uint32_t>(std::min(ready_wait_ms, 5000));
    std::vector<LunInfo> v;
    Status st = b->bus->scan_luns(o, skip_mask, &v);
    if (!st.ok()) return code(st);
    std::string j = "[";
    for (size_t i = 0; i < v.size(); i++) {
      const LunInfo& l = v[i];
      if (i) j += ',';
      j += "{\"lun\":" + std::to_string(l.lun) + ",\"state\":\"" + lun_state_name(l.state) + "\",\"ptype\":" + std::to_string(l.ptype) +
           ",\"removable\":" + (l.removable ? "true" : "false") + ",\"vendor\":";
      json_str(j, l.vendor); j += ",\"product\":"; json_str(j, l.product);
      j += ",\"blocks\":\"" + std::to_string(l.blocks) + "\",\"blockSize\":" + std::to_string(l.block_size) + ",\"note\":"; json_str(j, l.note);
      j += '}';
    }
    j += ']';
    char* p = static_cast<char*>(std::malloc(j.size() + 1));
    if (!p) return static_cast<int>(Errc::NoMemory);
    std::memcpy(p, j.c_str(), j.size() + 1);
    *out_json = p;
    return 0;
  } catch (...) { return static_cast<int>(Errc::NoMemory); }
}

int usbmem_open_usb_msc_lun(usbmem_usb_bus* b, int lun, int read_only, usbmem_device** out) {
  if (!out) return static_cast<int>(Errc::InvalidArgument);
  *out = nullptr;
  if (!b || !b->bus || lun < 0 || lun > 15) return static_cast<int>(Errc::InvalidArgument);
  MscOptions o;
  o.lun = static_cast<uint8_t>(lun);
  o.read_only = read_only != 0;
  BlockDevicePtr d;
  Status st = b->bus->open_lun(o, &d);
  if (!st.ok()) return code(st);
  return wrap(std::move(d), out);
}

void usbmem_usb_bus_close(usbmem_usb_bus* b) { delete b; }

int usbmem_usb_start_stop(usbmem_device* dev, int start) {
  if (!dev) return static_cast<int>(Errc::InvalidArgument);
  auto* m = dynamic_cast<MscDevice*>(dev->dev.get());
  if (!m) return static_cast<int>(Errc::Unsupported);
  Status st = m->start_stop(start != 0);
  return st.ok() ? 0 : static_cast<int>(st.code);
}

int usbmem_usb_idle_stop(usbmem_device* dev, unsigned long long idle_ms) {
  if (!dev) return -static_cast<int>(Errc::InvalidArgument);
  auto* m = dynamic_cast<MscDevice*>(dev->dev.get());
  if (!m) return 0;
  return m->idle_stop(idle_ms);
}

int usbmem_usb_spun_down(usbmem_device* dev) {
  auto* m = dev ? dynamic_cast<MscDevice*>(dev->dev.get()) : nullptr;
  return m && m->spun_down() ? 1 : 0;
}

int usbmem_usb_media_info(usbmem_device* dev, int out[6], char* detail, size_t detail_len) {
  if (detail && detail_len) detail[0] = 0;
  if (!dev || !out) return static_cast<int>(Errc::InvalidArgument);
  auto* m = dynamic_cast<MscDevice*>(dev->dev.get());
  if (!m) return static_cast<int>(Errc::Unsupported);
  const MediaInfo& mi = m->media_info();
  out[0] = static_cast<int>(mi.kind); out[1] = static_cast<int>(mi.rpm); out[2] = static_cast<int>(mi.basis); out[3] = static_cast<int>(mi.hint);
  out[4] = mi.removable ? 1 : 0; out[5] = mi.probed ? 1 : 0;
  if (detail && detail_len) snprintf(detail, detail_len, "%s", mi.describe().c_str());
  return 0;
}

int usbmem_usb_tune(usbmem_device* dev, size_t transport_chunk, size_t max_transfer) {
  if (!dev) return static_cast<int>(Errc::InvalidArgument);
  auto* m = dynamic_cast<MscDevice*>(dev->dev.get());
  if (!m) return static_cast<int>(Errc::Unsupported);
  m->tune(transport_chunk, static_cast<uint32_t>(max_transfer));
  return 0;
}

int usbmem_bench_read(usbmem_device* dev, unsigned long long offset, unsigned long long total, size_t buf_len, long long* out_us, unsigned long long* out_hash) {
  if (!dev || !out_us || !buf_len || buf_len > (64u << 20)) return static_cast<int>(Errc::InvalidArgument);
  try {
    std::vector<uint8_t> buf(buf_len);
    auto t0 = std::chrono::steady_clock::now();
    unsigned long long done = 0, hash = 1469598103934665603ull;   // FNV-1a (8 バイト単位)
    long long hash_us = 0;
    while (done < total) {
      size_t n = static_cast<size_t>(std::min<unsigned long long>(buf_len, total - done));
      Status st = dev->dev->read_at(offset + done, buf.data(), n);
      if (!st.ok()) return code(st);
      if (out_hash) {   // ハッシュの計算時間は速度に含めない
        auto h0 = std::chrono::steady_clock::now();
        size_t i = 0;
        for (; i + 8 <= n; i += 8) { uint64_t w; std::memcpy(&w, buf.data() + i, 8); hash = (hash ^ w) * 1099511628211ull; }
        for (; i < n; i++) hash = (hash ^ buf[i]) * 1099511628211ull;
        hash_us += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - h0).count();
      }
      done += n;
    }
    if (out_hash) *out_hash = hash;
    *out_us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count() - hash_us;
    if (*out_us < 1) *out_us = 1;
    return 0;
  } catch (...) { return static_cast<int>(Errc::NoMemory); }
}

void usbmem_usb_last_open_trace(char* buf, size_t len) {
  if (!buf || !len) return;
  snprintf(buf, len, "%s", MscDevice::last_open_trace().c_str());
}

void usbmem_usb_op_stats(usbmem_device* dev, char* buf, size_t len) {
  if (!buf || !len) return;
  buf[0] = 0;
  if (dev) if (auto* m = dynamic_cast<MscDevice*>(dev->dev.get())) { std::string s = m->op_stats(); snprintf(buf, len, "%s", s.c_str()); }
}

void usbmem_usb_last_sense(usbmem_device* dev, int* key, int* asc, int* ascq) {
  int k = 0, a = 0, q = 0;
  if (dev) {
    if (auto* m = dynamic_cast<MscDevice*>(dev->dev.get())) { ScsiSense s = m->last_sense(); k = s.key; a = s.asc; q = s.ascq; }
  }
  if (key) *key = k;
  if (asc) *asc = a;
  if (ascq) *ascq = q;
}

void usbmem_close(usbmem_device* h) {
  if (!h) return;
  if (h->dev) h->dev->close();
  delete h;
}

void usbmem_free(void* p) { std::free(p); }

const char* usbmem_errc_name(int c) { return errc_name(static_cast<Errc>(c)); }

}  // extern "C"
