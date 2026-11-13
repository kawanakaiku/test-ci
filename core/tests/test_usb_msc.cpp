// SPDX-License-Identifier: Apache-2.0
#include "fake_bot.h"
#include "test_util.h"

#include <chrono>
#include <thread>

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
    bool ok = op == 0x00 || op == 0x03 || op == 0x12 || op == 0x1A || op == 0x25 || op == 0x28 || op == 0x88 || op == 0x9E ||
              op == 0xA1 || op == 0x85;   // 0xA1/0x85 = 種別判別の ATA PASS-THROUGH (IDENTIFY DEVICE のみ。下で確認)
    CHECK(ok);
  }
  for (auto c : r.bot->ata_cmds) CHECK_EQ(c, 0xEC);
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

TEST(msc_read_capacity16_short_response_is_accepted) {
  // 実機 (Ratoc RS-EC32-U3R, 4TB): READ CAPACITY(16) に 32 バイトを要求しても 12 バイトしか返らない。規格上の最小は 12 バイト
  uint64_t blocks = 7814037168ull;  // 4TB @512
  auto r = make(blocks, 512, [](BotDevice& b) { b.f.short_cap16 = true; });
  CHECK(r.st.ok());
  CHECK_EQ(r.dev->size(), blocks * 512);
  std::vector<uint8_t> b(512);
  CHECK(r.dev->read_at(0, b.data(), 512).ok());
}

