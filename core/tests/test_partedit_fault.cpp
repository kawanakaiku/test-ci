// SPDX-License-Identifier: Apache-2.0
// パーティション編集の耐障害性テスト: 書き込み失敗の注入、電源断 (書き込みの途中で止まる) の再現、ファジング。
#include "partedit_test_util.h"
#include "usbmem/sha256.h"

using namespace pt;

namespace {

struct Scenario {
  const char* name;
  Bytes img;
  uint32_t ss;
  bool gpt_atomic;  // 電源断のどの時点でも「元の表」か「新しい表」のどちらかに見えるべき (GPT・単一セクタの MBR)
  std::function<void(PartitionEditor&)> ops;
};

PartitionSpec sp(uint64_t start, uint64_t count, PartRole role = PartRole::Auto) {
  PartitionSpec s;
  s.start = start;
  s.count = count;
  s.role = role;
  return s;
}

std::vector<Scenario> scenarios() {
  std::vector<Scenario> v;
  v.push_back({"gpt-edit", gpt_disk3(65536), 512, true, [](PartitionEditor& e) {
                 e.create(sp(0, 4096));
                 e.set_name(1, "renamed");
                 e.remove(3);
                 e.resize(2, 2000);
               }});
  v.push_back({"gpt-new-table", gpt_disk3(65536), 512, true, [](PartitionEditor& e) {
                 e.new_table(TableKind::Gpt);
                 e.create(sp(0, 8192));
                 e.create(sp(0, 0));
               }});
  {
    Bytes grown = gpt_disk3(65536);
    grown.resize(131072 * 512, 0);
    v.push_back({"gpt-move-backup", grown, 512, true, [](PartitionEditor& e) { e.repair_gpt(GptRepairKind::MoveBackupToEnd); }});
  }
  v.push_back({"gpt-4096", tb::make_gpt_disk(4096, 8192, {{0x28, 64, 127, u"a"}, {0x28, 200, 299, u"b"}}), 4096, true,
               [](PartitionEditor& e) {
                 e.create(sp(0, 100));
                 e.remove(1);
                 e.set_disk_id("01234567-89ab-cdef-0123-456789abcdef");
               }});
  v.push_back({"mbr-primary", mbr_disk(65536), 512, true, [](PartitionEditor& e) {
                 e.set_mbr_type(2, 0x07);
                 e.resize(1, 3000);
                 e.create(sp(0, 100, PartRole::Primary));
               }});
  v.push_back({"mbr-logical", mbr_disk(65536), 512, false, [](PartitionEditor& e) {
                 e.create(sp(0, 1024, PartRole::Logical));
                 e.remove(5);
                 e.set_bootable(1, false);
               }});
  v.push_back({"mbr-to-gpt", mbr_disk(65536), 512, false, [](PartitionEditor& e) {
                 e.new_table(TableKind::Gpt);
                 e.create(sp(0, 4096));
               }});
  v.push_back({"gpt-to-mbr", gpt_disk3(65536), 512, false, [](PartitionEditor& e) {
                 e.new_table(TableKind::Mbr);
                 e.create(sp(0, 4096, PartRole::Primary));
               }});
  return v;
}

int g_fault_cases = 0, g_cut_cases = 0, g_cut_intermediate = 0;

EditPlan make_plan(const Scenario& sc, Bytes img, PartitionEditor** keep = nullptr) {
  (void)keep;
  auto d = std::make_shared<WDev>(std::move(img), sc.ss);
  EditorOptions o;
  o.rng_seed = 99;
  std::unique_ptr<PartitionEditor> ed;
  PartitionEditor::open(d, o, &ed);
  sc.ops(*ed);
  EditPlan plan;
  ed->plan(&plan);
  return plan;
}

ApplyOptions apply_opts(bool rollback) {
  ApplyOptions ao;
  ao.rollback_on_error = rollback;
  ao.backup_sink = [](const Bytes&) { return Status::Ok(); };
  return ao;
}

std::set<uint64_t> plan_lbas(const EditPlan& p) {
  std::set<uint64_t> s;
  for (auto& st : p.stages) for (auto& w : st.writes) s.insert(w.lba);
  return s;
}

}  // namespace

