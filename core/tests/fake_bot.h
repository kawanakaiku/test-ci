// SPDX-License-Identifier: Apache-2.0
// テスト用: USB Mass Storage Bulk-Only デバイスのソフトウェア模擬 (SCSI 透過コマンドセットの最小実装)。
// 疎なブロックストア (未書き込みブロックは LBA 由来の決定的パターン) なので、2TiB 超の容量でもメモリを使わない。
#pragma once
#include <cstring>
#include <functional>
#include <map>
#include <set>

#include "usbmem/usb_msc.h"

namespace fake {
using namespace usbmem;

struct Faults {
  int stall_data_in_once = 0;        // 次の n 回の READ データ段で STALL を返す
  int stall_csw_once = 0;            // 次の n 回の CSW 読み出しで STALL
  bool bad_csw_tag = false;          // CSW のタグを壊す
  bool bad_csw_sig = false;
  bool phase_error = false;          // CSW status = 2
  int unit_attention_first = 0;      // 最初の n コマンドを UNIT ATTENTION で失敗させる
  int not_ready_first = 0;           // 最初の n 回の TEST UNIT READY を NOT READY(becoming ready) にする
  std::set<uint64_t> medium_error_lbas;  // これらの LBA を含む READ/WRITE は MEDIUM ERROR
  bool write_protected = false;      // MODE SENSE が WP を返し、WRITE は DATA PROTECT
  bool medium_not_present = false;
  int disconnect_after_commands = -1;    // n コマンド目以降は Disconnected
  bool short_cbw_accept = false;     // CBW の bulk_out が 30 バイトしか進まない
  bool lie_residue = false;          // CSW の residue を len より大きく返す
  bool no_sync_cache = false;        // SYNCHRONIZE CACHE を ILLEGAL REQUEST にする
  bool short_cap16 = false;          // READ CAPACITY(16) を規格の最小 12 バイトだけ返す (実機の RAID ブリッジの挙動)
};

class BotDevice : public BulkTransport {
 public:
  BotDevice(uint64_t blocks, uint32_t bs) : blocks_(blocks), bs_(bs) {}
  Faults f;
  uint64_t blocks_;
  uint32_t bs_;
  std::map<uint64_t, std::vector<uint8_t>> written;  // 書き込まれたブロック
  std::vector<uint8_t> log_ops;                       // 受け取った SCSI opcode の履歴
  int resets = 0, clear_in = 0, clear_out = 0, max_data_len = 0;
  int sync_count = 0;

  // 設定すると、未書き込みブロックの内容をこの関数から得る (実イメージを USB デバイスとして見せる統合テスト用)
  std::function<void(uint64_t lba, uint8_t* out)> backing;
  static uint8_t pattern(uint64_t lba, uint32_t off) { return static_cast<uint8_t>((lba * 131 + off * 7 + (lba >> 8)) & 0xFF); }
  std::vector<uint8_t> block(uint64_t lba) const {
    auto it = written.find(lba);
    if (it != written.end()) return it->second;
    std::vector<uint8_t> b(bs_);
    if (backing) { backing(lba, b.data()); return b; }
    for (uint32_t i = 0; i < bs_; i++) b[i] = pattern(lba, i);
    return b;
  }

  Status bulk_out(const void* buf, size_t len, size_t* done, uint32_t) override {
    *done = 0;
    if (dead()) return Status::Make(Errc::Disconnected);
    auto* p = static_cast<const uint8_t*>(buf);
    if (state_ == St::Idle) {  // CBW
      if (len != 31 || rd32(p) != 0x43425355) return Status::Make(Errc::Io, EPIPE);
      cmds_++;
      if (dead()) return Status::Make(Errc::Disconnected);
      tag_ = rd32(p + 4); dlen_ = rd32(p + 8); dir_in_ = (p[12] & 0x80) != 0; cdb_.assign(p + 15, p + 15 + p[14]);
      start_command();
      *done = f.short_cbw_accept ? 30 : len;
      return Status::Ok();
    }
    if (state_ == St::DataOut) {
      outbuf_.insert(outbuf_.end(), p, p + len);
      *done = len;
      if (outbuf_.size() >= dlen_) finish_data_out();
      return Status::Ok();
    }
    return Status::Make(Errc::Io, EPIPE);
  }

