// SPDX-License-Identifier: GPL-2.0-or-later
#include "ntfs3g_session.h"

#include <cerrno>
#include <getopt.h>
#include <vector>

extern "C" int ntfs3g_main(int argc, char* argv[]);  // src/ntfs-3g.c の main (ビルドで -Dmain=ntfs3g_main)

namespace usbmem {

namespace {
struct DevBinding {
  BlockDevicePtr dev;
  static int to_errno(const Status& s) {
    switch (s.code) {
      case Errc::Ok: return 0;
      case Errc::OutOfRange: return ERANGE;
      case Errc::ReadOnly: return EROFS;
      case Errc::Io: return s.sys_errno ? s.sys_errno : EIO;
      default: return EIO;
    }
  }
  static int rd(void* c, uint64_t off, void* buf, size_t len) { return -to_errno(static_cast<DevBinding*>(c)->dev->read_at(off, buf, len)); }
  static int wr(void* c, uint64_t off, const void* buf, size_t len) { return -to_errno(static_cast<DevBinding*>(c)->dev->write_at(off, buf, len)); }
  static int fl(void* c) { return -to_errno(static_cast<DevBinding*>(c)->dev->flush()); }
};
// セッションより長く生きる (stop() 後の ntfs_umount が書き込み / flush を呼ぶため)。プロセスで 1 回の前提なので解放しない。
DevBinding* g_binding = nullptr;
}  // namespace

std::unique_ptr<FuseShimSession> open_ntfs3g(BlockDevicePtr dev, const std::string& extra_opts, std::string* err) {
  g_binding = new DevBinding{dev};
  fuseshim_blkdev bd{};
  bd.ctx = g_binding;
  bd.read_at = &DevBinding::rd;
  bd.write_at = &DevBinding::wr;
  bd.flush = &DevBinding::fl;
  bd.size = dev->size();
  bd.block_size = dev->logical_block_size();
  bd.read_only = dev->is_read_only() ? 1 : 0;
  fuseshim_ntfs3g_io_bind(&bd);

  // ntfs-3g [-o opts] DEVICE MOUNTPOINT。DEVICE は存在するパスなら何でもよい (I/O は上の差し替えを通り、名前は使わない)。
  // no_detach: daemon(3) で fork しない (同一プロセスで fork + 親の exit が起きると致命的)。ro: 読み取り専用。
  std::string opts = "ro,no_detach,noatime";
  if (!extra_opts.empty()) opts += "," + extra_opts;
  auto args = std::make_shared<std::vector<std::string>>(std::vector<std::string>{"ntfs-3g", "-o", opts, "/dev/null", "/mnt/usbmem-ntfs3g"});
  auto main_fn = [args]() -> int {
    std::vector<char*> argv;
    for (auto& a : *args) argv.push_back(&a[0]);
    argv.push_back(nullptr);
    optind = 0;  // getopt_long の内部状態 (プロセス全体で 1 つ) を初期化する
    return ntfs3g_main(static_cast<int>(args->size()), argv.data());
  };
  int rc = 0;
  auto s = FuseShimSession::start(main_fn, "ntfs3g", &rc);
  if (!s && err) *err = "ntfs-3g の main が fuse_loop の前に終了した (終了コード " + std::to_string(rc) + ")";
  return s;
}

}  // namespace usbmem
