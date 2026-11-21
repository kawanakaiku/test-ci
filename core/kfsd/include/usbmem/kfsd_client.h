// SPDX-License-Identifier: Apache-2.0
// kfsd のクライアント (本体側)。FsVolume をプロトコル (kfsd_proto.h) で実装する。カーネル由来のコードを一切含まない。
// 使い方:
//   int err = 0;
//   auto v = usbmem::kfsd::RpcVolume::mount(usbmem::kfsd::find_exe(), dev, opts, &err);   // kfsd を起こして、dev を貸してマウントさせる
//   if (!v && err == ENOPKG) { /* kfsd (プラグイン) が見つからない: 「このファイルシステムには kfsd が必要です」と案内する */ }
// 失敗時の err は正の errno (FsVolume のドライバ直接の mount と同じ規約)。FsVolume のメソッドは 0 / 負の errno。
//   kfsd が死んだ / 応答が壊れた / タイムアウト: 以降の呼び出しは -EPIPE (または -ETIMEDOUT / -EPROTO) を返す。本体は落ちない。
#pragma once
#include <sys/types.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "usbmem/block_device.h"
#include "usbmem/fs_volume.h"
#include "usbmem/kfsd_proto.h"

namespace usbmem {
namespace kfsd {

constexpr int kErrNoKfsd = 65;   // ENOPKG: kfsd の実行ファイルが無い / 起こせない (公開版では「プラグインが必要」の合図)

struct MountOptions {
  bool rw = false;              // 書き込み可を要求
  bool allow_noload = false;    // ジャーナルの未再生を無視して読み取り専用で開く (利用者が確認した場合だけ)
  bool allow_dirty = true;      // NTFS: ダーティフラグだけのボリュームを書き込み可で開く (false なら確認待ちの読み取り専用で返す)
  int tz_minutes_west = 0;      // FAT の更新時刻の変換 (UTC より何分西か)
  int timeout_ms = 60000;       // kfsd からの 1 回の待ち (要求への応答 / 次の callback) の上限。超えたら kfsd を殺して -ETIMEDOUT。0 = 無期限
  int mount_timeout_ms = 120000;  // マウント (走査・ジャーナル再生を含む) の上限。0 = 無期限
  int dev_readahead_kb = 0;     // kfsd がデバイス (本体への callback) を連続して読むときの先読みの上限 (KiB)。往復の削減 (docs/kfsd.md 4 章)。0 = 先読みなし
  int dev_write_coalesce_kb = 0;  // kfsd がデバイスへ書く連続した小さな書き込み (4 KiB 単位) を、まとめて 1 回で送る上限 (KiB)。要求の終わりまでに必ず書き切る。0 = まとめない
  bool prefetch_stat = false;   // readdir で各エントリの stat も一緒に受け取り、続く stat() をキャッシュで答える (往復の削減。変更系の呼び出しで捨てる)
  std::vector<std::string> kfsd_args;  // kfsd へ渡す追加の引数 (例: --mem-limit-mb=1024)
};

// kfsd の実行ファイルの場所。環境変数 USBMEM_KFSD を優先し、無ければ set_exe() で渡された値。どちらも無ければ空。
std::string find_exe();
void set_exe(const std::string& path);   // Android: nativeLibraryDir の libkfsd.so など。プロセスで 1 回設定する

// 実行ファイルを起こし、socketpair の一端を *fd に返す (子はその相手を fd 3 で受け取り、--fd=3 を引数に取る)。0 / -errno。
int spawn(const std::string& exe, const std::vector<std::string>& extra_args, int* fd, pid_t* pid);

// 抽象名前空間の UNIX ソケットで kfsd を迎える方式 (Android: Java の ProcessBuilder は fd を子へ渡せないので、起こされた kfsd が `--connect=<name>` で接続してくる。wimfuse と同じ)。
//   本体: listen_abstract(name) → (kfsd を起こす: Java の ProcessBuilder / spawn_connect) → accept_peer() → RpcVolume::mount_fd()
// accept_peer は SO_PEERCRED で相手の uid を確かめる (expected_uid < 0 なら検査しない)。他のアプリが先に接続して成りすますのを避けるため、a の方式では自分の uid、b (プラグイン APK) ではプラグインの uid。
int listen_abstract(const std::string& name, int* listen_fd);
int accept_peer(int listen_fd, int timeout_ms, int expected_uid, int* fd);   // 0 / -errno (ETIMEDOUT / EACCES (uid 違い) など)
// exe を `--connect=<name>` で起こす (ホストのテスト・Android 以外用)。
int spawn_connect(const std::string& exe, const std::vector<std::string>& extra_args, const std::string& name, pid_t* pid);

class RpcVolume final : public FsVolume {
 public:
  // exe を起こしてマウント。exe が空 / 実行できない: nullptr と *err = kErrNoKfsd。それ以外の失敗は kfsd 側のマウントの errno。
  static std::unique_ptr<RpcVolume> mount(const std::string& exe, BlockDevicePtr dev, const MountOptions& opt, int* err);
  // すでに接続された fd (別 APK のプラグイン / Android のサービスが起こした kfsd) でマウント。fd を所有する。pid <= 0 なら、子として待たない。
  static std::unique_ptr<RpcVolume> mount_fd(int fd, pid_t pid, BlockDevicePtr dev, const MountOptions& opt, int* err);
  ~RpcVolume() override;