  Status bulk_in(void* buf, size_t len, size_t* done, uint32_t) override {
    *done = 0;
    if (dead()) return Status::Make(Errc::Disconnected);
    auto* p = static_cast<uint8_t*>(buf);
    if (state_ == St::DataIn) {
      if (f.stall_data_in_once > 0 && cdb_[0] != 0x03) { f.stall_data_in_once--; state_ = St::Csw; csw_status_ = 1; stalled_in_ = true; return Status::Make(Errc::Io, EPIPE); }
      size_t n = std::min(len, inbuf_.size());
      std::memcpy(p, inbuf_.data(), n);
      *done = n;
      state_ = St::Csw;
      residue_ = dlen_ - static_cast<uint32_t>(n);
      return Status::Ok();
    }
    if (state_ == St::Csw) {
      if (stalled_in_) { return Status::Make(Errc::Io, EPIPE); }  // clear_halt まで STALL のまま
      if (f.stall_csw_once > 0) { f.stall_csw_once--; stalled_in_ = true; return Status::Make(Errc::Io, EPIPE); }
      uint8_t csw[13] = {0};
      wr32(csw, f.bad_csw_sig ? 0xDEADBEEF : 0x53425355);
      wr32(csw + 4, f.bad_csw_tag ? tag_ + 1 : tag_);
      wr32(csw + 8, f.lie_residue ? dlen_ + 10 : residue_);
      csw[12] = f.phase_error ? 2 : csw_status_;
      std::memcpy(p, csw, std::min<size_t>(len, 13));
      *done = std::min<size_t>(len, 13);
      state_ = St::Idle;
      return Status::Ok();
    }
    return Status::Make(Errc::Io, EPIPE);
  }

  Status clear_halt(UsbDir d) override {
    if (dead()) return Status::Make(Errc::Disconnected);
    if (d == UsbDir::In) { clear_in++; stalled_in_ = false; } else clear_out++;
    return Status::Ok();
  }
  Status mass_storage_reset() override {
    if (dead()) return Status::Make(Errc::Disconnected);
    resets++; state_ = St::Idle; stalled_in_ = false;
    return Status::Ok();
  }

 private:
  enum class St { Idle, DataIn, DataOut, Csw } state_ = St::Idle;
  uint32_t tag_ = 0, dlen_ = 0, residue_ = 0;
  bool dir_in_ = false, stalled_in_ = false;
  uint8_t csw_status_ = 0;
  std::vector<uint8_t> cdb_, inbuf_, outbuf_;
  int cmds_ = 0, ua_left_ = -1, nr_left_ = -1;
  uint8_t sk_ = 0, asc_ = 0, ascq_ = 0;
  uint8_t pending_sense_[3] = {0, 0, 0};
  bool have_sense_ = false;

