// SPDX-License-Identifier: GPL-2.0-or-later
// WimVolume: WIM ファイル 1 つを FsVolume として見せる (読み取り専用)。ルート直下に、イメージ (index) ごとのフォルダ "1 - 名前" / "2 - 名前" ... が並び、
// 各フォルダの中身は、そのイメージを別プロセス (libwimfuse.so) でマウントした RpcVolume (wimfuse_client.h) の中身。
// イメージごとに 1 プロセス (使い捨て) で、**フォルダを最初に開いたときに起こす** (ルートの一覧だけなら起こさない)。docs/fuse-layer.md 10 章 段階 B
// ここには wimlib のコードは入らない (LGPL / GPL のコードは別プロセス側だけ)。
#pragma once
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "usbmem/block_device.h"
#include "usbmem/fs_volume.h"

namespace usbmem {
namespace wimfuse {

struct WimImageName {
  int index = 0;
  std::string name;  // WIM の中のイメージ名 (空もありうる)
};

// JSON (wimfuse_main.cpp の wim_info_json) のイメージ一覧を取り出す。知っている形 (index / name の順) だけを読む簡易の取り出し。
std::vector<WimImageName> parse_image_names(const std::string& json);
// イメージのフォルダ名: "<index> - <名前>" (名前が空なら "<index>")。/ と制御文字は _ にし、長すぎれば UTF-8 の境界で切る。
std::string image_folder_name(const WimImageName& i);

class WimVolume final : public FsVolume {
 public:
  // 子プロセスを起こして、子とつながった接続済みのソケットの fd を返す (所有権はこちらへ)。失敗は負の errno。
  using SpawnFn = std::function<int()>;
  // WIM のヘッダとイメージ一覧を子に 1 回問い合わせて作る。失敗は nullptr と *err (正の errno)・*msg。
  static std::unique_ptr<WimVolume> open(BlockDevicePtr src, SpawnFn spawn, int* err, std::string* msg);

  const char* fs_name() const override { return "wim"; }
  uint64_t root() const override { return kRoot; }
  int lookup(uint64_t dir, const std::string& name, uint64_t* ino) override;
  int stat(uint64_t ino, FsStat* st) override;
  int readdir(uint64_t dir, std::vector<FsDirEntry>* out) override;
  int64_t read(uint64_t ino, uint64_t off, void* buf, size_t len) override;
  int readlink(uint64_t ino, std::string* out) override;

  const std::vector<WimImageName>& images() const { return imgs_; }
  std::string info_json() const { return info_json_; }
  // 起こした子の数 (試験・診断用)
  int spawned() const;

 private:
  WimVolume() = default;
  static constexpr uint64_t kRoot = 1;
  static constexpr int kShift = 40;                                  // ino = (イメージ番号 << 40) | 子の ino。下位が 0 = そのイメージのルート
  static constexpr uint64_t kMask = (uint64_t(1) << kShift) - 1;
  struct Kid {
    std::unique_ptr<FsVolume> vol;
    int err = 0;
    bool tried = false;
  };
  FsVolume* kid(size_t slot, int* err);   // 必要なら起こしてマウントする
  uint64_t to_comp(size_t slot, FsVolume* k, uint64_t child_ino) const;
  uint64_t to_child(FsVolume* k, uint64_t comp_ino) const;
  bool slot_of(uint64_t ino, size_t* slot) const;

  BlockDevicePtr src_;
  SpawnFn spawn_;
  std::string info_json_;
  std::vector<WimImageName> imgs_;
  std::vector<std::string> folder_;
  std::vector<Kid> kids_;
  mutable std::mutex mu_;
};

}  // namespace wimfuse
}  // namespace usbmem
