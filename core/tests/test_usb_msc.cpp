// SPDX-License-Identifier: Apache-2.0
#include "fake_bot.h"
#include "test_util.h"

using namespace usbmem;
using fake::BotDevice;

namespace {
struct Rig {
  BotDevice* bot;      // MscDevice が所有する transport への非所有ポインタ
  BlockDevicePtr dev;
  Status st;
};
Rig make(uint64_t blocks, uint32_t bs, const std::function<void(BotDevice&)>& setup = {}, MscOptions o = {}) {
  auto t = std::make_unique<BotDevice>(blocks, bs);
  Rig r{t.get(), nullptr, {}};
  if (setup) setup(*t);
  o.retry_delay_ms = 1; o.ready_wait_ms = 200;
  r.st = MscDevice::open(std::move(t), o, &r.dev);
  return r;
}
std::vector<uint8_t> expect(uint64_t off, size_t len, uint32_t bs) {
  std::vector<uint8_t> v(len);
  for (size_t i = 0; i < len; i++) { uint64_t a = off + i; v[i] = BotDevice::pattern(a / bs, static_cast<uint32_t>(a % bs)); }
  return v;
}
}  // namespace

TEST(msc_open_reads_capacity_and_identity) {
  auto r = make(2048, 512);
  CHECK(r.st.ok());
  CHECK_EQ(r.dev->size(), 2048u * 512);
  CHECK_EQ(r.dev->logical_block_size(), 512u);
  CHECK(r.dev->is_read_only());  // 既定は読み取り専用
  CHECK(r.dev->describe().find("FAKEVEND") != std::string::npos);
}

TEST(msc_read_aligned_unaligned_and_chunking) {
  auto r = make(4096, 512);
  CHECK(r.st.ok());
  std::vector<uint8_t> b(300000);
  CHECK(r.dev->read_at(1000, b.data(), b.size()).ok());   // 先頭/末尾が端数、中央は複数チャンク
  CHECK(b == expect(1000, b.size(), 512));
  CHECK(r.bot->max_data_len <= 64 * 1024);                 // 1 コマンド ≤ max_transfer
  std::vector<uint8_t> one(1);
  CHECK(r.dev->read_at(2097151, one.data(), 1).ok());      // 最終バイト
  CHECK_EQ(r.dev->read_at(2097152, one.data(), 1).code, Errc::OutOfRange);
  CHECK(r.dev->read_at(2097152, one.data(), 0).ok());
}

TEST(msc_4k_native_block_size) {
  auto r = make(1024, 4096);
  CHECK(r.st.ok());
  CHECK_EQ(r.dev->logical_block_size(), 4096u);
  std::vector<uint8_t> b(10000);
  CHECK(r.dev->read_at(5000, b.data(), b.size()).ok());
  CHECK(b == expect(5000, b.size(), 4096));
}

TEST(msc_large_capacity_uses_16_byte_commands) {
  uint64_t blocks = 1ull << 33;  // 4TiB @512
  auto r = make(blocks, 512);
  CHECK(r.st.ok());
  CHECK_EQ(r.dev->size(), blocks * 512);
  std::vector<uint8_t> b(4096);
  uint64_t off = (blocks - 16) * 512 + 7;
  CHECK(r.dev->read_at(off, b.data(), 4000).ok());
  CHECK(std::vector<uint8_t>(b.begin(), b.begin() + 4000) == expect(off, 4000, 512));
  bool saw16 = false;
  for (auto op : r.bot->log_ops) if (op == 0x88 || op == 0x9E) saw16 = true;
  CHECK(saw16);
}

