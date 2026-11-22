// SPDX-License-Identifier: Apache-2.0
// 失われたパーティション表の復元計画。partition_scan.h の探索結果から GPT / MBR の組み直し案 (複数案・信頼度順) を作り、
// PartitionEditor の「計画 → 確認トークン → 適用」と書き込み前の自動バックアップ (partition_backup) に接続する。設計は docs/partition-scan.md。
//
// 安全の要点:
//  - この API 自体は何も書かない。案は PartitionEditor の操作列に直せるだけで、書き込みは PartitionEditor::apply (確認トークン必須・書き込み前に表のバックアップ・読み戻し検証) を通る。
//  - 案の検証は読み取り専用のビューの上で PartitionEditor::plan() を実際に呼んで行う (重なり・範囲外・MBR の制限などはエディタが判定する)。
//  - 復元するのはテーブルのエントリだけ。パーティションのデータには触れない。元の種別 GUID・名前・属性は失われているので、FS から推定した値になる (docs に明記)。
#pragma once
#include <string>
#include <vector>

#include "usbmem/partition_scan.h"

namespace usbmem {

struct ProposedPartition {
  uint64_t start_lba = 0, sector_count = 0;
  PartRole role = PartRole::Auto;     // MBR: Primary / Extended / Logical
  uint8_t mbr_type = 0x83;
  std::array<uint8_t, 16> type_guid{};    // GPT (ディスク上の生バイト)
  std::array<uint8_t, 16> unique_guid{};  // 全 0 = 新規生成
  std::string name;
  uint64_t attrs = 0;
  bool bootable = false;
  FsType fs = FsType::Unknown;
  std::string source;                 // "scan" (探索の候補から) / "table" (既存のエントリを残す) / "gpt-backup" (バックアップ GPT のエントリ)
  int32_t candidate = -1;             // source == "scan" のとき ScanResult::candidates の添字
  int confidence = 0;
  bool extent_estimated = false;      // 大きさを FS のヘッダだけから決められなかった (次の候補の手前まで伸ばした / 切り上げた)
};

enum class RecoveryAction : uint8_t {
  RepairGpt = 1,   // GPT の修復 (PartitionEditor::repair_gpt)
  NewTable = 2,    // 新しい表を作って、候補からエントリを作る
  AddMissing = 3,  // 既存の表を残して、表に無い候補のエントリだけ足す
};

struct RecoveryProposal {
  std::string id;                  // "gpt-copy-backup-to-primary" / "add-missing" / "rebuild-gpt" / "rebuild-mbr" など (機械可読)
  std::string title;               // 人間向け (英語)
  RecoveryAction action = RecoveryAction::NewTable;
  TableKind table = TableKind::None;
  GptRepairKind repair = GptRepairKind::CopyBackupToPrimary;  // action == RepairGpt のとき
  int confidence = 0;              // 0..100
  std::vector<ProposedPartition> partitions;  // 適用後の表の中身 (RepairGpt は修復後に残るエントリ。AddMissing は残す分も含む)
  std::vector<std::string> notes;  // 機械可読の注記 ("mbr-needs-extended" / "types-guessed" / "sizes-estimated" / "gpt-backup-valid" ...)
  bool plan_ok = false;            // 読み取り専用のビューで PartitionEditor::plan() が通った
  std::vector<Diagnostic> problems;  // plan_ok == false のときの理由 (エディタの診断)
  std::vector<std::string> changes;  // エディタの差分 (人間向け)
  std::string confirm_phrase;      // 計画の確認文字列 (参考。実際の計画は適用時にもう一度作る)
};

struct RecoveryOptions {
  uint64_t alignment_sectors = 0;  // 0 = 1 MiB 相当
  int min_confidence = 50;         // 候補をテーブルに入れる最低の信頼度
  bool validate = true;            // PartitionEditor::plan() で検証する
  uint64_t rng_seed = 1;           // 検証用の乱数 (GUID の生成)。適用時の GUID は適用側の PartitionEditor が決める
};

// scan (scan_partitions の結果) から復元案を作る。信頼度の高い順。dev は読むだけ。
Status propose_table_recovery(BlockDevice& dev, const ScanResult& scan, const RecoveryOptions& opt, std::vector<RecoveryProposal>* out);

// ed の操作列を捨てて、案の操作を積む (書き込みはしない)。この後 ed.plan() → make_confirm_token → ed.apply (backup_sink を付ける) で適用する。
EditResult queue_recovery_proposal(PartitionEditor& ed, const RecoveryProposal& p);

std::string recovery_proposals_to_json(const std::vector<RecoveryProposal>& v);

}  // namespace usbmem
