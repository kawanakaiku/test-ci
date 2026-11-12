// SPDX-License-Identifier: Apache-2.0
// mkfs (ファイルシステムの新規作成): ext2 / exFAT / FAT32 のみ。NTFS は対象外。設計は docs/disk-tools.md。
//
// 安全の要点 (partition_edit.h と同じ流儀):
//  - 「計画 → 確認トークン → 適用」の 3 段。plan() は何も書かない。
//  - 確認トークンは「計画の内容 (書く領域とその中身) + デバイス識別 + 計画ごとの確認文字列」から作る。計画が 1 ビットでも変わると無効。
//    既存のファイルシステムやパーティションテーブルを壊す計画は確認文字列が "ERASE xxxxxx"、何も無い領域なら "FORMAT xxxxxx"。
//  - 書き込む領域は mkfs が必要とするメタデータだけ (ブート領域・FAT・ビットマップ・iノードテーブルなど)。データ領域は触らない。
//  - マウント中 (in_use) は適用を拒否する。書き込み可で開いたデバイスにつき同時に 1 つだけ適用できる (排他)。
//  - 書く前に、上書きする領域の元の内容 (小さい領域のみ) をバックアップとして渡す。保存できなければ何も書かない。
//  - 適用後は読み戻して計画どおりか確かめ、自前の検査 (fscheck) でも Clean を確認する。
#pragma once
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "usbmem/block_device.h"
#include "usbmem/fs_probe.h"
#include "usbmem/partition.h"  // Diagnostic

namespace usbmem {

struct MkfsOptions {
  FsType fs = FsType::Unknown;   // Ext2 / Exfat / Fat32 のみ
  uint64_t offset = 0;           // デバイス内の開始バイト (パーティションの先頭)
  uint64_t length = 0;           // バイト。0 = デバイスの終わりまで
  std::string label;             // UTF-8。ext2: 16 バイトまで / FAT32: 11 バイトまで (大文字化・ASCII の安全な文字) / exFAT: 11 文字まで
  uint32_t cluster_size = 0;     // バイト。0 = 自動。ext2 ではブロックサイズ (1024 / 2048 / 4096)
  uint32_t sector_size = 0;      // FAT32 / exFAT のセクタサイズ。0 = デバイスの論理ブロックサイズ
  uint32_t inode_ratio = 0;      // ext2: 何バイトにつき iノード 1 つか。0 = 16384
  uint32_t inode_size = 0;       // ext2: 128 / 256。0 = 256
  uint32_t reserved_percent = 5; // ext2: root 用の予約ブロックの割合 (0〜50)
  uint32_t hidden_sectors = 0xFFFFFFFFu;  // FAT32: BPB の hidden sectors。既定 = offset / sector_size
  uint64_t seed = 0;             // 0 以外ならボリュームシリアル / UUID を決定的に作る (テスト用)。0 = OS 乱数
  uint64_t timestamp = 0;        // ファイルシステムに記録する時刻 (UNIX 秒)。0 = 現在時刻
  bool in_use = false;           // マウント中など。true の間は適用を拒否する
};

// 書く領域。data が空なら length バイトのゼロ埋め。offset は range 先頭 (MkfsOptions::offset) からのバイト位置。
struct MkfsRegion {
  uint64_t offset = 0;
  uint64_t length = 0;
  std::vector<uint8_t> data;  // 空 = ゼロ埋め。そうでなければ data.size() == length
  std::string what;           // 例: "fat32-boot-sector"
};
struct MkfsStage {
  std::string name;
  std::vector<MkfsRegion> regions;
};

struct MkfsPlan {
  bool ok = false;                     // 適用できる (Error 無し)
  FsType fs = FsType::Unknown;
  uint64_t offset = 0, length = 0;     // 範囲 (デバイス内のバイト)
  std::vector<Diagnostic> problems;    // Error があれば適用不可。Warning は適用可
  std::vector<std::string> changes;    // 人間向けの説明
  std::vector<std::pair<std::string, std::string>> layout;  // 作られる構造の要約 (キー, 値)。例: ("clusterSize", "4096")
  std::string existing_fs;             // 範囲にあった既存のファイルシステム ("" = 認識できず)
  bool destructive = true;
  bool requires_force = false;         // 既存のファイルシステム / パーティションテーブルを壊す
  uint64_t bytes_to_write = 0;
  std::vector<MkfsStage> stages;
  std::string device_id;
  std::string preimage_digest;         // 小さい領域の元の内容の SHA-256 (hex)。適用時に変わっていないか確かめる
  std::string plan_hash;               // hex SHA-256
  std::string confirm_phrase;          // "FORMAT xxxxxx" / "ERASE xxxxxx"
};

struct MkfsApplyOptions {
  // 書き込み前に、上書きする領域の元の内容を渡す (形式は parse_mkfs_backup / mkfs_restore_backup が読む)
  std::function<Status(const std::vector<uint8_t>&)> backup_sink;
  bool require_backup = true;      // true: backup_sink が無い / 失敗なら何も書かずに拒否
  bool rollback_on_error = true;   // 失敗 / キャンセルで、バックアップした領域を戻そうとする
  bool in_use = false;             // 適用の直前の状態 (マウント中など)。true なら拒否する
  const CancelToken* cancel = nullptr;
  std::function<void(const char* phase, uint64_t done, uint64_t total)> progress;
  uint64_t timestamp = 0;          // バックアップに記録する時刻
};

struct MkfsResult {
  Status status;
  std::string code;       // "ok" / "busy" / "device-read-only" / "in-use" / "bad-token" / "device-changed" / "backup-required" /
                          // "plan-invalid" / "plan-tampered" / "wrong-device" / "write-failed" / "verify-failed" / "cancelled" ...
  std::string message;
  uint64_t bytes_written = 0;
  bool rolled_back = false;
  bool verified = false;            // 読み戻しが計画どおり、かつ fscheck が Clean
  std::string check_verdict;        // fscheck の結果 ("clean" など)
  std::vector<Diagnostic> verify_problems;
  bool ok() const { return status.ok(); }
};

class Mkfs {
 public:
  // 何も書かない。範囲・サイズ・ラベルの妥当性を調べ、書く内容を組み立てる。I/O エラーは result.status。
  static MkfsResult plan(BlockDevice& dev, const MkfsOptions& opt, MkfsPlan* out);
  static std::string make_confirm_token(const MkfsPlan& plan, const std::string& typed_phrase);
  static MkfsResult apply(BlockDevice& dev, const MkfsPlan& plan, const std::string& confirm_token, const MkfsApplyOptions& opt = {});
  // 作成に必要な最小サイズ (バイト)。0 = 非対応
  static uint64_t min_size(FsType fs, uint32_t sector_size = 512);
  static bool supported(FsType fs);
};

std::string mkfs_plan_to_json(const MkfsPlan& plan);
std::string mkfs_result_to_json(const MkfsResult& r);

// バックアップ (apply が backup_sink に渡す)。壊れていれば Corrupt。
struct MkfsBackupInfo {
  uint32_t version = 0;
  uint64_t timestamp = 0, range_offset = 0, range_length = 0;
  size_t regions = 0;
  uint64_t bytes = 0;
};
Status parse_mkfs_backup(const std::vector<uint8_t>& file, MkfsBackupInfo* info);
// バックアップした領域を書き戻す (書き込み可のデバイス)。ゼロ埋めだけで置き換えられた大きな領域は戻らない。
Status mkfs_restore_backup(BlockDevice& dev, const std::vector<uint8_t>& file);

}  // namespace usbmem
