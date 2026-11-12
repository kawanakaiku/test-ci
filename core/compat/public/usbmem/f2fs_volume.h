// SPDX-License-Identifier: Apache-2.0
// F2FS の読み取り専用リーダー (オンディスク形式からの独自実装。Linux の fs/f2fs は取り込まない: docs/f2fs-port.md)。
// 圧縮ファイルの展開だけ、カーネルの展開ライブラリ (usbmem_kdecomp。GPL 系) を使う。
#pragma once
#include "usbmem/fs_volume.h"

namespace usbmem {

struct F2fsMountOptions {
  // false (既定): 最後のチェックポイントのあとに書かれた更新 (fsync されたノードの roll-forward 回復が必要な状態) や、
  //   チェックポイントがエラー / fsck 要求の印を持つボリュームは EUCLEAN で拒否する。
  // true: それらを無視して読み取り専用で開く (回復は行わない。古い内容や、欠けたファイルが見えうる)。
  //   アプリはユーザーの明示的な確認の後だけ true にする。常に読み取り専用。
  bool allow_noload = false;
};

class F2fsVolume final : public FsVolume {
 public:
  // 読み取り専用でマウント。失敗時 nullptr と errno (正の値) を *err に返す。
  // EINVAL = F2FS ではない、EUCLEAN = 破損 / 要回復、EOPNOTSUPP = 未対応の機能 (暗号化 / casefold / verity / 複数デバイス / ブロックサイズ 4 KiB 以外 など)。
  static std::unique_ptr<F2fsVolume> mount(BlockDevicePtr dev, int* err = nullptr, const F2fsMountOptions& opts = {});
  ~F2fsVolume() override;
  const char* fs_name() const override { return "f2fs"; }
  uint64_t root() const override;
  int lookup(uint64_t dir, const std::string& name, uint64_t* ino) override;
  int stat(uint64_t ino, FsStat* st) override;
  int readdir(uint64_t dir, std::vector<FsDirEntry>* out) override;
  int64_t read(uint64_t ino, uint64_t off, void* buf, size_t len) override;
  int readlink(uint64_t ino, std::string* target) override;
  // allow_noload で開き、実際に回復が必要な状態だった (= 更新を無視している)。
  bool recovery_ignored() const;

 private:
  F2fsVolume() = default;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace usbmem
