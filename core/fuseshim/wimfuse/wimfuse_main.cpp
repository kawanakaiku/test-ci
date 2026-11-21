// SPDX-License-Identifier: GPL-2.0-or-later
// libwimfuse.so: wimlib (LGPL-3.0+ / GPL-3.0+、無改変の libwim.a) の wimmount を載せた**別プロセス**の実行ファイル (docs/fuse-layer.md 10 章)。
// APK の nativeLibraryDir に lib*.so の名前で入り、本体 (または :wimfuse サービス) が ProcessBuilder で exec する。
//
// 通信: 本体が待ち受ける UNIX ドメインソケット (抽象名前空間) に --connect <名前> でつなぎ、../rpc_volume.h の RPC で
//   - 本体 → 子: 基本の操作 (LOOKUP / STAT / READDIR / READ / READLINK) と、拡張 (wimfuse_proto.h: INFO / MOUNT / PING / QUIT)
//   - 子 → 本体: コールバック (WIM ファイルの中身を読ませる。wimfuse_io.c が wimlib の open / pread を差し替える)
// を話す。1 プロセス = 1 ボリューム (1 イメージ) で使い捨て。本体が死ぬ (ソケットが EOF) か QUIT を受けたら終わる。
#include <errno.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../fuseshim_volume.h"
#include "../rpc_volume.h"
#include "wim_session.h"
#include "wimfuse_io.h"
#include "wimfuse_proto.h"
#include "wimlib.h"

using namespace usbmem;

namespace {
int g_fd = -1;

// ---- WIM ファイル (本体の BlockDevice) の読み。wimfuse_io.c から呼ばれる ----
constexpr size_t kMaxCb = 4u << 20;  // 1 回のコールバックで受ける上限 (本体の応答の payload_len は 32 ビット)

std::mutex g_size_mu;
int64_t g_size = -1;
}  // namespace

extern "C" int64_t wimfuse_src_size(void) {
  std::lock_guard<std::mutex> l(g_size_mu);
  if (g_size < 0) g_size = rpc_callback(g_fd, wimfuse::kCbSrcSize, 0, 0, 0, nullptr, 0);
  return g_size;
}

extern "C" int64_t wimfuse_src_read(void* buf, size_t len, uint64_t off) {
  size_t done = 0;
  while (done < len) {
    size_t n = std::min(len - done, kMaxCb);
    int64_t r = rpc_callback(g_fd, wimfuse::kCbSrcRead, 0, off + done, n, static_cast<char*>(buf) + done, n);
    if (r < 0) return done ? static_cast<int64_t>(done) : r;
    if (r == 0) break;
    done += static_cast<size_t>(r);
    if (static_cast<size_t>(r) < n) break;  // EOF
  }
  return static_cast<int64_t>(done);
}

namespace {

// マウント前は ENODEV を返し、マウント後は FuseShimVolume に転送する FsVolume (rpc_serve は FsVolume を 1 つ受ける)
class LateVolume final : public FsVolume {
 public:
  FuseShimVolume* t = nullptr;
  const char* fs_name() const override { return "wim"; }
  uint64_t root() const override { return t ? t->root() : 0; }
  int lookup(uint64_t d, const std::string& n, uint64_t* i) override { return t ? t->lookup(d, n, i) : -ENODEV; }
  int stat(uint64_t i, FsStat* s) override { return t ? t->stat(i, s) : -ENODEV; }
  int readdir(uint64_t d, std::vector<FsDirEntry>* o) override { return t ? t->readdir(d, o) : -ENODEV; }
  int64_t read(uint64_t i, uint64_t off, void* b, size_t n) override { return t ? t->read(i, off, b, n) : -ENODEV; }
  int readlink(uint64_t i, std::string* o) override { return t ? t->readlink(i, o) : -ENODEV; }
};

void json_str(std::string& o, const char* s) {
  o += '"';
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s ? s : ""); *p; ++p) {
    if (*p == '"' || *p == '\\') { o += '\\'; o += static_cast<char>(*p); }
    else if (*p < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", *p); o += b; }
    else o += static_cast<char>(*p);
  }
  o += '"';
}

