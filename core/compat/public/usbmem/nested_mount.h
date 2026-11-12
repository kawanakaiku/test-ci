// SPDX-License-Identifier: GPL-2.0-or-later
// ネスト (ループバック) マウント: 親ボリューム (FsVolume) の中にある 1 つのファイル (ディスクイメージ / ISO など) を BlockDevice として開き、
// 既存の probe_filesystem → マウントの経路にそのまま流す。docs/nested-mount.md
//
// 設計の要点:
//  - SAF の fd は経由しない。親の FsVolume を直接読む (SAF プロバイダの単一 IO スレッドとのデッドロックを避ける)。
//  - ボリューム単位のネイティブ mutex (LockedFsVolume)。JNI からの呼び出しと、子ボリュームからの読み出し (VolumeFileDevice) が
//    同じ mutex で直列化される。ロック順は常に「子 → 親」 (子のドライバが親を読む向きだけ)。
//  - 子は親の shared_ptr を保持する。Kotlin 側が親を先に閉じても、ネイティブのメモリは壊れない (ただし閉じる順は 内 → 外 にする)。
//  - 子は常に読み取り専用。
#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "usbmem/fs_volume.h"

namespace usbmem {

constexpr int kMaxNestDepth = 3;  // 最上位 (USB / SAF fd) のボリュームを 0 として、子は 1..3 まで

struct NestedLink { const void* volume; uint64_t ino; };  // 「この volume の ino のファイルが裏打ち」
struct NestedLineage {
  int depth = 0;                    // このボリュームのネスト段数 (最上位 = 0)
  std::vector<NestedLink> chain;    // 祖先をたどった裏打ちファイルの列 (最上位に近い順)
};

// FsVolume を mutex で直列化する装飾。同じスレッドが再入すると (= ネストの循環) デッドロックせず -ELOOP を返す。
class LockedFsVolume final : public FsVolume {
 public:
  explicit LockedFsVolume(std::unique_ptr<FsVolume> inner) : inner_(std::move(inner)) {}
  const char* fs_name() const override { return inner_->fs_name(); }
  uint64_t root() const override { return inner_->root(); }
  bool writable() const override { return inner_->writable(); }
  int lookup(uint64_t dir, const std::string& name, uint64_t* ino) override;
  int stat(uint64_t ino, FsStat* st) override;
  int readdir(uint64_t dir, std::vector<FsDirEntry>* out) override;
  int64_t read(uint64_t ino, uint64_t off, void* buf, size_t len) override;
  int sync() override;
  int create(uint64_t dir, const std::string& name, uint32_t mode, uint64_t* ino) override;
  int mkdir(uint64_t dir, const std::string& name, uint32_t mode, uint64_t* ino) override;
  int unlink(uint64_t dir, const std::string& name) override;
  int rmdir(uint64_t dir, const std::string& name) override;
  int rename(uint64_t od, const std::string& on, uint64_t nd, const std::string& nn) override;
  int64_t write(uint64_t ino, uint64_t off, const void* buf, size_t len) override;
  int truncate(uint64_t ino, uint64_t size) override;
  int64_t write_inplace(uint64_t ino, uint64_t off, const void* buf, size_t len) override;
  int readlink(uint64_t ino, std::string* target) override;
  int symlink(uint64_t dir, const std::string& name, const std::string& target, uint64_t* ino) override;
  int link(uint64_t dir, const std::string& name, uint64_t ino) override;
  FsVolume* inner() const { return inner_.get(); }

 private:
  friend struct LockedGuard;
  std::unique_ptr<FsVolume> inner_;
  std::mutex mu_;
  std::atomic<std::thread::id> owner_{};
};

inline std::shared_ptr<FsVolume> make_locked(std::unique_ptr<FsVolume> v) {
  if (!v) return nullptr;
  return std::make_shared<LockedFsVolume>(std::move(v));
}

// 親ボリューム内の 1 ファイルを読み取り専用の BlockDevice として見せる。
// 親が書き込み可能 (writable()) なら、読むたびに親ファイルのサイズ/更新時刻が開いたときと同じかを確かめ、変わっていれば Io (ESTALE) を返す
// (マウント中のイメージを書き換えられて、子が壊れたデータを読まないため)。
class VolumeFileDevice final : public BlockDevice {
 public:
  // 0 または正の errno。
  static int open(std::shared_ptr<FsVolume> parent, uint64_t ino, uint32_t block_size, std::shared_ptr<VolumeFileDevice>* out);
  Status read_at(uint64_t offset, void* buf, size_t len, const IoOptions& opt = {}) override;
  Status write_at(uint64_t, const void*, size_t, const IoOptions& = {}) override { return Status::Make(Errc::ReadOnly); }
  Status flush() override { return Status::Ok(); }
  Status close() override { closed_.store(true); return Status::Ok(); }
  uint64_t size() const override { return size_; }
  uint32_t logical_block_size() const override { return block_size_; }
  bool is_read_only() const override { return true; }
  std::string describe() const override;
  uint64_t ino() const { return ino_; }

 private:
  VolumeFileDevice() = default;
  std::shared_ptr<FsVolume> parent_;
  uint64_t ino_ = 0, size_ = 0;
  int64_t mtime_ = 0;
  uint32_t block_size_ = 512;
  bool check_stale_ = false;
  std::atomic<bool> closed_{false};
};

struct NestedOpen {
  int err = 0;                                  // 0 = 成功。それ以外は正の errno
  std::string why;                              // 失敗の理由 (英語。ログ用)
  BlockDevicePtr dev;                           // 成功時: 子の BlockDevice (読み取り専用)
  std::shared_ptr<const NestedLineage> lineage; // 成功時: 子ボリュームに持たせる系譜 (この dev の上に載るボリュームのもの)
};

// parent の path にあるファイルを BlockDevice として開く。parent_lineage = 親ボリューム自身の系譜 (最上位なら nullptr)。
//  ELOOP(40): 段数が max_depth を超える / 同じファイルを祖先がすでに裏打ちにしている (循環)。
//  EISDIR(21) ディレクトリ、EINVAL(22) 通常ファイルでない・小さすぎる (512 バイト未満)。ENOENT など: 親の errno。
NestedOpen open_nested_image(const std::shared_ptr<FsVolume>& parent, const std::shared_ptr<const NestedLineage>& parent_lineage,
                             const std::string& path, int max_depth = kMaxNestDepth);

}  // namespace usbmem
