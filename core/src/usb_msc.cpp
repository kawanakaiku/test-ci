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
uint32_t get16be(const uint8_t* p) { return (static_cast<uint32_t>(p[0]) << 8) | p[1]; }
uint32_t get16le(const uint8_t* p) { return p[0] | (static_cast<uint32_t>(p[1]) << 8); }
bool is_stall(const Status& s) { return s.code == Errc::Io && s.sys_errno == EPIPE; }
bool is_fatal(const Status& s) { return s.code == Errc::Disconnected; }

std::string trim(const uint8_t* p, size_t n) {
  std::string s(reinterpret_cast<const char*>(p), n);
  for (auto& c : s) if (c < 0x20 || c > 0x7E) c = ' ';
  while (!s.empty() && s.back() == ' ') s.pop_back();
  return s;
}
// 直近の open の記録 (診断用)。**スレッドごと**: 複数の USB を別スレッドで同時に開いても、別の機器の記録と混ざらない。
// open を呼んだスレッドが、失敗後すぐに同じスレッドで読む (JNI の nativeOpenUsbMsc -> nativeUsbLastOpenTrace)。docs/multi-usb.md
thread_local std::string g_last_open_trace;
}  // namespace

std::string MscDevice::last_open_trace() { return g_last_open_trace; }

// ---- CDB の許可検査 (純粋関数)
bool scsi_cdb_ata_passthrough_ok(const uint8_t* cdb, size_t len, bool allow_ata_identify) {
  if (!cdb || len == 0) return false;
  if (cdb[0] != 0xA1 && cdb[0] != 0x85) return true;   // ATA PASS-THROUGH 以外は対象外
  if (!allow_ata_identify) return false;
  // SAT: PROTOCOL=4 (PIO データイン)・EXTEND=0 (byte1=0x08)、T_DIR=1・BYTE_BLOCK=1・T_LENGTH=2 = セクタ数フィールドで 512 バイト (byte2=0x0E)。
  // レジスタ値はセクタ数=1 以外すべて 0、コマンド=0xEC (IDENTIFY DEVICE)。他のコマンドバイト・フラグの組み合わせは一切通さない。
  if (cdb[0] == 0xA1) {
    if (len != 12) return false;
    for (int i : {3, 5, 6, 7, 8, 10, 11}) if (cdb[i] != 0) return false;
    return cdb[1] == 0x08 && cdb[2] == 0x0E && cdb[4] == 1 && cdb[9] == 0xEC;
  }
  if (len != 16) return false;
  for (int i : {3, 4, 5, 7, 8, 9, 10, 11, 12, 13, 15}) if (cdb[i] != 0) return false;
  return cdb[1] == 0x08 && cdb[2] == 0x0E && cdb[6] == 1 && cdb[14] == 0xEC;
}

bool scsi_cdb_allowed_read_only(const uint8_t* cdb, size_t len, bool allow_start_stop, bool allow_ata_identify) {
  if (!cdb || len == 0) return false;
  switch (cdb[0]) {
    case 0x00: case 0x03: case 0x1A: case 0x25: case 0x28: case 0x88: return true;
    case 0x12: return len >= 6 && (cdb[1] & 0xFE) == 0;                          // INQUIRY: EVPD ビットだけ (CMDDT は不可)
    case 0x1B: return allow_start_stop;                                           // START STOP UNIT: start_stop() の中だけ (媒体の内容は変えない)
    case 0x9E: return len >= 2 && (cdb[1] & 0x1F) == 0x10;                        // SERVICE ACTION IN: READ CAPACITY(16) のみ
    case 0xA1: case 0x85: return scsi_cdb_ata_passthrough_ok(cdb, len, allow_ata_identify);
    default: return false;
  }
}

bool parse_ata_identify(const uint8_t* d, MediaInfo* out) {
  uint32_t w0 = get16le(d), w169 = get16le(d + 169 * 2), w217 = get16le(d + 217 * 2), w255 = get16le(d + 255 * 2);
  bool zero = true;
  unsigned sum = 0;
  for (int i = 0; i < 512; i++) { if (d[i]) zero = false; sum += d[i]; }
  if (zero) return false;                         // 全部 0 = 何も返していない
  if (w0 & 0x8000) return false;                  // ATAPI など ATA ディスクではない
  if ((w255 & 0xFF) == 0xA5 && (sum & 0xFF) != 0) return false;   // 完全性シグネチャがあるのにチェックサムが合わない
  if (out) { out->ata_word0 = static_cast<int32_t>(w0); out->ata_word169 = static_cast<int32_t>(w169); out->ata_word217 = static_cast<int32_t>(w217); }
  return true;
}