// WIM のヘッダ・イメージ一覧の JSON。{"image_count":N,"boot_index":N,"compression":"LZX","chunk_size":N,"total_bytes":N,"version":N,"pipable":0,"split":0,
//   "images":[{"index":1,"name":"..","description":"..","dirs":N,"files":N,"bytes":N,"product":"..","build":".."},...]}
int wim_info_json(std::string* out, std::string* err) {
  WIMStruct* wim = nullptr;
  int rc = wimlib_open_wim(WIMFUSE_SRC_PATH, 0, &wim);
  if (rc) { *err = std::string("wimlib_open_wim: ") + wimlib_get_error_string(static_cast<wimlib_error_code>(rc)); return -EIO; }
  wimlib_wim_info info;
  std::memset(&info, 0, sizeof info);
  wimlib_get_wim_info(wim, &info);
  std::string o = "{";
  char b[256];
  snprintf(b, sizeof b, "\"image_count\":%u,\"boot_index\":%u,\"version\":%u,\"chunk_size\":%u,\"total_bytes\":%llu,\"pipable\":%u,\"split\":%u,\"compression\":",
           info.image_count, info.boot_index, info.wim_version, info.chunk_size, static_cast<unsigned long long>(info.total_bytes), info.pipable, info.total_parts > 1 ? 1u : 0u);
  o += b;
  json_str(o, wimlib_get_compression_type_string(static_cast<wimlib_compression_type>(info.compression_type)));
  o += ",\"images\":[";
  for (uint32_t i = 1; i <= info.image_count; i++) {
    if (i > 1) o += ',';
    snprintf(b, sizeof b, "{\"index\":%u,\"name\":", i);
    o += b;
    json_str(o, wimlib_get_image_name(wim, static_cast<int>(i)));
    o += ",\"description\":";
    json_str(o, wimlib_get_image_description(wim, static_cast<int>(i)));
    auto num = [&](const char* key, const char* prop) {
      const wimlib_tchar* v = wimlib_get_image_property(wim, static_cast<int>(i), prop);
      o += ",\""; o += key; o += "\":";
      o += (v && *v) ? v : "0";
    };
    num("dirs", "DIRCOUNT");
    num("files", "FILECOUNT");
    num("bytes", "TOTALBYTES");
    o += ",\"product\":";
    json_str(o, wimlib_get_image_property(wim, static_cast<int>(i), "WINDOWS/PRODUCTNAME"));
    o += ",\"build\":";
    json_str(o, wimlib_get_image_property(wim, static_cast<int>(i), "WINDOWS/VERSION/BUILD"));
    o += '}';
  }
  o += "]}";
  wimlib_free(wim);
  *out = std::move(o);
  return 0;
}

int connect_abstract(const char* name) {
  int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;
  sockaddr_un a;
  std::memset(&a, 0, sizeof a);
  a.sun_family = AF_UNIX;
  size_t n = std::strlen(name);
  if (n + 1 > sizeof a.sun_path) { ::close(fd); errno = ENAMETOOLONG; return -1; }
  std::memcpy(a.sun_path + 1, name, n);  // sun_path[0] = '\0' = 抽象名前空間
  socklen_t len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + n);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&a), len) != 0) { int e = errno; ::close(fd); errno = e; return -1; }
  return fd;
}

}  // namespace

int main(int argc, char** argv) {
  const char* connect_name = nullptr;
  bool pdeathsig = false;
  for (int i = 1; i < argc; i++) {
    if (!std::strcmp(argv[i], "--version")) {
      // exec できることの最小の確認 (WIM もソケットも要らない)
      std::printf("wimfuse wimlib %s pid %d\n", wimlib_get_version_string(), static_cast<int>(getpid()));
      return 0;
    } else if (!std::strcmp(argv[i], "--connect") && i + 1 < argc) {
      connect_name = argv[++i];
    } else if (!std::strcmp(argv[i], "--pdeathsig")) {
      pdeathsig = true;
    } else {
      std::fprintf(stderr, "usage: libwimfuse.so [--version] --connect <abstract-socket-name> [--pdeathsig]\n");
      return 2;
    }
  }
  if (!connect_name) { std::fprintf(stderr, "wimfuse: --connect が要る\n"); return 2; }
  if (pdeathsig) {
    // 親 (起こしたスレッド) が死んだら SIGKILL。主たる終了の条件は RPC の EOF。fork から prctl までに親が死んでいた場合に備えて getppid も見る
    pid_t pp = getppid();
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (getppid() != pp) return 3;
  }
  signal(SIGPIPE, SIG_IGN);  // 本体が死んだあとの write は EPIPE で返す
  g_fd = connect_abstract(connect_name);
  if (g_fd < 0) { std::fprintf(stderr, "wimfuse: connect(@%s) 失敗: %s\n", connect_name, std::strerror(errno)); return 4; }
  wimlib_global_init(WIMLIB_INIT_FLAG_ASSUME_UTF8);
  wimlib_set_print_errors(true);  // 警告 / エラーは stderr (サービスが logcat へ転送する)

  LateVolume late;
  std::unique_ptr<FuseShimSession> session;
  RpcExtraFn extra = [&](uint32_t op, uint64_t a, uint64_t, uint64_t, const std::string&, int64_t* rc, std::vector<uint8_t>* out) -> bool {
    std::string text, err;
    switch (op) {
      case wimfuse::kOpPing:
        *rc = getpid();
        return true;
      case wimfuse::kOpInfo: {
        int r = wim_info_json(&text, &err);
        *rc = r;
        const std::string& s = r ? err : text;
        out->assign(s.begin(), s.end());
        return true;
      }
      case wimfuse::kOpMount: {
        if (session) { *rc = -EBUSY; return true; }
        session = open_wim(WIMFUSE_SRC_PATH, static_cast<int>(a), &err);
        if (!session) {
          *rc = -EIO;
          out->assign(err.begin(), err.end());
          return true;
        }
        late.t = session->volume();
        *rc = static_cast<int64_t>(late.root());
        return true;
      }
      default:
        return false;
    }
  };
  rpc_serve(g_fd, late, nullptr, extra);

  // 本体が閉じた (EOF) か QUIT: 後始末して終わる。万一アンマウントが詰まっても残らないよう、時限でシグナル終了させる
  alarm(10);
  late.t = nullptr;
  session.reset();
  ::close(g_fd);
  return 0;
}
