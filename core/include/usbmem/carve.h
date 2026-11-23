// SPDX-License-Identifier: Apache-2.0
// シグネチャによるファイルの切り出し (カービング。PhotoRec 相当)。ファイルシステムの管理情報が無くても、ファイルの先頭の署名 (マジックナンバー) と
// 形式に従った長さ / 終端から、連続した領域をファイルとして切り出す。形式の仕様 (公開されているもの) から書いた自前のコード。設計と限界は docs/undelete-carving.md。
//
// 前提と限界:
//  - ファイルは連続した領域に置かれ、先頭はブロック境界 (既定は 512 バイト。align で変えられる) にある。断片化したファイルは扱わない。
//  - 長さが形式の構造から確定できる形式だけを対象にする (JPEG・PNG・GIF・BMP・PDF・ZIP 系・MP4/MOV・MP3・WAV/AVI/WebP・7z・gzip・SQLite・PE)。
//  - 構造の整合 (チャンクの CRC・マーカーの連なり・終端) を確かめて誤検出を減らす。
//  - 読み取りだけ。切り出したファイルは呼び出し側が別の場所へ書く (carve_extract)。元のデバイスには書かない。
#pragma once
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "usbmem/block_device.h"
#include "usbmem/undelete.h"  // ByteSinkFn / ExtractResult

namespace usbmem {

struct CarveTypeInfo {
  std::string id;           // 選択に使う名前 ("jpeg" "png" "gif" "bmp" "pdf" "zip" "mp4" "mp3" "riff" "7z" "gzip" "sqlite" "pe")
  std::string extensions;   // 切り出したファイルの拡張子 (',' 区切り)
  std::string description;
};
std::vector<CarveTypeInfo> carve_types();

struct CarveHit {
  uint64_t offset = 0;      // 検査したデバイスの先頭からのバイト位置
  uint64_t length = 0;
  std::string format;       // carve_types() の id
  std::string ext;          // 拡張子 (拡張子の細分: docx / xlsx / pptx / jar / apk / odt / wav / avi / mov / m4a ...)
  std::string detail;       // 画像の大きさなど (人が読む補助情報)
  int confidence = 0;       // 0〜100 (構造をどこまで確かめられたか)
};

// 再開用の進捗 (mapfile 風)。carve_scan に渡すと続きから行う。
struct CarveState {
  uint64_t dev_size = 0;        // 0 = 新規
  uint64_t options_hash = 0;    // 形式の選択・align・最小サイズ・未割り当てのみ が変わると再開できない
  uint64_t pos = 0;             // ここまで調べ終えた (これ以降が未調査)
  uint64_t rejected = 0;        // 署名は合ったが構造が合わず捨てた数
  std::vector<CarveHit> hits;
  bool finished = false;
};
std::string carve_state_to_text(const CarveState& s);
bool carve_state_parse(const std::string& text, CarveState* out);   // 壊れていれば false

struct CarveOptions {
  std::set<std::string> types;          // 空 = すべて。carve_types() の id
  uint64_t min_size = 0;                // これ未満のものは出さない
  uint64_t max_size = 0;                // 0 = 既定 (16 GiB)
  uint32_t align = 0;                   // ファイルの先頭の置き場所の単位。0 = 512。2 のべき乗
  bool unallocated_only = false;        // FS が割り当て済みの領域を飛ばす (FS を判別できない / 割り当て情報が信用できなければ全体)
  size_t max_hits = 1000000;
  const CancelToken* cancel = nullptr;
  std::function<void(const char* phase, uint64_t done, uint64_t total)> progress;   // phase: "alloc" / "scan"
  std::function<Status(const CarveHit&)> on_hit;        // 見つかるたびに (状態を進める前に) 呼ぶ。失敗を返すと止まる
  std::function<void(const CarveState&)> checkpoint;    // 状態の保存の契機 (ヒット時と checkpoint_bytes ごと)
  uint64_t checkpoint_bytes = 64ull << 20;
};

struct CarveResult {
  std::string fs;                       // unallocated_only のとき判別した FS
  bool allocation_known = false;
  bool cancelled = false;
  uint64_t bytes_scanned = 0;           // この実行で調べたバイト数
  uint64_t read_errors = 0;
  std::vector<std::string> warnings;
};

// dev を走査して state->hits に足す。state は入出力 (新規なら CarveState{} のまま)。
// 取り消しは Cancelled (state は再開できる状態)。state が別のデバイス / 別の設定のものなら InvalidArgument。
Status carve_scan(BlockDevice& dev, const CarveOptions& opt, CarveState* state, CarveResult* res);

// ヒットの領域を sink へ渡す (読み取りだけ)。
Status carve_extract(BlockDevice& dev, const CarveHit& hit, const ByteSinkFn& sink, const CancelToken* cancel, ExtractResult* res);

std::string carve_result_to_json(const CarveResult& r, const CarveState& s);
std::string carve_hit_filename(const CarveHit& h);   // "f<16 桁の 16 進オフセット>.<ext>"

}  // namespace usbmem