TEST(partedit_fault_write_errors_roll_back_without_touching_data) {
  for (auto& sc : scenarios()) {
    EditPlan plan = make_plan(sc, sc.img);
    CHECK(plan.ok);
    if (!plan.ok) { for (auto& p : plan.problems) std::fprintf(stderr, "    %s: %s %s\n", sc.name, p.code.c_str(), p.message.c_str()); continue; }
    std::set<uint64_t> allowed = plan_lbas(plan);
    size_t W = plan.sector_count();
    CHECK(W >= 1);
    for (int torn = 0; torn < 2; torn++) {
      for (size_t k = 0; k < W; k++) {
        auto d = std::make_shared<WDev>(sc.img, sc.ss);
        d->fail_write_index = static_cast<int>(k);
        d->torn = torn != 0;
        EditorOptions o;
        o.rng_seed = 99;
        std::unique_ptr<PartitionEditor> ed;
        CHECK(PartitionEditor::open(d, o, &ed).ok());
        sc.ops(*ed);
        EditPlan p2;
        CHECK(ed->plan(&p2).ok());
        EditResult r = ed->apply(p2, PartitionEditor::make_confirm_token(p2, p2.confirm_phrase), apply_opts(true));
        g_fault_cases++;
        if (r.ok() || r.code != "write-failed" || !r.rolled_back || d->bytes() != sc.img) {
          std::fprintf(stderr, "  FAULT %s write#%zu torn=%d code=%s rolled_back=%d same=%d\n", sc.name, k, torn, r.code.c_str(), r.rolled_back,
                       d->bytes() == sc.img);
          g_failures++;
        }
        for (auto& w : d->writes) if (!allowed.count(w.first / sc.ss)) { std::fprintf(stderr, "  %s wrote outside plan: %llu\n", sc.name, (unsigned long long)(w.first / sc.ss)); g_failures++; }
      }
    }
    // flush の失敗 (段階ごと)
    for (size_t j = 0; j < plan.stages.size(); j++) {
      auto d = std::make_shared<WDev>(sc.img, sc.ss);
      d->fail_flush_index = static_cast<int>(j);
      EditorOptions o;
      o.rng_seed = 99;
      std::unique_ptr<PartitionEditor> ed;
      CHECK(PartitionEditor::open(d, o, &ed).ok());
      sc.ops(*ed);
      EditPlan p2;
      CHECK(ed->plan(&p2).ok());
      EditResult r = ed->apply(p2, PartitionEditor::make_confirm_token(p2, p2.confirm_phrase), apply_opts(true));
      g_fault_cases++;
      CHECK(!r.ok());
      CHECK(r.code == "flush-failed");
      CHECK(r.rolled_back);
      CHECK(d->bytes() == sc.img);
    }
  }
}

TEST(partedit_fault_without_rollback_stays_recoverable) {
  // ロールバックしない設定でも、失敗した瞬間の状態は GPT なら必ず「どちらかのコピーが有効」で、元か新しい表のどちらか
  for (auto& sc : scenarios()) {
    if (!sc.gpt_atomic) continue;
    EditPlan plan = make_plan(sc, sc.img);
    if (!plan.ok) continue;
    MemoryDevice orig(sc.img, true, sc.ss);
    Snap before = snap(orig, sc.ss);
    PartitionTable after_t = plan.after;
    size_t W = plan.sector_count();
    for (int torn = 0; torn < 2; torn++) {
      for (size_t k = 0; k < W; k++) {
        auto d = std::make_shared<WDev>(sc.img, sc.ss);
        d->fail_write_index = static_cast<int>(k);
        d->torn = torn != 0;
        EditorOptions o;
        o.rng_seed = 99;
        std::unique_ptr<PartitionEditor> ed;
        PartitionEditor::open(d, o, &ed);
        sc.ops(*ed);
        EditPlan p2;
        ed->plan(&p2);
        EditResult r = ed->apply(p2, PartitionEditor::make_confirm_token(p2, p2.confirm_phrase), apply_opts(false));
        g_fault_cases++;
        CHECK(!r.ok());
        CHECK(!r.rolled_back);
        Snap now = snap(*d, sc.ss);
        if (now.kind == TableKind::Gpt) CHECK(now.gpt_usable);
        // 新しい表は (元と同じでない限り) 計画の after と一致する
        bool is_old = now == before;
        Snap fresh_new;
        {
          MemoryDevice full(sc.img, false, sc.ss);
          for (auto& st : p2.stages) for (auto& w : st.writes) full.write_at(w.lba * sc.ss, w.data.data(), sc.ss);
          fresh_new = snap(full, sc.ss);
        }
        bool is_new = now == fresh_new;
        if (!is_old && !is_new) { std::fprintf(stderr, "  %s write#%zu torn=%d: neither old nor new\n", sc.name, k, torn); g_failures++; }
      }
    }
  }
}

