// SPDX-License-Identifier: GPL-2.0-or-later
// fuseshim の C++ 側 (docs/fuse-layer.md): shim が捕まえた fuse_operations (パス単位) を FsVolume (ino 単位の読み取り API) に見せる。
//   FuseShimVolume  FsVolume の実装。lookup / stat / readdir / read / readlink を fuseshim_* に変換する。
//   FuseShimSession FUSE プログラムの main を別スレッドで走らせ、fuse_loop に入ったところ (= マウント完了) で戻る。stop() でアンマウント。
// tools/fuse/usbmem_fuse.cpp (FsVolume → FUSE) の逆向きで、操作の対応表は同じ (lookup / getattr / readdir / read / readlink / statfs)。
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "fuseshim.h"
#include "usbmem/fs_volume.h"

namespace usbmem {

class FuseShimVolume final : public FsVolume {
 public:
  // ready な shim に対して作る。ルートの ino は getattr("/") の st_ino (ntfs-3g は MFT 番号 = 5)。
  explicit FuseShimVolume(std::string fs_name);
  const char* fs_name() const override { return name_.c_str(); }
  uint64_t root() const override { return root_; }
  int lookup(uint64_t dir, const std::string& name, uint64_t* ino) override;
  int stat(uint64_t ino, FsStat* st) override;
  int readdir(uint64_t dir, std::vector<FsDirEntry>* out) override;
  int64_t read(uint64_t ino, uint64_t off, void* buf, size_t len) override;
  int readlink(uint64_t ino, std::string* out) override;
  int statfs(fuseshim_statvfs* out);  // FsVolume にはまだ無い (usbmem_fuse.cpp は近似で埋めている)
  bool ok() const { return root_ != 0; }

 private:
  bool path_of(uint64_t ino, std::string* p);
  void remember(uint64_t ino, const std::string& path);
  std::string name_;
  uint64_t root_ = 0;
  std::mutex mu_;
  // ino -> パス (最初に見えたもの)。パス単位 API (高水準 FUSE) を ino 単位に直すための表。ハードリンクはどのパスでも同じ中身なので 1 つでよい。
  // 読み取り専用では変わらない。書き込みを足すなら rename / unlink でこの表を更新する (docs/fuse-layer.md)。
  std::unordered_map<uint64_t, std::string> paths_;
};

class FuseShimSession {
 public:
  // main_fn (FUSE プログラムの main) を別スレッドで実行し、fuse_loop に入るか main_fn が戻るまで待つ。
  // 失敗 (main_fn が先に戻った) なら nullptr を返し、*exit_code に main_fn の戻り値を入れる。
  static std::unique_ptr<FuseShimSession> start(std::function<int()> main_fn, const std::string& fs_name, int* exit_code);
  ~FuseShimSession();
  FuseShimVolume* volume() { return vol_.get(); }
  // fuse_loop を返させ (= アンマウント)、main_fn の終了を待つ。main_fn の戻り値。二重に呼んでよい。
  int stop();

 private:
  FuseShimSession() = default;
  struct State;
  std::shared_ptr<State> st_;
  std::thread th_;
  std::unique_ptr<FuseShimVolume> vol_;
  bool stopped_ = false;
  int rc_ = -1;
};

}  // namespace usbmem
