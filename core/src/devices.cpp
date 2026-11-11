// SPDX-License-Identifier: Apache-2.0
#include "usbmem/devices.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace usbmem {

const char* errc_name(Errc c) {
  switch (c) {
    case Errc::Ok: return "ok";
    case Errc::InvalidArgument: return "invalid-argument";
    case Errc::Io: return "io-error";
    case Errc::OutOfRange: return "out-of-range";
    case Errc::ReadOnly: return "read-only";
    case Errc::Cancelled: return "cancelled";
    case Errc::Timeout: return "timeout";
    case Errc::Disconnected: return "disconnected";
    case Errc::Corrupt: return "corrupt";
    case Errc::Unsupported: return "unsupported";
    case Errc::NoMemory: return "no-memory";
    case Errc::Closed: return "closed";
  }
  return "unknown";
}

namespace {
constexpr size_t kChunk = 1u << 20;  // キャンセル確認の粒度

bool valid_block_size(uint32_t bs) { return bs >= 512 && bs <= 65536 && (bs & (bs - 1)) == 0; }

Status errno_status(int e) {
  // ENODEV/ENXIO/EIO は USB 抜去時に出うる。Disconnected 扱いは USB 層で行うため、ここでは Io。
  return Status::Make(Errc::Io, e);
}
}  // namespace

// ---------------------------------------------------------------- RawImageDevice

Status RawImageDevice::open_path(const std::string& path, bool read_only, uint32_t bs,
                                 BlockDevicePtr* out) {
  int fd = ::open(path.c_str(), (read_only ? O_RDONLY : O_RDWR) | O_CLOEXEC);
  if (fd < 0) return errno_status(errno);
  Status st = open_fd(fd, read_only, /*take_ownership=*/true, bs, out);
  if (!st.ok()) ::close(fd);
  return st;
}

Status RawImageDevice::open_fd(int fd, bool read_only, bool own, uint32_t bs, BlockDevicePtr* out) {
  if (fd < 0 || !out || !valid_block_size(bs)) return Status::Make(Errc::InvalidArgument);
  struct stat sb;
  if (::fstat(fd, &sb) != 0) return errno_status(errno);
  uint64_t size = 0;
  if (S_ISREG(sb.st_mode)) {
    size = static_cast<uint64_t>(sb.st_size);
  } else {
    off_t end = ::lseek(fd, 0, SEEK_END);  // ブロックデバイス等
    if (end < 0) return errno_status(errno);
    size = static_cast<uint64_t>(end);
  }
  out->reset(new (std::nothrow) RawImageDevice(fd, read_only, own, size, bs));
  return *out ? Status::Ok() : Status::Make(Errc::NoMemory);
}

RawImageDevice::~RawImageDevice() { close(); }

Status RawImageDevice::read_at(uint64_t off, void* buf, size_t len, const IoOptions& opt) {
  std::shared_lock<std::shared_mutex> lk(mu_);
  if (fd_ < 0) return Status::Make(Errc::Closed);
  if (!range_ok(size_, off, len)) return Status::Make(Errc::OutOfRange);
  auto* p = static_cast<uint8_t*>(buf);
  while (len > 0) {
    if (opt.cancel && opt.cancel->cancelled()) return Status::Make(Errc::Cancelled);
    size_t n = std::min(len, kChunk);
    ssize_t r = ::pread(fd_, p, n, static_cast<off_t>(off));
    if (r < 0) {
      if (errno == EINTR) continue;
      return errno_status(errno);
    }
    if (r == 0) return Status::Make(Errc::Io, EIO);  // 想定外の EOF (ファイルが縮んだ等)
    p += r; off += static_cast<uint64_t>(r); len -= static_cast<size_t>(r);
  }
  return Status::Ok();
}