TEST(partedit_power_cut_at_every_write_prefix) {
  for (auto& sc : scenarios()) {
    EditPlan plan = make_plan(sc, sc.img);
    if (!plan.ok) { CHECK(plan.ok); continue; }
    std::set<uint64_t> allowed = plan_lbas(plan);
    size_t W = plan.sector_count();
    MemoryDevice orig(sc.img, true, sc.ss);
    Snap before = snap(orig, sc.ss);
    Snap fresh_new;
    {
      MemoryDevice full(sc.img, false, sc.ss);
      for (auto& st : plan.stages) for (auto& w : st.writes) full.write_at(w.lba * sc.ss, w.data.data(), sc.ss);
      fresh_new = snap(full, sc.ss);
    }
    for (int torn = 0; torn < 2; torn++) {
      for (size_t k = 0; k <= W; k++) {
        auto d = std::make_shared<WDev>(sc.img, sc.ss);
        d->cut_after = static_cast<int>(k);
        d->cut_torn = torn != 0;
        EditorOptions o;
        o.rng_seed = 99;
        std::unique_ptr<PartitionEditor> ed;
        PartitionEditor::open(d, o, &ed);
        sc.ops(*ed);
        EditPlan p2;
        ed->plan(&p2);
        ed->apply(p2, PartitionEditor::make_confirm_token(p2, p2.confirm_phrase), apply_opts(false));
        g_cut_cases++;
        for (auto& w : d->writes) CHECK(allowed.count(w.first / sc.ss) == 1);
        Snap now = snap(*d, sc.ss);
        bool old_or_new = now == before || now == fresh_new;
        bool gpt_to_gpt = before.kind == TableKind::Gpt && fresh_new.kind == TableKind::Gpt;
        if (now.kind == TableKind::Gpt && gpt_to_gpt && !now.gpt_usable) {
          std::fprintf(stderr, "  %s cut@%zu torn=%d: no usable GPT copy\n", sc.name, k, torn);
          g_failures++;
        }
        if (sc.gpt_atomic && !old_or_new) {
          std::fprintf(stderr, "  %s cut@%zu torn=%d: neither old nor new table\n", sc.name, k, torn);
          g_failures++;
        }
        if (!sc.gpt_atomic && !old_or_new) g_cut_intermediate++;
      }
    }
  }
  std::fprintf(stderr, "  [partedit] fault-injection cases so far: %d, power-cut cases: %d (intermediate states in non-atomic MBR chain / table-kind switches: %d)\n",
               g_fault_cases, g_cut_cases, g_cut_intermediate);
}

TEST(partedit_power_cut_restore_stays_valid) {
  // 復元の途中で電源が切れても、GPT はどちらかのコピーが有効
  Bytes img = gpt_disk3(65536);
  auto d0 = std::make_shared<WDev>(img);
  Bytes bk;
  {
    std::unique_ptr<PartitionEditor> ed;
    PartitionEditor::open(d0, EditorOptions{}, &ed);
    CHECK(ed->backup(0, &bk).ok());
    ed->remove(1);
    ed->remove(2);
    ed->create(sp(0, 9000));
    EditPlan p;
    ed->plan(&p);
    CHECK(ed->apply(p, PartitionEditor::make_confirm_token(p, p.confirm_phrase), apply_opts(true)).ok());
  }
  Bytes changed = d0->bytes();
  EditPlan rp;
  {
    auto d = std::make_shared<WDev>(changed);
    std::unique_ptr<PartitionEditor> ed;
    PartitionEditor::open(d, EditorOptions{}, &ed);
    CHECK(ed->plan_restore(bk, &rp).ok());
    CHECK(rp.ok);
  }
  MemoryDevice o(img, true), n(changed, true);
  Snap s_orig = snap(o), s_changed = snap(n);
  size_t W = rp.sector_count();
  for (size_t k = 0; k <= W; k++) {
    for (int torn = 0; torn < 2; torn++) {
      auto d = std::make_shared<WDev>(changed);
      d->cut_after = static_cast<int>(k);
      d->cut_torn = torn != 0;
      std::unique_ptr<PartitionEditor> ed;
      PartitionEditor::open(d, EditorOptions{}, &ed);
      EditPlan p;
      ed->plan_restore(bk, &p);
      ed->apply(p, PartitionEditor::make_confirm_token(p, p.confirm_phrase), apply_opts(false));
      g_cut_cases++;
      Snap now = snap(*d);
      CHECK(now.kind == TableKind::Gpt && now.gpt_usable);
      CHECK(now == s_orig || now == s_changed);
    }
  }
}