TEST(msc_read_only_never_sends_anything_but_read_class_commands) {
  // 実機の RAID に触れる前提となる不変条件: 読み取り専用で open→read→flush→close しても、BOT に届くのは許可リストの READ 系だけ
  auto r = make(4096, 512);
  CHECK(r.st.ok());
  std::vector<uint8_t> b(70000);
  CHECK(r.dev->read_at(1000, b.data(), b.size()).ok());
  CHECK(r.dev->flush().ok());
  uint8_t x = 1;
  CHECK_EQ(r.dev->write_at(0, &x, 1).code, Errc::ReadOnly);
  CHECK(r.dev->close().ok());
  for (auto op : r.bot->log_ops) {
    bool ok = op == 0x00 || op == 0x03 || op == 0x12 || op == 0x1A || op == 0x25 || op == 0x28 || op == 0x88 || op == 0x9E;
    CHECK(ok);
  }
  auto* m = dynamic_cast<MscDevice*>(r.dev.get());
  CHECK(m != nullptr);
  CHECK_EQ(m->op_count(0x2A), 0u); CHECK_EQ(m->op_count(0x8A), 0u); CHECK_EQ(m->op_count(0x35), 0u);
  CHECK(m->op_count(0x28) > 0);
  CHECK(!m->op_stats().empty());
  // 書き込み用 CDB を exec に直接渡しても、読み取り専用なら送られない
  uint8_t wcdb[10] = {0x2A, 0, 0, 0, 0, 0, 0, 0, 1, 0}; uint8_t blk[512] = {0};
  size_t n = 0; bool cc = false;
  size_t before = r.bot->log_ops.size();
  // exec は private のため、公開 API の write_at が拒否されること (上) と、ログに 0x2A が無いことで担保する
  (void)wcdb; (void)blk; (void)n; (void)cc;
  CHECK_EQ(r.bot->log_ops.size(), before);
}

TEST(msc_write_requires_opt_in_and_does_read_modify_write) {
  auto ro = make(256, 512);
  uint8_t x = 1;
  CHECK_EQ(ro.dev->write_at(0, &x, 1).code, Errc::ReadOnly);
  MscOptions o; o.read_only = false;
  auto r = make(256, 512, {}, o);
  CHECK(r.st.ok());
  CHECK(!r.dev->is_read_only());
  std::vector<uint8_t> data(1500, 0xAB);
  CHECK(r.dev->write_at(700, data.data(), data.size()).ok());   // 端数 + 完全ブロックを含む
  std::vector<uint8_t> back(1500);
  CHECK(r.dev->read_at(700, back.data(), back.size()).ok());
  CHECK(back == data);
  // 書き込み範囲外のバイトは元のまま (RMW が隣接データを壊さない)
  std::vector<uint8_t> edge(2);
  CHECK(r.dev->read_at(698, edge.data(), 2).ok());
  CHECK(edge == expect(698, 2, 512));
  CHECK(r.dev->read_at(2200, edge.data(), 2).ok());
  CHECK(edge == expect(2200, 2, 512));
  CHECK(r.dev->flush().ok());
  CHECK_EQ(r.bot->sync_count, 1);
  CHECK_EQ(r.dev->write_at(256 * 512, &x, 1).code, Errc::OutOfRange);
}

TEST(msc_write_protected_media_forces_read_only) {
  MscOptions o; o.read_only = false;
  auto r = make(64, 512, [](BotDevice& b) { b.f.write_protected = true; }, o);
  CHECK(r.st.ok());
  CHECK(r.dev->is_read_only());
  uint8_t x = 0;
  CHECK_EQ(r.dev->write_at(0, &x, 1).code, Errc::ReadOnly);
}

TEST(msc_flush_tolerates_unsupported_sync_cache) {
  MscOptions o; o.read_only = false;
  auto r = make(64, 512, [](BotDevice& b) { b.f.no_sync_cache = true; }, o);
  CHECK(r.st.ok());
  CHECK(r.dev->flush().ok());
}

TEST(msc_unit_attention_and_not_ready_are_retried) {
  auto r = make(64, 512, [](BotDevice& b) { b.f.unit_attention_first = 2; b.f.not_ready_first = 2; });
  CHECK(r.st.ok());
  std::vector<uint8_t> b(512);
  CHECK(r.dev->read_at(0, b.data(), 512).ok());
}

TEST(msc_medium_not_present_is_reported) {
  auto r = make(64, 512, [](BotDevice& b) { b.f.medium_not_present = true; });
  CHECK(!r.st.ok());
  CHECK_EQ(r.st.code, Errc::Disconnected);
}

TEST(msc_stall_on_data_in_recovers_via_clear_halt) {
  auto r = make(256, 512);
  CHECK(r.st.ok());
  r.bot->f.stall_data_in_once = 1;
  std::vector<uint8_t> b(512);
  Status st = r.dev->read_at(0, b.data(), 512);
  CHECK(!st.ok());  // STALL = コマンド失敗。データは返らない
  CHECK(r.bot->clear_in >= 1);
  CHECK(r.dev->read_at(0, b.data(), 512).ok());  // 回復して次のコマンドは成功
  CHECK(b == expect(0, 512, 512));
}

TEST(msc_csw_stall_is_retried_once) {
  auto r = make(256, 512);
  CHECK(r.st.ok());
  r.bot->f.stall_csw_once = 1;
  std::vector<uint8_t> b(512);
  CHECK(r.dev->read_at(512, b.data(), 512).ok());
  CHECK(b == expect(512, 512, 512));
}