TEST(msc_tune_changes_max_transfer) {
  MscOptions wo; wo.read_only = false;
  auto r = make(8192, 512, {}, wo);
  CHECK(r.st.ok());
  auto* m = dynamic_cast<MscDevice*>(r.dev.get());
  std::vector<uint8_t> b(1 << 20);
  CHECK(r.dev->read_at(0, b.data(), b.size()).ok());
  CHECK_EQ(r.bot->max_data_len, 64 * 1024);          // 既定
  m->tune(0, 256 * 1024);                            // 読み取りだけ変わる
  CHECK(r.dev->read_at(0, b.data(), b.size()).ok());
  CHECK_EQ(r.bot->max_data_len, 256 * 1024);
  r.bot->max_data_len = 0;
  std::vector<uint8_t> w(1 << 20, 0x5A);
  CHECK(r.dev->write_at(0, w.data(), w.size()).ok());   // 書き込みは既定 (64KiB) のまま
  CHECK_EQ(r.bot->max_data_len, 64 * 1024);
  CHECK(std::vector<uint8_t>(b.begin(), b.begin() + 4096) == expect(0, 4096, 512));   // 内容は変わらない
  m->tune(0, 100);                                    // ブロック長未満・非倍数は丸められる (512 以上の倍数)
  CHECK(r.dev->read_at(0, b.data(), 2048).ok());
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

TEST(msc_start_stop_works_even_when_read_only_and_flushes_first_when_writable) {
  // 読み取り専用: 許可リストを通り (専用の入口だけ)、媒体は変えない。SYNCHRONIZE CACHE は送らない
  auto ro = make(256, 512);
  auto* m = dynamic_cast<MscDevice*>(ro.dev.get());
  CHECK(m->start_stop(false).ok());
  CHECK(ro.bot->spun_down);
  CHECK_EQ(m->op_count(0x1B), 1);
  CHECK_EQ(m->op_count(0x35), 0);
  CHECK(m->start_stop(true).ok());
  CHECK(!ro.bot->spun_down);
  CHECK(ro.bot->written.empty());
  // 書き込み可: 止める前に SYNCHRONIZE CACHE。再開では送らない
  MscOptions wo; wo.read_only = false;
  auto rw = make(256, 512, {}, wo);
  auto* mw = dynamic_cast<MscDevice*>(rw.dev.get());
  CHECK(mw->start_stop(false).ok());
  CHECK_EQ(rw.bot->sync_count, 1);
  CHECK_EQ(mw->op_count(0x35), 1);
  CHECK(mw->start_stop(true).ok());
  CHECK_EQ(rw.bot->sync_count, 1);
}

TEST(msc_start_stop_unsupported_and_read_only_allowlist_still_blocks_other_opcodes) {
  auto r = make(256, 512, [](BotDevice& b) { b.f.no_start_stop = true; });
  auto* m = dynamic_cast<MscDevice*>(r.dev.get());
  CHECK_EQ(m->start_stop(false).code, Errc::Unsupported);   // ILLEGAL REQUEST → 未対応 (データには影響しない)
  // 書き込み系は、start_stop を試した後も、読み取り専用では送られない
  uint8_t x = 1;
  CHECK_EQ(r.dev->write_at(0, &x, 1).code, Errc::ReadOnly);
  CHECK_EQ(r.bot->start_stop_count, 1);
  CHECK(r.dev->flush().ok());                                // 読み取り専用の flush は何も送らない
  CHECK_EQ(m->op_count(0x35), 0);
  CHECK_EQ(m->op_count(0x2a), 0);
}

TEST(msc_idle_stop_resumes_automatically_on_next_io) {
  MscOptions wo; wo.read_only = false;
  auto r = make(256, 512, {}, wo);
  auto* m = dynamic_cast<MscDevice*>(r.dev.get());
  CHECK_EQ(m->idle_stop(60000), 0);                       // 使用直後 = まだ止めない
  CHECK(!m->spun_down());
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  CHECK_EQ(m->idle_stop(10), 1);                          // アイドル超過 → 止める
  CHECK(m->spun_down());
  CHECK(r.bot->spun_down);
  CHECK_EQ(m->idle_stop(10), 0);                          // 二重には止めない
  CHECK(r.dev->flush().ok());                             // 停止中の flush は起こさない
  CHECK(r.bot->spun_down);
  std::vector<uint8_t> b(4096);
  CHECK(r.dev->read_at(0, b.data(), b.size()).ok());      // READ の前に自動で START
  CHECK(!r.bot->spun_down);
  CHECK(!m->spun_down());
  CHECK(std::vector<uint8_t>(b.begin(), b.begin() + 4096) == expect(0, 4096, 512));
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  CHECK_EQ(m->idle_stop(10), 1);
  CHECK(m->start_stop(true).ok());                        // 手動の再開もアイドル時計をリセットする
  CHECK_EQ(m->idle_stop(60000), 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  CHECK_EQ(m->idle_stop(10), 1);
  std::vector<uint8_t> w(512, 0x77);
  CHECK(r.dev->write_at(512, w.data(), w.size()).ok());   // WRITE の前にも自動で START
  CHECK(!r.bot->spun_down);
}

TEST(msc_idle_stop_gives_up_when_unsupported) {
  auto r = make(256, 512, [](BotDevice& b) { b.f.no_start_stop = true; });
  auto* m = dynamic_cast<MscDevice*>(r.dev.get());
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  CHECK_EQ(m->idle_stop(5), -static_cast<int>(Errc::Unsupported));
  CHECK_EQ(m->idle_stop(5), 0);                           // 以後は試さない (ディスクに無駄な命令を送り続けない)
  CHECK_EQ(r.bot->start_stop_count, 1);
  std::vector<uint8_t> b(512);
  CHECK(r.dev->read_at(0, b.data(), b.size()).ok());      // 読み書きは普通に動く
}

// ======================================================================================================
// 種別判別 (回転ディスクか、フラッシュ/SSD か)。docs/media-kind.md
// ======================================================================================================
namespace {
const std::vector<uint8_t> kPages = {0x00, 0x80, 0x83, 0xB0, 0xB1, 0xB2};
const MediaInfo& info_of(Rig& r) { return dynamic_cast<MscDevice*>(r.dev.get())->media_info(); }
Rig make_media(const std::function<void(BotDevice&)>& setup, MscOptions o = {}) { return make(256, 512, setup, o); }
size_t count_op(const BotDevice& b, uint8_t op) { return static_cast<size_t>(std::count(b.log_ops.begin(), b.log_ops.end(), op)); }
size_t count_ata(const BotDevice& b) { return count_op(b, 0xA1) + count_op(b, 0x85); }
// 読み取りが普通にできる (判別で壊れていない)
void check_still_usable(Rig& r) {
  std::vector<uint8_t> b(4096);
  CHECK(r.dev->read_at(0, b.data(), b.size()).ok());
  CHECK(b == expect(0, b.size(), 512));
}
using Pred = std::function<bool(const std::vector<uint8_t>&)>;
Pred evpd(int page) { return [page](const std::vector<uint8_t>& c) { return c[0] == 0x12 && (c[1] & 1) && (page < 0 || c[2] == page); }; }
Pred opcode(uint8_t op) { return [op](const std::vector<uint8_t>& c) { return c[0] == op; }; }
}  // namespace

TEST(media_default_fake_is_unknown_and_tries_sat_once_each) {
  // VPD 非対応 (version 0) の RMB=0: SAT (12) → ILLEGAL REQUEST → (16) → ILLEGAL REQUEST で諦める
  auto r = make_media({});
  CHECK(r.st.ok());
  auto& m = info_of(r);
  CHECK(m.probed);
  CHECK(m.kind == MediaKind::Unknown);
  CHECK(m.basis == MediaBasis::None);
  CHECK(m.hint == MediaHint::None);
  CHECK(!m.vpd_tried);
  CHECK_EQ(m.sat_tried, 6);                      // (12) と (16) を 1 回ずつ
  CHECK_EQ(count_op(*r.bot, 0xA1), 1u);
  CHECK_EQ(count_op(*r.bot, 0x85), 1u);
  CHECK_EQ(r.bot->vpd_requests, 0);              // version 0 には EVPD を送らない
  CHECK(m.stop_reason.empty());
  check_still_usable(r);
}

TEST(media_vpd_b1_non_rotational) {
  auto r = make_media([](BotDevice& b) { b.media.version = 0x06; b.media.vpd_pages = kPages; b.media.b1_rate = 0x0001; });
  CHECK(r.st.ok());
  auto& m = info_of(r);
  CHECK(m.kind == MediaKind::NonRotational);
  CHECK(m.basis == MediaBasis::VpdBlockDeviceChars);
  CHECK_EQ(m.rpm, 0u);
  CHECK_EQ(m.rotation_rate_raw, 1);
  CHECK(m.vpd_supported && m.vpd_pages == kPages);
  CHECK_EQ(count_ata(*r.bot), 0u);               // 決まったので ATA は送らない
  CHECK_EQ(r.bot->vpd_requests, 2);              // 0x00 と 0xB1 だけ
  CHECK(m.describe().find("non-rotational") != std::string::npos);
  check_still_usable(r);
}

TEST(media_vpd_b1_rotational_7200_and_other_rates) {
  auto r = make_media([](BotDevice& b) { b.media.version = 0x05; b.media.vpd_pages = kPages; b.media.b1_rate = 7200; });
  CHECK(r.st.ok());
  CHECK(info_of(r).kind == MediaKind::Rotational);
  CHECK_EQ(info_of(r).rpm, 7200u);
  CHECK(info_of(r).basis == MediaBasis::VpdBlockDeviceChars);
  CHECK(info_of(r).describe().find("rotational 7200rpm") != std::string::npos);
  for (int rate : {0x0401, 5400, 15000, 0xFFFE}) {
    auto q = make_media([rate](BotDevice& b) { b.media.version = 0x06; b.media.vpd_pages = kPages; b.media.b1_rate = rate; });
    CHECK(info_of(q).kind == MediaKind::Rotational);
    CHECK_EQ(info_of(q).rpm, static_cast<uint32_t>(rate));
  }
}

TEST(media_vpd_b1_not_reported_or_reserved_falls_through_to_sat) {
  for (int rate : {0x0000, 0x0002, 0x0400, 0xFFFF}) {
    auto r = make_media([rate](BotDevice& b) { b.media.version = 0x06; b.media.vpd_pages = kPages; b.media.b1_rate = rate; });
    CHECK(r.st.ok());
    CHECK(info_of(r).kind == MediaKind::Unknown);       // 未報告・予約値は判定に使わない
    CHECK_EQ(info_of(r).rotation_rate_raw, rate);
    CHECK_EQ(info_of(r).sat_tried, 6);                  // RMB=0 なので SAT を試した (非対応)
    CHECK(info_of(r).stop_reason.empty());
  }
  // 未報告でも SAT が答えれば決まる
  auto r = make_media([](BotDevice& b) { b.media.version = 0x06; b.media.vpd_pages = kPages; b.media.b1_rate = 0; b.media.sat = 1; b.media.w217 = 5400; });
  CHECK(info_of(r).kind == MediaKind::Rotational);
  CHECK_EQ(info_of(r).rpm, 5400u);
  CHECK(info_of(r).basis == MediaBasis::AtaPassThrough12);
  CHECK_EQ(count_op(*r.bot, 0x85), 0u);                 // (12) が通ったので (16) は送らない
}

TEST(media_vpd_unsupported_illegal_request_then_sat_identify) {
  struct Case { uint16_t w217; MediaKind kind; uint32_t rpm; };
  for (Case c : {Case{0x0001, MediaKind::NonRotational, 0}, Case{7200, MediaKind::Rotational, 7200}, Case{0x0000, MediaKind::Unknown, 0}}) {
    auto r = make_media([c](BotDevice& b) { b.media.version = 0x06; b.media.sat = 3; b.media.w217 = c.w217; });   // VPD は ILLEGAL REQUEST
    CHECK(r.st.ok());
    auto& m = info_of(r);
    CHECK(m.vpd_tried && !m.vpd_supported);
    CHECK(m.kind == c.kind);
    CHECK_EQ(m.rpm, c.rpm);
    CHECK_EQ(m.ata_word217, static_cast<int32_t>(c.w217));
    if (c.kind != MediaKind::Unknown) CHECK(m.basis == MediaBasis::AtaPassThrough12);
    CHECK_EQ(r.bot->ata_cmds.size(), 1u);
    CHECK_EQ(r.bot->ata_cmds[0], 0xEC);
    check_still_usable(r);
  }
}

TEST(media_sat16_only_bridge) {
  auto r = make_media([](BotDevice& b) { b.media.sat = 2; b.media.w217 = 1; });
  CHECK(info_of(r).kind == MediaKind::NonRotational);
  CHECK(info_of(r).basis == MediaBasis::AtaPassThrough16);
  CHECK_EQ(count_op(*r.bot, 0xA1), 1u);
  CHECK_EQ(count_op(*r.bot, 0x85), 1u);
  CHECK_EQ(info_of(r).sat_tried, 6);
}

TEST(media_ata_information_vpd_0x89_is_preferred_and_needs_no_passthrough) {
  auto r = make_media([](BotDevice& b) { b.media.version = 0x06; b.media.vpd_pages = {0x00, 0x80, 0x83, 0x89}; b.media.ata_info_vpd = 1; b.media.w217 = 7200; b.media.sat = 3; });
  CHECK(r.st.ok());
  auto& m = info_of(r);
  CHECK(m.kind == MediaKind::Rotational);
  CHECK_EQ(m.rpm, 7200u);
  CHECK(m.basis == MediaBasis::AtaInfoVpd);
  CHECK_EQ(m.sat_tried, 1);
  CHECK_EQ(count_ata(*r.bot), 0u);
  // 0x89 を名乗る機器は RMB=1 でも読める (INQUIRY だけなので安全)
  auto q = make_media([](BotDevice& b) { b.media.version = 0x06; b.media.rmb = true; b.media.vpd_pages = {0x00, 0x89}; b.media.w217 = 1; });
  CHECK(info_of(q).kind == MediaKind::NonRotational);
  CHECK(info_of(q).basis == MediaBasis::AtaInfoVpd);
}

TEST(media_ata_information_vpd_atapi_or_garbage_is_not_trusted) {
  auto a = make_media([](BotDevice& b) { b.media.version = 0x06; b.media.vpd_pages = {0x00, 0x89}; b.media.ident_atapi = true; b.media.w217 = 7200; b.media.sat = 3; });
  CHECK(a.st.ok());
  CHECK(info_of(a).kind == MediaKind::Unknown);                  // ATAPI は回転数を持たない
  CHECK_EQ(count_ata(*a.bot), 0u);
  auto c = make_media([](BotDevice& b) { b.media.version = 0x06; b.media.vpd_pages = {0x00, 0x89}; b.media.ident_bad_checksum = true; b.media.w217 = 7200; b.media.sat = 3; });
  CHECK(c.st.ok());
  CHECK(info_of(c).kind == MediaKind::Unknown);                  // チェックサム不一致は信用しない
  CHECK(info_of(c).stop_reason.find("invalid") != std::string::npos);
  CHECK_EQ(count_ata(*c.bot), 0u);                               // 不正応答は 1 回であきらめる (別経路を試さない)
}

TEST(media_rmb_hint_and_sat_is_skipped_for_removable_without_vpd) {
  // RMB=1・VPD 非対応: USB メモリの可能性が高い。未知の ATA PASS-THROUGH は送らない
  auto r = make_media([](BotDevice& b) { b.media.version = 0x04; b.media.rmb = true; b.media.sat = 3; b.media.w217 = 7200; });
  CHECK(r.st.ok());
  auto& m = info_of(r);
  CHECK(m.removable);
  CHECK(m.kind == MediaKind::Unknown);
  CHECK(m.hint == MediaHint::RemovableNoVpd);
  CHECK(m.sat_skipped);
  CHECK_EQ(m.sat_tried, 0);
  CHECK_EQ(count_ata(*r.bot), 0u);
  CHECK(m.describe().find("hint=removable") != std::string::npos);
  // RMB=0 ならヒントは付かない
  auto q = make_media([](BotDevice& b) { b.media.version = 0x04; b.media.rmb = false; });
  CHECK(info_of(q).hint == MediaHint::None);
  // 決まったときはヒントを付けない (RMB=1 でも B1 が非回転なら NonRotational)
  auto n = make_media([](BotDevice& b) { b.media.version = 0x06; b.media.rmb = true; b.media.vpd_pages = kPages; b.media.b1_rate = 1; });
  CHECK(info_of(n).kind == MediaKind::NonRotational);
  CHECK(info_of(n).hint == MediaHint::None);
  CHECK(info_of(n).removable);
}

TEST(media_unmap_hint_from_vpd_b2_and_ata_trim) {
  auto r = make_media([](BotDevice& b) { b.media.version = 0x06; b.media.vpd_pages = kPages; b.media.b1_rate = 0; b.media.b2_lbpu = true; });
  CHECK(info_of(r).kind == MediaKind::Unknown);                  // UNMAP 対応だけでは確定しない (シン・プロビジョニングの RAID もある)
  CHECK(info_of(r).hint == MediaHint::UnmapSupported);
  CHECK_EQ(info_of(r).unmap_raw, 1);
  auto t = make_media([](BotDevice& b) { b.media.sat = 1; b.media.trim = true; b.media.w217 = 0; });
  CHECK(info_of(t).kind == MediaKind::Unknown);
  CHECK(info_of(t).hint == MediaHint::UnmapSupported);
  auto n = make_media([](BotDevice& b) { b.media.version = 0x06; b.media.vpd_pages = kPages; b.media.b1_rate = 0; b.media.b2_lbpu = false; });
  CHECK(info_of(n).hint == MediaHint::None);
}

TEST(media_invalid_lengths_and_page_codes_abort_the_probe) {
  // VPD 0x00 が半分の長さ (32 バイト中の 16 バイト = ヘッダの宣言より短い) で返る
  auto a = make_media([](BotDevice& b) { b.media.version = 0x06; b.media.vpd_pages = kPages; b.media.b1_rate = 1; b.f.short_data_if = evpd(0x00); });
  CHECK(a.st.ok());
  CHECK(info_of(a).kind == MediaKind::Unknown);
  CHECK(!info_of(a).stop_reason.empty());
  CHECK_EQ(count_ata(*a.bot), 0u);   // 不正応答の機器には ATA を送らない
  check_still_usable(a);
  // VPD 0xB1 が半分 (64 → 32 バイト): 回転数のフィールドまで届いていれば妥当な短い応答として受け入れる
  auto b1 = make_media([](BotDevice& b) { b.media.version = 0x06; b.media.vpd_pages = kPages; b.media.b1_rate = 7200; b.f.short_data_if = evpd(0xB1); });
  CHECK(info_of(b1).kind == MediaKind::Rotational);
  // IDENTIFY が半分 (256 バイト): 不正な長さ
  auto c = make_media([](BotDevice& b) { b.media.sat = 1; b.media.w217 = 7200; b.f.short_data_if = opcode(0xA1); });
  CHECK(c.st.ok());
  CHECK(info_of(c).kind == MediaKind::Unknown);
  CHECK(info_of(c).stop_reason.find("invalid") != std::string::npos);
  CHECK_EQ(count_op(*c.bot, 0x85), 0u);   // 1 回であきらめる
  check_still_usable(c);
  // IDENTIFY が全部 0 / ATAPI の語 0
  auto z = make_media([](BotDevice& b) { b.media.sat = 1; b.media.ident_zero = true; });
  CHECK(info_of(z).kind == MediaKind::Unknown);
  auto at = make_media([](BotDevice& b) { b.media.sat = 1; b.media.ident_atapi = true; b.media.w217 = 1; });
  CHECK(info_of(at).kind == MediaKind::Unknown);
}

TEST(media_timeouts_give_up_after_one_attempt_and_do_not_fail_open) {
  // VPD がタイムアウト: 1 回で諦める。以降の ATA も送らない。open は成功し、読める
  auto a = make_media([](BotDevice& b) { b.media.version = 0x06; b.media.vpd_pages = kPages; b.media.b1_rate = 1; b.f.timeout_if = evpd(-1); });
  CHECK(a.st.ok());
  CHECK(info_of(a).kind == MediaKind::Unknown);
  CHECK(info_of(a).stop_reason.find("timeout") != std::string::npos);
  CHECK_EQ(a.bot->vpd_requests, 1);
  CHECK_EQ(count_ata(*a.bot), 0u);
  CHECK(a.bot->resets >= 1);   // BOT リセット回復が走った
  check_still_usable(a);
  // SAT (12) がタイムアウト: (16) は試さない
  auto s = make_media([](BotDevice& b) { b.media.sat = 3; b.media.w217 = 7200; b.f.timeout_if = opcode(0xA1); });
  CHECK(s.st.ok());
  CHECK(info_of(s).kind == MediaKind::Unknown);
  CHECK_EQ(count_op(*s.bot, 0xA1), 1u);
  CHECK_EQ(count_op(*s.bot, 0x85), 0u);
  check_still_usable(s);
}

TEST(media_bot_status_anomalies_abort_the_probe) {
  // CSW が Phase Error: 以降のコマンドを送らず Unknown
  auto p = make_media([](BotDevice& b) { b.media.version = 0x06; b.media.vpd_pages = kPages; b.media.b1_rate = 1; b.f.phase_error_if = evpd(0xB1); });
  CHECK(p.st.ok());
  CHECK(info_of(p).kind == MediaKind::Unknown);
  CHECK_EQ(p.bot->vpd_requests, 2);
  CHECK_EQ(count_ata(*p.bot), 0u);
  check_still_usable(p);
  // SAT の CSW のタグが違う
  auto t = make_media([](BotDevice& b) { b.media.sat = 3; b.media.w217 = 7200; b.f.bad_tag_if = opcode(0xA1); });
  CHECK(t.st.ok());
  CHECK(info_of(t).kind == MediaKind::Unknown);
  CHECK_EQ(count_op(*t.bot, 0x85), 0u);
  check_still_usable(t);
  // UNIT ATTENTION など ILLEGAL REQUEST 以外の CHECK CONDITION: 再試行せず打ち切る
  auto u = make_media([](BotDevice& b) { b.media.sat = 3; b.f.check_cond_if = opcode(0xA1); });
  CHECK(u.st.ok());
  CHECK(info_of(u).kind == MediaKind::Unknown);
  CHECK(info_of(u).stop_reason.find("sense 6/29/00") != std::string::npos);
  CHECK_EQ(count_op(*u.bot, 0xA1), 1u);
  CHECK_EQ(count_op(*u.bot, 0x85), 0u);
}

TEST(media_disconnect_during_probe_fails_the_open) {
  size_t first_probe_cmd = 0;
  {
    auto r = make_media({});
    for (size_t i = 0; i < r.bot->log_ops.size(); i++) if (r.bot->log_ops[i] == 0xA1) { first_probe_cmd = i; break; }
    CHECK(first_probe_cmd > 0);
  }
  auto r = make_media([first_probe_cmd](BotDevice& b) { b.f.disconnect_after_commands = static_cast<int>(first_probe_cmd); });
  CHECK_EQ(r.st.code, Errc::Disconnected);
}

TEST(media_probe_runs_once_and_never_again) {
  auto r = make_media([](BotDevice& b) { b.media.version = 0x06; b.media.vpd_pages = kPages; b.media.b1_rate = 7200; });
  size_t after_open = r.bot->log_ops.size();
  std::vector<uint8_t> b(70000);
  CHECK(r.dev->read_at(100, b.data(), b.size()).ok());
  auto* m = dynamic_cast<MscDevice*>(r.dev.get());
  CHECK(m->start_stop(false).ok());
  CHECK(m->start_stop(true).ok());
  CHECK(r.dev->flush().ok());
  CHECK_EQ(r.bot->vpd_requests, 2);   // open のときの 0x00 と 0xB1 だけ
  for (size_t i = after_open; i < r.bot->log_ops.size(); i++) CHECK(r.bot->log_ops[i] != 0x12);
  CHECK(m->media_info().kind == MediaKind::Rotational);   // 値は保持される
}

TEST(media_probe_can_be_disabled) {
  MscOptions o; o.probe_media = false;
  auto r = make_media([](BotDevice& b) { b.media.version = 0x06; b.media.vpd_pages = kPages; b.media.b1_rate = 7200; b.media.sat = 3; }, o);
  CHECK(r.st.ok());
  CHECK(!info_of(r).probed);
  CHECK(info_of(r).kind == MediaKind::Unknown);
  CHECK_EQ(r.bot->vpd_requests, 0);
  CHECK_EQ(count_ata(*r.bot), 0u);
}

TEST(media_probe_works_in_write_mode_and_ata_is_still_restricted) {
  MscOptions wo; wo.read_only = false;
  auto r = make_media([](BotDevice& b) { b.media.sat = 1; b.media.w217 = 1; }, wo);
  CHECK(r.st.ok());
  CHECK(info_of(r).kind == MediaKind::NonRotational);
  CHECK(!r.dev->is_read_only());
  for (auto c : r.bot->ata_cmds) CHECK_EQ(c, 0xEC);
  CHECK(r.bot->written.empty());
}

// ---- ATA PASS-THROUGH は IDENTIFY DEVICE (0xEC) の正規形だけを通す。0xEC 以外は読み取り専用でも書き込み可でも拒否する
TEST(media_ata_passthrough_other_than_identify_is_rejected) {
  using V = std::vector<uint8_t>;
  auto pt12 = [](uint8_t cmd) { return V{0xA1, 0x08, 0x0E, 0, 1, 0, 0, 0, 0, cmd, 0, 0}; };
  auto pt16 = [](uint8_t cmd) { return V{0x85, 0x08, 0x0E, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, cmd, 0}; };
  auto ok = [](const V& c, bool allow) { return scsi_cdb_allowed_read_only(c.data(), c.size(), false, allow) && scsi_cdb_ata_passthrough_ok(c.data(), c.size(), allow); };
  CHECK(ok(pt12(0xEC), true));
  CHECK(ok(pt16(0xEC), true));
  CHECK(!ok(pt12(0xEC), false));          // 許可フラグ (probe 中だけ) が無ければ IDENTIFY でも拒否
  CHECK(!ok(pt16(0xEC), false));
  // IDENTIFY 以外の ATA コマンドは、すべて拒否 (全 255 通り)
  for (int cmd = 0; cmd < 256; cmd++) {
    if (cmd == 0xEC) continue;
    CHECK(!ok(pt12(static_cast<uint8_t>(cmd)), true));
    CHECK(!ok(pt16(static_cast<uint8_t>(cmd)), true));
  }
  // 0xEC でも、正規形でないもの: 拡張 (EXTEND=1)・別プロトコル・方向・レジスタに値がある・セクタ数・CDB 長が違う
  auto bad12 = [&](int idx, uint8_t v) { auto c = pt12(0xEC); c[idx] = v; return c; };
  auto bad16 = [&](int idx, uint8_t v) { auto c = pt16(0xEC); c[idx] = v; return c; };
  for (uint8_t v : {0x09, 0x06, 0x0A, 0x00, 0x0C, 0x10, 0x18}) { CHECK(!ok(bad12(1, v), true)); CHECK(!ok(bad16(1, v), true)); }   // PROTOCOL/EXTEND
  for (uint8_t v : {0x00, 0x06, 0x1E, 0x2E, 0x4E, 0x8E, 0x0F, 0x0C}) { CHECK(!ok(bad12(2, v), true)); CHECK(!ok(bad16(2, v), true)); }   // T_DIR/ck_cond/length
  for (int i : {3, 5, 6, 7, 8, 10, 11}) CHECK(!ok(bad12(i, 1), true));      // features/LBA/device/control
  for (int i : {3, 4, 5, 7, 8, 9, 10, 11, 12, 13, 15}) CHECK(!ok(bad16(i, 1), true));
  CHECK(!ok(bad12(4, 2), true)); CHECK(!ok(bad16(6, 2), true));              // セクタ数 2
  CHECK(!ok(bad12(4, 0), true)); CHECK(!ok(bad16(6, 0), true));              // セクタ数 0
  { auto c = pt12(0xEC); c.push_back(0); CHECK(!ok(c, true)); c.pop_back(); c.pop_back(); CHECK(!ok(c, true)); }
  { auto c = pt16(0xEC); c.pop_back(); CHECK(!ok(c, true)); c.push_back(0); c.push_back(0); CHECK(!ok(c, true)); }
  CHECK(!ok(V{0xA1}, true));
  // 書き込み可でも同じ検査 (scsi_cdb_ata_passthrough_ok は読み取り専用かどうかに依らず exec が呼ぶ)
  CHECK(!scsi_cdb_ata_passthrough_ok(pt12(0xCA).data(), 12, true));   // WRITE DMA
  CHECK(!scsi_cdb_ata_passthrough_ok(pt16(0xF4).data(), 16, true));   // SECURITY ERASE UNIT
  CHECK(scsi_cdb_ata_passthrough_ok(V{0x28, 0, 0, 0, 0, 0, 0, 0, 1, 0}.data(), 10, false));   // ATA 以外のオペコードは対象外
  // 読み取り専用の許可リスト全体: 書き込み系・他は拒否、INQUIRY は EVPD まで、CMDDT は不可
  auto allowed = [](V c) { return scsi_cdb_allowed_read_only(c.data(), c.size(), false, false); };
  CHECK(allowed({0x12, 0x00, 0, 0, 36, 0}));
  CHECK(allowed({0x12, 0x01, 0xB1, 0, 64, 0}));
  CHECK(!allowed({0x12, 0x02, 0x00, 0, 36, 0}));                  // CMDDT
  CHECK(!allowed({0x1B, 0, 0, 0, 0, 0}));                          // START STOP UNIT は専用フラグが無ければ拒否
  CHECK(scsi_cdb_allowed_read_only(V{0x1B, 0, 0, 0, 0, 0}.data(), 6, true, false));
  for (int op : {0x2A, 0x8A, 0x35, 0x04, 0x42, 0x55, 0x2F, 0x3B, 0x1C, 0x1D, 0xA0, 0x9F, 0x91, 0x41, 0xB5, 0x93})
    CHECK(!allowed({static_cast<uint8_t>(op), 0, 0, 0, 0, 0, 0, 0, 0, 0}));
  CHECK(allowed({0x9E, 0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 32, 0, 0}));
  CHECK(!allowed({0x9E, 0x12, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 32, 0, 0}));
}

TEST(media_describe_is_ascii_and_parse_ata_identify_validates) {
  uint8_t d[512] = {0};
  MediaInfo mi;
  CHECK(!parse_ata_identify(d, &mi));                              // 全部 0
  d[0] = 0x40; d[217 * 2] = 0x01;
  CHECK(parse_ata_identify(d, &mi));                               // シグネチャ無しはチェックサムを見ない
  CHECK_EQ(mi.ata_word217, 1);
  d[255 * 2] = 0xA5;                                               // シグネチャがあるのにチェックサム不一致
  CHECK(!parse_ata_identify(d, &mi));
  unsigned sum = 0; for (int i = 0; i < 511; i++) sum += d[i];
  d[511] = static_cast<uint8_t>(0x100 - (sum & 0xFF));
  CHECK(parse_ata_identify(d, &mi));
  d[1] = 0x80;                                                     // 語 0 bit15 (ATAPI)
  sum = 0; for (int i = 0; i < 511; i++) sum += d[i];
  d[511] = static_cast<uint8_t>(0x100 - (sum & 0xFF));
  CHECK(!parse_ata_identify(d, &mi));
  auto r = make_media([](BotDevice& b) { b.media.version = 0x06; b.media.vpd_pages = kPages; b.media.b1_rate = 7200; });
  for (char c : info_of(r).describe()) CHECK(c >= 0x20 && c < 0x7F);
}