// ------------------------------------------------------------------ ファジング

namespace {

Bytes fuzz_gpt() {
  return tb::make_gpt_disk(512, 16384, {{0x28, 64, 1023, u"a"}, {0x28, 2048, 4095, u"bb"}, {0x28, 8192, 10239, u"ccc"}});
}
Bytes fuzz_mbr() {
  Bytes img = blank(16384);
  tb::put_mbr_entry(img, 0, {0x80, 0x83, 64, 1000});
  tb::put_mbr_entry(img, 1, {0, 0x0C, 2048, 1000});
  tb::put_mbr_entry(img, 2, {0, 0x05, 4096, 4096});
  tb::le32(img, 440, 0x12345678);
  tb::sign55aa(img);
  Bytes e1(512, 0);
  tb::put_mbr_entry(e1, 0, {0, 0x83, 64, 500});
  tb::put_mbr_entry(e1, 1, {0, 0x05, 1024, 1500});
  tb::sign55aa(e1);
  std::memcpy(img.data() + 4096 * 512, e1.data(), 512);
  Bytes e2(512, 0);
  tb::put_mbr_entry(e2, 0, {0, 0x83, 1, 400});
  tb::sign55aa(e2);
  std::memcpy(img.data() + (4096 + 1024) * 512, e2.data(), 512);
  return img;
}

bool in_data_area(const PartitionTable& t, uint64_t lba) {
  for (auto& p : t.partitions) {
    if (p.extended_container || p.protective) continue;
    if (lba >= p.start_lba && lba - p.start_lba < p.sector_count) return true;
  }
  return false;
}

}  // namespace

