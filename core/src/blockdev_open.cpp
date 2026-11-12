// SPDX-License-Identifier: Apache-2.0
// 形式の自動判別と公開 API (open_*_image / open_disk_image / open_disk_image_path)。
#include <fcntl.h>
#include <unistd.h>

#include "blockdev_internal.h"
#include "usbmem/devices.h"

namespace usbmem {

const char* image_format_name(ImageFormat f) {
  switch (f) {
    case ImageFormat::Raw: return "raw";
    case ImageFormat::Qcow2: return "qcow2";
    case ImageFormat::Vhd: return "vhd";
    case ImageFormat::Vhdx: return "vhdx";
    case ImageFormat::Vmdk: return "vmdk";
  }
  return "?";
}

namespace {
using OpenFn = Status (*)(BlockDevicePtr, const ImageOpenOptions&, uint32_t, BlockDevicePtr*, ImageInfo*);
// BlockDevice の契約は「例外を投げない」。確保失敗などの例外はここで Status にする。
Status guarded(OpenFn fn, BlockDevicePtr file, const ImageOpenOptions& o, BlockDevicePtr* out, ImageInfo* info) {
  try {
    return fn(std::move(file), o, 0, out, info);
  } catch (const std::bad_alloc&) {
    if (info) info->detail = "out of memory";
    return Status::Make(Errc::NoMemory);
  } catch (...) {
    if (info) info->detail = "internal error";
    return Status::Make(Errc::Corrupt);
  }
}

// VHD フッタか (末尾 512 B / 511 B。チェックサム検証つき)
// I/O エラーは Raw 扱いにせず、そのまま返す。
Status looks_like_vhd_footer(BlockDevice& f, bool* yes) {
  *yes = false;
  uint64_t sz = f.size();
  uint8_t b[512];
  auto check = [&](const uint8_t* p) {
    if (std::memcmp(p, "conectix", 8) != 0) return false;
    uint32_t sum = 0;
    for (size_t i = 0; i < 512; i++) if (i < 64 || i >= 68) sum += p[i];
    return ~sum == imgint::be32(p + 64);
  };
  if (sz >= 512) {
    Status st = f.read_at(sz - 512, b, 512);
    if (!st.ok()) return st;
    if (check(b)) { *yes = true; return Status::Ok(); }
  }
  if (sz >= 1024) {   // 動的/差分は先頭にもコピーがある
    Status st = f.read_at(0, b, 512);
    if (!st.ok()) return st;
    if (check(b)) { *yes = true; return Status::Ok(); }
  }
  return Status::Ok();
}
}  // namespace

Status detect_image_format(BlockDevice& file, ImageFormat* out) {
  if (!out) return Status::Make(Errc::InvalidArgument);
  uint8_t h[512] = {0};
  uint64_t sz = file.size();
  size_t n = static_cast<size_t>(std::min<uint64_t>(sz, sizeof h));
  if (n > 0) {
    Status st = file.read_at(0, h, n);
    if (!st.ok()) return st;
  }
  if (n >= 4 && imgint::be32(h) == 0x514649fbu) { *out = ImageFormat::Qcow2; return Status::Ok(); }
  if (n >= 8 && std::memcmp(h, "vhdxfile", 8) == 0) { *out = ImageFormat::Vhdx; return Status::Ok(); }
  if (n >= 4 && std::memcmp(h, "KDMV", 4) == 0) { *out = ImageFormat::Vmdk; return Status::Ok(); }
  if (n >= 21 && std::memcmp(h, "# Disk DescriptorFile", 21) == 0) { *out = ImageFormat::Vmdk; return Status::Ok(); }
  bool vhd = false;
  Status vs = looks_like_vhd_footer(file, &vhd);
  if (!vs.ok()) return vs;
  if (vhd) { *out = ImageFormat::Vhd; return Status::Ok(); }
  *out = ImageFormat::Raw;
  return Status::Ok();
}

Status open_qcow2_image(BlockDevicePtr file, const ImageOpenOptions& o, BlockDevicePtr* out, ImageInfo* info) {
  if (!file || !out) return Status::Make(Errc::InvalidArgument);
  return guarded(imgint::open_qcow2_depth, std::move(file), o, out, info);
}
Status open_vhd_image(BlockDevicePtr file, const ImageOpenOptions& o, BlockDevicePtr* out, ImageInfo* info) {
  if (!file || !out) return Status::Make(Errc::InvalidArgument);
  return guarded(imgint::open_vhd_depth, std::move(file), o, out, info);
}
Status open_vhdx_image(BlockDevicePtr file, const ImageOpenOptions& o, BlockDevicePtr* out, ImageInfo* info) {
  if (!file || !out) return Status::Make(Errc::InvalidArgument);
  return guarded(imgint::open_vhdx_depth, std::move(file), o, out, info);
}
Status open_vmdk_image(BlockDevicePtr file, const ImageOpenOptions& o, BlockDevicePtr* out, ImageInfo* info) {
  if (!file || !out) return Status::Make(Errc::InvalidArgument);
  return guarded(imgint::open_vmdk_depth, std::move(file), o, out, info);
}

namespace imgint {
Status open_disk_image_depth(BlockDevicePtr file, const ImageOpenOptions& o, uint32_t depth, BlockDevicePtr* out, ImageInfo* info) {
  ImageFormat f = ImageFormat::Raw;
  Status st = detect_image_format(*file, &f);
  if (!st.ok()) return st;
  switch (f) {
    case ImageFormat::Qcow2: return open_qcow2_depth(std::move(file), o, depth, out, info);
    case ImageFormat::Vhd: return open_vhd_depth(std::move(file), o, depth, out, info);
    case ImageFormat::Vhdx: return open_vhdx_depth(std::move(file), o, depth, out, info);
    case ImageFormat::Vmdk: return open_vmdk_depth(std::move(file), o, depth, out, info);
    case ImageFormat::Raw: break;
  }
  if (info) {
    *info = ImageInfo();
    info->format = ImageFormat::Raw;
    info->subformat = "raw";
    info->virtual_size = file->size();
    info->logical_block_size = file->logical_block_size();
  }
  *out = std::move(file);
  return Status::Ok();
}
}  // namespace imgint

Status open_disk_image(BlockDevicePtr file, const ImageOpenOptions& o, BlockDevicePtr* out, ImageInfo* info) {
  if (!file || !out) return Status::Make(Errc::InvalidArgument);
  return guarded(imgint::open_disk_image_depth, std::move(file), o, out, info);
}

Status open_disk_image_path(const std::string& path, bool allow_unsafe, BlockDevicePtr* out, ImageInfo* info) {
  if (!out) return Status::Make(Errc::InvalidArgument);
  BlockDevicePtr f;
  Status st = RawImageDevice::open_path(path, true, 512, &f);
  if (!st.ok()) return st;
  std::string dir;
  size_t slash = path.rfind('/');
  if (slash != std::string::npos) dir = path.substr(0, slash + 1);
  ImageOpenOptions o;
  o.resolver = [dir, allow_unsafe](const ImageResolveRequest& rq, BlockDevicePtr* dev) -> Status {
    std::vector<std::string> cands{rq.name};
    cands.insert(cands.end(), rq.alt_names.begin(), rq.alt_names.end());
    Status last = Status::Make(Errc::Io);
    for (const std::string& n : cands) {
      if (n.empty()) continue;
      bool unsafe = n[0] == '/' || n.find("..") != std::string::npos || (n.size() > 1 && n[1] == ':');
      if (unsafe && !allow_unsafe) { last = Status::Make(Errc::InvalidArgument); continue; }
      std::string full = (n[0] == '/') ? n : dir + n;
      Status s = RawImageDevice::open_path(full, true, 512, dev);
      if (s.ok()) return s;
      last = s;
    }
    return last;
  };
  return open_disk_image(std::move(f), o, out, info);
}

}  // namespace usbmem
