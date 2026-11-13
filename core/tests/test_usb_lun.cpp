// SPDX-License-Identifier: Apache-2.0
// USB マスストレージの複数 LUN (カードリーダー): GET MAX LUN、LUN ごとの CBW、LUN 一覧、メディアの有無、別 LUN の同時 I/O。docs/multi-usb.md
#include "fake_bot.h"
#include "test_util.h"

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <thread>

#include "usbmem/c_api.h"

using namespace usbmem;
using fake::BotDevice;
using fake::MultiLunBot;

namespace {
struct Spec { uint64_t blocks; uint32_t bs; uint8_t salt; bool media; };

struct Rig {
  MultiLunBot* bot = nullptr;                              // bus が所有する transport への非所有ポインタ
  std::vector<std::shared_ptr<BotDevice>> luns;
  std::shared_ptr<MscBus> bus;
  Status st;
};

Rig make_bus(const std::vector<Spec>& specs, bool query = true, const std::function<void(MultiLunBot&)>& setup = {}) {
  Rig r;
  for (size_t i = 0; i < specs.size(); i++) {
    auto d = std::make_shared<BotDevice>(specs[i].blocks, specs[i].bs);
    d->salt = specs[i].salt;
    d->media.rmb = true;                                   // カードリーダー: リムーバブル
    d->f.medium_not_present = !specs[i].media;
    r.luns.push_back(d);
  }
  auto t = std::make_unique<MultiLunBot>(r.luns);
  r.bot = t.get();
  if (setup) setup(*t);
  r.st = MscBus::create(std::move(t), query, &r.bus);
  return r;
}

MscOptions opts(uint8_t lun = 0, bool ro = true) {
  MscOptions o;
  o.lun = lun; o.read_only = ro; o.retry_delay_ms = 1; o.ready_wait_ms = 200;
  return o;
}

std::vector<uint8_t> expect(uint64_t off, size_t len, uint32_t bs, uint8_t salt) {
  std::vector<uint8_t> v(len);
  for (size_t i = 0; i < len; i++) { uint64_t a = off + i; v[i] = static_cast<uint8_t>(BotDevice::pattern(a / bs, static_cast<uint32_t>(a % bs)) ^ salt); }
  return v;
}

bool all_equal(const std::vector<uint8_t>& v, uint8_t x) { for (auto b : v) if (b != x) return false; return true; }
}  // namespace

// ---- GET MAX LUN

TEST(lun_get_max_lun_values_and_open_never_fails) {
  {  // 通常: 最大 LUN 1 = 2 つ
    auto r = make_bus({{1024, 512, 0, true}, {2048, 512, 1, true}});
    CHECK(r.st.ok());
    CHECK_EQ(r.bus->lun_count(), 2u);
    CHECK_EQ(r.bot->max_lun_calls, 1);
    CHECK(r.bus->max_lun_note() == "max_lun=1");
  }
  {  // 最大 LUN 0 = 1 つ
    auto r = make_bus({{1024, 512, 0, true}}, true);
    CHECK_EQ(r.bus->lun_count(), 1u);
  }
  {  // STALL = LUN 1 つ (LUN 1 つの機器の正常な応答)。open は失敗しない
    auto r = make_bus({{1024, 512, 0, true}}, true, [](MultiLunBot& b) { b.max_lun_mode = MultiLunBot::MaxLunMode::Stall; });
    CHECK(r.st.ok());
    CHECK_EQ(r.bus->lun_count(), 1u);
    CHECK(r.bus->max_lun_note() == "stall");
    BlockDevicePtr d;
    CHECK(r.bus->open_lun(opts(0), &d).ok());
  }
  {  // 制御転送のエラー・切断扱いでも LUN 1 つで続ける
    auto r = make_bus({{1024, 512, 0, true}}, true, [](MultiLunBot& b) { b.max_lun_mode = MultiLunBot::MaxLunMode::Error; });
    CHECK(r.st.ok());
    CHECK_EQ(r.bus->lun_count(), 1u);
    CHECK(r.bus->max_lun_note().rfind("error:", 0) == 0);
    auto r2 = make_bus({{1024, 512, 0, true}}, true, [](MultiLunBot& b) { b.max_lun_mode = MultiLunBot::MaxLunMode::Disconnected; });
    CHECK(r2.st.ok());
    CHECK_EQ(r2.bus->lun_count(), 1u);
  }
  for (int bogus : {16, 17, 255}) {   // 壊れた値 (16 以上) の機器は無視して LUN 1 つ
    auto r = make_bus({{1024, 512, 0, true}, {1024, 512, 1, true}}, true, [&](MultiLunBot& b) { b.max_lun_value = bogus; });
    CHECK(r.st.ok());
    CHECK_EQ(r.bus->lun_count(), 1u);
    CHECK(r.bus->max_lun_note().rfind("bogus:", 0) == 0);
    BlockDevicePtr d;
    CHECK_EQ(r.bus->open_lun(opts(1), &d).code, Errc::InvalidArgument);   // 無視したので LUN 1 は開けない
  }
  {  // 最大 LUN 15 (16 個) は有効
    std::vector<Spec> specs(16, Spec{256, 512, 0, false});
    auto r = make_bus(specs);
    CHECK_EQ(r.bus->lun_count(), 16u);
  }
  {  // 問い合わせない構成 (従来の単一 LUN の使い方) では送らない
    auto r = make_bus({{1024, 512, 0, true}}, false);
    CHECK_EQ(r.bot->max_lun_calls, 0);
    CHECK_EQ(r.bus->lun_count(), 1u);
    CHECK(r.bus->max_lun_note() == "not-queried");
  }
}