  bool dead() const { return f.disconnect_after_commands >= 0 && cmds_ > f.disconnect_after_commands; }
  static uint32_t rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
  static void wr32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = static_cast<uint8_t>(v >> (8 * i)); }
  static uint32_t be32(const uint8_t* p) { return (static_cast<uint32_t>(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
  static uint64_t be64(const uint8_t* p) { return (static_cast<uint64_t>(be32(p)) << 32) | be32(p + 4); }
  static void wbe32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = static_cast<uint8_t>(v >> (24 - 8 * i)); }
  static void wbe64(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = static_cast<uint8_t>(v >> (56 - 8 * i)); }

  void fail(uint8_t sk, uint8_t asc, uint8_t ascq) { csw_status_ = 1; pending_sense_[0] = sk; pending_sense_[1] = asc; pending_sense_[2] = ascq; have_sense_ = true; residue_ = dlen_; inbuf_.clear(); state_ = St::Csw;
    if (dir_in_ && dlen_) stalled_in_ = true;  // BOT 6.7.2: データ段が残っているのに失敗 → デバイスは IN を STALL する
  }
  void data_in(std::vector<uint8_t> d) { inbuf_ = std::move(d); if (inbuf_.size() > dlen_) inbuf_.resize(dlen_); csw_status_ = 0; state_ = dlen_ ? St::DataIn : St::Csw; }

  void start_command() {
    uint8_t op = cdb_[0];
    log_ops.push_back(op);
    max_data_len = std::max<int>(max_data_len, static_cast<int>(dlen_));
    stalled_in_ = false; csw_status_ = 0; residue_ = 0; outbuf_.clear();
    if (op != 0x03 && f.unit_attention_first > 0) { f.unit_attention_first--; fail(0x06, 0x29, 0x00); return; }
    if (f.medium_not_present && op != 0x03 && op != 0x12) { fail(0x02, 0x3A, 0x00); return; }
    switch (op) {
      case 0x12: {  // INQUIRY
        std::vector<uint8_t> d(36, 0);
        d[4] = 31;
        std::memcpy(&d[8], "FAKEVEND", 8); std::memcpy(&d[16], "FAKE-BOT-DISK   ", 16); std::memcpy(&d[32], "0001", 4);
        data_in(d); break;
      }
      case 0x00:  // TEST UNIT READY
        if (f.not_ready_first > 0) { f.not_ready_first--; fail(0x02, 0x04, 0x01); } else { csw_status_ = 0; state_ = St::Csw; }
        break;
      case 0x03: {  // REQUEST SENSE
        std::vector<uint8_t> d(18, 0);
        d[0] = 0x70; d[7] = 10;
        if (have_sense_) { d[2] = pending_sense_[0]; d[12] = pending_sense_[1]; d[13] = pending_sense_[2]; have_sense_ = false; }
        data_in(d); break;
      }
      case 0x25: {  // READ CAPACITY(10)
        std::vector<uint8_t> d(8);
        wbe32(&d[0], blocks_ - 1 > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(blocks_ - 1));
        wbe32(&d[4], bs_);
        data_in(d); break;
      }
      case 0x9E: {  // SERVICE ACTION IN(16): READ CAPACITY(16)
        std::vector<uint8_t> d(f.short_cap16 ? 12 : 32, 0);
        wbe64(&d[0], blocks_ - 1); wbe32(&d[8], bs_);
        data_in(d); break;
      }
      case 0x1A: {  // MODE SENSE(6)
        std::vector<uint8_t> d(4, 0);
        d[0] = 3; d[2] = f.write_protected ? 0x80 : 0x00;
        data_in(d); break;
      }
      case 0x28: case 0x88: {  // READ(10/16)
        uint64_t lba = op == 0x28 ? be32(&cdb_[2]) : be64(&cdb_[2]);
        uint32_t n = op == 0x28 ? static_cast<uint32_t>((cdb_[7] << 8) | cdb_[8]) : be32(&cdb_[10]);
        if (lba + n > blocks_) { fail(0x05, 0x21, 0x00); return; }  // LBA OUT OF RANGE
        std::vector<uint8_t> d;
        for (uint32_t i = 0; i < n; i++) {
          if (f.medium_error_lbas.count(lba + i)) { fail(0x03, 0x11, 0x00); return; }
          auto b = block(lba + i);
          d.insert(d.end(), b.begin(), b.end());
        }
        data_in(d); break;
      }
      case 0x2A: case 0x8A:  // WRITE(10/16): データ段を待つ
        if (f.write_protected) { fail(0x07, 0x27, 0x00); return; }
        state_ = dlen_ ? St::DataOut : St::Csw;
        break;
      case 0x35:  // SYNCHRONIZE CACHE(10)
        sync_count++;
        if (f.no_sync_cache) { fail(0x05, 0x20, 0x00); return; }
        state_ = St::Csw; break;
      default: fail(0x05, 0x20, 0x00);  // INVALID COMMAND OPERATION CODE
    }
  }

  void finish_data_out() {
    uint8_t op = cdb_[0];
    uint64_t lba = op == 0x2A ? be32(&cdb_[2]) : be64(&cdb_[2]);
    uint32_t n = op == 0x2A ? static_cast<uint32_t>((cdb_[7] << 8) | cdb_[8]) : be32(&cdb_[10]);
    state_ = St::Csw; csw_status_ = 0; residue_ = 0;
    if (lba + n > blocks_) { fail(0x05, 0x21, 0x00); return; }
    for (uint32_t i = 0; i < n; i++) {
      if (f.medium_error_lbas.count(lba + i)) { fail(0x03, 0x0C, 0x00); return; }
      written[lba + i] = std::vector<uint8_t>(outbuf_.begin() + static_cast<long>(i) * bs_, outbuf_.begin() + static_cast<long>(i + 1) * bs_);
    }
  }
};
}  // namespace fake
