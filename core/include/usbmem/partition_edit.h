// SPDX-License-Identifier: Apache-2.0
// パーティションテーブルの検査・編集・バックアップ/復元 (要件書 4 章)。設計は docs/partition-edit.md。
//
// 安全の要点:
//  - 編集は「操作を積む → plan() で検証と差分を得る → 確認トークンを付けて apply()」の 3 段。plan() は何も書かない。
//  - 確認トークンは「計画の内容 + デバイス識別 + 計画ごとに決まる確認文字列」から作る。計画が 1 ビットでも変わると無効になる。
//  - 書き込むのはテーブル用のセクタだけ (パーティションのデータ領域には書かない)。書き込みは段階に分け、各段階の後に flush する。
//    どの時点で電源が切れても、GPT はプライマリかバックアップのどちらかが有効に残る順序で書く。
//  - 書き込み可能で開いた編集は、同じデバイスにつき 1 つだけ (排他)。マウント中 (in_use) は適用を拒否する。
#pragma once
#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "usbmem/block_device.h"
#include "usbmem/partition.h"

namespace usbmem {

// ------------------------------------------------------------------ GPT の詳細検査

enum class GptRepairKind : uint8_t {
  CopyPrimaryToBackup = 1,  // プライマリが正しい: バックアップを作り直す (末尾に置く)
  CopyBackupToPrimary,      // バックアップが正しい: プライマリを作り直す
  MoveBackupToEnd,          // ディスクが大きくなった / イメージが縮んだ: バックアップを末尾へ移し、使用可能範囲と保護 MBR を更新
  RebuildProtectiveMbr,     // 保護 MBR が無い / 範囲が違う
};
const char* gpt_repair_name(GptRepairKind k);  // "copy-primary-to-backup" など (機械可読)

struct GptCopyInfo {
  bool present = false;        // "EFI PART" の署名がある
  bool header_valid = false;   // ヘッダの CRC とフィールドが妥当
  bool entries_valid = false;  // エントリ配列の CRC まで通った
  bool truncated = false;      // ヘッダは正しいが、使用可能範囲かバックアップの位置がディスクの外 (イメージが縮んだ)。entries / entries_valid は埋まる
  std::string reason;          // ヘッダが妥当でない理由 (英語)
  uint64_t lba = 0;            // ヘッダの位置
  uint64_t alt_lba = 0, first_usable = 0, last_usable = 0, entries_lba = 0;
  uint32_t num_entries = 0, entry_size = 0, entries_crc = 0, revision = 0;
  std::array<uint8_t, 16> disk_guid{};
  std::vector<uint8_t> header_sector;  // 生のセクタ (present のとき)
  std::vector<uint8_t> entries;        // エントリ配列の生バイト (header_valid のとき)
  bool usable() const { return header_valid && entries_valid; }
};

struct GptInspection {
  uint32_t sector_size = 0;
  uint64_t disk_sectors = 0;
  GptCopyInfo primary, backup;
  bool backup_at_end = false;        // バックアップのヘッダが最終セクタにある
  bool disk_grew = false;            // バックアップが最終セクタより手前にある (ディスクが大きくなった)
  bool backup_beyond_disk = false;   // プライマリが示すバックアップの位置がディスクの外 (イメージが縮んだ)
  bool copies_agree = false;         // 両方使用可能で、GUID・範囲・エントリ配列の CRC が一致
  bool mbr_present = false;          // セクタ 0 に 0x55AA
  bool mbr_protective = false;       // 保護 MBR (0xEE) のエントリがある
  bool mbr_protective_ok = false;    // 開始 LBA 1、大きさ = min(ディスク-1, 0xFFFFFFFF)
  bool mbr_hybrid = false;           // 0xEE 以外のエントリもある
  std::vector<Diagnostic> diagnostics;
  std::vector<GptRepairKind> suggested_repairs;
  bool any_present() const { return primary.present || backup.present; }
};

// sector_size で GPT を調べる (GPT が無ければ any_present() == false)。I/O エラーだけを Status で返す。
Status inspect_gpt(BlockDevice& dev, uint32_t sector_size, GptInspection* out);

// ------------------------------------------------------------------ GUID

bool parse_guid(const std::string& text, std::array<uint8_t, 16>* raw);  // "C12A7328-F81F-..." → ディスク上の生バイト
std::array<uint8_t, 16> make_random_guid(uint64_t seed);                // seed == 0 なら OS の乱数。version 4

// ------------------------------------------------------------------ 編集

enum class PartRole : uint8_t { Auto = 0, Primary, Extended, Logical };

struct PartitionSpec {
  PartRole role = PartRole::Auto;  // MBR のみ。Auto: 主が空いていれば主、無ければ (拡張があれば) 論理
  uint64_t start = 0;              // セクタ。0 = 自動配置 (最初に入る空き)
  uint64_t count = 0;              // セクタ数。0 = 空きの最後まで
  uint8_t mbr_type = 0x83;         // MBR
  std::array<uint8_t, 16> type_guid{};    // GPT。全 0 なら Linux filesystem
  std::array<uint8_t, 16> unique_guid{};  // GPT。全 0 なら新規生成
  std::string name;                // GPT の名前 (UTF-8, 最大 36 UTF-16 単位)
  uint64_t attrs = 0;              // GPT の属性
  bool bootable = false;           // MBR の active フラグ
};

struct EditorOptions {
  uint32_t sector_size = 0;        // 0 = 自動
  uint64_t alignment_sectors = 0;  // 0 = 1 MiB 相当 (512B: 2048 / 4096B: 256)
  bool in_use = false;             // マウント中など。true の間は apply / restore を拒否する
  uint64_t rng_seed = 0;           // 0 = OS 乱数。0 以外なら GUID / シグネチャを決定的に作る (テスト用)
};

struct SectorWrite {
  uint64_t lba = 0;             // EditPlan::sector_size 単位
  std::vector<uint8_t> data;    // セクタ 1 つ分 (sector_size バイト)
  std::vector<uint8_t> before;  // 計画時点のディスク上の内容 (競合検出・自動バックアップ・ロールバック用)
  std::string what;             // 例: "gpt-primary-header"
};
struct EditStage {
  std::string name;  // 例: "gpt-backup-entries"
  std::vector<SectorWrite> writes;
};

struct EditPlan {
  bool ok = false;                     // 適用できる (Error 無し・変更あり)
  uint32_t sector_size = 512;          // writes の LBA の単位
  std::vector<Diagnostic> problems;    // Error があれば適用不可。Warning は適用可
  std::vector<std::string> changes;    // 人間向けの差分
  bool destructive = false;            // 既存パーティションの削除・縮小・移動・新規テーブルなど
  bool requires_force = false;         // 復元でディスクサイズ / セクタサイズが違う
  PartitionTable before, after;        // after は「計画の書き込みを重ねた」状態を実際にパースした結果
  std::vector<EditStage> stages;
  std::string device_id;
  std::string plan_hash;               // hex SHA-256 (device_id と全書き込み内容)
  std::string confirm_phrase;          // ユーザーに入力させる文字列 ("WRITE xxxxxx" / 強制時は "FORCE xxxxxx")
  size_t sector_count() const;
};

struct ApplyOptions {
  // 適用の直前に、書き換えるセクタの元の内容 + テーブル関連セクタをバックアップ形式で渡す。
  std::function<Status(const std::vector<uint8_t>&)> backup_sink;
  bool require_backup = true;  // true: backup_sink が無い / 失敗なら何も書かずに拒否
  bool rollback_on_error = true;  // 途中で失敗したら、書いた分を元の内容に戻そうとする (戻せなくても報告する)
  uint64_t timestamp = 0;      // バックアップに記録する時刻 (UNIX 秒。0 可)
};

struct EditResult {
  Status status;            // Ok か、InvalidArgument (拒否・検証失敗) / Io など
  std::string code;         // 機械可読 ("ok", "busy", "device-read-only", "in-use", "bad-token", "device-changed", ...)
  std::string message;      // 人間向け (英語)
  size_t sectors_written = 0;
  bool rolled_back = false;
  bool verified = false;    // 読み戻しと再パースが計画どおり
  std::vector<Diagnostic> verify_problems;
  bool ok() const { return status.ok(); }
};

class PartitionEditor {
 public:
  // dev を編集対象として開く。書き込み可 (is_read_only() == false) で開くと排他を取る (同じ dev で 2 つ目は code "busy")。
  static EditResult open(BlockDevicePtr dev, const EditorOptions& opt, std::unique_ptr<PartitionEditor>* out);
  ~PartitionEditor();
  PartitionEditor(const PartitionEditor&) = delete;
  PartitionEditor& operator=(const PartitionEditor&) = delete;

