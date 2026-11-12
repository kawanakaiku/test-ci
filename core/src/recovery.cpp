// SPDX-License-Identifier: Apache-2.0
// データ救出 (ddrescue 方式)。mapfile と RescueEngine。設計は docs/recovery-backup.md。
#include "usbmem/recovery.h"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <thread>

namespace usbmem {

// ======================================================================================================== Mapfile
Mapfile::Mapfile(uint64_t total) { reset(total); }

void Mapfile::reset(uint64_t total) {
  map_.clear();
  total_ = total;
  if (total) map_[0] = {total, kNonTried};
}

void Mapfile::split_at(uint64_t pos) {
  if (pos == 0 || pos >= total_) return;
  auto it = map_.upper_bound(pos);
  if (it == map_.begin()) return;
  --it;
  if (it->first == pos) return;
  uint64_t start = it->first, size = it->second.size;
  char st = it->second.status;
  it->second.size = pos - start;
  map_[pos] = {size - (pos - start), st};
}

void Mapfile::merge_around(std::map<uint64_t, Ent>::iterator it) {
  if (it != map_.begin()) {
    auto prev = std::prev(it);
    if (prev->second.status == it->second.status && prev->first + prev->second.size == it->first) {
      prev->second.size += it->second.size;
      map_.erase(it);
      it = prev;
    }
  }
  auto next = std::next(it);
  if (next != map_.end() && next->second.status == it->second.status && it->first + it->second.size == next->first) {
    it->second.size += next->second.size;
    map_.erase(next);
  }
}

void Mapfile::set_status(uint64_t pos, uint64_t size, char status) {
  if (size == 0 || pos >= total_) return;
  if (size > total_ - pos) size = total_ - pos;
  split_at(pos);
  split_at(pos + size);
  auto lo = map_.lower_bound(pos);
  auto hi = map_.lower_bound(pos + size);
  map_.erase(lo, hi);
  auto it = map_.emplace(pos, Ent{size, status}).first;
  merge_around(it);
}

char Mapfile::status_at(uint64_t pos) const {
  if (pos >= total_) return 0;
  auto it = map_.upper_bound(pos);
  --it;
  return it->second.status;
}

bool Mapfile::find_next(char status, uint64_t from, MapBlock* out) const {
  if (from >= total_) return false;
  auto it = map_.upper_bound(from);
  --it;
  for (; it != map_.end(); ++it) {
    uint64_t end = it->first + it->second.size;
    if (end <= from || it->second.status != status) continue;
    out->pos = std::max(from, it->first);
    out->size = end - out->pos;
    out->status = status;
    return true;
  }
  return false;
}

bool Mapfile::find_prev(char status, uint64_t before, MapBlock* out) const {
  if (before == 0 || total_ == 0) return false;
  if (before > total_) before = total_;
  auto it = map_.lower_bound(before);  // before 以降で最初
  while (it != map_.begin()) {
    --it;
    if (it->second.status != status) continue;
    uint64_t end = std::min(before, it->first + it->second.size);
    if (end <= it->first) continue;
    out->pos = it->first;
    out->size = end - it->first;
    out->status = status;
    return true;
  }
  return false;
}

uint64_t Mapfile::count(char status) const {
  uint64_t n = 0;
  for (auto& kv : map_) if (kv.second.status == status) n += kv.second.size;
  return n;
}

std::vector<MapBlock> Mapfile::blocks() const {
  std::vector<MapBlock> v;
  v.reserve(map_.size());
  for (auto& kv : map_) v.push_back({kv.first, kv.second.size, kv.second.status});
  return v;
}

std::string Mapfile::to_text(const MapMeta& meta) const {
  std::string s;
  char line[160];
  s += "# Mapfile. Created by usbmem recovery (GNU ddrescue compatible)\n";
  if (!meta.command.empty()) s += "# Command line: " + meta.command + "\n";
  if (!meta.start_time.empty()) s += "# Start time:   " + meta.start_time + "\n";
  if (!meta.current_time.empty()) s += "# Current time: " + meta.current_time + "\n";
  if (meta.finished) s += "# Finished\n";
  s += "# current_pos  current_status  current_pass\n";
  std::snprintf(line, sizeof line, "0x%08" PRIX64 "     %c               %u\n", meta.current_pos, meta.current_status, meta.current_pass);
  s += line;
  s += "#      pos        size  status\n";
  for (auto& kv : map_) {
    std::snprintf(line, sizeof line, "0x%08" PRIX64 "  0x%08" PRIX64 "  %c\n", kv.first, kv.second.size, kv.second.status);
    s += line;
  }
  return s;
}

namespace {
bool parse_u64(const std::string& tok, uint64_t* v) {
  if (tok.empty()) return false;
  errno = 0;
  char* end = nullptr;
  unsigned long long x = std::strtoull(tok.c_str(), &end, 0);
  if (errno != 0 || !end || *end != 0) return false;
  if (tok[0] == '-') return false;
  *v = x;
  return true;
}
bool valid_block_status(char c) { return c == '?' || c == '*' || c == '/' || c == '-' || c == '+'; }
}  // namespace

Status Mapfile::parse(const std::string& text, uint64_t total_hint, Mapfile* out, MapMeta* meta) {
  constexpr size_t kMaxBlocks = 20u * 1000 * 1000;
  MapMeta m;
  std::vector<MapBlock> blocks;
  bool first = true;
  size_t i = 0;
  while (i < text.size()) {
    size_t j = text.find('\n', i);
    if (j == std::string::npos) j = text.size();
    std::string line = text.substr(i, j - i);
    i = j + 1;
    size_t a = line.find_first_not_of(" \t\r");
    if (a == std::string::npos) continue;
    if (line[a] == '#') {
      if (line.compare(a, 10, "# Finished") == 0) m.finished = true;
      continue;
    }
    std::vector<std::string> tok;
    size_t p = a;
    while (p < line.size()) {
      size_t q = line.find_first_of(" \t\r", p);
      if (q == std::string::npos) q = line.size();
      tok.push_back(line.substr(p, q - p));
      p = line.find_first_not_of(" \t\r", q);
      if (p == std::string::npos) break;
    }
    if (first) {
      first = false;
      uint64_t cp;
      if (tok.size() < 2 || tok.size() > 3 || !parse_u64(tok[0], &cp) || tok[1].size() != 1) return Status::Make(Errc::Corrupt);
      m.current_pos = cp;
      m.current_status = tok[1][0];
      if (tok.size() == 3) {
        uint64_t pass;
        if (!parse_u64(tok[2], &pass) || pass > 1000000) return Status::Make(Errc::Corrupt);
        m.current_pass = static_cast<uint32_t>(pass);
      }
      continue;
    }
    uint64_t pos, size;
    if (tok.size() != 3 || !parse_u64(tok[0], &pos) || !parse_u64(tok[1], &size) || tok[2].size() != 1 || !valid_block_status(tok[2][0]))
      return Status::Make(Errc::Corrupt);
    if (size == 0 || pos + size < pos) return Status::Make(Errc::Corrupt);
    if (!blocks.empty() && blocks.back().pos + blocks.back().size != pos) return Status::Make(Errc::Corrupt);  // 隙間・重なり・逆順
    if (blocks.empty() && pos != 0) blocks.push_back({0, pos, kNonTried});
    if (blocks.size() >= kMaxBlocks) return Status::Make(Errc::Corrupt);
    blocks.push_back({pos, size, tok[2][0]});
  }
  if (first) return Status::Make(Errc::Corrupt);  // 現在位置の行がない
  uint64_t end = blocks.empty() ? 0 : blocks.back().pos + blocks.back().size;
  if (total_hint && end > total_hint) return Status::Make(Errc::Corrupt);
  uint64_t total = total_hint ? total_hint : end;
  Mapfile r(0);
  r.total_ = total;
  uint64_t cursor = 0;
  for (auto& b : blocks) {
    auto it = r.map_.emplace(b.pos, Ent{b.size, b.status}).first;
    r.merge_around(it);
    cursor = b.pos + b.size;
  }
  if (cursor < total) {
    auto it = r.map_.emplace(cursor, Ent{total - cursor, kNonTried}).first;
    r.merge_around(it);
  }
  *out = std::move(r);
  if (meta) *meta = m;
  return Status::Ok();
}

// ============================================================================================================ 救出
namespace {
uint64_t steady_ms() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct Range { uint64_t start = 0, end = 0, sector = 512, cluster = 0; };

RescueResult bad_arg(const char* code, const char* msg) {
  RescueResult r;
  r.stop = RescueStop::BadArgument;
  r.status = Status::Make(Errc::InvalidArgument);
  r.code = code;
  r.message = msg;
  return r;
}

RescueResult preflight_impl(const BlockDevicePtr& in, const BlockDevicePtr& out, const Mapfile& map, const RescueOptions& opt, Range* rg) {
  if (!in || !out) return bad_arg("no-device", "input or output device is missing");
  if (in.get() == out.get()) return bad_arg("same-device", "the output must not be the input device");
  if (out->is_read_only()) {
    RescueResult r = bad_arg("output-read-only", "the output device is read-only");
    r.status = Status::Make(Errc::ReadOnly);
    return r;
  }
  uint64_t sector = opt.sector_size ? opt.sector_size : std::max<uint32_t>(512, in->logical_block_size());
  if (sector == 0 || sector > (1u << 20)) return bad_arg("bad-sector-size", "sector size is out of range");
  uint64_t csec = std::max<uint32_t>(1, opt.cluster_sectors);
  if (csec > (64ull << 20) / sector) return bad_arg("bad-cluster-size", "cluster size is too large");
  uint64_t size = in->size();
  if (opt.start > size) return bad_arg("bad-range", "the start is beyond the end of the input");
  uint64_t end = opt.length ? opt.start + opt.length : size;
  if (opt.length && (end < opt.start || end > size)) return bad_arg("bad-range", "the range is beyond the end of the input");
  if (opt.start % sector) return bad_arg("unaligned-range", "the start must be a multiple of the sector size");
  if (map.size() != size) return bad_arg("map-size-mismatch", "the mapfile does not cover the input size");
  if (opt.out_offset > UINT64_MAX - (end - opt.start) || opt.out_offset + (end - opt.start) > out->size())
    return bad_arg("output-too-small", "the output device is too small for the range");
  rg->start = opt.start;
  rg->end = end;
  rg->sector = sector;
  rg->cluster = sector * csec;
  RescueResult ok;
  ok.stop = RescueStop::Finished;
  ok.code = "ok";
  return ok;
}

class Runner {
 public:
  Runner(const BlockDevicePtr& in, const BlockDevicePtr& out, Mapfile* map, const RescueOptions& opt, const Range& rg, RescueControl* ctl, const RescueHooks& hooks)
      : in_(in), out_(out), map_(*map), opt_(opt), rg_(rg), ctl_(ctl), hooks_(hooks) {
    buf_.resize(static_cast<size_t>(rg.cluster));
    t_start_ = now();
    last_good_ = t_start_;
    last_ckpt_ = t_start_;
  }