std::string MediaInfo::describe() const {
  std::string o;
  char b[64];
  snprintf(b, sizeof b, "rmb=%d ver=%02x", removable ? 1 : 0, inquiry_version); o += b;
  if (!probed) return o + " (not probed)";
  o += " vpd=";
  if (!vpd_tried) o += "skip";
  else if (!vpd_supported) o += "none";
  else { for (size_t i = 0; i < vpd_pages.size(); i++) { snprintf(b, sizeof b, "%s%02x", i ? "," : "", vpd_pages[i]); o += b; } }
  if (rotation_rate_raw >= 0) { snprintf(b, sizeof b, " b1=0x%04x", static_cast<unsigned>(rotation_rate_raw)); o += b; }
  if (unmap_raw >= 0) { snprintf(b, sizeof b, " b2.lbpu=%d", unmap_raw); o += b; }
  if (ata_word0 >= 0) { snprintf(b, sizeof b, " ata.w0=0x%04x w169=0x%04x w217=0x%04x", static_cast<unsigned>(ata_word0), static_cast<unsigned>(ata_word169), static_cast<unsigned>(ata_word217)); o += b; }
  o += " sat=";
  if (sat_skipped) o += "skip(rmb)";
  else if (!sat_tried) o += "-";
  else { if (sat_tried & 1) o += "89"; if (sat_tried & 2) o += "+12"; if (sat_tried & 4) o += "+16"; }
  o += " -> ";
  if (kind == MediaKind::Rotational) { o += "rotational"; if (rpm) o += " " + std::to_string(rpm) + "rpm"; }
  else if (kind == MediaKind::NonRotational) o += "non-rotational";
  else o += "unknown";
  static const char* const basis_name[] = {"none", "vpd-b1", "ata-info-vpd", "sat12", "sat16"};
  o += std::string(" [") + basis_name[static_cast<int>(basis) % 5] + "]";
  if (hint == MediaHint::RemovableNoVpd) o += " hint=removable";
  else if (hint == MediaHint::UnmapSupported) o += " hint=unmap";
  if (!stop_reason.empty()) o += " stop=" + stop_reason;
  return o;
}

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
  // 最後の安全弁 (1): ATA PASS-THROUGH は、読み取り専用かどうかに関わらず、probe_media() の中で IDENTIFY DEVICE (0xEC) の正規形だけを通す。
  if (!scsi_cdb_ata_passthrough_ok(cdb, cdb_len, allow_ata_identify_)) return Status::Make(Errc::InvalidArgument);
  // 最後の安全弁 (2): 読み取り専用のときは、メディアの内容を変えうるコマンドを (呼び出し側の誤りがあっても) 絶対に送らない。
  // 許可: TEST UNIT READY / REQUEST SENSE / INQUIRY / MODE SENSE(6) / READ CAPACITY(10,16) / READ(10,16)、START STOP UNIT (start_stop() の中だけ)、
  // ATA PASS-THROUGH の IDENTIFY DEVICE (probe_media() の中だけ)。詳細は scsi_cdb_allowed_read_only。
  if (read_only_ && !scsi_cdb_allowed_read_only(cdb, cdb_len, allow_start_stop_, allow_ata_identify_)) return Status::Make(Errc::ReadOnly);
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
  // 回転ディスクか・フラッシュ/SSD かの判別 (読み取り専用のコマンドだけ。1 度だけ)。この時点では read_only_ はまだ true なので許可リストが効く。
  // 判別の失敗は open の失敗にしない (Unknown のまま続ける)。デバイスが抜けた (Disconnected) ときだけ伝える。
  if (opt_.probe_media) probe_media(inq);
  else { media_ = MediaInfo{}; media_.removable = (inq[1] & 0x80) != 0; media_.inquiry_version = inq[2]; }
  if (dead_) return Status::Make(Errc::Disconnected);
  read_only_ = opt_.read_only || write_protected_;

  if (opt_.max_transfer < block_size_) opt_.max_transfer = block_size_;
  opt_.max_transfer -= opt_.max_transfer % block_size_;
  if (opt_.max_read_transfer == 0) opt_.max_read_transfer = opt_.max_transfer;
  else { if (opt_.max_read_transfer < block_size_) opt_.max_read_transfer = block_size_; opt_.max_read_transfer -= opt_.max_read_transfer % block_size_; }
  return Status::Ok();
}

