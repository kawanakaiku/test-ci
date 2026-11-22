// SPDX-License-Identifier: Apache-2.0
// ブートセクタの検査と修復 (TestDisk の Boot 相当)。設計は docs/partition-scan.md。
//
// 検査 (inspect_boot) は読み取り専用で、書き込まない。修復は「計画 → 確認トークン → 適用」の 3 段 (PartitionEditor / mkfs と同じ流儀):
//  - 計画 (plan_boot_repair) は何も書かない。書く場所と中身・今の内容 (before)・確認文字列 ("REPAIR xxxxxx") を返す。
//  - 適用 (apply_boot_repair) は、確認トークン・マウント中でないこと・書き込み可・計画後にデバイスが変わっていないこと・書き込み前のバックアップ (保存できなければ書かない) を確かめてから書く。
//    書いた後は読み戻して計画どおりか確かめ、もう一度検査して問題が消えたことを確認する。途中で失敗したら、書いた分を元の内容に戻す。
//  - 書くのはブートセクタ / ブート領域 / スーパーブロック (+ ext のグループ記述子) だけで、ファイルシステムのデータには触れない。
//  - NTFS の ntfsfix などの自動修復は使わない。バックアップのブートセクタを書き戻すだけ。
// 対応: FAT32 (バックアップブートセクタ = セクタ 6 と FSInfo)、FAT12/16 (バックアップなし: 検査と BPB の再構築の提案だけ。書かない)、
//       NTFS (最終セクタのバックアップ)、exFAT (バックアップのブート領域 12〜23 とチェックサム)、ext2/3/4 (予備スーパーブロック。e2fsck -b 相当の位置)。
#pragma once
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "usbmem/block_device.h"
#include "usbmem/fs_probe.h"
#include "usbmem/partition.h"  // Severity / Diagnostic