  const char* fs_name() const override { return fs_name_.c_str(); }
  uint64_t root() const override { return root_; }
  bool writable() const override;
  uint32_t state() const override;
  int lookup(uint64_t dir, const std::string& name, uint64_t* ino) override;
  int stat(uint64_t ino, FsStat* st) override;
  int readdir(uint64_t dir, std::vector<FsDirEntry>* out) override;
  int64_t read(uint64_t ino, uint64_t off, void* buf, size_t len) override;
  int readlink(uint64_t ino, std::string* out) override;
  int sync() override;
  int create(uint64_t dir, const std::string& name, uint32_t mode, uint64_t* ino) override;
  int mkdir(uint64_t dir, const std::string& name, uint32_t mode, uint64_t* ino) override;
  int unlink(uint64_t dir, const std::string& name) override;
  int rmdir(uint64_t dir, const std::string& name) override;
  int rename(uint64_t od, const std::string& on, uint64_t nd, const std::string& nn) override;
  int64_t write(uint64_t ino, uint64_t off, const void* buf, size_t len) override;
  int truncate(uint64_t ino, uint64_t size) override;
  int64_t write_inplace(uint64_t ino, uint64_t off, const void* buf, size_t len) override;
  int symlink(uint64_t dir, const std::string& name, const std::string& target, uint64_t* ino) override;
  int link(uint64_t dir, const std::string& name, uint64_t target_ino) override;

  const std::string& fs_type() const { return fs_type_; }   // probe_filesystem の結果の名前 ("ext4" / "fat32" ...)
  bool alive() const { return !dead_.load(); }
  // kfsd を強制終了する (他のスレッドから呼んでよい)。以降の呼び出しは -EPIPE。デバイス抜去・ユーザーの中止用。
  void abort();
  // 本体のデバイスへの callback の回数とバイト数 (kfsd が USB に対して行った読み書き)。
  void dev_stats(uint64_t* read_calls, uint64_t* read_bytes, uint64_t* write_calls, uint64_t* write_bytes) const;
  // kfsd の統計 (kStats)。失敗は負の errno。
  int remote_stats(StatsRep* out);
  // kfsd プロセスの終了状態 (waitpid の status。まだ終わっていない / 子として管理していないなら -1)。
  int exit_status() const { return exit_status_; }
  pid_t pid() const { return pid_; }   // kfsd の pid (テスト・診断用。子として管理していなければ -1)
  // キャッシュから stat を返せた回数 (prefetch_stat の効果の測定用)
  uint64_t stat_cache_hits() const { return stat_hits_.load(); }

 private:
  RpcVolume() = default;
  struct Reply { int64_t rc = 0; uint64_t b = 0, c = 0; uint32_t len = 0; };
  // 要求を送り、callback をさばきつつ Reply を待つ。payload は out (vector) か direct (呼び出し側のバッファ。direct_cap バイトまで) に受ける。
  // 戻り値は Reply.rc (失敗時は通信の失敗を表す負の errno。その場合 kfsd は dead)。
  int64_t call(uint32_t op, uint64_t a, uint64_t b, uint64_t c, const void* pl, size_t plen, Reply* rep, std::vector<uint8_t>* out = nullptr,
               void* direct = nullptr, size_t direct_cap = 0, int timeout_ms = -1);
  int64_t fail(int err, bool kill);
  bool handle_callback(const Hdr& h, int timeout_ms);
  int init(int fd, pid_t pid, BlockDevicePtr dev, const MountOptions& opt);
  int simple_name_op(uint32_t op, uint64_t a, uint64_t b, const std::string& name, uint64_t* ino_out);
  void reap(bool graceful);

  int fd_ = -1;
  pid_t pid_ = -1;
  BlockDevicePtr dev_;
  MountOptions opt_;
  std::string fs_name_, fs_type_;
  uint64_t root_ = 0;
  mutable std::mutex mu_;
  std::atomic<bool> dead_{false};
  uint64_t seq_ = 0;
  std::vector<uint8_t> devbuf_;
  std::unordered_map<uint64_t, FsStat> stat_cache_;
  std::atomic<uint64_t> stat_hits_{0};
  std::atomic<uint64_t> gen_{0};   // 変更系の呼び出しの通し番号 (readdir が古い stat をキャッシュに入れないため)
  std::atomic<uint64_t> dev_rc_{0}, dev_rb_{0}, dev_wc_{0}, dev_wb_{0};
  int exit_status_ = -1;
};

}  // namespace kfsd
}  // namespace usbmem