// ---- 種別判別 (docs/media-kind.md)。すべて読み取り専用のコマンド。1 コマンド 1 回 (再試行なし)・probe_timeout_ms の上限。
// 想定外の応答 (転送エラー・タイムアウト・CSW 不整合・不正な長さ) が 1 つでもあれば、そこで判別を打ち切って Unknown にする。
MscDevice::ProbeResult MscDevice::probe_cmd(const uint8_t* cdb, uint8_t cdb_len, void* data, size_t len, const char* what) {
  ProbeResult r;
  size_t n = 0;
  bool cc = false;
  Status st = exec(cdb, cdb_len, UsbDir::In, data, len, &n, opt_.probe_timeout_ms, &cc);
  auto stop = [&](const std::string& why) { if (media_.stop_reason.empty()) media_.stop_reason = std::string(what) + ": " + why; };
  if (dead_) { stop("disconnected"); return r; }
  if (!cc) {
    if (!st.ok()) { stop(errc_name(st.code)); return r; }     // 転送エラー・タイムアウト・CSW 不整合 (exec が BOT リセット回復済み)
    r.kind = ProbeResult::Ok; r.n = n;
    return r;
  }
  ScsiSense s;
  if (!request_sense(&s).ok()) { stop("request sense failed"); return r; }
  if (s.key == 0x05) { r.kind = ProbeResult::IllegalRequest; return r; }   // 未対応
  char b[48]; snprintf(b, sizeof b, "sense %x/%02x/%02x", s.key, s.asc, s.ascq);
  stop(b);                                                  // それ以外 (UNIT ATTENTION など) も、再試行せず打ち切る
  return r;
}

// VPD ページを 1 回読む。true = 応答が妥当 (ページ番号・タイプ・長さ)。false のとき *res に未対応か打ち切りかが入る。
bool MscDevice::probe_vpd(uint8_t page, uint8_t* buf, size_t len, size_t min_len, ProbeResult* res) {
  std::memset(buf, 0, len);
  uint8_t cdb[6] = {0x12, 0x01, page, static_cast<uint8_t>(len >> 8), static_cast<uint8_t>(len), 0};
  char what[24]; snprintf(what, sizeof what, "vpd 0x%02x", page);
  *res = probe_cmd(cdb, 6, buf, len, what);
  if (res->kind != ProbeResult::Ok) return false;
  // 妥当性: 長さ、ペリフェラルタイプ (標準 INQUIRY と同じ)、ページ番号
  if (res->n < min_len || res->n > len || (buf[0] >> 5) != 0 || buf[1] != page || (buf[0] & 0x1F) != (media_.inquiry_ptype & 0x1F)) {
    res->kind = ProbeResult::Abort;
    char b[64]; snprintf(b, sizeof b, "%s: invalid response (len=%zu)", what, res->n);
    if (media_.stop_reason.empty()) media_.stop_reason = b;
    return false;
  }
  return true;
}

