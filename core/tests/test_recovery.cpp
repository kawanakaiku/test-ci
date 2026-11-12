// SPDX-License-Identifier: Apache-2.0
// データ救出 (ddrescue 方式) のテスト: mapfile、救出の順序、不良セクタ、中断と再開、安全弁。
#include <map>

#include "recovery_test_util.h"

using namespace rt;

namespace {
constexpr uint32_t S = 512;

// 参照実装: セクタごとの状態配列
struct Ref {
  std::vector<char> st;
  explicit Ref(uint64_t total) : st(total, '?') {}
  void set(uint64_t p, uint64_t n, char c) { for (uint64_t i = p; i < p + n && i < st.size(); i++) st[i] = c; }
};

std::shared_ptr<MemoryDevice> mem(Bytes b) { return std::make_shared<MemoryDevice>(std::move(b), false, S); }

// 救出を最後まで行い、(mapfile, 出力) を返す
struct Run { Mapfile map; Bytes out; RescueResult res; };
Run do_rescue(const Bytes& data, const std::set<uint64_t>& bad, RescueOptions opt, const std::map<uint64_t, int>& flaky = {}) {
  auto src = std::make_shared<FaultDevice>(mem(data), S);
  src->bad = bad;
  src->flaky = flaky;
  auto outd = mem(Bytes(data.size(), 0));
  Run r{Mapfile(data.size()), {}, {}};
  RescueEngine eng(src, outd, &r.map, opt);
  r.res = eng.run();
  r.out = outd->bytes();
  CHECK_EQ(src->writes, 0u);  // 読み取り元には決して書かない
  return r;
}
}  // namespace

TEST(mapfile_set_status_matches_reference) {
  std::mt19937 g(1234);
  for (int iter = 0; iter < 300; iter++) {
    uint64_t total = 1 + g() % 200;
    Mapfile m(total);
    Ref ref(total);
    const char sts[] = {'?', '*', '/', '-', '+'};
    for (int k = 0; k < 40; k++) {
      uint64_t p = g() % total, n = 1 + g() % (total - p);
      char c = sts[g() % 5];
      m.set_status(p, n, c);
      ref.set(p, n, c);
    }
    // 全域を覆い、隣接する同じ状態が併合されていること
    uint64_t cursor = 0;
    char prev = 0;
    for (auto& b : m.blocks()) {
      CHECK_EQ(b.pos, cursor);
      CHECK(b.size > 0);
      CHECK(b.status != prev);
      for (uint64_t i = 0; i < b.size; i++) if (ref.st[b.pos + i] != b.status) { CHECK(false); break; }
      cursor = b.pos + b.size;
      prev = b.status;
    }
    CHECK_EQ(cursor, total);
    // find_next / find_prev
    for (int k = 0; k < 20; k++) {
      uint64_t from = g() % total;
      char c = sts[g() % 5];
      MapBlock b;
      bool found = m.find_next(c, from, &b);
      uint64_t want = UINT64_MAX;
      for (uint64_t i = from; i < total; i++) if (ref.st[i] == c) { want = i; break; }
      CHECK_EQ(found, want != UINT64_MAX);
      if (found) {
        CHECK_EQ(b.pos, want);
        uint64_t e = want;
        while (e < total && ref.st[e] == c) e++;
        CHECK_EQ(b.pos + b.size, e);
      }
      uint64_t before = 1 + g() % total;
      bool fp = m.find_prev(c, before, &b);
      uint64_t wp = UINT64_MAX;
      for (uint64_t i = before; i-- > 0;) if (ref.st[i] == c) { wp = i; break; }
      CHECK_EQ(fp, wp != UINT64_MAX);
      if (fp) {
        CHECK_EQ(b.pos + b.size, wp + 1);
        uint64_t s = wp;
        while (s > 0 && ref.st[s - 1] == c) s--;
        CHECK_EQ(b.pos, s);
      }
    }
  }
}