  RescueResult run() {
    bool ok = phase_copy(true) && phase_copy(false);
    if (ok && opt_.do_trim) ok = phase_trim();
    if (ok) ok = phase_scrape();
    for (uint32_t pass = 1; ok && pass <= opt_.retries; pass++) ok = phase_retry(pass);
    if (ok) {
      res_.stop = RescueStop::Finished;
      res_.code = unresolved() ? "ok-partial" : "ok";
    }
    cur_status_ = '+';
    checkpoint(true, ok);
    res_.last = snapshot("done");
    if (hooks_.on_progress) hooks_.on_progress(res_.last);
    return res_;
  }

 private:
  uint64_t now() const { return opt_.clock_ms ? opt_.clock_ms() : steady_ms(); }
  uint64_t out_pos(uint64_t pos) const { return pos - rg_.start + opt_.out_offset; }

  bool unresolved() const {
    MapBlock b;
    for (char c : {kNonTried, kNonTrimmed, kNonScraped})
      if (map_.find_next(c, rg_.start, &b) && b.pos < rg_.end) return true;
    return false;
  }

  RescueProgress snapshot(const char* phase) {
    RescueProgress p;
    p.phase = phase;
    p.pos = cur_pos_;
    p.size = rg_.end - rg_.start;
    p.finished = map_.count(kFinished);
    p.non_tried = map_.count(kNonTried);
    p.non_trimmed = map_.count(kNonTrimmed);
    p.non_scraped = map_.count(kNonScraped);
    p.bad = map_.count(kBad);
    p.read_ops = ops_;
    p.read_errors = errors_;
    p.bytes_copied = copied_;
    p.ms_since_last_good = now() - last_good_;
    return p;
  }

