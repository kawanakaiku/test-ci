// SPDX-License-Identifier: GPL-2.0-or-later
#include "wimfuse_client.h"

#include <unistd.h>

#include <algorithm>
#include <cerrno>

#include "../rpc_volume.h"
#include "wimfuse_proto.h"

namespace usbmem {
namespace wimfuse {

namespace {
// 子の WIM ファイル読みに答える (rpc の応答待ちの中で呼ばれる)
RpcVolume::CallbackFn make_source(BlockDevicePtr src) {
  return [src](uint32_t op, uint64_t, uint64_t b, uint64_t c, std::vector<uint8_t>* out) -> int64_t {
    if (op == kCbSrcSize) return static_cast<int64_t>(src->size());
    if (op == kCbSrcRead) {
      uint64_t sz = src->size();
      if (b >= sz) return 0;
      size_t n = static_cast<size_t>(std::min<uint64_t>(c, sz - b));
      out->resize(n);
      Status st = src->read_at(b, out->data(), n);
      if (!st.ok()) return st.code == Errc::Io && st.sys_errno ? -st.sys_errno : -EIO;
      return static_cast<int64_t>(n);
    }
    return -ENOSYS;
  };
}
}  // namespace

int client_info(int fd, BlockDevicePtr src, std::string* out) {
  RpcVolume r(fd, 0, 0);
  r.set_callback(make_source(std::move(src)));
  std::vector<uint8_t> p;
  int64_t rc = r.ext_call(kOpInfo, 0, 0, 0, "", &p);
  out->assign(p.begin(), p.end());
  std::vector<uint8_t> q;
  r.ext_call(kRpcOpQuit, 0, 0, 0, "", &q);
  return static_cast<int>(rc);
}

std::unique_ptr<FsVolume> client_mount(int fd, BlockDevicePtr src, int image, int* err, std::string* msg) {
  auto r = std::make_unique<RpcVolume>(fd, 0, 0);
  r->set_callback(make_source(std::move(src)));
  std::vector<uint8_t> p;
  int64_t rc = r->ext_call(kOpMount, static_cast<uint64_t>(image), 0, 0, "", &p);
  if (rc <= 0) {
    if (err) *err = rc < 0 ? static_cast<int>(-rc) : EIO;
    if (msg) msg->assign(p.begin(), p.end());
    return nullptr;
  }
  r->set_root(static_cast<uint64_t>(rc));
  return r;
}

}  // namespace wimfuse
}  // namespace usbmem