void MscDevice::probe_media(const uint8_t* inq) {
  media_ = MediaInfo{};
  media_.probed = true;
  media_.removable = (inq[1] & 0x80) != 0;
  media_.inquiry_version = inq[2];
  media_.inquiry_ptype = inq[0] & 0x1F;

  auto decide_rate = [&](uint32_t rate, MediaBasis basis) {   // 回転数フィールドの解釈 (VPD 0xB1 と IDENTIFY 語 217 で同じ)
    if (rate == 0x0001) { media_.kind = MediaKind::NonRotational; media_.basis = basis; return true; }
    if (rate >= 0x0401 && rate <= 0xFFFE) { media_.kind = MediaKind::Rotational; media_.rpm = rate; media_.basis = basis; return true; }
    return false;   // 0 = 未報告、0x0002-0x0400 / 0xFFFF = 予約: 判定に使わない
  };
  auto finish = [&] {
    if (media_.kind == MediaKind::Unknown) {   // 手掛かり (確定ではない)
      if (media_.removable) media_.hint = MediaHint::RemovableNoVpd;
      else if (media_.unmap_raw == 1 || (media_.ata_word169 >= 0 && (media_.ata_word169 & 1))) media_.hint = MediaHint::UnmapSupported;
    }
  };
  auto has = [&](uint8_t pg) { return std::find(media_.vpd_pages.begin(), media_.vpd_pages.end(), pg) != media_.vpd_pages.end(); };

  uint8_t buf[600];
  ProbeResult r;

  // a. 標準 INQUIRY は済み (RMB / VERSION)。b. VPD 0x00: 対応ページ。Linux と同じく VERSION が SPC-2 (4) 以上の機器にだけ問い合わせる
  //    (古い USB メモリには EVPD を受け付けず固まるものがある)。
  if ((inq[2] & 0x07) >= 4) {
    media_.vpd_tried = true;
    if (probe_vpd(0x00, buf, 64, 4, &r)) {
      uint32_t plen = get16be(buf + 2);
      if (plen <= 60 && r.n < 4 + plen) { media_.stop_reason = "vpd 0x00: invalid response (page length)"; finish(); return; }
      size_t avail = std::min<size_t>(plen, r.n - 4);
      media_.vpd_pages.assign(buf + 4, buf + 4 + avail);
      media_.vpd_supported = true;
    } else if (r.kind != ProbeResult::IllegalRequest) { finish(); return; }
  }

  // b. Block Device Characteristics (0xB1): MEDIUM ROTATION RATE
  if (media_.vpd_supported && has(0xB1)) {
    if (probe_vpd(0xB1, buf, 64, 8, &r)) {
      media_.rotation_rate_raw = static_cast<int32_t>(get16be(buf + 4));
      if (decide_rate(get16be(buf + 4), MediaBasis::VpdBlockDeviceChars)) { finish(); return; }
    } else if (r.kind != ProbeResult::IllegalRequest) { finish(); return; }
  }

  // c. ATA の IDENTIFY DEVICE (語 217)。まず ATA Information VPD (0x89。読み取りだけで済む)、無ければ SAT の ATA PASS-THROUGH。
  //    RMB=1 の機器は USB メモリの可能性が高く、未知のコマンドで固まる恐れがあるので、0x89 で SAT を名乗っていない限り PASS-THROUGH は送らない。
  uint8_t ident[512];
  bool have_ident = false;
  MediaBasis ident_basis = MediaBasis::None;
  if (media_.vpd_supported && has(0x89)) {
    media_.sat_tried |= 1;
    if (probe_vpd(0x89, buf, 572, 572, &r)) {
      // 56: コマンドコード (0xEC = ATA ディスク / 0xA1 = ATAPI)、60..571: IDENTIFY の 512 バイト
      if (buf[56] == 0xEC && get16be(buf + 2) >= 0x238) {
        std::memcpy(ident, buf + 60, 512);
        if (parse_ata_identify(ident, &media_)) { have_ident = true; ident_basis = MediaBasis::AtaInfoVpd; }
        else { media_.stop_reason = "vpd 0x89: invalid IDENTIFY data"; finish(); return; }
      } else if (buf[56] == 0xA1) { finish(); return; }   // ATAPI (光学ドライブなど): 回転数は分からない
      else { media_.stop_reason = "vpd 0x89: invalid response (command code)"; finish(); return; }
    } else if (r.kind != ProbeResult::IllegalRequest) { finish(); return; }
  }
  if (!have_ident) {
    if (media_.removable && !(media_.vpd_supported && has(0x89))) {
      media_.sat_skipped = true;
    } else {
      struct { uint8_t cdb[16]; uint8_t len; uint8_t bit; MediaBasis basis; const char* what; } tries[2] = {
        {{0xA1, 0x08, 0x0E, 0, 1, 0, 0, 0, 0, 0xEC, 0, 0}, 12, 2, MediaBasis::AtaPassThrough12, "sat12 identify"},
        {{0x85, 0x08, 0x0E, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0xEC, 0}, 16, 4, MediaBasis::AtaPassThrough16, "sat16 identify"},
      };
      for (auto& t : tries) {   // (12) が ILLEGAL REQUEST のときだけ (16) を 1 回。それぞれ再試行なし
        media_.sat_tried |= t.bit;
        std::memset(ident, 0, sizeof ident);
        allow_ata_identify_ = true;
        r = probe_cmd(t.cdb, t.len, ident, sizeof ident, t.what);
        allow_ata_identify_ = false;
        if (r.kind == ProbeResult::Ok) {
          if (r.n == sizeof ident && parse_ata_identify(ident, &media_)) { have_ident = true; ident_basis = t.basis; }
          else if (media_.stop_reason.empty()) media_.stop_reason = std::string(t.what) + ": invalid IDENTIFY data";
          break;
        }
        if (r.kind != ProbeResult::IllegalRequest) break;
      }
      if (!have_ident && !media_.stop_reason.empty()) { finish(); return; }
    }
  }
  if (have_ident && decide_rate(static_cast<uint32_t>(media_.ata_word217), ident_basis)) { finish(); return; }

  // d. 決め手が無かった: Logical Block Provisioning (0xB2) の UNMAP 対応は SSD/フラッシュのヒント
  if (media_.vpd_supported && has(0xB2)) {
    if (probe_vpd(0xB2, buf, 64, 8, &r)) media_.unmap_raw = (buf[5] & 0x80) ? 1 : 0;
  }
  finish();
}

Status MscDevice::open(std::unique_ptr<BulkTransport> transport, const MscOptions& opt, BlockDevicePtr* out) {
  if (!transport || !out) return Status::Make(Errc::InvalidArgument);
  std::unique_ptr<MscDevice> d(new (std::nothrow) MscDevice(std::move(transport), opt));
  if (!d) return Status::Make(Errc::NoMemory);
  Status st = d->initialize();
  { g_last_open_trace = d->trace_ + (st.ok() ? " [open ok]" : std::string(" [open failed: ") + errc_name(st.code) + "/" + std::to_string(st.sys_errno) + "]"); }
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