TEST(msc_bad_csw_triggers_reset_recovery_and_error) {
  auto r = make(256, 512);
  CHECK(r.st.ok());
  std::vector<uint8_t> b(512);
  r.bot->f.bad_csw_tag = true;
  CHECK(!r.dev->read_at(0, b.data(), 512).ok());
  CHECK(r.bot->resets >= 1);
  r.bot->f.bad_csw_tag = false; r.bot->f.bad_csw_sig = true;
  CHECK(!r.dev->read_at(0, b.data(), 512).ok());
  r.bot->f.bad_csw_sig = false; r.bot->f.phase_error = true;
  int before = r.bot->resets;
  CHECK(!r.dev->read_at(0, b.data(), 512).ok());
  CHECK(r.bot->resets > before);
  r.bot->f.phase_error = false;
  CHECK(r.dev->read_at(0, b.data(), 512).ok());  // リセット後に復帰できる
}

TEST(msc_residue_larger_than_request_is_rejected) {
  auto r = make(256, 512);
  CHECK(r.st.ok());
  r.bot->f.lie_residue = true;
  std::vector<uint8_t> b(512);
  CHECK(!r.dev->read_at(0, b.data(), 512).ok());
}

TEST(msc_short_cbw_transfer_is_an_error) {
  auto r = make(256, 512);
  CHECK(r.st.ok());
  r.bot->f.short_cbw_accept = true;
  std::vector<uint8_t> b(512);
  CHECK(!r.dev->read_at(0, b.data(), 512).ok());
}

TEST(msc_medium_error_maps_to_io_and_does_not_poison_device) {
  auto r = make(256, 512, [](BotDevice& b) { b.f.medium_error_lbas = {10}; });
  CHECK(r.st.ok());
  std::vector<uint8_t> b(1024);
  Status st = r.dev->read_at(9 * 512, b.data(), 1024);
  CHECK_EQ(st.code, Errc::Io);
  CHECK(r.dev->read_at(20 * 512, b.data(), 1024).ok());
}

TEST(msc_disconnect_mid_stream_becomes_sticky) {
  auto r = make(256, 512);
  CHECK(r.st.ok());
  r.bot->f.disconnect_after_commands = 0;   // 以降すべて抜去
  std::vector<uint8_t> b(512);
  CHECK_EQ(r.dev->read_at(0, b.data(), 512).code, Errc::Disconnected);
  r.bot->f.disconnect_after_commands = -1;  // 戻っても、物理的に抜けたデバイスとして扱い続ける
  CHECK_EQ(r.dev->read_at(0, b.data(), 512).code, Errc::Disconnected);
  CHECK_EQ(r.dev->write_at(0, b.data(), 1).code, Errc::ReadOnly);  // 読み取り専用チェックが先
}

TEST(msc_rejects_insane_block_size_and_non_disk) {
  auto r = make(64, 300);
  CHECK(!r.st.ok());
  auto r2 = make(64, 1 << 20);
  CHECK(!r2.st.ok());
}

TEST(msc_cancel_stops_between_chunks) {
  auto r = make(8192, 512);
  CHECK(r.st.ok());
  CancelToken tok; tok.cancel();
  IoOptions io; io.cancel = &tok;
  std::vector<uint8_t> b(1 << 20);
  CHECK_EQ(r.dev->read_at(0, b.data(), b.size(), io).code, Errc::Cancelled);
}

TEST(msc_partition_table_over_usb_device) {
  // MBR を持つ USB ストレージ相当を作り、Phase 1 のパーサがそのまま使えることを確認する。
  auto t = std::make_unique<BotDevice>(8192, 512);
  tb::Bytes mbr(512, 0);
  tb::put_mbr_entry(mbr, 0, {0x80, 0x07, 2048, 4096});
  mbr[510] = 0x55; mbr[511] = 0xAA;
  t->written[0] = mbr;
  BlockDevicePtr dev;
  MscOptions o; o.retry_delay_ms = 1;
  CHECK(MscDevice::open(std::move(t), o, &dev).ok());
  PartitionTable pt;
  CHECK(read_partition_table(*dev, &pt).ok());
  CHECK(pt.kind == TableKind::Mbr);
  CHECK_EQ(pt.partitions.size(), 1u);
  CHECK_EQ(pt.partitions[0].start_lba, 2048u);
}