  const PartitionTable& table() const;         // 開いた時点のディスク上のテーブル (編集の影響は受けない)
  const GptInspection& gpt_inspection() const; // GPT を調べた結果 (GPT が無ければ any_present() == false)
  bool writable() const;
  void set_in_use(bool in_use);

  // ---- 操作 (作業中のテーブルを変える。無効な操作は何も変えずにエラーを返す)。handle は元の index (既存) か create の戻り値 (新規)
  EditResult create(const PartitionSpec& spec, uint32_t* handle = nullptr);
  EditResult remove(uint32_t handle);
  EditResult set_mbr_type(uint32_t handle, uint8_t type);
  EditResult set_gpt_type(uint32_t handle, const std::array<uint8_t, 16>& guid);
  EditResult set_name(uint32_t handle, const std::string& utf8);
  EditResult set_attrs(uint32_t handle, uint64_t attrs);
  EditResult set_bootable(uint32_t handle, bool bootable);
  EditResult set_unique_guid(uint32_t handle, const std::array<uint8_t, 16>& guid);
  EditResult resize(uint32_t handle, uint64_t new_count);  // テーブルのエントリだけ。ファイルシステムは変えない
  EditResult move(uint32_t handle, uint64_t new_start);    // 同上。データは動かさない
  EditResult set_disk_id(const std::string& id);           // GPT: ディスク GUID / MBR: シグネチャ (8 桁 16 進)
  // 既存のテーブルを破棄して空にする。パーティション無しのファイルシステム (スーパーフロッピー) が
  // ある場合は overwrite_filesystem = true を明示しない限り計画が拒否される (ブートセクタを壊すため)。
  EditResult new_table(TableKind kind, uint32_t gpt_entries = 128, bool overwrite_filesystem = false);
  EditResult repair_gpt(GptRepairKind kind);  // 最初の操作としてのみ受け付ける
  void clear_ops();                           // 操作を捨ててディスク上の状態に戻す
  const std::vector<std::string>& op_log() const;