TEST(partedit_fuzz_random_ops_on_random_tables) {
  std::mt19937_64 rng(0xC0FFEE);
  int applied = 0, rejected = 0, planned_ok = 0, corrupt_open = 0;
  const int kIter = 2500;
  for (int it = 0; it < kIter; it++) {
    Bytes img = (rng() & 1) ? fuzz_gpt() : fuzz_mbr();
    // テーブル周辺をランダムに壊す
    int flips = static_cast<int>(rng() % 4);
    for (int f = 0; f < flips; f++) {
      size_t region = rng() % 3;
      size_t base = region == 0 ? 0 : region == 1 ? 512 * 1 : (img.size() - 512 * 40);
      img[base + rng() % (512 * 8)] ^= static_cast<uint8_t>(1u << (rng() % 8));
    }
    if (rng() % 17 == 0) img.resize(img.size() - 512 * (rng() % 5000));
    if (rng() % 19 == 0) img.resize(img.size() + 512 * (rng() % 20000), 0);
    auto d = std::make_shared<WDev>(img);
    EditorOptions o;
    o.rng_seed = rng() | 1;
    std::unique_ptr<PartitionEditor> ed;
    EditResult orr = PartitionEditor::open(d, o, &ed);
    if (!orr.ok()) { corrupt_open++; continue; }
    uint64_t N = img.size() / 512;
    int nops = 1 + static_cast<int>(rng() % 6);
    for (int k = 0; k < nops; k++) {
      uint32_t h = static_cast<uint32_t>(rng() % 20);
      auto pick = [&]() -> uint64_t {
        switch (rng() % 6) {
          case 0: return 0;
          case 1: return rng() % (N + 100);
          case 2: return rng() % 5000;
          case 3: return UINT64_MAX - rng() % 4;
          case 4: return (1ull << 32) + rng() % 3;
          default: return rng() % (N * 2 + 1);
        }
      };
      switch (rng() % 14) {
        case 0: case 1: { PartitionSpec s; s.start = pick(); s.count = pick() % 8000; s.role = static_cast<PartRole>(rng() % 4);
                          s.mbr_type = static_cast<uint8_t>(rng() % 256); s.name = (rng() & 1) ? "n" : std::string(rng() % 40, 'x'); ed->create(s); break; }
        case 2: ed->remove(h); break;
        case 3: ed->resize(h, pick() % 9000); break;
        case 4: ed->move(h, pick()); break;
        case 5: ed->set_mbr_type(h, static_cast<uint8_t>(rng() % 256)); break;
        case 6: { std::array<uint8_t, 16> g{}; for (auto& b : g) b = static_cast<uint8_t>(rng()); ed->set_gpt_type(h, g); break; }
        case 7: ed->set_name(h, std::string(rng() % 38, 'q')); break;
        case 8: ed->set_attrs(h, rng()); break;
        case 9: ed->set_bootable(h, rng() & 1); break;
        case 10: ed->new_table((rng() & 1) ? TableKind::Gpt : TableKind::Mbr, 128 + static_cast<uint32_t>(rng() % 3) * 64, true); break;
        case 11: ed->repair_gpt(static_cast<GptRepairKind>(1 + rng() % 4)); break;
        case 12: ed->set_disk_id((rng() & 1) ? "deadbeef" : "11111111-2222-3333-4444-555555555555"); break;
        default: { std::array<uint8_t, 16> g{}; for (auto& b : g) b = static_cast<uint8_t>(rng()); ed->set_unique_guid(h, g); break; }
      }
    }
    EditPlan plan;
    EditResult pr = ed->plan(&plan);
    CHECK(pr.ok());
    if (!plan.ok) { rejected++; CHECK(d->writes.empty()); continue; }
    planned_ok++;
    // 計画の不変条件: 書き込みは元の表・適用後の表のどのパーティションの中身にも触れない
    for (auto& st : plan.stages)
      for (auto& w : st.writes) {
        bool hits = in_data_area(plan.before, w.lba) || in_data_area(plan.after, w.lba);
        if (hits && !plan.before.whole_device_volume) { std::fprintf(stderr, "  fuzz iter %d: write at %llu hits data\n", it, (unsigned long long)w.lba); g_failures++; }
      }
    Bytes before_bytes = d->bytes();
    EditResult ar = ed->apply(plan, PartitionEditor::make_confirm_token(plan, plan.confirm_phrase), apply_opts(true));
    if (ar.ok()) {
      applied++;
      CHECK(ar.verified);
      // 書き込んだセクタ以外は 1 バイトも変わっていない
      std::set<uint64_t> allowed = plan_lbas(plan);
      for (size_t s = 0; s < d->bytes().size() / 512; s++)
        if (!allowed.count(s) && std::memcmp(&d->bytes()[s * 512], &before_bytes[s * 512], 512) != 0) { std::fprintf(stderr, "  fuzz iter %d: sector %zu changed outside plan\n", it, s); g_failures++; break; }
    } else {
      CHECK(d->bytes() == before_bytes);
    }
  }
  std::fprintf(stderr, "  [partedit] fuzz: %d iterations, %d refused at open, %d plans rejected, %d plans ok, %d applied and verified\n", kIter, corrupt_open, rejected, planned_ok, applied);
  CHECK(applied > 100);
  CHECK(rejected > 100);
}

TEST(partedit_fuzz_backup_files_and_restore) {
  std::mt19937_64 rng(42);
  auto d = std::make_shared<WDev>(gpt_disk3(65536));
  std::unique_ptr<PartitionEditor> ed;
  PartitionEditor::open(d, EditorOptions{}, &ed);
  Bytes bk;
  CHECK(ed->backup(123, &bk).ok());
  int accepted = 0;
  for (int i = 0; i < 4000; i++) {
    Bytes c = bk;
    int mode = static_cast<int>(rng() % 4);
    if (mode == 0) c[rng() % c.size()] ^= static_cast<uint8_t>(1 + rng() % 255);
    else if (mode == 1) c.resize(rng() % c.size());
    else if (mode == 2) { for (int k = 0; k < 8; k++) c[rng() % c.size()] = static_cast<uint8_t>(rng()); }
    else { c.assign(rng() % 600, 0); for (auto& b : c) b = static_cast<uint8_t>(rng()); }
    BackupInfo bi;
    bool parsed = parse_table_backup(c, &bi).ok();
    EditPlan p;
    EditResult r = ed->plan_restore(c, &p);
    if (parsed) accepted++;
    CHECK(parsed == r.ok());  // 検証結果が一致 (壊れたものは計画に進まない)
  }
  CHECK(accepted == 0);  // どの壊し方でも CRC-32 / SHA-256 / 構造検査で弾かれる
}