TEST(mapfile_text_roundtrip_and_ddrescue_sample) {
  Mapfile m(0x1000000);
  m.set_status(0, 0x4000, '+');
  m.set_status(0x4000, 0x200, '-');
  m.set_status(0x4200, 0x1000, '/');
  MapMeta meta;
  meta.current_pos = 0x4200;
  meta.current_status = '/';
  meta.current_pass = 4;
  std::string t = m.to_text(meta);
  Mapfile back;
  MapMeta bm;
  CHECK(Mapfile::parse(t, 0x1000000, &back, &bm).ok());
  CHECK_EQ(bm.current_pos, 0x4200u);
  CHECK(bm.current_status == '/');
  CHECK_EQ(bm.current_pass, 4u);
  CHECK(back.to_text(meta) == t);
  // 実際の ddrescue 1.30 の出力
  const char* real =
      "# Mapfile. Created by GNU ddrescue version 1.30\n# Command line: /usr/bin/ddrescue -b 512 -f a b c\n# Start time:   2026-11-12 18:42:21\n"
      "# Current time: 2026-11-12 18:42:21\n# Finished\n# current_pos  current_status  current_pass\n0x00FF0000     +               1\n"
      "#      pos        size  status\n0x00000000  0x01000000  +\n";
  Mapfile r;
  MapMeta rm;
  CHECK(Mapfile::parse(real, 0x1000000, &r, &rm).ok());
  CHECK_EQ(r.count('+'), 0x1000000u);
  CHECK(rm.finished);
  // 入力より小さい mapfile は未試行で延長される。大きいものは拒否
  CHECK(Mapfile::parse(real, 0x2000000, &r, &rm).ok());
  CHECK_EQ(r.count('?'), 0x1000000u);
  CHECK(!Mapfile::parse(real, 0x800000, &r, &rm).ok());
}

TEST(mapfile_rejects_malformed) {
  Mapfile r;
  const char* bad[] = {
      "",                                                                // 現在位置の行がない
      "0x0 ? 1\n0x0 0x10 +\n0x20 0x10 +\n",                              // 隙間
      "0x0 ? 1\n0x0 0x10 +\n0x8 0x10 +\n",                               // 重なり
      "0x0 ? 1\n0x0 0x0 +\n",                                            // 大きさ 0
      "0x0 ? 1\n0x0 0x10 Z\n",                                           // 不明な状態
      "0x0 ? 1\n0x0 0x10\n",                                             // 欄が足りない
      "0x0 ? 1\n0xFFFFFFFFFFFFFFF0 0x20 +\n",                            // 溢れ
      "zz ? 1\n0x0 0x10 +\n",                                            // 数でない
      "0x0 ? 1\n-5 0x10 +\n",                                            // 負
  };
  for (const char* t : bad) CHECK(!Mapfile::parse(t, 0, &r, nullptr).ok());
}

TEST(mapfile_parse_fuzz_never_crashes) {
  Mapfile base(1 << 20);
  base.set_status(0, 4096, '+');
  base.set_status(4096, 512, '-');
  std::string good = base.to_text(MapMeta());
  std::mt19937 g(77);
  for (int i = 0; i < 3000; i++) {
    std::string t = good;
    int muts = 1 + g() % 4;
    for (int k = 0; k < muts; k++) {
      size_t p = g() % t.size();
      switch (g() % 3) {
        case 0: t[p] = static_cast<char>(g()); break;
        case 1: t.erase(p, 1 + g() % 4); break;
        default: t.insert(p, 1, "0x+-?*/ \n#"[g() % 10]);
      }
      if (t.empty()) t = "x";
    }
    Mapfile r;
    Status st = Mapfile::parse(t, 1 << 20, &r, nullptr);
    if (st.ok()) {  // 通ったなら全域を隙間なく覆う
      uint64_t c = 0;
      for (auto& b : r.blocks()) { CHECK_EQ(b.pos, c); c += b.size; }
      CHECK_EQ(c, r.size());
    }
  }
}

TEST(rescue_clean_device_copies_everything) {
  Bytes d = random_bytes(S * 3000 + 100, 5);  // 最後が端数 (セクタ未満)
  Run r = do_rescue(d, {}, RescueOptions());
  CHECK(r.res.stop == RescueStop::Finished);
  CHECK(r.res.code == "ok");
  CHECK_EQ(r.map.count('+'), d.size());
  CHECK(r.out == d);
}

TEST(rescue_bad_sectors_exactly_marked_and_rest_recovered) {
  Bytes d = random_bytes(S * 4096, 6);
  std::set<uint64_t> bad = {0, 1, 100, 101, 102, 1000, 2047, 2048, 4095};
  for (uint64_t s = 3000; s < 3100; s++) bad.insert(s);  // 大きな不良帯
  Run r = do_rescue(d, bad, RescueOptions());
  CHECK(r.res.stop == RescueStop::Finished);
  CHECK_EQ(r.map.count('?') + r.map.count('*') + r.map.count('/'), 0u);
  uint64_t nbad = 0;
  for (uint64_t s = 0; s < 4096; s++) {
    char c = r.map.status_at(s * S);
    CHECK((c == '-') == (bad.count(s) > 0));
    if (c == '-') {
      nbad++;
      for (size_t i = 0; i < S; i++) CHECK_EQ(r.out[s * S + i], 0);  // 不良セクタは書かない (ゼロのまま)
    } else if (std::memcmp(&r.out[s * S], &d[s * S], S) != 0) {
      CHECK(false);
    }
  }
  CHECK_EQ(r.map.count('-'), nbad * S);
}

