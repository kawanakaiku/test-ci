// SPDX-License-Identifier: Apache-2.0
#include "usbmem/c_api.h"
#include "usbmem/c_api_internal.h"
#include "usbmem/usb_msc.h"
#include "usbmem/usbdevfs_transport.h"

#include <cstdlib>
#include <cstring>
#include <new>

#include "usbmem/devices.h"
#include "usbmem/partition.h"


using namespace usbmem;

namespace {
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
