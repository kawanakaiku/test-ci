// SPDX-License-Identifier: Apache-2.0
// データ救出 (GNU ddrescue 方式)。壊れかけた読み取り元から、読める所だけを先に・安全に出力先へ複製し、
// 読めなかった範囲を mapfile (GNU ddrescue の mapfile 形式と互換) に記録する。中断・再開ができ、
// ddrescue 本体で続きをすることも、逆に ddrescue の mapfile から続けることもできる。設計は docs/recovery-backup.md。
//
// 安全の原則:
//  - 読み取り元には write_at を一切呼ばない。読み取り元と出力先が同一のデバイスなら実行を拒否する。
//  - デバイスが消えた (Disconnected / Closed) 場合は、その範囲を「不良」と記録せずに止める (再接続して再開できる)。
//  - 読み取りエラーが続く・一定時間進まない・単位時間あたりのエラー数が多い場合は自動で止める (USB デバイスを痛めない)。
//  - 出力先への書き込みが失敗したら、その範囲を不良と記録せずに止める。
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "usbmem/block_device.h"

namespace usbmem {

// ---- mapfile -------------------------------------------------------------------------------------------------
// ブロックの状態 (GNU ddrescue と同じ文字)
constexpr char kNonTried = '?';     // 未試行
constexpr char kNonTrimmed = '*';   // 大きい読み取りが失敗した (未トリム)
constexpr char kNonScraped = '/';   // トリム済みで内側が残っている (未スクレイプ)
constexpr char kBad = '-';          // 読めないセクタ
constexpr char kFinished = '+';     // 複製済み

struct MapBlock {
  uint64_t pos = 0;
  uint64_t size = 0;
  char status = kNonTried;
};

// mapfile の先頭の「現在位置」行 + コメントに載せる情報
struct MapMeta {
  uint64_t current_pos = 0;
  char current_status = '?';  // ? * / - F G + (ddrescue の current_status)
  uint32_t current_pass = 1;
  std::string command;         // コメント用 (任意)
  std::string start_time, current_time;
  bool finished = false;
};

class Mapfile {
 public:
  explicit Mapfile(uint64_t total = 0);
  // [0, total) をすべて未試行にする
  void reset(uint64_t total);
  uint64_t size() const { return total_; }

  // [pos, pos+size) を status にする (区間は [0, total) に収まること)。隣接する同じ状態は併合する。
  void set_status(uint64_t pos, uint64_t size, char status);
  char status_at(uint64_t pos) const;
  // from 以降で最初の status のブロック (from を含むブロックは from から始まるものに切り詰める)。
  bool find_next(char status, uint64_t from, MapBlock* out) const;
  // before より前で最後の status のブロック (before にかかるブロックは before で切り詰める)。
  bool find_prev(char status, uint64_t before, MapBlock* out) const;
  uint64_t count(char status) const;
  std::vector<MapBlock> blocks() const;
  size_t block_count() const { return map_.size(); }

  // ddrescue 互換のテキスト
  std::string to_text(const MapMeta& meta) const;
  // 不正な入力は Corrupt。total_hint != 0 なら、mapfile が total_hint より小さい分を未試行で延長し、大きければ Corrupt にする。
  static Status parse(const std::string& text, uint64_t total_hint, Mapfile* out, MapMeta* meta);

 private:
  struct Ent { uint64_t size; char status; };
  void merge_around(std::map<uint64_t, Ent>::iterator it);
  void split_at(uint64_t pos);
  std::map<uint64_t, Ent> map_;  // 先頭位置 → (大きさ, 状態)。隙間なく [0, total_) を覆う
  uint64_t total_ = 0;
};

// ---- 救出エンジン -----------------------------------------------------------------------------------------------
struct RescueOptions {
  uint64_t start = 0;            // 入力の対象範囲 [start, start+length)。length = 0 は末尾まで
  uint64_t length = 0;
  uint64_t out_offset = 0;       // 入力位置 start に対応する出力位置 (ddrescue の -o と同じ向き: 出力位置 = 入力位置 - start + out_offset)
  uint32_t sector_size = 0;      // 0 = max(512, 入力の論理ブロックサイズ)
  uint32_t cluster_sectors = 64; // 最初の読み取りの大きさ (セクタ数)
  uint32_t retries = 0;          // 不良セクタの再試行回数 (0 = しない)
  bool do_trim = true;
  bool do_scrape = true;
  bool skip_on_error = true;     // 最初のパスでエラー後に先へ飛ぶ (飛ばした所は未試行のまま残り、後で戻る)
  uint64_t skip_min = 64 * 1024; // 飛ぶ大きさ (連続エラーのたびに倍、最大 skip_max)
  uint64_t skip_max = 1024ull * 1024 * 1024;
  uint32_t read_timeout_ms = 0;  // IoOptions::timeout_ms に渡す (0 = 実装既定)