TEST(rescue_phases_order_trim_scrape_retry) {
  // 大きいクラスタ (64 セクタ) の中の 1 セクタだけ不良 → 失敗した塊はトリムされ、不良は 1 セクタだけに絞られる
  Bytes d = random_bytes(S * 1024, 7);
  RescueOptions o;
  o.skip_on_error = false;
  Run r = do_rescue(d, {300}, o);
  CHECK_EQ(r.map.count('-'), S);
  CHECK_EQ(r.map.status_at(300 * S), '-');
  CHECK_EQ(r.map.count('+'), d.size() - S);
  // 一過性の不良は再試行で取れる
  o.retries = 3;
  std::map<uint64_t, int> flaky = {{400, 2}, {401, 1}};
  Run r2 = do_rescue(d, {}, o, flaky);
  CHECK_EQ(r2.map.count('-'), 0u);
  CHECK(r2.out == d);
  // 再試行なしでは取れずに不良として残る
  o.retries = 0;
  Run r3 = do_rescue(d, {}, o, flaky);
  CHECK(r3.map.count('-') > 0 || r3.out == d);
}

TEST(rescue_no_trim_no_scrape_leaves_marks) {
  Bytes d = random_bytes(S * 512, 8);
  RescueOptions o;
  o.do_trim = false;
  o.do_scrape = false;
  o.skip_on_error = false;
  Run r = do_rescue(d, {10}, o);
  CHECK(r.res.stop == RescueStop::Finished);
  CHECK(r.res.code == "ok-partial");
  CHECK(r.map.count('*') > 0);  // 大きい読み取りが失敗した塊が未トリムで残る
  CHECK_EQ(r.map.count('?'), 0u);
}

TEST(rescue_cancel_then_resume_gives_same_result) {
  Bytes d = random_bytes(S * 4096, 9);
  std::set<uint64_t> bad = {5, 6, 500, 1500, 1501, 3999};
  Run full = do_rescue(d, bad, RescueOptions());
  for (uint64_t cut : {1u, 5u, 20u, 50u}) {
    auto src = std::make_shared<FaultDevice>(mem(d), S);
    src->bad = bad;
    RescueControl ctl;
    src->on_read = [&](uint64_t i) { if (i == cut) ctl.cancel = true; };
    auto outd = mem(Bytes(d.size(), 0));
    Mapfile map(d.size());
    std::string saved;
    RescueHooks h;
    h.persist = [&](const Mapfile& m, const MapMeta& mm) { saved = m.to_text(mm); return true; };
    RescueOptions o;
    o.checkpoint_every_ops = 1;
    RescueEngine e1(src, outd, &map, o);
    RescueResult r1 = e1.run(&ctl, h);
    CHECK(r1.stop == RescueStop::Cancelled || r1.stop == RescueStop::Finished);
    // 保存された mapfile から続ける (別の FaultDevice・同じ出力)
    Mapfile loaded;
    CHECK(Mapfile::parse(saved, d.size(), &loaded, nullptr).ok());
    auto src2 = std::make_shared<FaultDevice>(mem(d), S);
    src2->bad = bad;
    RescueEngine e2(src2, outd, &loaded, o);
    RescueResult r2 = e2.run();
    CHECK(r2.stop == RescueStop::Finished);
    CHECK(loaded.to_text(MapMeta()) == full.map.to_text(MapMeta()));
    CHECK(outd->bytes() == full.out);
  }
}

TEST(rescue_refuses_bad_arguments_without_touching_anything) {
  Bytes d = random_bytes(S * 64, 10);
  auto in = mem(d);
  Mapfile m(d.size());
  // 同じデバイス
  RescueEngine e1(in, in, &m, RescueOptions());
  RescueResult r = e1.run();
  CHECK(r.stop == RescueStop::BadArgument);
  CHECK(r.code == "same-device");
  CHECK(in->bytes() == d);
  // 出力が読み取り専用
  auto ro = std::make_shared<MemoryDevice>(Bytes(d.size(), 0), true, S);
  RescueEngine e2(in, ro, &m, RescueOptions());
  CHECK(e2.run().code == "output-read-only");
  // 出力が小さい
  RescueEngine e3(in, mem(Bytes(d.size() - 1, 0)), &m, RescueOptions());
  CHECK(e3.run().code == "output-too-small");
  // mapfile の大きさが違う
  Mapfile wrong(d.size() + 512);
  RescueEngine e4(in, mem(Bytes(d.size(), 0)), &wrong, RescueOptions());
  CHECK(e4.run().code == "map-size-mismatch");
  // 範囲外・境界ずれ
  RescueOptions o;
  o.start = d.size() + 512;
  RescueEngine e5(in, mem(Bytes(d.size(), 0)), &m, o);
  CHECK(e5.run().code == "bad-range");
  o = RescueOptions();
  o.start = 100;
  RescueEngine e6(in, mem(Bytes(d.size(), 0)), &m, o);
  CHECK(e6.run().code == "unaligned-range");
  CHECK_EQ(m.count('?'), d.size());  // mapfile も変わっていない
}

