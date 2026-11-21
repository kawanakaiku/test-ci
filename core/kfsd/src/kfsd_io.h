// SPDX-License-Identifier: Apache-2.0
// kfsd のソケット入出力 (本体側と kfsd 側の共通)。内部用 (公開ヘッダではない)。
// 全入出力に timeout_ms (<= 0 は無期限) を付けられる。SIGPIPE は MSG_NOSIGNAL で避ける (EPIPE が errno で返る)。
#pragma once
#include <poll.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>

#include "usbmem/kfsd_proto.h"

namespace usbmem {
namespace kfsd {
namespace io {

// fd が ev になるまで待つ。1 = 準備できた / 0 = タイムアウト (errno = ETIMEDOUT) / -1 = エラー (errno)。相手が閉じた (POLLHUP) ときは 1 を返し、続く recv / send が EOF / EPIPE で知らせる。
inline int wait_fd(int fd, short ev, int timeout_ms) {
  pollfd p{fd, ev, 0};
  for (;;) {
    int r = ::poll(&p, 1, timeout_ms > 0 ? timeout_ms : -1);
    if (r < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (r == 0) { errno = ETIMEDOUT; return 0; }
    return 1;
  }
}

// Hdr + payload をまとめて送る (sendmsg で 1 回に詰め、部分書きは続ける)。成功 true。失敗は false で errno (EPIPE / ETIMEDOUT など)。
inline bool send_msg(int fd, const Hdr& h, const void* payload, size_t plen, int timeout_ms) {
  iovec iv[2] = {{const_cast<Hdr*>(&h), sizeof h}, {const_cast<void*>(payload), plen}};
  int n = plen ? 2 : 1, i = 0;
  while (i < n) {
    if (wait_fd(fd, POLLOUT, timeout_ms) <= 0) return false;
    msghdr m{};
    m.msg_iov = &iv[i];
    m.msg_iovlen = static_cast<size_t>(n - i);
    ssize_t w = ::sendmsg(fd, &m, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (w < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
      return false;
    }
    size_t left = static_cast<size_t>(w);
    while (i < n && left >= iv[i].iov_len) { left -= iv[i].iov_len; i++; }
    if (i < n && left) {
      iv[i].iov_base = static_cast<char*>(iv[i].iov_base) + left;
      iv[i].iov_len -= left;
    }
  }
  return true;
}

// ちょうど n バイト読む。1 = 成功 / 0 = 相手が閉じた (EOF) / -1 = エラー (errno。タイムアウトは ETIMEDOUT)。
inline int recv_all(int fd, void* p, size_t n, int timeout_ms) {
  char* c = static_cast<char*>(p);
  while (n) {
    int w = wait_fd(fd, POLLIN, timeout_ms);
    if (w == 0) return -1;
    if (w < 0) return -1;
    ssize_t r = ::recv(fd, c, n, MSG_DONTWAIT);
    if (r < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
      return -1;
    }
    if (r == 0) return 0;
    c += r;
    n -= static_cast<size_t>(r);
  }
  return 1;
}

// Hdr を読む。magic のような検査は無い (op と len の範囲だけ見る)。1 / 0 (EOF) / -1 (errno。長さ違反は EPROTO)。
inline int recv_hdr(int fd, Hdr* h, int timeout_ms) {
  int r = recv_all(fd, h, sizeof *h, timeout_ms);
  if (r <= 0) return r;
  if (h->len > kMaxPayload) { errno = EPROTO; return -1; }
  return 1;
}

}  // namespace io
}  // namespace kfsd
}  // namespace usbmem
