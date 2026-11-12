// SPDX-License-Identifier: Apache-2.0
// バックアップ (Partclone 方式)。ファイルシステムの割り当て情報 (ビットマップ) から「使用中のブロックだけ」を
// 読み取り専用で取り出し、Partclone のイメージ形式 (image format 0002) で書き出す。復元は 計画 → 確認トークン → 適用 の 3 段で、
// 画像の検査・書き込み・読み戻し検証を行う。対応: ext2/3/4, exFAT, FAT12/16/32, NTFS。設計は docs/recovery-backup.md。
//
// 安全の原則:
//  - バックアップは読み取り元に一切書き込まない (read_at だけを使う)。
//  - 割り当て情報が信用できない (ext の journal 未再生・NTFS の dirty) ときは、既定で拒否する。許可すると「全ブロックを使用中」として
//    完全に複製する (大きいが正しい)。
//  - 復元は書き込み先を上書きする。計画で内容を確認し、確認文字列を打ち込んだ人だけが適用できる。
#pragma once
#include <array>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "usbmem/block_device.h"

namespace usbmem {

// ---- 逐次入出力 (ファイル / メモリ / 呼び出し側のコールバック / 圧縮の層) ----------------------------------------
class ByteSink {
 public:
  virtual ~ByteSink() = default;
  virtual Status write(const void* data, size_t len) = 0;
  virtual Status finish() { return Status::Ok(); }  // 書き終わり (fsync / 圧縮の終端など)
};
class ByteSource {
 public:
  virtual ~ByteSource() = default;
  // 最大 len バイト読む。*got < len は終端 (EOF) のときだけ。
  virtual Status read(void* buf, size_t len, size_t* got) = 0;
};
using SourceFactory = std::function<std::unique_ptr<ByteSource>()>;  // 画像を何度でも先頭から開き直せるようにする

class MemorySink final : public ByteSink {
 public:
  Status write(const void* d, size_t n) override { const uint8_t* p = static_cast<const uint8_t*>(d); data.insert(data.end(), p, p + n); return Status::Ok(); }
  std::vector<uint8_t> data;
};
class MemorySource final : public ByteSource {
 public:
  explicit MemorySource(const std::vector<uint8_t>* d) : d_(d) {}
  Status read(void* buf, size_t len, size_t* got) override;
 private:
  const std::vector<uint8_t>* d_;
  size_t pos_ = 0;
};
class FileSink final : public ByteSink {  // stdio。fsync して閉じる
 public:
  static Status create(const std::string& path, std::unique_ptr<FileSink>* out);
  ~FileSink() override;
  Status write(const void* d, size_t n) override;
  Status finish() override;
 private:
  FileSink() = default;
  FILE* f_ = nullptr;
};
class FileSource final : public ByteSource {
 public:
  static Status open(const std::string& path, std::unique_ptr<FileSource>* out);
  ~FileSource() override;
  Status read(void* buf, size_t len, size_t* got) override;
 private:
  FileSource() = default;
  FILE* f_ = nullptr;
};

// LZ4 フレーム形式 (lz4 コマンドと互換) の圧縮・展開の層。Partclone の流儀 (partclone ... | lz4) と同じ並び。
std::unique_ptr<ByteSink> make_lz4_sink(ByteSink* inner);        // inner は呼び出し側が所有。finish() で終端を書く
std::unique_ptr<ByteSource> make_lz4_source(ByteSource* inner);  // inner は呼び出し側が所有
// 先頭が LZ4 フレームの magic (0x184D2204) か
bool looks_like_lz4(const uint8_t* head4);

// ---- 使用中ブロックの地図 -----------------------------------------------------------------------------------------
struct UsedMap {
  std::string fs;                 // Partclone の識別子: "EXTFS" "EXFAT" "FAT12" "FAT16" "FAT32" "NTFS"
  uint32_t block_size = 0;        // Partclone のブロック (ext: FS ブロック / FAT・exFAT: セクタ / NTFS: クラスタ)
  uint64_t total_blocks = 0;
  uint64_t device_size = 0;       // バイト
  uint64_t used_blocks = 0;
  std::vector<uint8_t> bits;      // total_blocks ビット (LSB が先頭ブロック)
  std::vector<std::string> warnings;
  bool full_copy = false;         // 信用できないため全ブロックを使用中にした
  bool test(uint64_t b) const { return b < total_blocks && ((bits[b >> 3] >> (b & 7)) & 1); }
  void set(uint64_t b) { bits[b >> 3] |= static_cast<uint8_t>(1u << (b & 7)); }
  void set_range(uint64_t first, uint64_t count);
  void count_used();
};

struct ExtractOptions {
  bool full_if_unclean = false;   // 割り当て情報が信用できないとき、全ブロックを使用中として複製する
  uint64_t max_bitmap_bytes = 1ull << 30;
};
struct FsStatus {                 // 失敗の詳細つきの Status
  Status st;
  std::string code;               // "ok" "unsupported-fs" "fs-not-clean" "bad-superblock" "bitmap-too-large" "read-error" ...
  std::string message;
  bool ok() const { return st.ok(); }
};
// dev の先頭を FS の先頭として、割り当て情報から地図を作る (読み取りのみ)。
FsStatus build_used_map(BlockDevice& dev, const ExtractOptions& opt, UsedMap* out);

// ---- Partclone イメージ (format 0002) -----------------------------------------------------------------------------
struct ImageInfo {
  std::string ptc_version, fs;
  std::string version;                  // "0002"
  uint32_t block_size = 0;
  uint64_t device_size = 0, total_blocks = 0, used_blocks = 0, used_bitmap = 0;
  uint32_t blocks_per_checksum = 0;
  uint16_t checksum_mode = 0, checksum_size = 0;
  bool reseed = false;
  uint8_t bitmap_mode = 0;
  uint64_t header_bytes = 0;            // 画像の先頭からビットマップまでのバイト数
  uint64_t bitmap_bytes = 0;
  std::string header_sha256;            // ヘッダ全体の SHA-256
  std::string bitmap_sha256;
  uint64_t first_used = 0, last_used = 0;  // 使用中ブロックの最初と最後の番号 (used_blocks == 0 のとき 0)
  uint64_t counted_used = 0;            // ビットマップを数えた値
};

struct BackupOptions {
  ExtractOptions extract;
  const CancelToken* cancel = nullptr;
  std::function<void(uint64_t done_blocks, uint64_t total_used)> progress;
  uint64_t read_chunk_bytes = 1u << 20;
};
struct BackupResult {
  FsStatus status;
  UsedMap map;                          // bits は空にして返す (大きいため)
  std::string image_sha256;             // 書き出した (圧縮前の) 画像全体の SHA-256
  uint64_t image_bytes = 0;             // 圧縮前の画像の大きさ
};
// dev (FS の先頭から始まるデバイス) を sink に Partclone 形式で書く。途中で失敗したら sink は不完全 (呼び出し側で捨てる)。
BackupResult backup_filesystem(BlockDevice& dev, ByteSink& sink, const BackupOptions& opt = {});

// 画像の先頭 (ヘッダ + ビットマップ) を読み、検査する。bits (NULL 可) にビットマップを返す。画像の途中までしか読まない。
FsStatus read_image_info(ByteSource& src, ImageInfo* info, std::vector<uint8_t>* bits, uint64_t max_bitmap_bytes = 1ull << 30);

struct VerifyResult {
  FsStatus status;
  ImageInfo info;
  uint64_t blocks_checked = 0, checksums_checked = 0;
  std::string image_sha256;             // 画像全体 (展開後) の SHA-256
  uint64_t image_bytes = 0;
};
// 画像全体を読み、すべてのチェックサムと大きさを確かめる (復元しない)。
VerifyResult verify_image(ByteSource& src, const CancelToken* cancel = nullptr, const std::function<void(uint64_t, uint64_t)>& progress = {});

// ---- 復元: 計画 → 確認トークン → 適用 -------------------------------------------------------------------------------
struct RestorePlanOptions {
  bool target_in_use = false;           // 書き込み先がマウント中・使用中と呼び出し側が知っている
  bool verify_all = true;               // 計画の時点で画像全体を検査する (画像の SHA-256 を計画に束縛する)
  bool write_ntfs_backup_boot = true;   // NTFS: 最後のセクタに予備ブートセクタを書く
  const CancelToken* cancel = nullptr;
};
struct RestorePlan {
  bool ok = false;
  std::string code;                     // "ok" "target-read-only" "target-in-use" "target-too-small" "image-corrupt" ...
  std::string message;
  ImageInfo image;
  std::vector<std::string> warnings;
  uint64_t target_size = 0;
  std::string target_id;                // 書き込み先の describe()
  std::string target_existing_fs;       // 上書きされる側に見つかった FS (なければ空)
  uint64_t blocks_to_write = 0, bytes_to_write = 0;
  bool ntfs_backup_boot = false;
  bool opt_ntfs_backup_boot = true;     // 計画を作ったときの指定 (適用時の再計画に使う)
  bool verified = false;                // verify_all を実行した
  std::string image_sha256;             // verified のとき
  std::string plan_hash;                // 計画の指紋
  std::string confirm_phrase;           // 利用者に打たせる文字列: "RESTORE xxxxxx"
};
RestorePlan plan_restore(const SourceFactory& image, BlockDevice& target, const RestorePlanOptions& opt = {});
std::string make_restore_token(const RestorePlan& plan, const std::string& typed_phrase);

struct RestoreOptions {
  const CancelToken* cancel = nullptr;
  bool target_in_use = false;
  std::function<void(const char* phase, uint64_t done_blocks, uint64_t total_blocks)> progress;
  uint64_t write_chunk_bytes = 1u << 20;
};
struct RestoreResult {
  FsStatus status;
  uint64_t blocks_written = 0;
  bool verified = false;                // 書き込んだ内容を読み戻して、画像のチェックサムと一致した
  bool target_touched = false;          // 書き込み先に 1 バイトでも書いた (失敗時に利用者へ伝える)
};
RestoreResult apply_restore(const SourceFactory& image, BlockDevice& target, const RestorePlan& plan, const std::string& token, const RestoreOptions& opt = {});

}  // namespace usbmem