TEST(lun_get_max_lun_is_sent_once_only) {
  auto r = make_bus({{1024, 512, 0, true}, {1024, 512, 1, true}});
  std::vector<LunInfo> v;
  for (int i = 0; i < 3; i++) CHECK(r.bus->scan_luns(opts(), 0, &v).ok());
  BlockDevicePtr a, b;
  CHECK(r.bus->open_lun(opts(0), &a).ok());
  CHECK(r.bus->open_lun(opts(1), &b).ok());
  CHECK_EQ(r.bot->max_lun_calls, 1);   // open の最初の 1 回だけ。一覧の再取得・LUN を開くたびには送らない
}

// ---- LUN 一覧

TEST(lun_scan_no_media_on_lun0_media_on_lun1) {
  // 実機の不具合の再現: カードリーダーの LUN 0 (SD スロット) は空、LUN 1 (microSD スロット) にカード
  uint64_t blocks = 15ull * 1000 * 1000 * 1000 / 512;
  auto r = make_bus({{blocks, 512, 0, false}, {blocks, 512, 7, true}});
  CHECK(r.st.ok());
  std::vector<LunInfo> v;
  CHECK(r.bus->scan_luns(opts(), 0, &v).ok());
  CHECK_EQ(v.size(), 2u);
  CHECK(v[0].state == LunState::NoMedia);
  CHECK_EQ(v[0].lun, 0);
  CHECK(v[0].removable);
  CHECK(v[0].vendor == "FAKEVEND");
  CHECK(v[1].state == LunState::Ready);
  CHECK_EQ(v[1].lun, 1);
  CHECK_EQ(v[1].blocks, blocks);
  CHECK_EQ(v[1].block_size, 512u);
  CHECK_EQ(v[1].size(), blocks * 512);
  // LUN 0 を開こうとすると従来どおりメディアなし (Disconnected)。LUN 1 は開ける
  BlockDevicePtr d0, d1;
  CHECK_EQ(r.bus->open_lun(opts(0), &d0).code, Errc::Disconnected);
  CHECK(r.bus->open_lun(opts(1), &d1).ok());
  CHECK_EQ(d1->size(), blocks * 512);
  std::vector<uint8_t> b(5000);
  CHECK(d1->read_at(1234, b.data(), b.size()).ok());
  CHECK(b == expect(1234, b.size(), 512, 7));
  // 開いた後の I/O の CBW は LUN 1 だけ。LUN 0 の CBW は一覧と開く試行 (INQUIRY / TUR / REQUEST SENSE) だけで、READ は無い
  for (auto l : r.luns[1]->cbw_luns) CHECK_EQ(l, 1);
  for (auto l : r.luns[0]->cbw_luns) CHECK_EQ(l, 0);
  for (auto op : r.luns[0]->log_ops) CHECK(op == 0x00 || op == 0x03 || op == 0x12);
}