  void stop_with(RescueStop s, Errc ec, const char* code, const char* msg, int err = 0) {
    if (stopped_) return;
    stopped_ = true;
    res_.stop = s;
    res_.status = Status::Make(ec, err);
    res_.code = code;
    res_.message = msg;
  }

  bool checkpoint(bool force, bool finished_flag = false) {
    uint64_t t = now();
    if (!force && ops_since_ckpt_ < opt_.checkpoint_every_ops && t - last_ckpt_ < opt_.checkpoint_every_ms) return true;
    ops_since_ckpt_ = 0;
    last_ckpt_ = t;
    if (!hooks_.persist) return true;
    MapMeta m;
    m.current_pos = cur_pos_;
    m.current_status = cur_status_;
    m.current_pass = cur_pass_;
    m.finished = finished_flag;
    if (!hooks_.persist(map_, m)) {
      stop_with(RescueStop::MapfileError, Errc::Io, "mapfile-save-failed", "could not save the mapfile; stopping so that no progress is lost silently");
      return false;
    }
    return true;
  }

  void progress(const char* phase, bool force = false) {
    uint64_t t = now();
    if (!hooks_.on_progress) return;
    if (!force && t - last_prog_ < opt_.progress_every_ms) return;
    last_prog_ = t;
    hooks_.on_progress(snapshot(phase));
  }

