// SPDX-License-Identifier: Apache-2.0
#include "usbmem/c_api.h"

#include <cstdlib>
#include <cstring>
#include <new>

#include "usbmem/devices.h"
#include "usbmem/partition.h"

struct usbmem_device {
  usbmem::BlockDevicePtr dev;
};

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

void usbmem_close(usbmem_device* h) {
  if (!h) return;
  if (h->dev) h->dev->close();
  delete h;
}

void usbmem_free(void* p) { std::free(p); }

const char* usbmem_errc_name(int c) { return errc_name(static_cast<Errc>(c)); }

}  // extern "C"