TEST(lun_scan_states_and_skip_and_read_only_allowlist) {
  auto r = make_bus({{1000, 512, 0, true}, {2000, 4096, 1, true}, {3000, 512, 2, true}, {4000, 512, 3, true}, {5000, 512, 4, true}, {6000, 512, 5, true}},
                    true, [](MultiLunBot& b) {
    b.luns[2]->media.ptype = 0x05;               // 光学ドライブ: 未対応
    b.luns[3]->media.pqual = 3;                  // LUN は存在するが何も接続されていない
    b.luns[4]->f.not_ready_first = 100000;       // いつまでも準備中
    b.luns[5]->f.unit_attention_first = 2;       // メディア交換直後の UNIT ATTENTION は待てば Ready
  });
  std::vector<LunInfo> v;
  MscOptions o = opts();
  o.ready_wait_ms = 30;
  CHECK(r.bus->scan_luns(o, 1u << 1, &v).ok());   // LUN 1 はスキップ
  CHECK_EQ(v.size(), 6u);
  CHECK(v[0].state == LunState::Ready);
  CHECK(v[1].state == LunState::Skipped);
  CHECK(r.luns[1]->log_ops.empty());                // スキップした LUN には何も送らない
  CHECK(v[2].state == LunState::Unsupported);
  CHECK_EQ(v[2].ptype, 0x05);
  CHECK(v[3].state == LunState::Unsupported);
  CHECK(v[4].state == LunState::NotReady);
  CHECK(v[5].state == LunState::Ready);
  CHECK_EQ(v[5].blocks, 6000u);
  CHECK(r.bus->scan_luns(opts(), 0, &v).ok());
  CHECK(v[1].state == LunState::Ready);
  CHECK_EQ(v[1].block_size, 4096u);                 // LUN ごとにブロック長も違ってよい
  // 一覧に使ったコマンドは、読み取り専用の許可リスト内 (INQUIRY / TEST UNIT READY / REQUEST SENSE / READ CAPACITY) だけ
  for (auto& l : r.luns)
    for (auto op : l->log_ops) CHECK(op == 0x00 || op == 0x03 || op == 0x12 || op == 0x25 || op == 0x9E);
}

TEST(lun_scan_large_capacity_uses_read_capacity16) {
  uint64_t blocks = 1ull << 33;   // 4TiB @512 -> READ CAPACITY(16)
  auto r = make_bus({{blocks, 512, 0, true}});
  std::vector<LunInfo> v;
  CHECK(r.bus->scan_luns(opts(), 0, &v).ok());
  CHECK(v[0].state == LunState::Ready);
  CHECK_EQ(v[0].blocks, blocks);
}

TEST(lun_scan_command_failure_is_per_lun_and_disconnect_is_overall) {
  auto r = make_bus({{1000, 512, 0, true}, {1000, 512, 1, true}}, true, [](MultiLunBot& b) {
    b.luns[1]->f.phase_error_if = [](const std::vector<uint8_t>& cdb) { return cdb[0] == 0x25; };   // READ CAPACITY が Phase Error
  });
  std::vector<LunInfo> v;
  CHECK(r.bus->scan_luns(opts(), 0, &v).ok());
  CHECK(v[0].state == LunState::Ready);
  CHECK(v[1].state == LunState::Error);                     // 1 つの LUN の失敗は一覧全体の失敗にしない
  CHECK(!v[1].note.empty());
  std::vector<LunInfo> w;
  r.bot->yank();
  CHECK_EQ(r.bus->scan_luns(opts(), 0, &w).code, Errc::Disconnected);   // 抜去は全体の失敗
  CHECK(r.bus->dead());
}

// ---- LUN ごとの I/O

