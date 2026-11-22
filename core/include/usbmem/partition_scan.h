// SPDX-License-Identifier: Apache-2.0
// パーティション探索 (TestDisk の analyse / search 相当・読み取り専用)。設計は docs/partition-scan.md。
//
// 安全の要点:
//  - scan_partitions は書き込まない (write_at / flush を一切呼ばない)。読み取り専用で開いたデバイスで使える。
//  - ディスク上の構造は信用しない。すべての長さ・オフセットを境界検査し、壊れた入力でも例外・無限ループ・範囲外アクセスを起こさない。
//  - ゼロ埋め・乱数の領域から候補を作らない (シグネチャ + 構造の整合で採用する)。
//  - 結果は「候補」であって、パーティション表を直す操作ではない。表の再構築は partition_recover.h の計画 → 確認トークン → 適用 (PartitionEditor) を通る。
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "usbmem/block_device.h"
#include "usbmem/fs_probe.h"
#include "usbmem/partition.h"
#include "usbmem/partition_edit.h"

namespace usbmem {

enum class ScanLevel : uint8_t {
  Quick = 0,  // 典型的な位置 (先頭の数十セクタ、1 MiB 境界、旧 CHS 境界、既存の表のエントリの前後、見つけた FS の直後) だけを調べる
  Full = 1,   // すべてのセクタを読む (進捗・取り消し付き。ディスク全体を 1 回読む)
};

enum class ScanRelation : uint8_t {
  NotInTable = 0,  // 表のどのエントリとも重ならない (失われたパーティションの候補)
  Match = 1,       // 表のエントリと開始が一致し、FS が収まる
  Overlap = 2,     // 表のエントリと重なるが一致しない (開始が違う / FS が大きすぎる)
  Contained = 3,   // 表のエントリの内側にあり、開始が違う (パーティション内のイメージファイルなど)
};
const char* scan_relation_name(ScanRelation r);

struct ScanOptions {
  uint32_t sector_size = 0;        // 0 = dev.logical_block_size()
  ScanLevel level = ScanLevel::Quick;
  uint64_t range_start = 0;        // 調べる範囲 (バイト)。0, 0 = ディスク全体
  uint64_t range_end = 0;
  uint64_t max_positions = 8192;   // Quick: 1 MiB 境界などの基本の位置の数の上限 (見つけた FS の直後の位置は別)
  int min_confidence = 40;         // これ未満の候補は返さない (0..100)
  size_t max_candidates = 512;
  const CancelToken* cancel = nullptr;
  // 進捗 (phase は "table" / "scan" / "verify")。done / total の単位は phase ごと (scan: Quick = 位置の数、Full = バイト)。省略可
  std::function<void(const char* phase, uint64_t done, uint64_t total)> progress;
};

struct ScanCandidate {
  FsType fs = FsType::Unknown;
  std::string variant;             // "luks1" / "luks2" / "udf-bridge" など。空のこともある
  uint32_t sector_size = 512;
  uint64_t start_lba = 0;          // sector_size 単位
  uint64_t sector_count = 0;       // FS のヘッダから算出した大きさ (size_known == false なら 0)
  uint64_t start_bytes = 0;
  uint64_t size_bytes = 0;
  bool size_known = false;         // false: ヘッダに大きさが無い (LUKS など)。size_bytes は 0 か下限
  bool size_minimum = false;       // true: size_bytes は下限 (squashfs など。パーティションはこれ以上ありうる)
  bool fits_device = true;         // false: start + size がデバイスの外 (イメージが切り詰められた)
  int confidence = 0;              // 0..100。80 以上: 構造の整合まで確認 / 60 以上: シグネチャ + 基本の整合 / それ未満: 弱い (バックアップだけなど)
  bool primary_ok = false;         // 先頭の構造 (ブートセクタ / スーパーブロック) が正しい
  bool backup_ok = false;          // バックアップの構造が正しい (ブートセクタのバックアップ / 予備スーパーブロックなど)
  std::vector<std::string> notes;  // 機械可読の注記 ("primary-damaged" / "found-by-backup" / "exceeds-device" / "fat-first-entry-mismatch" ...)
  ScanRelation relation = ScanRelation::NotInTable;
  int32_t table_index = -1;        // relation が Match / Overlap / Contained のとき、対応する表のパーティション番号 (Partition::index)
  int32_t nested_in = -1;          // 別の候補の内側にある (candidates の添字)。FS の中のイメージファイルなど。-1 = 無い
  uint64_t end_bytes() const { return start_bytes + size_bytes; }
};

struct ScanResult {
  uint32_t sector_size = 512;
  uint64_t disk_bytes = 0;
  ScanLevel level = ScanLevel::Quick;
  PartitionTable table;                 // 既存の表 (probe 無し)。壊れていれば diagnostics に理由
  GptInspection gpt;                    // GPT の詳細検査 (primary / backup の状態)
  std::vector<ScanCandidate> candidates;  // 開始位置の昇順
  std::vector<uint32_t> table_entries_without_fs;  // FS が見つからなかった表のパーティション番号 (空き / 未フォーマット / 未対応の FS)
  uint64_t positions_probed = 0;        // 調べた位置の数 (Full ではセクタ数)
  uint64_t bytes_read = 0;
  uint64_t read_errors = 0;             // 読めなかった箇所の数 (読めなかった位置は飛ばして続ける)
  bool cancelled = false;
  bool truncated_candidates = false;    // max_candidates で切った
  std::vector<Diagnostic> notes;
};

// dev を読み取り専用で走査して、FS のシグネチャからパーティションの候補を列挙する。
// I/O エラー (読めない箇所は飛ばす。Disconnected / Closed のときだけ中断) と Cancelled だけを Status で返す。キャンセルされても、それまでの結果は out に入る。
Status scan_partitions(BlockDevice& dev, const ScanOptions& opt, ScanResult* out);

// JSON (UTF-8)。64 ビット値は文字列で出す。
std::string scan_result_to_json(const ScanResult& r);

}  // namespace usbmem