  // 次の読み取りの前の確認。true = ここで止める。
  bool interrupted() {
    if (stopped_) return true;
    if (abort_pending_) {
      stop_with(RescueStop::SafetyAbort, Errc::Io, abort_code_, abort_msg_);
      return true;
    }
    if (opt_.no_progress_timeout_ms && now() - last_good_ > opt_.no_progress_timeout_ms) {
      stop_with(RescueStop::SafetyAbort, Errc::Timeout, "no-progress-timeout", "nothing could be read for too long; stopping to protect the device");
      return true;
    }
    if (ctl_) {
      while (ctl_->pause.load() && !ctl_->cancel.load()) {
        progress(phase_name_, true);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        last_good_ += 20;  // 一時停止中は「進んでいない」時間に数えない
        last_ckpt_ += 20;
      }
      if (ctl_->cancel.load()) {
        stop_with(RescueStop::Cancelled, Errc::Cancelled, "cancelled", "cancelled by the user; the mapfile allows resuming");
        return true;
      }
    }
    if (opt_.pause_between_reads_ms) std::this_thread::sleep_for(std::chrono::milliseconds(opt_.pause_between_reads_ms));
    return false;
  }
  void note_error() {
    errors_++;
    consecutive_++;
    uint64_t t = now();
    if (opt_.max_consecutive_errors && consecutive_ > opt_.max_consecutive_errors) {
      abort_pending_ = true; abort_code_ = "max-consecutive-errors"; abort_msg_ = "too many read errors in a row";
    }
    if (opt_.max_errors && errors_ > opt_.max_errors) {
      abort_pending_ = true; abort_code_ = "max-errors"; abort_msg_ = "too many read errors";
    }
    if (opt_.error_window_ms && opt_.max_errors_in_window) {
      window_.push_back(t);
      while (!window_.empty() && t - window_.front() > opt_.error_window_ms) window_.pop_front();
      if (window_.size() >= opt_.max_errors_in_window) {
        abort_pending_ = true; abort_code_ = "error-rate"; abort_msg_ = "the read error rate is too high";
      }
    }
  }

  enum class R { Good, Bad, Stop };
  // [pos, pos+len) を読んで出力へ書く。Bad のときは何も書かない。
  R read_copy(uint64_t pos, uint64_t len) {
    if (interrupted()) return R::Stop;
    IoOptions io;
    io.timeout_ms = opt_.read_timeout_ms;
    Status st = in_->read_at(pos, buf_.data(), static_cast<size_t>(len), io);
    ops_++;
    ops_since_ckpt_++;
    if (st.ok()) {
      Status w = out_->write_at(out_pos(pos), buf_.data(), static_cast<size_t>(len));
      if (!w.ok()) {
        stop_with(RescueStop::OutputError, w.code, "output-write-failed", "writing to the output failed; the range was not marked bad", w.sys_errno);
        return R::Stop;
      }
      copied_ += len;
      last_good_ = now();
      consecutive_ = 0;
      return R::Good;
    }
    switch (st.code) {
      case Errc::Disconnected:
      case Errc::Closed:
        stop_with(RescueStop::InputGone, st.code, "input-gone", "the input device disappeared; reconnect it and resume", st.sys_errno);
        return R::Stop;
      case Errc::Cancelled:
        stop_with(RescueStop::Cancelled, Errc::Cancelled, "cancelled", "the read was cancelled");
        return R::Stop;
      case Errc::OutOfRange:
      case Errc::InvalidArgument:
        stop_with(RescueStop::SafetyAbort, st.code, "input-range", "the input rejected the read range", st.sys_errno);
        return R::Stop;
      default:
        note_error();
        return R::Bad;
    }
  }