TEST(lun_cbw_carries_lun_and_images_are_separate) {
  auto r = make_bus({{4096, 512, 0x11, true}, {8192, 512, 0x22, true}});
  BlockDevicePtr a, b;
  CHECK(r.bus->open_lun(opts(0), &a).ok());
  CHECK(r.bus->open_lun(opts(1), &b).ok());
  CHECK_EQ(a->size(), 4096u * 512);
  CHECK_EQ(b->size(), 8192u * 512);
  std::vector<uint8_t> x(70000), y(70000);
  CHECK(a->read_at(1000, x.data(), x.size()).ok());
  CHECK(b->read_at(1000, y.data(), y.size()).ok());
  CHECK(x == expect(1000, x.size(), 512, 0x11));
  CHECK(y == expect(1000, y.size(), 512, 0x22));
  CHECK(x != y);
  for (auto l : r.luns[0]->cbw_luns) CHECK_EQ(l, 0);   // CBW の LUN が正しく付く (宛先以外の LUN には届かない)
  for (auto l : r.luns[1]->cbw_luns) CHECK_EQ(l, 1);
  CHECK(!r.luns[0]->cbw_luns.empty() && !r.luns[1]->cbw_luns.empty());
  CHECK_EQ(r.bot->bad_lun_cbws, 0);
  // CDB の byte 1 の上位 3 ビットには LUN を入れない (CBW の LUN で足りる)
  for (auto& l : r.luns) for (auto& cdb : l->cdb_log) CHECK_EQ(cdb[1] & 0xE0, 0);
  // 範囲外
  std::vector<uint8_t> one(1);
  CHECK_EQ(a->read_at(4096ull * 512, one.data(), 1).code, Errc::OutOfRange);
  CHECK(b->read_at(4096ull * 512, one.data(), 1).ok());   // LUN 1 の方は大きい
}

TEST(lun_write_goes_to_the_right_lun_only) {
  auto r = make_bus({{1024, 512, 0, true}, {1024, 512, 0, true}});
  BlockDevicePtr a, b;
  CHECK(r.bus->open_lun(opts(0, false), &a).ok());
  CHECK(r.bus->open_lun(opts(1, false), &b).ok());
  std::vector<uint8_t> w(4096, 0xC3);
  CHECK(b->write_at(512, w.data(), w.size()).ok());
  CHECK(b->flush().ok());
  CHECK(r.luns[0]->written.empty());
  CHECK_EQ(r.luns[1]->written.size(), 8u);
  CHECK_EQ(r.luns[0]->sync_count, 0);
  CHECK_EQ(r.luns[1]->sync_count, 1);
  std::vector<uint8_t> back(4096);
  CHECK(b->read_at(512, back.data(), back.size()).ok());
  CHECK(back == w);
  CHECK(a->read_at(512, back.data(), back.size()).ok());   // LUN 0 の同じ位置は元のまま
  CHECK(back == expect(512, back.size(), 512, 0));
  // 読み取り専用で開いた LUN は、同じ bus に書き込み可の LUN があっても書き込めない
  BlockDevicePtr ro;
  CHECK(r.bus->open_lun(opts(0, true), &ro).ok());
  uint8_t x = 1;
  CHECK_EQ(ro->write_at(0, &x, 1).code, Errc::ReadOnly);
}

TEST(lun_start_stop_is_per_lun) {
  auto r = make_bus({{1024, 512, 0, true}, {1024, 512, 0, true}});
  BlockDevicePtr a, b;
  CHECK(r.bus->open_lun(opts(0), &a).ok());
  CHECK(r.bus->open_lun(opts(1), &b).ok());
  auto* ma = dynamic_cast<MscDevice*>(a.get());
  auto* mb = dynamic_cast<MscDevice*>(b.get());
  CHECK(mb->start_stop(false).ok());
  CHECK(mb->spun_down());
  CHECK(!ma->spun_down());
  CHECK_EQ(r.luns[0]->start_stop_count, 0);
  CHECK_EQ(r.luns[1]->start_stop_count, 1);
  CHECK(r.luns[1]->spun_down);
  std::vector<uint8_t> x(1024);
  CHECK(b->read_at(0, x.data(), x.size()).ok());          // 止めた LUN への読み取りは先に START を送る
  CHECK(!r.luns[1]->spun_down);
  CHECK_EQ(r.luns[0]->start_stop_count, 0);
  CHECK_EQ(mb->lun(), 1);
}

