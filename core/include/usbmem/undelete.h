// SPDX-License-Identifier: Apache-2.0
// 削除ファイルの一覧と救出 (読み取り専用)。FAT12/16/32・exFAT・ext2/3/4・NTFS のオンディスク構造を直接解析する自前のコード
// (FsVolume・カーネル由来のドライバには依存しない = 公開版でも動く)。設計と限界は docs/undelete-carving.md。
//
// 安全の原則:
//  - 読み取り元には絶対に書かない (BlockDevice::write_at を呼ばない)。
//  - 復元 = 選んだファイルを呼び出し側の出力 (sink) へ書き出すこと。元のボリュームには一切書かない。
//  - 「復元できる」と言う根拠は、(1) データ領域が今も未割り当てか (割り当てビットマップ)、(2) 先頭の数 KB がファイル名の拡張子と整合するか、
//    (3) 配置が確実か (連続の仮定か) の 3 つ。再利用されていれば確度は必ず「低」になる。
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "usbmem/block_device.h"

namespace usbmem {

enum class RecoverLevel : uint8_t { High, Medium, Low, None };  // None = 復元できない (圧縮 NTFS など)
const char* recover_level_name(RecoverLevel l);

struct FileExtent {
  uint64_t offset = 0;  // dev の先頭からのバイト位置
  uint64_t length = 0;
  bool hole = false;    // スパース (ゼロとして書き出す)
};

struct DeletedFile {
  uint32_t id = 0;               // 結果の中の通し番号 (undelete_extract に渡す)
  std::string fs;                // "fat12" "fat16" "fat32" "exfat" "ext2" "ext3" "ext4" "ntfs"
  std::string name;              // ファイル名 (UTF-8)。FAT の 8.3 名は先頭文字が失われる (name_partial)
  std::string path;              // 削除前のパス ("/dir/sub/name")。分からない部分は "?"
  uint64_t size = 0;
  int64_t mtime = 0;             // UNIX 秒 (0 = 不明。FAT / exFAT は時刻帯の情報が無いので UTC とみなす)
  bool name_partial = false;
  bool in_deleted_dir = false;   // 親ディレクトリも削除済み
  uint64_t node = 0;             // ext の inode 番号 / NTFS の MFT レコード番号 / FAT・exFAT は先頭クラスタ
  std::vector<FileExtent> extents;   // データの置き場所 (ファイルの先頭から順)
  bool has_resident = false;         // NTFS の resident データ (MFT レコードの中)
  std::vector<uint8_t> resident;
  uint64_t block_bytes = 0;      // 割り当て確認の単位 (FS ごと: FAT/exFAT = セクタ、ext = ブロック、NTFS = クラスタ)
  uint64_t total_blocks = 0;     // データが占める単位の数
  uint64_t reused_blocks = 0;    // うち、今は別の用途に割り当て済みの数 (割り当て情報が不明なら 0 で allocation_known = false)
  bool allocation_known = false;
  bool layout_exact = false;     // 配置が FS の記録から確定している (ext の extent・NTFS の run・exFAT の NoFatChain・1 クラスタ) か。false = 連続の仮定
  bool overlaps_other_deleted = false;   // 別の削除済みファイルとデータ領域が重なる (どちらかが上書きされている)
  std::string signature;         // 先頭から判定した形式 ("jpeg" "png" ...。判定できなければ空)
  std::string signature_check;   // "match" / "mismatch" / "unknown" (拡張子との整合)
  int confidence = 0;            // 0〜100
  RecoverLevel level = RecoverLevel::Low;   // 解析側が復元不可と決めたら None。確度は後処理で決まる
  std::vector<std::string> notes;
};

struct UndeleteOptions {
  const CancelToken* cancel = nullptr;
  std::function<void(const char* phase, uint64_t done, uint64_t total)> progress;  // phase: "alloc" / "scan" / "verify"
  size_t max_files = 200000;     // これを超えたら打ち切り (truncated)
  bool include_empty = false;    // サイズ 0 のファイルも出す
  bool verify_head = true;       // 先頭の数 KB を読んで形式を判定する
};

struct UndeleteResult {
  std::string fs;                // 判別した FS ("" = 未対応・判別できない)
  std::vector<DeletedFile> files;
  bool truncated = false;
  bool cancelled = false;
  bool allocation_known = false; // 割り当て情報が使えたか (false なら確度に上限がかかる)
  uint64_t entries_seen = 0;     // 調べたディレクトリエントリ / inode / MFT レコードの数
  uint64_t skipped_invalid = 0;  // 削除済みだが先頭クラスタ・長さが不正で救出できないと分かったもの
  std::vector<std::string> warnings;
};

// dev (FS の先頭から始まるデバイス。パーティションなら SubRangeDevice) の削除ファイルを列挙する。読み取りだけ。
// 未対応の FS は Unsupported。取り消しは Cancelled (それまでの結果は *out に入る)。
Status list_deleted(BlockDevice& dev, const UndeleteOptions& opt, UndeleteResult* out);

struct ExtractResult {
  uint64_t bytes = 0;            // sink に渡したバイト数
  std::string sha256;            // 渡したバイト列の SHA-256 (16 進)
};
using ByteSinkFn = std::function<Status(const void* data, size_t len)>;
// f のデータを sink へ順に渡す (sink が失敗したらそのエラーで止まる)。読み取り元の読み取りエラーはそのまま返す。
// 取り消しは Cancelled。f.level == None のものは Unsupported。
Status extract_deleted(BlockDevice& dev, const DeletedFile& f, const ByteSinkFn& sink, const CancelToken* cancel, ExtractResult* res);

// 結果の JSON (UTF-8。64 ビット値は数値のまま。キーは docs/undelete-carving.md)
std::string undelete_result_to_json(const UndeleteResult& r);

}  // namespace usbmem