  bool end_op(const char* phase) {
    progress(phase);
    return checkpoint(false);
  }

  // ---- 1 回目 (skip あり) / 2 回目 (skip なし): 未試行を大きい単位で読む
  bool phase_copy(bool first_pass) {
    phase_name_ = first_pass ? "copy" : "copy-2";
    cur_status_ = '?';
    cur_pass_ = first_pass ? 1 : 2;
    uint64_t skip = opt_.skip_min;
    uint64_t pos = rg_.start;
    MapBlock b;
    while (map_.find_next(kNonTried, pos, &b) && b.pos < rg_.end) {
      uint64_t p = b.pos, e = std::min(b.pos + b.size, rg_.end);
      while (p < e) {
        uint64_t ce = std::min(e, rg_.start + ((p - rg_.start) / rg_.cluster + 1) * rg_.cluster);
        cur_pos_ = p;
        R r = read_copy(p, ce - p);
        if (r == R::Stop) return false;
        if (r == R::Good) {
          map_.set_status(p, ce - p, kFinished);
          skip = opt_.skip_min;
          p = ce;
        } else {
          map_.set_status(p, ce - p, kNonTrimmed);
          p = ce;
          if (first_pass && opt_.skip_on_error) {
            uint64_t sk = std::min(skip, opt_.skip_max);
            skip = std::min(skip * 2, opt_.skip_max);
            uint64_t np = p + sk;
            np = rg_.start + ((np - rg_.start + rg_.sector - 1) / rg_.sector) * rg_.sector;
            p = std::min(np, e);  // 飛ばした部分は未試行のまま残る
          }
        }
        if (!end_op(phase_name_)) return false;
      }
      pos = e;
    }
    return !interrupted_after_phase();
  }

  bool interrupted_after_phase() {
    // フェーズの区切りで確実に保存し、保留中の安全弁を反映する
    if (stopped_) return true;
    if (abort_pending_) { interrupted(); return true; }
    if (!checkpoint(true)) return true;
    return false;
  }

  // セクタ境界へ揃えた「t の直前のセクタの先頭」
  uint64_t prev_sector_start(uint64_t t) const { return rg_.start + ((t - 1 - rg_.start) / rg_.sector) * rg_.sector; }

  // ---- トリム: 未トリムのブロックの両端から 1 セクタずつ、最初の不良まで読む。内側は未スクレイプにする
  bool phase_trim() {
    phase_name_ = "trim";
    cur_status_ = '*';
    cur_pass_ = 3;
    uint64_t pos = rg_.start;
    MapBlock b;
    while (map_.find_next(kNonTrimmed, pos, &b) && b.pos < rg_.end) {
      uint64_t s = b.pos, e = std::min(b.pos + b.size, rg_.end);
      uint64_t q = s;
      while (q < e) {
        uint64_t len = std::min<uint64_t>(rg_.sector, e - q);
        cur_pos_ = q;
        R r = read_copy(q, len);
        if (r == R::Stop) return false;
        map_.set_status(q, len, r == R::Good ? kFinished : kBad);
        q += len;
        if (!end_op("trim")) return false;
        if (r == R::Bad) break;
      }
      uint64_t t = e;
      while (t > q) {
        uint64_t ps = std::max(prev_sector_start(t), q);
        uint64_t len = t - ps;
        cur_pos_ = ps;
        R r = read_copy(ps, len);
        if (r == R::Stop) return false;
        map_.set_status(ps, len, r == R::Good ? kFinished : kBad);
        t = ps;
        if (!end_op("trim")) return false;
        if (r == R::Bad) break;
      }
      if (q < t) map_.set_status(q, t - q, kNonScraped);
      pos = e;
    }
    return !interrupted_after_phase();
  }

