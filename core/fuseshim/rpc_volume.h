// SPDX-License-Identifier: GPL-2.0-or-later
// FsVolume を socketpair (パイプ) 越しの RPC にする (docs/fuse-layer.md 3 章: 本番の主案 = 別プロセス)。
//   rpc_serve()  FUSE プログラム (shim + ntfs-3g / wimlib) を載せた側のプロセスで、FsVolume への要求を処理する。
//   RpcVolume    本体側のプロセスで、FsVolume として振る舞い、要求を fd に流す。
// 試作は fork した子プロセスで動かして、同一プロセスの形との性能差 (読みデータの余計なコピー 2 回 + コンテキストスイッチ) を測る。
//
// メッセージ (ホストのバイト順・同じマシン前提): 要求 = Req (固定 40 バイト) + 名前 (name_len バイト)。応答 = Rep (16 バイト) + 本体 (payload_len バイト)。
//   Req {op, name_len, a, b, c}   LOOKUP(a=dir, name) / STAT(a=ino) / READDIR(a=dir) / READ(a=ino, b=off, c=len) / READLINK(a=ino) / STATS()
//   Rep {rc (>=0 成功 / 負の errno。READ は読めたバイト数), payload_len, kind}
//   (wimfuse 用の拡張) kind = 1 の Rep は「子 → 親の呼び出し」(コールバック。子が WIM ファイルの中身を親に読ませる)。rc = 操作コード、本体 = {a, b, c} の 24 バイト。
//   親は処理して kind = 0 の Rep (+ データ) で答える。子は 1 つの要求を処理している途中でだけ呼べる (親はその応答を待っているので、待ちの中でコールバックを処理する)。
//   READDIR の本体 = 繰り返し {u64 ino, u32 dtype, u32 name_len, name}。STAT の本体 = FsStat。STATS の本体 = {u64 calls, u64 bytes} (下位デバイスの読み)。
#pragma once
#include <sys/types.h>

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "usbmem/fs_volume.h"

namespace usbmem {

// 拡張の操作 (基本の 6 つ以外の op) の処理。処理したら true (*rc と *out を答えにする)、知らない操作なら false (= -ENOSYS)。op == kRpcOpQuit は rpc_serve が自分で扱う。
using RpcExtraFn = std::function<bool(uint32_t op, uint64_t a, uint64_t b, uint64_t c, const std::string& name, int64_t* rc, std::vector<uint8_t>* out)>;
constexpr uint32_t kRpcOpQuit = 0x7f;  // 応答 (rc = 0) を返してから rpc_serve を抜ける

// fd の先の要求を EOF (または kRpcOpQuit) まで処理する。stats は STATS の答え (下位デバイスの読み回数とバイト数。無ければ null)。
void rpc_serve(int fd, FsVolume& v, const std::function<void(uint64_t*, uint64_t*)>& stats, const RpcExtraFn& extra = nullptr);

// 子 → 親の呼び出し (rpc_serve の要求の処理中に、同じ fd で呼ぶ)。親の答え rc を返す (>= 0 のとき、付いてきたデータを buf へ最大 cap バイト受ける)。通信の失敗は -EPIPE。
// 複数のスレッドから呼んでよい (内部で直列化)。
int64_t rpc_callback(int fd, uint32_t op, uint64_t a, uint64_t b, uint64_t c, void* buf, size_t cap);

class RpcVolume final : public FsVolume {
 public:
  // fd: socketpair の本体側。pid: 子プロセス (デストラクタで fd を閉じて waitpid)。root: 子が最初に送る値 (ルートの ino)。
  RpcVolume(int fd, pid_t pid, uint64_t root) : fd_(fd), pid_(pid), root_(root) {}
  ~RpcVolume() override;
  const char* fs_name() const override { return "rpc"; }
  uint64_t root() const override { return root_; }
  int lookup(uint64_t dir, const std::string& name, uint64_t* ino) override;
  int stat(uint64_t ino, FsStat* st) override;
  int readdir(uint64_t dir, std::vector<FsDirEntry>* out) override;
  int64_t read(uint64_t ino, uint64_t off, void* buf, size_t len) override;
  int readlink(uint64_t ino, std::string* out) override;
  void dev_stats(uint64_t* calls, uint64_t* bytes);

  // ---- wimfuse 用の拡張 ----
  // 子からのコールバック (kind = 1) の処理。op / a / b / c を受け、データを *out に入れて rc を返す (負の errno = エラー)。call の応答待ちの中で呼ばれる。
  using CallbackFn = std::function<int64_t(uint32_t op, uint64_t a, uint64_t b, uint64_t c, std::vector<uint8_t>* out)>;
  void set_callback(CallbackFn f) { cb_ = std::move(f); }
  void set_root(uint64_t r) { root_ = r; }
  // 拡張の操作を 1 回呼ぶ (応答の rc と本体)。通信の失敗は -EPIPE。
  int64_t ext_call(uint32_t op, uint64_t a, uint64_t b, uint64_t c, const std::string& name, std::vector<uint8_t>* payload) { return call(op, a, b, c, name, payload); }

 private:
  int64_t call(uint32_t op, uint64_t a, uint64_t b, uint64_t c, const std::string& name, std::vector<uint8_t>* payload, void* direct = nullptr, size_t direct_cap = 0);
  int fd_;
  pid_t pid_;
  uint64_t root_;
  std::mutex mu_;
  CallbackFn cb_;
};

}  // namespace usbmem