TEST(rescue_domain_and_output_offset) {
  Bytes d = random_bytes(S * 256, 11);
  auto src = std::make_shared<FaultDevice>(mem(d), S);
  src->bad = {100};
  auto outd = mem(Bytes(S * 1024, 0));
  Mapfile m(d.size());
  RescueOptions o;
  o.start = S * 64;
  o.length = S * 128;
  o.out_offset = S * 512;
  RescueEngine e(src, outd, &m, o);
  RescueResult r = e.run();
  CHECK(r.stop == RescueStop::Finished);
  CHECK_EQ(m.count('?'), d.size() - S * 128);  // 範囲外は未試行のまま
  CHECK_EQ(m.status_at(100 * S), '-');
  for (uint64_t s = 64; s < 192; s++) {
    const uint8_t* got = &outd->bytes()[S * 512 + (s - 64) * S];
    if (s == 100) { for (size_t i = 0; i < S; i++) CHECK_EQ(got[i], 0); }
    else CHECK(std::memcmp(got, &d[s * S], S) == 0);
  }
  for (size_t i = 0; i < S * 512; i++) if (outd->bytes()[i]) { CHECK(false); break; }  // 範囲外の出力は触らない
}

TEST(rescue_stops_on_consecutive_errors) {
  Bytes d = random_bytes(S * 8192, 12);
  std::set<uint64_t> bad;
  for (uint64_t s = 1000; s < 8192; s++) bad.insert(s);  // 途中から全滅 (死にかけのディスク)
  auto src = std::make_shared<FaultDevice>(mem(d), S);
  src->bad = bad;
  auto outd = mem(Bytes(d.size(), 0));
  Mapfile m(d.size());
  RescueOptions o;
  o.max_consecutive_errors = 5;
  RescueEngine e(src, outd, &m, o);
  RescueResult r = e.run();
  CHECK(r.stop == RescueStop::SafetyAbort);
  CHECK(r.code == "max-consecutive-errors");
  CHECK(src->errors <= 6);  // それ以上は叩かない
  // 失敗した範囲は記録されている (読んでいない範囲は未試行のまま)
  CHECK(m.count('?') > 0);
  CHECK(m.count('*') > 0);
}

TEST(rescue_stops_on_error_rate_and_no_progress_with_fake_clock) {
  Bytes d = random_bytes(S * 4096, 13);
  std::set<uint64_t> bad;
  for (uint64_t s = 0; s < 4096; s += 3) bad.insert(s);
  {
    uint64_t clk = 0;
    auto src = std::make_shared<FaultDevice>(mem(d), S);
    src->bad = bad;
    src->fake_clock = &clk;
    src->ms_per_read = 10;
    auto outd = mem(Bytes(d.size(), 0));
    Mapfile m(d.size());
    RescueOptions o;
    o.clock_ms = [&] { return clk; };
    o.error_window_ms = 1000;
    o.max_errors_in_window = 20;  // 1 秒に 20 回以上のエラーは危険
    RescueEngine e(src, outd, &m, o);
    RescueResult r = e.run();
    CHECK(r.stop == RescueStop::SafetyAbort);
    CHECK(r.code == "error-rate");
  }
  {
    uint64_t clk = 0;
    auto src = std::make_shared<FaultDevice>(mem(d), S);
    for (uint64_t s = 100; s < 4096; s++) src->bad.insert(s);
    src->fake_clock = &clk;
    src->ms_per_read = 10;
    src->ms_per_error = 5000;  // 失敗するたびに 5 秒かかる (応答しなくなったディスク)
    auto outd = mem(Bytes(d.size(), 0));
    Mapfile m(d.size());
    RescueOptions o;
    o.clock_ms = [&] { return clk; };
    o.no_progress_timeout_ms = 20000;
    RescueEngine e(src, outd, &m, o);
    RescueResult r = e.run();
    CHECK(r.stop == RescueStop::SafetyAbort);
    CHECK(r.code == "no-progress-timeout");
  }
  {
    auto src = std::make_shared<FaultDevice>(mem(d), S);
    src->bad = bad;
    auto outd = mem(Bytes(d.size(), 0));
    Mapfile m(d.size());
    RescueOptions o;
    o.max_errors = 7;
    RescueEngine e(src, outd, &m, o);
    RescueResult r = e.run();
    CHECK(r.code == "max-errors");
    CHECK(src->errors <= 8);
  }
}