TEST(lun_media_removal_and_reinsert_affect_only_that_lun) {
  auto r = make_bus({{4096, 512, 1, true}, {4096, 512, 2, true}});
  BlockDevicePtr a, b;
  CHECK(r.bus->open_lun(opts(0), &a).ok());
  CHECK(r.bus->open_lun(opts(1), &b).ok());
  std::vector<uint8_t> x(2048);
  CHECK(b->read_at(0, x.data(), x.size()).ok());
  r.luns[1]->f.medium_not_present = true;                 // LUN 1 のカードを抜く
  CHECK(!b->read_at(0, x.data(), x.size()).ok());          // 失敗する (NOT READY / medium not present)
  CHECK(a->read_at(0, x.data(), x.size()).ok());           // LUN 0 は使い続けられる
  CHECK(x == expect(0, x.size(), 512, 1));
  CHECK(!r.bus->dead());                                   // 切断ではない: 接続全体は生きている
  std::vector<LunInfo> v;
  CHECK(r.bus->scan_luns(opts(), 1u << 0, &v).ok());       // 開いている LUN 0 は調べない
  CHECK(v[0].state == LunState::Skipped);
  CHECK(v[1].state == LunState::NoMedia);                  // メディアの有無を取り直せる
  r.luns[1]->f.medium_not_present = false;                // 入れ直す (UNIT ATTENTION を 1 回返す)
  r.luns[1]->f.unit_attention_first = 1;
  CHECK(r.bus->scan_luns(opts(), 1u << 0, &v).ok());
  CHECK(v[1].state == LunState::Ready);
  CHECK(b->read_at(0, x.data(), x.size()).ok());
  CHECK(x == expect(0, x.size(), 512, 2));
  CHECK_EQ(r.bot->overlap_violations, 0);
}

TEST(lun_concurrent_io_on_different_luns_is_serialized_on_the_bus) {
  auto r = make_bus({{16384, 512, 0x31, true}, {16384, 512, 0x42, true}});
  BlockDevicePtr a, b;
  CHECK(r.bus->open_lun(opts(0, false), &a).ok());
  CHECK(r.bus->open_lun(opts(1, false), &b).ok());
  std::atomic<int> bad{0};
  auto reader = [&](BlockDevicePtr d, uint8_t salt, uint64_t seed) {
    std::vector<uint8_t> buf(300000);
    for (int i = 0; i < 12; i++) {
      uint64_t off = (seed * 7919 + static_cast<uint64_t>(i) * 104729) % (6ull << 20);   // 書き込み領域 (7MiB 以降) とは重ならない
      if (!d->read_at(off, buf.data(), buf.size()).ok() || buf != expect(off, buf.size(), 512, salt)) bad++;
    }
  };
  std::vector<LunInfo> v;
  std::thread t1(reader, a, 0x31, 1), t2(reader, b, 0x42, 2);
  std::thread t3([&] { for (int i = 0; i < 5; i++) if (!r.bus->scan_luns(opts(), 0, &v).ok()) bad++; });   // 一覧の再取得も並行
  std::thread t4([&] {   // 書き込みも並行 (別の LUN の別の領域)
    std::vector<uint8_t> w(65536, 0x7E);
    for (int i = 0; i < 6; i++) if (!b->write_at((7ull << 20) + static_cast<uint64_t>(i) * 65536, w.data(), w.size()).ok()) bad++;
  });
  t1.join(); t2.join(); t3.join(); t4.join();
  CHECK_EQ(bad.load(), 0);
  CHECK_EQ(r.bot->overlap_violations, 0);   // 1 つのコマンド (CBW → データ → CSW) の途中に別の LUN のコマンドが割り込まない
  for (auto l : r.luns[0]->cbw_luns) CHECK_EQ(l, 0);
  for (auto l : r.luns[1]->cbw_luns) CHECK_EQ(l, 1);
  CHECK_EQ(r.luns[0]->written.size(), 0u);
}

