// SPDX-License-Identifier: Apache-2.0
#include "usbmem/usb_msc.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <cerrno>
#include <cstring>
#include <thread>
#include <vector>

namespace usbmem {
namespace {
constexpr uint32_t kCbwSig = 0x43425355;  // "USBC"
constexpr uint32_t kCswSig = 0x53425355;  // "USBS"
constexpr size_t kCbwLen = 31, kCswLen = 13;

void put32le(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = static_cast<uint8_t>(v >> (8 * i)); }
uint32_t get32le(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
void put32be(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = static_cast<uint8_t>(v >> (24 - 8 * i)); }
void put64be(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = static_cast<uint8_t>(v >> (56 - 8 * i)); }
uint32_t get32be(const uint8_t* p) { return (static_cast<uint32_t>(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
uint64_t get64be(const uint8_t* p) { return (static_cast<uint64_t>(get32be(p)) << 32) | get32be(p + 4); }
bool is_stall(const Status& s) { return s.code == Errc::Io && s.sys_errno == EPIPE; }
bool is_fatal(const Status& s) { return s.code == Errc::Disconnected; }

std::string trim(const uint8_t* p, size_t n) {
  std::string s(reinterpret_cast<const char*>(p), n);
  for (auto& c : s) if (c < 0x20 || c > 0x7E) c = ' ';
  while (!s.empty() && s.back() == ' ') s.pop_back();
  return s;
}
std::mutex g_trace_mu;
std::string g_last_open_trace;
}  // namespace

std::string MscDevice::last_open_trace() { std::lock_guard<std::mutex> g(g_trace_mu); return g_last_open_trace; }

// ---- BOT: 1 コマンド (CBW → データ → CSW)。check_condition=true で CSW が Command Failed のとき呼び出し側が REQUEST SENSE する。
Status MscDevice::exec(const uint8_t* cdb, uint8_t cdb_len, UsbDir dir, void* data, size_t len, size_t* transferred,
                       uint32_t timeout_ms, bool* check_condition) {
  phase_ = '-';
  Status st = exec_impl(cdb, cdb_len, dir, data, len, transferred, timeout_ms, check_condition);
  // 診断用の記録 (オペコード・長さ・失敗した段階・結果)。秘密情報は含まない。成功した READ は件数だけで足りるので記録しない。
  if (!st.ok() || (cdb_len && cdb[0] != 0x28 && cdb[0] != 0x88)) {
    if (trace_.size() < 3000) {
      char b[96];
      snprintf(b, sizeof b, "%s%02x len=%zu ph=%c -> %s/%d%s", trace_.empty() ? "" : "; ", cdb_len ? cdb[0] : 0, len, phase_, errc_name(st.code), st.sys_errno,
               (check_condition && *check_condition) ? " CC" : "");
      trace_ += b;
    }
  }
  return st;
}

Status MscDevice::exec_impl(const uint8_t* cdb, uint8_t cdb_len, UsbDir dir, void* data, size_t len, size_t* transferred,
                            uint32_t timeout_ms, bool* check_condition) {
  if (check_condition) *check_condition = false;
  if (transferred) *transferred = 0;
  if (dead_) return Status::Make(Errc::Disconnected);
  if (cdb_len == 0 || cdb_len > 16 || len > 0xFFFFFFFFu) return Status::Make(Errc::InvalidArgument);
  // 最後の安全弁: 読み取り専用のときは、メディアの内容を変えうるコマンドを (呼び出し側の誤りがあっても) 絶対に送らない。
  // 許可: TEST UNIT READY / REQUEST SENSE / INQUIRY / MODE SENSE(6) / READ CAPACITY(10,16) / READ(10,16)
  if (read_only_) {
    switch (cdb[0]) {
      case 0x00: case 0x03: case 0x12: case 0x1A: case 0x25: case 0x28: case 0x88: case 0x9E: break;
      case 0x1B: if (allow_start_stop_) break; return Status::Make(Errc::ReadOnly);   // START STOP UNIT: start_stop() の中だけ (媒体の内容は変えない)
      default: return Status::Make(Errc::ReadOnly);
    }
    if (cdb[0] == 0x9E && (cdb_len < 2 || (cdb[1] & 0x1F) != 0x10)) return Status::Make(Errc::ReadOnly);   // SERVICE ACTION IN: READ CAPACITY(16) のみ
  }
  op_counts_[cdb[0]]++;
  uint8_t cbw[kCbwLen] = {0};
  uint32_t tag = tag_++;
  put32le(cbw, kCbwSig); put32le(cbw + 4, tag); put32le(cbw + 8, static_cast<uint32_t>(len));
  cbw[12] = (dir == UsbDir::In && len) ? 0x80 : 0x00;
  cbw[13] = opt_.lun; cbw[14] = cdb_len;
  std::memcpy(cbw + 15, cdb, cdb_len);

  size_t done = 0;
  phase_ = 'C';   // CBW 送信
  Status st = t_->bulk_out(cbw, kCbwLen, &done, timeout_ms);
  if (is_fatal(st)) { dead_ = true; return st; }
  if (!st.ok() || done != kCbwLen) { reset_recovery(); return st.ok() ? Status::Make(Errc::Io, EIO) : st; }

  size_t moved = 0;
  phase_ = 'D';   // データ段階
  if (len) {
    Status ds = dir == UsbDir::In ? t_->bulk_in(data, len, &moved, timeout_ms) : t_->bulk_out(data, len, &moved, timeout_ms);
    if (is_fatal(ds)) { dead_ = true; return ds; }
    if (is_stall(ds)) {
      Status c = t_->clear_halt(dir == UsbDir::In ? UsbDir::In : UsbDir::Out);  // 仕様 6.7.2/6.7.3: halt を解除して CSW を読む
      if (is_fatal(c)) { dead_ = true; return c; }
    } else if (!ds.ok()) {
      reset_recovery();
      return ds;
    }
  }

  phase_ = 'S';   // CSW 受信
  uint8_t csw[kCswLen] = {0};
  size_t cdone = 0;
  Status cs = t_->bulk_in(csw, kCswLen, &cdone, timeout_ms);
  if (is_stall(cs)) {  // CSW 取得時の STALL: 1 回だけ再試行 (仕様 6.7.2)
    Status c = t_->clear_halt(UsbDir::In);
    if (is_fatal(c)) { dead_ = true; return c; }
    cdone = 0;
    cs = t_->bulk_in(csw, kCswLen, &cdone, timeout_ms);
  }
  if (is_fatal(cs)) { dead_ = true; return cs; }
  if (!cs.ok() || cdone != kCswLen) { reset_recovery(); return cs.ok() ? Status::Make(Errc::Io, EIO) : cs; }
  // CSW の妥当性 (13.2): 長さ・署名・タグ。不一致はデバイスの状態が信用できないのでリセット回復。
  if (get32le(csw) != kCswSig || get32le(csw + 4) != tag) { reset_recovery(); return Status::Make(Errc::Corrupt); }
  uint32_t residue = get32le(csw + 8);
  uint8_t status = csw[12];
  if (status == 2) { reset_recovery(); return Status::Make(Errc::Io, EPROTO); }  // Phase Error
  if (status > 2) { reset_recovery(); return Status::Make(Errc::Corrupt); }
  if (residue > len) { reset_recovery(); return Status::Make(Errc::Corrupt); }
  if (transferred) *transferred = std::min<size_t>(moved, len - residue);
  if (status == 1) {
    if (check_condition) *check_condition = true;
    return Status::Make(Errc::Io, EIO);
  }
  return Status::Ok();
}

Status MscDevice::reset_recovery() {
  Status r = t_->mass_storage_reset();
  if (is_fatal(r)) { dead_ = true; return r; }
  Status a = t_->clear_halt(UsbDir::In);
  if (is_fatal(a)) { dead_ = true; return a; }
  Status b = t_->clear_halt(UsbDir::Out);
  if (is_fatal(b)) { dead_ = true; return b; }
  return r;
}

void MscDevice::tune(size_t read_chunk, uint32_t read_max_transfer) {
  if (read_chunk) t_->set_chunk(UsbDir::In, read_chunk);
  if (read_max_transfer) {
    opt_.max_read_transfer = std::max<uint32_t>(read_max_transfer, block_size_);
    opt_.max_read_transfer -= opt_.max_read_transfer % block_size_;
  }
}

std::string MscDevice::op_stats() const {
  std::string o;
  for (int i = 0; i < 256; i++) if (op_counts_[i]) { char b[24]; snprintf(b, sizeof b, "%s%02xx%u", o.empty() ? "" : " ", i, op_counts_[i]); o += b; }
  return o;
}

Status MscDevice::request_sense(ScsiSense* s) {
  uint8_t cdb[6] = {0x03, 0, 0, 0, 18, 0};
  uint8_t buf[18] = {0};
  size_t n = 0;
  bool cc = false;
  Status st = exec(cdb, 6, UsbDir::In, buf, sizeof buf, &n, opt_.io_timeout_ms, &cc);
  if (!st.ok()) return st;
  if (n < 14) { *s = ScsiSense{}; return Status::Ok(); }
  s->key = buf[2] & 0x0F; s->asc = buf[12]; s->ascq = buf[13];
  last_sense_ = *s;
  return Status::Ok();
}

Status MscDevice::map_sense(const ScsiSense& s) {
  switch (s.key) {
    case 0x02:  // NOT READY
      return s.asc == 0x3A ? Status::Make(Errc::Disconnected) : Status::Make(Errc::Io, EAGAIN);  // 3A = medium not present
    case 0x03: case 0x04: return Status::Make(Errc::Io, EIO);  // MEDIUM / HARDWARE ERROR
    case 0x05: return Status::Make(Errc::Unsupported);          // ILLEGAL REQUEST
    case 0x07: return Status::Make(Errc::ReadOnly);             // DATA PROTECT
    case 0x08: return Status::Make(Errc::Corrupt);              // BLANK CHECK
    default: return Status::Make(Errc::Io, EIO);
  }
}

// CHECK CONDITION を REQUEST SENSE で解釈し、UNIT ATTENTION (6) は 2 回まで自動再試行する。
// min_len: 応答がこの長さ以上なら、要求 (len) より短くても受け入れる。既定 (SIZE_MAX) は「len ちょうど」。
// 実機の RAID ブリッジは READ CAPACITY(16) に 32 バイトを要求しても、規格上の最小 12 バイトしか返さない。
Status MscDevice::exec_checked(const uint8_t* cdb, uint8_t cdb_len, UsbDir dir, void* data, size_t len, uint32_t timeout_ms, size_t min_len) {
  for (int attempt = 0; attempt < 3; attempt++) {
    size_t n = 0;
    bool cc = false;
    Status st = exec(cdb, cdb_len, dir, data, len, &n, timeout_ms, &cc);
    if (!cc) {
      if (st.ok() && (min_len == SIZE_MAX ? n != len : (n < min_len || n > len))) return Status::Make(Errc::Io, EIO);  // 成功なのにデータ不足は信用しない
      return st;
    }
    ScsiSense s;
    Status rs = request_sense(&s);
    if (!rs.ok()) return rs;
    if (s.key == 0x06 && attempt < 2) continue;  // UNIT ATTENTION: 状態通知なのでコマンドを再発行
    return map_sense(s);
  }
  return Status::Make(Errc::Io, EIO);
}

Status MscDevice::initialize() {
  uint8_t inq[36] = {0};
  uint8_t cdb_inq[6] = {0x12, 0, 0, 0, 36, 0};
  Status st = exec_checked(cdb_inq, 6, UsbDir::In, inq, sizeof inq, opt_.io_timeout_ms);
  if (!st.ok()) return st;
  uint8_t ptype = inq[0] & 0x1F;
  if ((inq[0] >> 5) != 0 || (ptype != 0x00 && ptype != 0x0E)) return Status::Make(Errc::Unsupported);  // 直接アクセス/RBC のみ
  vendor_ = trim(inq + 8, 8);
  product_ = trim(inq + 16, 16);

  // TEST UNIT READY: 起動直後は NOT READY / UNIT ATTENTION を返すので待つ
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(opt_.ready_wait_ms);
  for (;;) {
    uint8_t tur[6] = {0};
    size_t n = 0;
    bool cc = false;
    st = exec(tur, 6, UsbDir::In, nullptr, 0, &n, opt_.io_timeout_ms, &cc);
    if (st.ok()) break;
    if (!cc) return st;
    ScsiSense s;
    Status rs = request_sense(&s);
    if (!rs.ok()) return rs;
    if (s.key == 0x02 && s.asc == 0x3A) return Status::Make(Errc::Disconnected);  // メディアなし
    bool retryable = s.key == 0x06 || (s.key == 0x02);
    if (!retryable || std::chrono::steady_clock::now() >= deadline) return map_sense(s);
    std::this_thread::sleep_for(std::chrono::milliseconds(opt_.retry_delay_ms));
  }

  uint8_t cap10[8] = {0};
  uint8_t cdb_c10[10] = {0x25};
  st = exec_checked(cdb_c10, 10, UsbDir::In, cap10, sizeof cap10, opt_.io_timeout_ms);
  if (!st.ok()) return st;
  uint64_t last = get32be(cap10);
  uint32_t bs = get32be(cap10 + 4);
  if (last == 0xFFFFFFFFull) {  // 2TiB 超: READ CAPACITY(16)
    uint8_t cap16[32] = {0};
    uint8_t cdb_c16[16] = {0x9E, 0x10};
    put32be(cdb_c16 + 10, sizeof cap16);
    st = exec_checked(cdb_c16, 16, UsbDir::In, cap16, sizeof cap16, opt_.io_timeout_ms, 12);   // 12 = 最終 LBA (8) + ブロック長 (4)
    if (!st.ok()) return st;
    last = get64be(cap16);
    bs = get32be(cap16 + 8);
    if (last == UINT64_MAX) return Status::Make(Errc::Corrupt);
  }
  if (bs < 512 || bs > 65536 || (bs & (bs - 1))) return Status::Make(Errc::Corrupt);  // 異常なブロック長は拒否
  uint64_t blocks = last + 1;
  if (blocks > UINT64_MAX / bs) return Status::Make(Errc::Corrupt);
  blocks_ = blocks; block_size_ = bs;

  // 書き込み禁止 (MODE SENSE(6) の DEVICE-SPECIFIC PARAMETER bit7)。取得できなければ「禁止ではない」とみなすが、
  // 既定は読み取り専用なので安全側。
  uint8_t ms[192] = {0};
  uint8_t cdb_ms[6] = {0x1A, 0x08, 0x3F, 0, sizeof ms - 0, 0};
  cdb_ms[4] = 192;
  size_t n = 0;
  bool cc = false;
  Status mst = exec(cdb_ms, 6, UsbDir::In, ms, sizeof ms, &n, opt_.io_timeout_ms, &cc);
  if (cc) { ScsiSense s; request_sense(&s); }
  write_protected_ = mst.ok() && n >= 3 && (ms[2] & 0x80);
  read_only_ = opt_.read_only || write_protected_;

  if (opt_.max_transfer < block_size_) opt_.max_transfer = block_size_;
  opt_.max_transfer -= opt_.max_transfer % block_size_;
  if (opt_.max_read_transfer == 0) opt_.max_read_transfer = opt_.max_transfer;
  else { if (opt_.max_read_transfer < block_size_) opt_.max_read_transfer = block_size_; opt_.max_read_transfer -= opt_.max_read_transfer % block_size_; }
  return Status::Ok();
}

Status MscDevice::open(std::unique_ptr<BulkTransport> transport, const MscOptions& opt, BlockDevicePtr* out) {
  if (!transport || !out) return Status::Make(Errc::InvalidArgument);
  std::unique_ptr<MscDevice> d(new (std::nothrow) MscDevice(std::move(transport), opt));
  if (!d) return Status::Make(Errc::NoMemory);
  Status st = d->initialize();
  { std::lock_guard<std::mutex> g(g_trace_mu); g_last_open_trace = d->trace_ + (st.ok() ? " [open ok]" : std::string(" [open failed: ") + errc_name(st.code) + "/" + std::to_string(st.sys_errno) + "]"); }
  if (!st.ok()) return st;
  out->reset(d.release());
  return Status::Ok();
}

Status MscDevice::rw_blocks(bool write, uint64_t lba, uint32_t nblocks, void* buf, const IoOptions& io) {
  uint8_t cdb[16] = {0};
  uint8_t len;
  if (blocks_ <= 0x100000000ull && nblocks <= 0xFFFF) {  // 10 バイト CDB: LBA 32bit / 長さ 16bit
    cdb[0] = write ? 0x2A : 0x28;
    put32be(cdb + 2, static_cast<uint32_t>(lba));
    cdb[7] = static_cast<uint8_t>(nblocks >> 8); cdb[8] = static_cast<uint8_t>(nblocks);
    len = 10;
  } else {
    cdb[0] = write ? 0x8A : 0x88;
    put64be(cdb + 2, lba);
    put32be(cdb + 10, nblocks);
    len = 16;
  }
  uint32_t to = io.timeout_ms ? io.timeout_ms : opt_.io_timeout_ms;
  return exec_checked(cdb, len, write ? UsbDir::Out : UsbDir::In, buf, static_cast<size_t>(nblocks) * block_size_, to);
}

Status MscDevice::read_locked(uint64_t off, void* buf, size_t len, const IoOptions& io) {
  auto* p = static_cast<uint8_t*>(buf);
  std::vector<uint8_t> bounce;
  while (len > 0) {
    if (io.cancel && io.cancel->cancelled()) return Status::Make(Errc::Cancelled);
    uint64_t lba = off / block_size_;
    size_t in_blk = static_cast<size_t>(off % block_size_);
    if (in_blk != 0 || len < block_size_) {  // 先頭/末尾の端数: 1 ブロック読んで必要分だけ切り出す
      bounce.resize(block_size_);
      Status st = rw_blocks(false, lba, 1, bounce.data(), io);
      if (!st.ok()) return st;
      size_t n = std::min<size_t>(len, block_size_ - in_blk);
      std::memcpy(p, bounce.data() + in_blk, n);
      p += n; off += n; len -= n;
      continue;
    }
    size_t chunk = std::min<size_t>(len - len % block_size_, opt_.max_read_transfer);
    Status st = rw_blocks(false, lba, static_cast<uint32_t>(chunk / block_size_), p, io);
    if (!st.ok()) return st;
    p += chunk; off += chunk; len -= chunk;
  }
  return Status::Ok();
}

Status MscDevice::write_locked(uint64_t off, const void* buf, size_t len, const IoOptions& io) {
  auto* p = static_cast<const uint8_t*>(buf);
  std::vector<uint8_t> bounce;
  while (len > 0) {
    if (io.cancel && io.cancel->cancelled()) return Status::Make(Errc::Cancelled);
    uint64_t lba = off / block_size_;
    size_t in_blk = static_cast<size_t>(off % block_size_);
    if (in_blk != 0 || len < block_size_) {  // Read-Modify-Write
      bounce.resize(block_size_);
      Status st = rw_blocks(false, lba, 1, bounce.data(), io);
      if (!st.ok()) return st;
      size_t n = std::min<size_t>(len, block_size_ - in_blk);
      std::memcpy(bounce.data() + in_blk, p, n);
      st = rw_blocks(true, lba, 1, bounce.data(), io);
      if (!st.ok()) return st;
      p += n; off += n; len -= n;
      continue;
    }
    size_t chunk = std::min<size_t>(len - len % block_size_, opt_.max_transfer);
    // 書き込みデータは const。トランスポートは out 方向で読むだけだが、API の都合でコピーを避けるため const_cast する。
    Status st = rw_blocks(true, lba, static_cast<uint32_t>(chunk / block_size_), const_cast<uint8_t*>(p), io);
    if (!st.ok()) return st;
    p += chunk; off += chunk; len -= chunk;
  }
  return Status::Ok();
}

Status MscDevice::read_at(uint64_t off, void* buf, size_t len, const IoOptions& io) {
  std::lock_guard<std::mutex> lk(mu_);
  if (closed_) return Status::Make(Errc::Closed);
  if (dead_) return Status::Make(Errc::Disconnected);
  if (!range_ok(size(), off, len)) return Status::Make(Errc::OutOfRange);
  if (Status rs = resume_if_stopped_locked(); !rs.ok()) return rs;
  Status rst = read_locked(off, buf, len, io);
  last_io_ = std::chrono::steady_clock::now();
  return rst;
}

Status MscDevice::write_at(uint64_t off, const void* buf, size_t len, const IoOptions& io) {
  if (read_only_) return Status::Make(Errc::ReadOnly);
  std::lock_guard<std::mutex> lk(mu_);
  if (closed_) return Status::Make(Errc::Closed);
  if (dead_) return Status::Make(Errc::Disconnected);
  if (!range_ok(size(), off, len)) return Status::Make(Errc::OutOfRange);
  if (Status rs = resume_if_stopped_locked(); !rs.ok()) return rs;
  Status wst = write_locked(off, buf, len, io);
  last_io_ = std::chrono::steady_clock::now();
  return wst;
}

Status MscDevice::flush() {
  if (read_only_) return Status::Ok();
  std::lock_guard<std::mutex> lk(mu_);
  if (closed_) return Status::Make(Errc::Closed);
  if (dead_) return Status::Make(Errc::Disconnected);
  if (spun_down_) return Status::Ok();   // 止める前に SYNCHRONIZE CACHE 済み。以後の書き込みは自動再開してから行うので、ここで起こさない
  uint8_t cdb[10] = {0x35};  // SYNCHRONIZE CACHE(10): LBA 0 / 長さ 0 = 全体
  size_t n = 0;
  bool cc = false;
  Status st = exec(cdb, 10, UsbDir::Out, nullptr, 0, &n, opt_.io_timeout_ms, &cc);
  if (!cc) return st;
  ScsiSense s;
  Status rs = request_sense(&s);
  if (!rs.ok()) return rs;
  if (s.key == 0x05) return Status::Ok();  // 未対応デバイスは書き込みキャッシュなしとみなす
  return map_sense(s);
}

Status MscDevice::start_stop(bool start) {
  std::lock_guard<std::mutex> lk(mu_);
  return start_stop_locked(start);
}

bool MscDevice::spun_down() const {
  std::lock_guard<std::mutex> lk(mu_);
  return spun_down_;
}

Status MscDevice::resume_if_stopped_locked() {
  last_io_ = std::chrono::steady_clock::now();
  if (!spun_down_) return Status::Ok();
  Status st = start_stop_locked(true);   // 失敗したら I/O もエラー (停止したまま READ しても失敗するため)
  last_io_ = std::chrono::steady_clock::now();
  return st;
}

int MscDevice::idle_stop(uint64_t idle_ms) {
  std::unique_lock<std::mutex> lk(mu_, std::try_to_lock);
  if (!lk.owns_lock()) return 0;   // I/O 実行中 = 使用中
  if (closed_ || dead_ || spun_down_ || stop_unsupported_) return 0;
  auto idle = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - last_io_).count();
  if (idle < 0 || static_cast<uint64_t>(idle) < idle_ms) return 0;
  Status st = start_stop_locked(false);
  if (st.ok()) return 1;
  if (st.code == Errc::Unsupported) stop_unsupported_ = true;
  return -static_cast<int>(st.code);
}

Status MscDevice::start_stop_locked(bool start) {
  if (closed_) return Status::Make(Errc::Closed);
  if (dead_) return Status::Make(Errc::Disconnected);
  size_t n = 0;
  bool cc = false;
  if (!start && !read_only_) {   // 止める前に、書き込みキャッシュをディスクへ (未対応なら無視)
    uint8_t sc[10] = {0x35};
    Status fs = exec(sc, 10, UsbDir::Out, nullptr, 0, &n, opt_.io_timeout_ms, &cc);
    if (cc) { ScsiSense fsn; Status rs = request_sense(&fsn); if (!rs.ok()) return rs; if (fsn.key != 0x05) { Status ms = map_sense(fsn); if (!ms.ok()) return ms; } }
    else if (!fs.ok()) return fs;
  }
  uint8_t cdb[6] = {0x1B, 0x00 /* IMMED=0: 完了まで待つ */, 0, 0, static_cast<uint8_t>(start ? 0x01 : 0x00) /* START ビット、LOEJ=0 */, 0};
  allow_start_stop_ = true;
  Status st = exec(cdb, 6, UsbDir::Out, nullptr, 0, &n, std::max<uint32_t>(opt_.io_timeout_ms, 30000), &cc);   // 回転の停止/開始は時間がかかる
  allow_start_stop_ = false;
  if (!cc) { if (st.ok()) { spun_down_ = !start; if (start) last_io_ = std::chrono::steady_clock::now(); } return st; }
  ScsiSense s;
  Status rs = request_sense(&s);
  if (!rs.ok()) return rs;
  if (s.key == 0x05) { stop_unsupported_ = true; return Status::Make(Errc::Unsupported); }   // ILLEGAL REQUEST: このデバイス (ブリッジ) は START STOP UNIT に対応していない
  Status ms = map_sense(s);
  if (ms.ok()) { spun_down_ = !start; if (start) last_io_ = std::chrono::steady_clock::now(); }   // 手動の再開もアイドル時計をリセット
  return ms;
}

Status MscDevice::close() {
  std::lock_guard<std::mutex> lk(mu_);
  closed_ = true;
  return Status::Ok();
}

std::string MscDevice::describe() const {
  return "usb-msc(" + vendor_ + " " + product_ + ", " + (read_only_ ? "ro" : "rw") + ", " + std::to_string(blocks_) + "x" +
         std::to_string(block_size_) + ")";
}

}  // namespace usbmem