  // ---- スクレイプ: 未スクレイプ (トリムしない設定なら未トリムも) を 1 セクタずつ読む
  bool phase_scrape() {
    phase_name_ = "scrape";
    cur_status_ = '/';
    cur_pass_ = 4;
    if (!opt_.do_trim && opt_.do_scrape) {  // トリムしない設定では、未トリムの塊をそのままスクレイプする
      MapBlock b;
      uint64_t pos = rg_.start;
      while (map_.find_next(kNonTrimmed, pos, &b) && b.pos < rg_.end) {
        uint64_t e = std::min(b.pos + b.size, rg_.end);
        map_.set_status(b.pos, e - b.pos, kNonScraped);
        pos = e;
      }
    }
    if (!opt_.do_scrape) return !interrupted_after_phase();
    uint64_t pos = rg_.start;
    MapBlock b;
    while (map_.find_next(kNonScraped, pos, &b) && b.pos < rg_.end) {
      uint64_t e = std::min(b.pos + b.size, rg_.end);
      for (uint64_t q = b.pos; q < e;) {
        uint64_t len = std::min<uint64_t>(rg_.sector, e - q);
        cur_pos_ = q;
        R r = read_copy(q, len);
        if (r == R::Stop) return false;
        map_.set_status(q, len, r == R::Good ? kFinished : kBad);
        q += len;
        if (!end_op("scrape")) return false;
      }
      pos = e;
    }
    return !interrupted_after_phase();
  }

  // ---- 再試行: 不良セクタを 1 つずつ読み直す (奇数回は前から、偶数回は後ろから)
  bool phase_retry(uint32_t pass) {
    static thread_local char name[32];
    std::snprintf(name, sizeof name, "retry-%u", pass);
    phase_name_ = name;
    cur_status_ = '-';
    cur_pass_ = 4 + pass;
    bool forward = (pass % 2) == 1;
    MapBlock b;
    if (forward) {
      uint64_t pos = rg_.start;
      while (map_.find_next(kBad, pos, &b) && b.pos < rg_.end) {
        uint64_t e = std::min(b.pos + b.size, rg_.end);
        for (uint64_t q = b.pos; q < e;) {
          uint64_t len = std::min<uint64_t>(rg_.sector, e - q);
          cur_pos_ = q;
          R r = read_copy(q, len);
          if (r == R::Stop) return false;
          if (r == R::Good) map_.set_status(q, len, kFinished);
          q += len;
          if (!end_op(name)) return false;
        }
        pos = e;
      }
    } else {
      uint64_t before = rg_.end;
      while (map_.find_prev(kBad, before, &b) && b.pos + b.size > rg_.start) {
        uint64_t s = std::max(b.pos, rg_.start);
        for (uint64_t t = b.pos + b.size; t > s;) {
          uint64_t ps = std::max(prev_sector_start(t), s);
          cur_pos_ = ps;
          R r = read_copy(ps, t - ps);
          if (r == R::Stop) return false;
          if (r == R::Good) map_.set_status(ps, t - ps, kFinished);
          t = ps;
          if (!end_op(name)) return false;
        }
        before = s;
        if (before <= rg_.start) break;
      }
    }
    return !interrupted_after_phase();
  }

  BlockDevicePtr in_, out_;
  Mapfile& map_;
  RescueOptions opt_;
  Range rg_;
  RescueControl* ctl_;
  RescueHooks hooks_;
  std::vector<uint8_t> buf_;
  RescueResult res_;
  bool stopped_ = false;
  bool abort_pending_ = false;
  const char* abort_code_ = "";
  const char* abort_msg_ = "";
  const char* phase_name_ = "copy";
  char cur_status_ = '?';
  uint32_t cur_pass_ = 1;
  uint64_t cur_pos_ = 0;
  uint64_t ops_ = 0, errors_ = 0, consecutive_ = 0, copied_ = 0, ops_since_ckpt_ = 0;
  uint64_t t_start_ = 0, last_good_ = 0, last_ckpt_ = 0, last_prog_ = 0;
  std::deque<uint64_t> window_;
};
}  // namespace

RescueEngine::RescueEngine(BlockDevicePtr input, BlockDevicePtr output, Mapfile* map, RescueOptions opt)
    : in_(std::move(input)), out_(std::move(output)), map_(map), opt_(std::move(opt)) {}

RescueResult rescue_preflight(const BlockDevicePtr& in, const BlockDevicePtr& out, const Mapfile& map, const RescueOptions& opt) {
  Range rg;
  return preflight_impl(in, out, map, opt, &rg);
}

RescueResult RescueEngine::run(RescueControl* ctl, const RescueHooks& hooks) {
  Range rg;
  if (!map_) return bad_arg("no-map", "mapfile is missing");
  RescueResult pre = preflight_impl(in_, out_, *map_, opt_, &rg);
  if (pre.stop == RescueStop::BadArgument) return pre;
  Runner r(in_, out_, map_, opt_, rg, ctl, hooks);
  return r.run();
}

}  // namespace usbmem