namespace usbmem {

enum class BootRepairKind : uint8_t {
  FatRestorePrimaryFromBackup = 1,  // FAT32: セクタ 6〜8 (バックアップ) → セクタ 0〜2
  FatRefreshBackupFromPrimary,      // FAT32: セクタ 0〜2 → セクタ 6〜8
  FatRebuildFsInfo,                 // FAT32: FSInfo の署名を直し、空き数・次の空きを「不明」(0xFFFFFFFF) にする
  NtfsRestorePrimaryFromBackup,     // NTFS: 最終セクタ → セクタ 0
  NtfsRefreshBackupFromPrimary,     // NTFS: セクタ 0 → 最終セクタ
  ExfatRestoreMainFromBackup,       // exFAT: セクタ 12〜23 → 0〜11
  ExfatRefreshBackupFromMain,       // exFAT: セクタ 0〜11 → 12〜23
  ExfatRecomputeChecksums,          // exFAT: チェックサムのセクタ (11 / 23) だけ計算し直す (セクタ 0〜10 が正しいとき)
  ExtRestoreSuperblock,             // ext: 予備スーパーブロック → 先頭のスーパーブロック (必要ならグループ記述子も)
  ExtRestoreGdt,                    // ext: 予備のグループ記述子 → 先頭のグループ記述子 (スーパーブロックは無事なとき)
};
const char* boot_repair_name(BootRepairKind k);  // "fat32-restore-primary-from-backup" など (機械可読)

struct BootFinding {
  Severity severity = Severity::Info;
  std::string code;     // 安定した機械可読 ID (例: "fat32-boot-invalid")
  std::string message;  // 人間向け (英語)
};

struct BootRepairOption {
  BootRepairKind kind = BootRepairKind::FatRestorePrimaryFromBackup;
  std::string id;           // boot_repair_name(kind)
  std::string description;  // 人間向け (英語)
  uint64_t source = 0;      // ext: 使う予備スーパーブロックのブロック番号。他: 0
};

struct BootInspection {
  FsType fs = FsType::Unknown;
  bool recognized = false;
  bool boot_ok = false;               // 先頭のブート構造が正しい
  bool has_backup_structure = false;  // この FS にはバックアップがある (FAT32 / NTFS / exFAT / ext)。FAT12/16 は false
  bool backup_ok = false;             // バックアップが正しい
  std::vector<BootFinding> findings;
  std::vector<BootRepairOption> repairs;   // 今の状態で計画できる修復
  std::vector<std::pair<std::string, std::string>> details;        // 読み取れた値 (例: "bytesPerSector" = "512")
  std::vector<std::pair<std::string, std::string>> proposed_bpb;   // FAT12/16 で BPB が壊れているときの再構築の提案 (書かない)
  std::vector<uint64_t> ext_backup_blocks;  // 有効な予備スーパーブロックのブロック番号 (e2fsck -b の引数に使える)
  std::string verdict;  // "ok" / "warnings" / "repairable" (壊れていて直せる) / "damaged" (壊れていて直せない) / "unrecognized"
  size_t error_count() const;
};

struct BootInspectOptions {
  const CancelToken* cancel = nullptr;
};

// dev の [offset, offset+length) を fs として検査する (fs == Unknown なら判別する。先頭が壊れていてもバックアップから判別を試みる)。length 0 = デバイスの終わりまで。
// I/O エラーと Cancelled だけを Status で返す。壊れた構造は out の findings に記録して Ok を返す。
Status inspect_boot(BlockDevice& dev, uint64_t offset, uint64_t length, FsType fs, const BootInspectOptions& opt, BootInspection* out);
std::string boot_inspection_to_json(const BootInspection& r);

// ------------------------------------------------------------------ 修復

struct BootWrite {
  uint64_t offset = 0;          // デバイス内のバイト位置
  std::vector<uint8_t> data;    // 書く内容
  std::vector<uint8_t> before;  // 計画時点の内容 (競合検出・バックアップ・ロールバック用)
  std::string what;             // 例: "fat32-boot-sector"
};

struct BootRepairPlan {
  bool ok = false;                      // 適用できる (Error 無し)
  FsType fs = FsType::Unknown;
  BootRepairKind kind = BootRepairKind::FatRestorePrimaryFromBackup;
  std::string repair_id;
  uint64_t offset = 0, length = 0;
  std::vector<Diagnostic> problems;
  std::vector<std::string> changes;     // 人間向けの説明
  std::vector<BootWrite> writes;
  uint64_t bytes_to_write = 0;
  std::string device_id;
  std::string plan_hash;                // hex SHA-256 (device_id と全書き込みの内容)
  std::string confirm_phrase;           // "REPAIR xxxxxx"
};

// 修復の計画を作る (書かない)。repair_id は BootInspection::repairs[].id。source は ext の予備スーパーブロックのブロック番号 (0 = 最初に見つかったもの)。
// 計画が作れないとき (修復が今の状態に当てはまらない / 範囲外 / 書き込み先が構造の外) は out->ok == false で problems に理由を入れ、Ok を返す。
Status plan_boot_repair(BlockDevice& dev, uint64_t offset, uint64_t length, FsType fs, const std::string& repair_id, uint64_t source, BootRepairPlan* out);

std::string make_boot_confirm_token(const BootRepairPlan& plan, const std::string& typed_phrase);

struct BootApplyOptions {
  // 書き込み前に、上書きする領域の元の内容を渡す (形式は parse_boot_backup / restore_boot_backup)
  std::function<Status(const std::vector<uint8_t>&)> backup_sink;
  bool require_backup = true;     // true: backup_sink が無い / 失敗なら何も書かずに拒否
  bool rollback_on_error = true;  // 失敗 / キャンセルで、書いた分を元の内容に戻そうとする
  bool in_use = false;            // マウント中など。true なら拒否する
  const CancelToken* cancel = nullptr;
  std::function<void(const char* phase, uint64_t done, uint64_t total)> progress;  // phase: "backup" / "write" / "verify"
  uint64_t timestamp = 0;         // バックアップに記録する時刻 (UNIX 秒)
};

struct BootApplyResult {
  Status status;            // Ok か、InvalidArgument (拒否) / Io / Cancelled など
  std::string code;         // 機械可読 ("ok" / "bad-token" / "plan-tampered" / "device-changed" / "device-read-only" / "in-use" / "busy" / "backup-required" / "backup-failed" / "write-failed" / "verify-failed" ...)
  std::string message;      // 英語
  size_t writes_done = 0;
  bool rolled_back = false;
  bool verified = false;    // 読み戻しが計画どおりで、再検査でブート構造に問題が無い
  std::vector<BootFinding> verify_findings;  // 再検査の結果 (Error / Warning)
  bool ok() const { return status.ok(); }
};

BootApplyResult apply_boot_repair(BlockDevice& dev, const BootRepairPlan& plan, const std::string& confirm_token, const BootApplyOptions& opt = {});

// 書き込み前のバックアップの形式 (USBMBFX1)。壊れていれば Corrupt
struct BootBackupInfo {
  uint32_t version = 0, fs = 0;
  uint64_t timestamp = 0, offset = 0, length = 0;
  size_t record_count = 0;
  uint64_t bytes = 0;
};
Status parse_boot_backup(const std::vector<uint8_t>& file, BootBackupInfo* info);
// バックアップを書き戻す (dev は書き込み可。デバイスの大きさがバックアップ時以上であること)
Status restore_boot_backup(BlockDevice& dev, const std::vector<uint8_t>& file);

std::string boot_plan_to_json(const BootRepairPlan& plan, bool include_data = false);

}  // namespace usbmem