Status RawImageDevice::write_at(uint64_t off, const void* buf, size_t len, const IoOptions& opt) {
  if (read_only_) return Status::Make(Errc::ReadOnly);
  std::unique_lock<std::shared_mutex> lk(mu_);
  if (fd_ < 0) return Status::Make(Errc::Closed);
  if (!range_ok(size_, off, len)) return Status::Make(Errc::OutOfRange);
  auto* p = static_cast<const uint8_t*>(buf);
  while (len > 0) {
    if (opt.cancel && opt.cancel->cancelled()) return Status::Make(Errc::Cancelled);
    size_t n = std::min(len, kChunk);
    ssize_t r = ::pwrite(fd_, p, n, static_cast<off_t>(off));
    if (r < 0) {
      if (errno == EINTR) continue;
      return errno_status(errno);
    }
    if (r == 0) return Status::Make(Errc::Io, EIO);
    p += r; off += static_cast<uint64_t>(r); len -= static_cast<size_t>(r);
  }
  return Status::Ok();
}

Status RawImageDevice::flush() {
  if (read_only_) return Status::Ok();
  std::unique_lock<std::shared_mutex> lk(mu_);
  if (fd_ < 0) return Status::Make(Errc::Closed);
  if (::fsync(fd_) != 0 && errno != EINVAL && errno != EROFS) return errno_status(errno);
  return Status::Ok();
}

Status RawImageDevice::close() {
  std::unique_lock<std::shared_mutex> lk(mu_);
  if (fd_ < 0) return Status::Ok();
  int fd = fd_;
  fd_ = -1;
  if (own_ && ::close(fd) != 0) return errno_status(errno);
  return Status::Ok();
}

std::string RawImageDevice::describe() const {
  return std::string("raw-image(") + (read_only_ ? "ro" : "rw") + ", " + std::to_string(size_) + " bytes)";
}

// ---------------------------------------------------------------- MemoryDevice

Status MemoryDevice::read_at(uint64_t off, void* buf, size_t len, const IoOptions&) {
  std::lock_guard<std::mutex> lk(mu_);
  if (closed_) return Status::Make(Errc::Closed);
  if (!range_ok(data_.size(), off, len)) return Status::Make(Errc::OutOfRange);
  if (len) std::memcpy(buf, data_.data() + off, len);
  return Status::Ok();
}

Status MemoryDevice::write_at(uint64_t off, const void* buf, size_t len, const IoOptions&) {
  if (read_only_) return Status::Make(Errc::ReadOnly);
  std::lock_guard<std::mutex> lk(mu_);
  if (closed_) return Status::Make(Errc::Closed);
  if (!range_ok(data_.size(), off, len)) return Status::Make(Errc::OutOfRange);
  if (len) std::memcpy(data_.data() + off, buf, len);
  return Status::Ok();
}

Status MemoryDevice::close() {
  std::lock_guard<std::mutex> lk(mu_);
  closed_ = true;
  return Status::Ok();
}

// ---------------------------------------------------------------- SubRangeDevice

Status SubRangeDevice::create(BlockDevicePtr parent, uint64_t offset, uint64_t length, bool ro,
                              BlockDevicePtr* out) {
  if (!parent || !out) return Status::Make(Errc::InvalidArgument);
  if (!range_ok(parent->size(), offset, length)) return Status::Make(Errc::OutOfRange);
  out->reset(new (std::nothrow) SubRangeDevice(std::move(parent), offset, length, ro));
  if (!*out) return Status::Make(Errc::NoMemory);
  return Status::Ok();
}

Status SubRangeDevice::read_at(uint64_t off, void* buf, size_t len, const IoOptions& opt) {
  if (!range_ok(length_, off, len)) return Status::Make(Errc::OutOfRange);
  return parent_->read_at(offset_ + off, buf, len, opt);
}

Status SubRangeDevice::write_at(uint64_t off, const void* buf, size_t len, const IoOptions& opt) {
  if (read_only_ || parent_->is_read_only()) return Status::Make(Errc::ReadOnly);
  if (!range_ok(length_, off, len)) return Status::Make(Errc::OutOfRange);
  return parent_->write_at(offset_ + off, buf, len, opt);
}

std::string SubRangeDevice::describe() const {
  return "subrange(" + std::to_string(offset_) + "+" + std::to_string(length_) + " of " +
         parent_->describe() + ")";
}

}  // namespace usbmem
