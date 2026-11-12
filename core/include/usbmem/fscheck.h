// SPDX-License-Identifier: Apache-2.0
// ファイルシステムの読み取り専用の整合性検査 (fsck 相当・要件書 5 章)。設計は docs/disk-tools.md。
//
// 安全の要点:
//  - この API は書き込まない (BlockDevice の write_at / flush を一切呼ばない。読み取り専用で開いたデバイスで使える)。
//    修復は行わない。ntfsfix 等の自動修復も呼ばない。見つけた問題を構造化して報告するだけ。
//  - ディスク上の構造は信用しない。全オフセット・長さを境界検査し、ループや巨大な値でも停止して有限のメモリで終わる。
//  - 検査の結果 (verdict) は e2fsck / fsck.fat / fsck.exfat の終了コードの考え方に合わせる:
//      Clean   = 問題なし
//      Warning = データは壊れていないが、整えるべき点がある (未クリーンアンマウント / 空き容量の表記違い / 未再生のジャーナルなど)
//      Errors  = 構造の矛盾がある (修復しないで書き込むとデータを壊す恐れがある)
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "usbmem/block_device.h"
#include "usbmem/fs_probe.h"
#include "usbmem/partition.h"  // Severity

namespace usbmem {

struct FsFinding {
  Severity severity = Severity::Info;
  std::string code;     // 安定した機械可読 ID (例: "fat-chain-loop")
  std::string message;  // 人間向け (英語)
  uint64_t offset = UINT64_MAX;  // 検査範囲の先頭からのバイト位置 (分かるとき。UINT64_MAX = 無し)
  std::string object;   // 対象 (例: "inode 12", "/dir/file.txt", "cluster 345", "mft record 7")
};

enum class FsVerdict : uint8_t {
  Clean = 0,
  Warnings = 1,     // 警告だけ
  Errors = 2,       // 構造の矛盾あり
  Unsupported = 3,  // 検査できない形式 (または対応しない機能を持つ)
  Incomplete = 4,   // 途中で止めた (キャンセル / メモリ上限など)。見つかった分だけ報告
};
const char* fs_verdict_name(FsVerdict v);

struct FsCheckStats {
  uint64_t files = 0, directories = 0, symlinks = 0, other = 0;  // 検査した対象の数
  uint64_t units_total = 0, units_used = 0;                      // クラスタ / ブロック / MFT レコードの数
  uint64_t bytes_per_unit = 0;
};

struct FsCheckReport {
  FsType fs = FsType::Unknown;
  FsVerdict verdict = FsVerdict::Unsupported;
  bool incomplete = false;            // 途中で止めた (verdict も Incomplete になる。Errors が見つかっていれば Errors を優先)
  bool unclean = false;               // 「正常にアンマウントされていない」印 (ダーティフラグ / ext の not-clean / 未再生ジャーナル)
  bool needs_journal_recovery = false;  // ext3/4 の未再生ジャーナル (読み取り専用マウントは noload が必要)
  std::vector<FsFinding> findings;    // max_findings まで。それ以上は数だけ数える
  bool findings_truncated = false;
  uint64_t error_count = 0, warning_count = 0, info_count = 0;  // 切り詰めた分も含む
  FsCheckStats stats;
  bool has_errors() const { return error_count > 0; }
  bool consistent() const { return error_count == 0 && warning_count == 0; }
};

struct FsCheckOptions {
  size_t max_findings = 500;
  size_t memory_limit = 256u << 20;      // ビットマップ等に使ってよい上限 (超えると深い検査をやめて incomplete)
  uint64_t max_objects = 200000000;      // 走査する inode / エントリ数の上限 (悪意ある入力で終わらなくなるのを防ぐ)
  const CancelToken* cancel = nullptr;
  // 進捗 (phase は "boot" / "fat" / "directories" など)。done / total は単位が phase ごとに違う。省略可
  std::function<void(const char* phase, uint64_t done, uint64_t total)> progress;
};

// dev の [offset, offset+length) を fs として検査する。I/O エラー (デバイスの読み取り失敗) と Cancelled だけを Status で返し、
// それ以外 (壊れた構造) は out->findings に記録して Ok を返す。
// fs == Unknown なら probe_filesystem で判別する。検査に対応しない fs は verdict = Unsupported。
// 対応: Fat12 / Fat16 / Fat32 / Exfat / Ext2 / Ext3 / Ext4 / Ntfs。
Status check_filesystem(BlockDevice& dev, uint64_t offset, uint64_t length, FsType fs, const FsCheckOptions& opt, FsCheckReport* out);

bool fscheck_supported(FsType fs);

// JSON (UTF-8)。C API / JNI 用。
std::string fscheck_report_to_json(const FsCheckReport& r);

}  // namespace usbmem