  // 安全弁 (0 = 無効)
  uint32_t max_consecutive_errors = 0;   // 成功を挟まず連続して失敗した読み取りがこれを超えたら止める
  uint64_t max_errors = 0;               // この実行で失敗した読み取りの総数がこれを超えたら止める
  uint32_t error_window_ms = 0;          // 窓の長さ ...
  uint32_t max_errors_in_window = 0;     // ... その窓の中のエラー数がこれ以上なら止める (エラーの「レート」)
  uint64_t no_progress_timeout_ms = 0;   // 最後に読めてから、この時間 1 バイトも読めなければ止める
  uint32_t pause_between_reads_ms = 0;   // 読み取りの間の休み (弱ったデバイス向け)

  uint32_t checkpoint_every_ops = 256;     // mapfile を保存する読み取り回数の間隔
  uint32_t checkpoint_every_ms = 30000;    // 同じく時間の間隔
  uint32_t progress_every_ms = 250;
  std::function<uint64_t()> clock_ms;      // テスト用。未設定なら steady_clock
};

struct RescueControl {
  std::atomic<bool> cancel{false};  // true: 次の読み取りの前に止める (再開できる)
  std::atomic<bool> pause{false};   // true: 一時停止 (false に戻すと続行)。cancel が優先
};

struct RescueProgress {
  const char* phase = "";   // "copy" "copy-2" "trim" "scrape" "retry-N"
  uint64_t pos = 0;
  uint64_t size = 0;        // 対象範囲の大きさ
  uint64_t finished = 0, non_tried = 0, non_trimmed = 0, non_scraped = 0, bad = 0;
  uint64_t read_ops = 0, read_errors = 0;
  uint64_t bytes_copied = 0;      // この実行で出力に書いた量
  uint64_t ms_since_last_good = 0;
};

enum class RescueStop {
  Finished,       // 対象範囲のすべてが 「+」 か 「-」 になった
  Cancelled,      // 利用者の取り消し (再開できる)
  SafetyAbort,    // 安全弁で停止 (code に理由)
  InputGone,      // 読み取り元が消えた / 閉じられた (再接続して再開できる)
  OutputError,    // 出力先への書き込み失敗
  MapfileError,   // mapfile を保存できない
  BadArgument,    // 事前検査で拒否 (何も読み書きしていない)
};

struct RescueResult {
  RescueStop stop = RescueStop::BadArgument;
  Status status;       // 詳細な原因 (入力 / 出力のエラー)
  std::string code;    // 機械可読: "ok" "cancelled" "same-device" "output-read-only" "output-too-small" "max-consecutive-errors" "max-errors" "error-rate" "no-progress-timeout" "input-gone" "output-write-failed" "mapfile-save-failed" "bad-mapfile" ...
  std::string message;
  RescueProgress last;
};

struct RescueHooks {
  std::function<void(const RescueProgress&)> on_progress;
  // mapfile を永続化する。失敗 (false) を返すと、再開できなくなるので実行を止める。
  std::function<bool(const Mapfile&, const MapMeta&)> persist;
};

class RescueEngine {
 public:
  // map は [0, 入力デバイスの大きさ) を覆っていること (reset(dev.size()) 済み、または以前の mapfile)。
  RescueEngine(BlockDevicePtr input, BlockDevicePtr output, Mapfile* map, RescueOptions opt = {});
  RescueResult run(RescueControl* ctl = nullptr, const RescueHooks& hooks = {});

 private:
  struct Impl;
  BlockDevicePtr in_, out_;
  Mapfile* map_;
  RescueOptions opt_;
};

// 事前検査だけ (ctor + run の前半)。ok でなければ code に理由。
RescueResult rescue_preflight(const BlockDevicePtr& in, const BlockDevicePtr& out, const Mapfile& map, const RescueOptions& opt);

}  // namespace usbmem
