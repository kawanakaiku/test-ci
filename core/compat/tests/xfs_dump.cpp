// SPDX-License-Identifier: GPL-2.0-or-later
// xfs_dump: XFS イメージを XfsVolume で開き、ツリーを標準出力へ書く簡易ツール (開発・独立検証用)。
// 使い方: xfs_dump [--noload] <image> [--cat <path>]
//   出力: "D <path> <mtime> <nlink>" / "F <path> <size> <mtime> <nlink> <sha256 先頭 16 桁は使わず crc>" / "L <path> <target>" / "S <path> <c|b|p|s>"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <functional>
#include <string>

#include "usbmem/xfs_volume.h"

using namespace usbmem;

class FileDev final : public BlockDevice {
 public:
  explicit FileDev(int fd) : fd_(fd) { struct stat st; fstat(fd, &st); size_ = static_cast<uint64_t>(st.st_size); }
  ~FileDev() override { if (fd_ >= 0) ::close(fd_); }
  Status read_at(uint64_t off, void* buf, size_t len, const IoOptions& = {}) override {
    if (!range_ok(size_, off, len)) return Status::Make(Errc::OutOfRange);
    uint8_t* p = static_cast<uint8_t*>(buf);
    while (len) {
      ssize_t n = ::pread(fd_, p, len, static_cast<off_t>(off));
      if (n <= 0) return Status::Make(Errc::Io, errno);
      p += n; off += static_cast<uint64_t>(n); len -= static_cast<size_t>(n);
    }
    return Status::Ok();
  }
  Status write_at(uint64_t, const void*, size_t, const IoOptions& = {}) override { return Status::Make(Errc::ReadOnly); }
  Status flush() override { return Status::Ok(); }
  Status close() override { return Status::Ok(); }
  uint64_t size() const override { return size_; }
  uint32_t logical_block_size() const override { return 512; }
  bool is_read_only() const override { return true; }
  std::string describe() const override { return "file"; }
 private:
  int fd_; uint64_t size_;
};

static uint32_t crc32(const uint8_t* p, size_t n, uint32_t c) {
  c = ~c;
  for (size_t i = 0; i < n; i++) { c ^= p[i]; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1))); }
  return ~c;
}

static int walk(XfsVolume& v, uint64_t dir, const std::string& path, int depth) {
  if (depth > 64) return 0;
  std::vector<FsDirEntry> ents;
  int rc = v.readdir(dir, &ents);
  if (rc) { std::fprintf(stderr, "readdir %s: %d\n", path.c_str(), rc); return 1; }
  int bad = 0;
  for (auto& e : ents) {
    std::string full = path.empty() ? e.name : path + "/" + e.name;
    FsStat st;
    rc = v.stat(e.ino, &st);
    if (rc) { std::fprintf(stderr, "stat %s: %d\n", full.c_str(), rc); bad++; continue; }
    switch (st.mode & 0170000) {
      case 0040000:
        std::printf("D\t%s\t%lld\t%u\n", full.c_str(), (long long)st.mtime, st.nlink);
        bad += walk(v, e.ino, full, depth + 1);
        break;
      case 0100000: {
        uint32_t c = 0; uint64_t off = 0; std::vector<uint8_t> b(1 << 20);
        while (off < st.size) {
          int64_t n = v.read(e.ino, off, b.data(), b.size());
          if (n <= 0) { std::fprintf(stderr, "read %s @%llu: %lld\n", full.c_str(), (unsigned long long)off, (long long)n); bad++; break; }
          c = crc32(b.data(), (size_t)n, c); off += (uint64_t)n;
        }
        std::printf("F\t%s\t%llu\t%lld\t%u\t%08x\n", full.c_str(), (unsigned long long)st.size, (long long)st.mtime, st.nlink, c);
        break;
      }
      case 0120000: { std::string t; rc = v.readlink(e.ino, &t); std::printf("L\t%s\t%s\n", full.c_str(), rc ? "?" : t.c_str()); if (rc) bad++; break; }
      default: std::printf("S\t%s\t%c\n", full.c_str(), (st.mode & 0170000) == 0020000 ? 'c' : (st.mode & 0170000) == 0060000 ? 'b' : (st.mode & 0170000) == 0010000 ? 'p' : 's');
    }
  }
  return bad;
}

int main(int argc, char** argv) {
  XfsMountOptions o;
  int a = 1;
  if (a < argc && !std::strcmp(argv[a], "--noload")) { o.allow_noload = true; a++; }
  if (a >= argc) { std::fprintf(stderr, "usage: xfs_dump [--noload] <image>\n"); return 2; }
  int fd = ::open(argv[a], O_RDONLY);
  if (fd < 0) { std::perror("open"); return 2; }
  BlockDevicePtr dev = std::make_shared<FileDev>(fd);
  int err = 0;
  auto v = XfsVolume::mount(dev, &err, o);
  if (!v) { std::fprintf(stderr, "mount failed: %d\n", err); return 3; }
  std::fprintf(stderr, "%s\n", v->describe_features().c_str());
  return walk(*v, v->root(), "", 0) ? 1 : 0;
}