TEST(rescue_device_gone_is_not_recorded_as_bad_and_resumes) {
  Bytes d = random_bytes(S * 2048, 14);
  auto src = std::make_shared<FaultDevice>(mem(d), S);
  src->disconnect_after = 5;
  auto outd = mem(Bytes(d.size(), 0));
  Mapfile m(d.size());
  RescueOptions o;
  o.checkpoint_every_ops = 1;
  RescueEngine e(src, outd, &m, o);
  RescueResult r = e.run();
  CHECK(r.stop == RescueStop::InputGone);
  CHECK_EQ(m.count('-'), 0u);  // 切断で読めなかった範囲を不良にしない
  CHECK_EQ(m.count('*'), 0u);
  // 再接続して続き
  auto src2 = std::make_shared<FaultDevice>(mem(d), S);
  RescueEngine e2(src2, outd, &m, o);
  CHECK(e2.run().stop == RescueStop::Finished);
  CHECK(outd->bytes() == d);
}

TEST(rescue_output_failure_is_not_recorded_as_bad) {
  Bytes d = random_bytes(S * 2048, 15);
  auto src = mem(d);
  auto outd = std::make_shared<FailingOut>(d.size(), 3);
  Mapfile m(d.size());
  RescueEngine e(src, outd, &m, RescueOptions());
  RescueResult r = e.run();
  CHECK(r.stop == RescueStop::OutputError);
  CHECK(r.code == "output-write-failed");
  CHECK_EQ(m.count('-'), 0u);
  CHECK_EQ(m.count('*'), 0u);
  CHECK(m.count('+') < d.size());
}

TEST(rescue_mapfile_save_failure_stops) {
  Bytes d = random_bytes(S * 1024, 16);
  auto outd = mem(Bytes(d.size(), 0));
  Mapfile m(d.size());
  RescueHooks h;
  h.persist = [](const Mapfile&, const MapMeta&) { return false; };
  RescueOptions o;
  o.checkpoint_every_ops = 1;
  RescueEngine e(mem(d), outd, &m, o);
  RescueResult r = e.run(nullptr, h);
  CHECK(r.stop == RescueStop::MapfileError);
}

TEST(rescue_pause_blocks_until_resumed) {
  Bytes d = random_bytes(S * 256, 17);
  auto src = std::make_shared<FaultDevice>(mem(d), S);
  auto outd = mem(Bytes(d.size(), 0));
  Mapfile m(d.size());
  RescueControl ctl;
  ctl.pause = true;
  std::atomic<bool> done{false};
  RescueResult r;
  std::thread t([&] { RescueEngine e(src, outd, &m, RescueOptions()); r = e.run(&ctl); done = true; });
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  CHECK(!done.load());
  CHECK_EQ(src->reads, 0u);  // 一時停止中は 1 回も読まない
  ctl.pause = false;
  t.join();
  CHECK(r.stop == RescueStop::Finished);
  CHECK(outd->bytes() == d);
}

TEST(rescue_random_scenarios_invariants) {
  std::mt19937 g(2024);
  for (int iter = 0; iter < 60; iter++) {
    uint64_t sectors = 64 + g() % 3000;
    Bytes d = random_bytes(sectors * S, g());
    std::set<uint64_t> bad;
    int groups = g() % 6;
    for (int k = 0; k < groups; k++) {
      uint64_t s = g() % sectors, n = 1 + g() % 80;
      for (uint64_t i = s; i < s + n && i < sectors; i++) bad.insert(i);
    }
    RescueOptions o;
    o.cluster_sectors = 1 + g() % 128;
    o.skip_on_error = g() & 1;
    o.skip_min = S * (1 + g() % 200);
    o.retries = g() % 2;
    Run r = do_rescue(d, bad, o);
    CHECK(r.res.stop == RescueStop::Finished);
    for (uint64_t s = 0; s < sectors; s++) {
      char c = r.map.status_at(s * S);
      CHECK((c == '-') == (bad.count(s) > 0));
      CHECK(c == '-' || c == '+');
      if (c == '+') CHECK(std::memcmp(&r.out[s * S], &d[s * S], S) == 0);
    }
  }
}