  // ---- 計画と適用
  EditResult plan(EditPlan* out) const;  // 何も書かない。out->ok が適用可否
  static std::string make_confirm_token(const EditPlan& plan, const std::string& typed_phrase);
  EditResult apply(const EditPlan& plan, const std::string& confirm_token, const ApplyOptions& opt = {});

  // ---- バックアップ / 復元
  EditResult backup(uint64_t timestamp, std::vector<uint8_t>* out) const;                  // 現在のディスク上のテーブル
  EditResult plan_restore(const std::vector<uint8_t>& backup, EditPlan* out) const;        // 復元の計画 (何も書かない)
  EditResult export_sfdisk(std::string* out, const std::string& device_name = "/dev/disk") const;  // 現在のディスク上のテーブル

 private:
  struct Impl;
  explicit PartitionEditor(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

// JSON (UTF-8)。C API / JNI 用。
std::string edit_plan_to_json(const EditPlan& plan, bool include_sectors = false);
std::string gpt_inspection_to_json(const GptInspection& g);

// バックアップファイルの検査 (復元せずに中身を確認する)。壊れていれば status.code == Corrupt。
struct BackupInfo {
  uint32_t version = 0, sector_size = 0, table_kind = 0;  // table_kind: 0 none / 1 mbr / 2 gpt
  uint64_t disk_bytes = 0, timestamp = 0;
  std::string disk_id;
  size_t sector_count = 0;
};
Status parse_table_backup(const std::vector<uint8_t>& file, BackupInfo* info);

}  // namespace usbmem