TEST(lun_disconnect_is_shared_by_all_luns_of_the_bus) {
  auto r = make_bus({{1024, 512, 0, true}, {1024, 512, 1, true}});
  BlockDevicePtr a, b;
  CHECK(r.bus->open_lun(opts(0), &a).ok());
  CHECK(r.bus->open_lun(opts(1), &b).ok());
  std::vector<uint8_t> x(512);
  r.bot->yank();
  CHECK_EQ(b->read_at(0, x.data(), x.size()).code, Errc::Disconnected);
  CHECK_EQ(a->read_at(0, x.data(), x.size()).code, Errc::Disconnected);   // 同じインタフェースなので、片方が切断を検出すれば両方とも切断
  BlockDevicePtr c;
  CHECK_EQ(r.bus->open_lun(opts(0), &c).code, Errc::Disconnected);
}

TEST(lun_bus_outlives_closed_handles) {
  // 片方の LUN を閉じても、もう片方は使える。bus の参照を手放しても、開いた LUN は使える (インタフェースの共有)
  auto r = make_bus({{2048, 512, 0x55, true}, {2048, 512, 0x66, true}});
  BlockDevicePtr a, b;
  CHECK(r.bus->open_lun(opts(0), &a).ok());
  CHECK(r.bus->open_lun(opts(1), &b).ok());
  CHECK(a->close().ok());
  a.reset();
  r.bus.reset();
  std::vector<uint8_t> x(1500);
  CHECK(b->read_at(77, x.data(), x.size()).ok());
  CHECK(x == expect(77, x.size(), 512, 0x66));
}

TEST(lun_legacy_single_open_puts_lun_in_cbw_and_does_not_query) {
  auto t = std::make_unique<BotDevice>(1024, 512);
  BotDevice* bot = t.get();
  MscOptions o = opts(1);
  BlockDevicePtr d;
  CHECK(MscDevice::open(std::move(t), o, &d).ok());
  CHECK(!bot->cbw_luns.empty());
  for (auto l : bot->cbw_luns) CHECK_EQ(l, 1);
  auto t2 = std::make_unique<BotDevice>(1024, 512);
  CHECK_EQ(MscDevice::open(std::move(t2), opts(16), &d).code, Errc::InvalidArgument);
}

TEST(lun_open_out_of_range) {
  auto r = make_bus({{1024, 512, 0, true}, {1024, 512, 1, true}});
  BlockDevicePtr d;
  CHECK_EQ(r.bus->open_lun(opts(2), &d).code, Errc::InvalidArgument);   // GET MAX LUN が 1 だったのに LUN 2 は開かない
  CHECK(d == nullptr);
}

// ---- C API (usbdevfs の ioctl が使えない fd でも、GET MAX LUN は失敗にせず LUN 1 つ・一覧は LUN ごとの error)

TEST(lun_c_api_with_unusable_fd_degrades_gracefully) {
  int fd = ::open("/dev/null", O_RDWR);
  CHECK(fd >= 0);
  usbmem_usb_bus* bus = nullptr;
  CHECK_EQ(usbmem_usb_bus_open(fd, 0x81, 0x02, 0, 1, &bus), 0);
  CHECK(bus != nullptr);
  CHECK_EQ(usbmem_usb_bus_lun_count(bus), 1);
  char note[64];
  usbmem_usb_bus_max_lun_note(bus, note, sizeof note);
  CHECK(std::string(note).rfind("error:", 0) == 0);
  char* json = nullptr;
  CHECK_EQ(usbmem_usb_bus_scan_luns(bus, 0, 100, &json), 0);
  CHECK(json != nullptr);
  if (json) {
    CHECK(std::strstr(json, "\"lun\":0") != nullptr);
    CHECK(std::strstr(json, "\"state\":\"error\"") != nullptr);
    usbmem_free(json);
  }
  usbmem_device* dev = nullptr;
  CHECK(usbmem_open_usb_msc_lun(bus, 0, 1, &dev) != 0);   // ioctl が使えないので開けない
  CHECK(usbmem_open_usb_msc_lun(bus, 1, 1, &dev) != 0);   // 範囲外
  CHECK(dev == nullptr);
  usbmem_usb_bus_close(bus);
  // 引数検査
  usbmem_usb_bus* none = nullptr;
  CHECK(usbmem_usb_bus_open(-1, 0x81, 0x02, 0, 1, &none) != 0);
  CHECK(usbmem_usb_bus_open(fd, 0x01, 0x02, 0, 1, &none) != 0);   // IN エンドポイントではない
  CHECK(none == nullptr);
  ::close(fd);
}
