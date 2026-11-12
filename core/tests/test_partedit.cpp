// SPDX-License-Identifier: Apache-2.0
// パーティション編集 (partition_edit) のテスト。外部ツール (sfdisk / sgdisk / parted / fdisk) との照合は
// tools/check_partedit_tools.py が usbmem_partedit_cli を通して行う。
#include "partedit_test_util.h"
#include "usbmem/sha256.h"

using namespace pt;

namespace {

std::shared_ptr<WDev> wdev(Bytes img, uint32_t bs = 512) { return std::make_shared<WDev>(std::move(img), bs); }

std::unique_ptr<PartitionEditor> open_ed(BlockDevicePtr d, uint32_t ss = 0, bool in_use = false, uint64_t seed = 7) {
  EditorOptions o;
  o.sector_size = ss;
  o.in_use = in_use;
  o.rng_seed = seed;
  std::unique_ptr<PartitionEditor> ed;
  EditResult r = PartitionEditor::open(d, o, &ed);
  if (!r.ok()) { std::fprintf(stderr, "  open failed: %s\n", r.code.c_str()); return nullptr; }
  return ed;
}

EditResult commit(PartitionEditor& ed, EditPlan* plan_out = nullptr, Bytes* backup = nullptr, bool rollback = true) {
  EditPlan plan;
  EditResult r = ed.plan(&plan);
  if (plan_out) *plan_out = plan;
  if (!r.ok()) return r;
  if (!plan.ok) {
    EditResult e;
    e.status = Status::Make(Errc::InvalidArgument);
    e.code = "plan-invalid";
    for (auto& p : plan.problems) if (p.severity == Severity::Error) e.message += p.code + ";";
    return e;
  }
  ApplyOptions ao;
  ao.rollback_on_error = rollback;
  ao.backup_sink = [&](const Bytes& b) { if (backup) *backup = b; return Status::Ok(); };
  return ed.apply(plan, PartitionEditor::make_confirm_token(plan, plan.confirm_phrase), ao);
}

bool has_problem(const EditPlan& p, const char* code) {
  for (auto& d : p.problems) if (d.code == code) return true;
  return false;
}

PartitionSpec spec(uint64_t start, uint64_t count, PartRole role = PartRole::Auto) {
  PartitionSpec s;
  s.start = start;
  s.count = count;
  s.role = role;
  return s;
}

}  // namespace

// ------------------------------------------------------------------ SHA-256

TEST(partedit_sha256_known_answers) {
  CHECK(hex_encode(Sha256::hash("", 0)) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(hex_encode(Sha256::hash("abc", 3)) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  std::string m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  CHECK(hex_encode(Sha256::hash(m.data(), m.size())) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  std::string a(10000, 'a');
  Sha256 s1;
  for (size_t i = 0; i < a.size(); i += 37) s1.update(a.data() + i, std::min<size_t>(37, a.size() - i));
  CHECK(hex_encode(s1.finish()) == "27dd1f61b867b6a0f6e9d8a41c43231de52107e53ae424de8f847b821db4b711");
}

// ------------------------------------------------------------------ パーティション情報

TEST(partedit_partuuid_and_type_names) {
  PartitionTable t;
  CHECK(tb::parse(gpt_disk3(65536), &t).ok());
  CHECK_EQ(t.partitions.size(), 3u);
  // 一意 GUID は先頭 1 バイトだけ非 0 (make_gpt_disk)。PARTUUID は小文字の標準表記
  CHECK(t.partitions[0].part_uuid == "00000001-0000-0000-0000-000000000000");
  CHECK(t.partitions[1].part_uuid == "00000002-0000-0000-0000-000000000000");
  PartitionTable m;
  CHECK(tb::parse(mbr_disk(65536), &m).ok());
  CHECK(m.mbr_disk_id == "c0ffee01");
  CHECK(m.partitions[0].part_uuid == "c0ffee01-01");
  CHECK(m.partitions[1].part_uuid == "c0ffee01-02");
  CHECK(m.partitions.back().part_uuid == "c0ffee01-06");  // 論理 (5, 6)
  CHECK(std::string(mbr_type_name(0x83)) == "Linux");
  CHECK(std::string(mbr_type_name(0x0C)) == "FAT32");
  CHECK(std::string(mbr_type_name(0xFD)) == "Linux RAID");
  CHECK(std::string(mbr_type_name(0xAF)) == "HFS/HFS+");
  CHECK(std::string(mbr_type_name(0x99)).empty());
  CHECK(std::string(gpt_type_name(guid_of("C12A7328-F81F-11D2-BA4B-00A0C93EC93B"))) == "EFI System");
  CHECK(std::string(gpt_type_name(guid_of("0FC63DAF-8483-4772-8E79-3D69D8477DE4"))) == "Linux filesystem");
  CHECK(std::string(gpt_type_name(guid_of("EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"))) == "Microsoft basic data");
  CHECK(std::string(gpt_type_name(guid_of("7C3457EF-0000-11AA-AA11-00306543ECAC"))) == "APFS");
  CHECK(std::string(gpt_type_name(guid_of("11111111-2222-3333-4444-555555555555"))).empty());
  std::string js = partition_table_to_json(t);
  CHECK(js.find("\"partUuid\":\"00000001-0000-0000-0000-000000000000\"") != std::string::npos);
  CHECK(js.find("\"fsLabel\"") != std::string::npos);
  CHECK(partition_table_to_json(m).find("\"mbrDiskId\":\"c0ffee01\"") != std::string::npos);
}

// ------------------------------------------------------------------ GPT 検査

TEST(partedit_inspect_gpt_healthy_and_damaged) {
  Bytes img = gpt_disk3(65536);
  MemoryDevice d(img, true);
  GptInspection g;
  CHECK(inspect_gpt(d, 512, &g).ok());
  CHECK(g.primary.usable() && g.backup.usable() && g.copies_agree && g.backup_at_end);
  CHECK(g.mbr_protective_ok && !g.mbr_hybrid && g.suggested_repairs.empty());
  CHECK_EQ(g.primary.first_usable, 34u);
  CHECK_EQ(g.backup.lba, 65535u);

  Bytes a = img;  // プライマリのヘッダ破損
  a[512 + 20] ^= 0xFF;
  MemoryDevice da(a, true);
  CHECK(inspect_gpt(da, 512, &g).ok());
  CHECK(!g.primary.usable() && g.backup.usable());
  CHECK(g.suggested_repairs.size() == 1 && g.suggested_repairs[0] == GptRepairKind::CopyBackupToPrimary);

  Bytes b = img;  // バックアップのエントリ配列を破損
  b[(65536 - 33) * 512 + 3] ^= 0x55;
  MemoryDevice db(b, true);
  CHECK(inspect_gpt(db, 512, &g).ok());
  CHECK(g.primary.usable() && g.backup.header_valid && !g.backup.entries_valid);
  CHECK(g.suggested_repairs.size() == 1 && g.suggested_repairs[0] == GptRepairKind::CopyPrimaryToBackup);

  Bytes c = img;  // ディスクが大きくなった
  c.resize(131072 * 512, 0);
  MemoryDevice dc(c, true);
  CHECK(inspect_gpt(dc, 512, &g).ok());
  CHECK(g.disk_grew && !g.backup_at_end && g.primary.usable());
  CHECK(g.suggested_repairs[0] == GptRepairKind::MoveBackupToEnd);

  Bytes e = img;  // 保護 MBR が壊れている
  tb::le32(e, 446 + 12, 100);
  MemoryDevice de(e, true);
  CHECK(inspect_gpt(de, 512, &g).ok());
  CHECK(g.mbr_protective && !g.mbr_protective_ok);
  CHECK(g.suggested_repairs.size() == 1 && g.suggested_repairs[0] == GptRepairKind::RebuildProtectiveMbr);

  Bytes f = blank(4096);  // GPT なし
  MemoryDevice df(f, true);
  CHECK(inspect_gpt(df, 512, &g).ok());
  CHECK(!g.any_present());
  std::string js = gpt_inspection_to_json(g);
  CHECK(js.find("\"suggestedRepairs\":[]") != std::string::npos);
}

// ------------------------------------------------------------------ 編集 (GPT)

TEST(partedit_gpt_create_modify_remove_roundtrip) {
  auto d = wdev(gpt_disk3(65536));
  auto ed = open_ed(d);
  CHECK(ed != nullptr);
  CHECK(ed->writable());
  uint32_t h = 0;
  PartitionSpec s = spec(0, 4096);
  s.name = "データ😀";
  s.type_guid = guid_of("C12A7328-F81F-11D2-BA4B-00A0C93EC93B");
  CHECK(ed->create(s, &h).ok());
  CHECK_EQ(h, 4u);  // 空きスロットの先頭 (4 番)
  CHECK(ed->set_name(1, "renamed").ok());
  CHECK(ed->set_attrs(2, 0x8000000000000001ull).ok());
  CHECK(ed->remove(3).ok());
  CHECK(ed->resize(1, 2000).ok());
  EditPlan plan;
  EditResult r = commit(*ed, &plan);
  CHECK(r.ok());
  CHECK(r.verified);
  CHECK(r.code == "ok");
  CHECK(plan.destructive);  // 削除と縮小を含む
  CHECK(plan.changes.size() >= 4);
  PartitionTable t;
  CHECK(tb::parse(d->bytes(), &t).ok());
  CHECK(!t.has_errors());
  CHECK_EQ(t.partitions.size(), 3u);
  CHECK(t.partitions[0].name == "renamed" && t.partitions[0].sector_count == 2000);
  CHECK(t.partitions[1].gpt_attributes == 0x8000000000000001ull);
  CHECK(t.partitions[2].index == 4 && t.partitions[2].name == "データ😀");
  CHECK(std::string(t.partitions[2].type_name) == "EFI System");
  CHECK(t.partitions[2].start_lba == 34 || t.partitions[2].start_lba % 2048 == 0);
  // 書いたのはテーブル領域だけ: LBA 0..33 と末尾 33 セクタ以外には 1 バイトも書かれていない
  for (auto& w : d->writes) {
    uint64_t lba = w.first / 512;
    CHECK(lba < 34 || lba >= 65536 - 33);
  }
  // 編集後のエディタは新しい状態を見ている
  CHECK_EQ(ed->table().partitions.size(), 3u);
  CHECK(ed->op_log().empty());
}

TEST(partedit_gpt_auto_placement_and_alignment) {
  auto d = wdev(blank(65536));
  auto ed = open_ed(d);
  CHECK(ed->new_table(TableKind::Gpt).ok());
  uint32_t a = 0, b = 0, c = 0;
  CHECK(ed->create(spec(0, 1000), &a).ok());
  CHECK(ed->create(spec(0, 0), &b).ok());  // 残り全部
  CHECK(!ed->create(spec(0, 3000), &c).ok());  // 1 MiB 未満の隙間 (34..2047) には入らない大きさ
  EditPlan plan;
  CHECK(ed->plan(&plan).ok());
  CHECK(plan.ok);
  PartitionTable t = plan.after;
  CHECK_EQ(t.partitions.size(), 2u);
  CHECK_EQ(t.partitions[0].start_lba, 2048u);  // 1 MiB 境界
  CHECK_EQ(t.partitions[1].start_lba, 4096u);  // 次の 1 MiB 境界 (1000 セクタ分の後)
  CHECK_EQ(t.partitions[1].start_lba + t.partitions[1].sector_count - 1, 65536u - 34u);  // 末尾まで
  CHECK(plan.destructive == false);  // 空のディスクへの新規テーブル
}

TEST(partedit_gpt_rejects_invalid_operations) {
  auto d = wdev(gpt_disk3(65536));
  auto ed = open_ed(d);
  CHECK(!ed->create(spec(2048, 0)).ok());              // 既存と重なる
  CHECK(ed->create(spec(30000, 0)).ok());              // 空き末尾まで
  ed->clear_ops();
  CHECK(ed->create(spec(1, 100)).code == "outside-usable");
  CHECK(ed->create(spec(65500, 10)).code == "outside-usable");
  CHECK(ed->create(spec(40000, UINT64_MAX)).code == "overflow");
  CHECK(ed->create(spec(40000, 1ull << 40)).code == "out-of-range");
  CHECK(ed->create(spec(2048, 10)).code == "overlap");
  CHECK(ed->resize(1, 0).code == "zero-size");
  CHECK(ed->resize(1, 9000).code == "overlap");
  CHECK(ed->move(1, 7000).code == "overlap");
  CHECK(ed->remove(99).code == "no-such-partition");
  CHECK(ed->set_gpt_type(1, std::array<uint8_t, 16>{}).code == "gpt-type-zero");
  CHECK(ed->set_unique_guid(1, std::array<uint8_t, 16>{}).code == "gpt-unique-zero");
  CHECK(ed->set_name(1, std::string(37, 'a')).code == "gpt-name-invalid");
  CHECK(ed->set_name(1, std::string("\xff\xfe")).code == "gpt-name-invalid");
  CHECK(ed->set_name(1, std::string("a\0b", 3)).code == "gpt-name-invalid");
  CHECK(ed->set_name(1, std::string(36, 'a')).ok());
  CHECK(ed->set_mbr_type(1, 0x83).code == "wrong-table");
  CHECK(ed->set_bootable(1, true).code == "wrong-table");
  CHECK(ed->set_disk_id("not-a-guid").code == "bad-guid");
  CHECK(ed->set_disk_id("00000000-0000-0000-0000-000000000000").code == "gpt-disk-guid-zero");
  CHECK(ed->new_table(TableKind::Gpt, 64).code == "gpt-entries");
  CHECK(ed->new_table(TableKind::Gpt, 100000).code == "gpt-entries");
  CHECK(ed->set_unique_guid(1, guid_of("00000002-0000-0000-0000-000000000000")).code == "gpt-unique-duplicate");
  // 失敗した操作は何も変えない: 残っているのは set_name だけ
  EditPlan plan;
  CHECK(ed->plan(&plan).ok());
  CHECK(plan.ok);
  CHECK_EQ(plan.changes.size(), 1u);
  // 満杯のエントリ配列
  auto d2 = wdev(blank(65536));
  auto ed2 = open_ed(d2);
  CHECK(ed2->new_table(TableKind::Gpt, 128).ok());
  int made = 0;
  for (int i = 0; i < 129; i++) {
    PartitionSpec s = spec(34 + i * 2, 1);  // 小さな隙間なしの区画
    if (ed2->create(s).ok()) made++;
  }
  CHECK_EQ(made, 128);
}

TEST(partedit_gpt_disk_guid_and_4096_sectors) {
  auto d = wdev(tb::make_gpt_disk(4096, 8192, {{0x28, 64, 127, u"a"}}), 4096);
  auto ed = open_ed(d);
  CHECK_EQ(ed->table().sector_size, 4096u);
  CHECK(ed->set_disk_id("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee").ok());
  uint32_t h;
  CHECK(ed->create(spec(0, 100), &h).ok());
  EditResult r = commit(*ed);
  CHECK(r.ok());
  PartitionTable t;
  ParseOptions po;
  po.sector_size = 4096;
  CHECK(read_partition_table(*d, &t, po).ok());
  CHECK(!t.has_errors());
  CHECK(t.disk_guid == "AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE");
  CHECK_EQ(t.partitions.size(), 2u);
  CHECK_EQ(t.partitions[1].start_lba, 256u);  // 4096 バイトセクタでは 256 セクタ = 1 MiB
}

TEST(partedit_gpt_large_disk_over_2tib) {
  const uint64_t sectors = 8ull * 1024 * 1024 * 1024 / 512 * 512;  // 4 TiB = 8.6e9 セクタ
  auto d = std::make_shared<SparseDevice>(sectors * 512);
  auto ed = open_ed(d);
  CHECK(ed->new_table(TableKind::Gpt).ok());
  uint32_t h;
  CHECK(ed->create(spec(1ull << 32, 4096), &h).ok());  // 32 ビットを超える位置
  CHECK(ed->create(spec(0, 0), &h).ok());              // 空き最大 (末尾まで)
  EditResult r = commit(*ed);
  CHECK(r.ok());
  GptInspection g;
  CHECK(inspect_gpt(*d, 512, &g).ok());
  CHECK(g.mbr_protective_ok);  // サイズは 0xFFFFFFFF に丸められる
  CHECK(g.copies_agree && g.backup_at_end);
  CHECK(d->sectors_held() < 100);  // テーブル以外には書いていない
  // MBR は 2^32 セクタを超えられない
  auto ed2 = open_ed(std::make_shared<SparseDevice>(sectors * 512));
  CHECK(ed2->new_table(TableKind::Mbr).ok());
  CHECK(ed2->create(spec(1ull << 32, 4096)).code == "mbr-lba-limit");
  CHECK(ed2->create(spec(2048, 1ull << 32)).code == "mbr-lba-limit");
  CHECK(ed2->create(spec(2048, 4096)).ok());
}

// ------------------------------------------------------------------ 編集 (MBR)

TEST(partedit_mbr_extended_logical_chain) {
  auto d = wdev(blank(262144));
  auto ed = open_ed(d);
  CHECK(ed->new_table(TableKind::Mbr).ok());
  uint32_t p1, ext, l[12];
  PartitionSpec a = spec(0, 4096, PartRole::Primary);
  a.bootable = true;
  CHECK(ed->create(a, &p1).ok());
  CHECK_EQ(p1, 1u);
  CHECK(ed->create(spec(0, 0, PartRole::Extended), &ext).ok());
  CHECK_EQ(ext, 2u);
  CHECK(!ed->create(spec(0, 0, PartRole::Extended)).ok());  // 拡張は 1 つ
  for (int i = 0; i < 12; i++) {
    PartitionSpec s = spec(0, 4096, PartRole::Logical);
    s.mbr_type = i % 2 ? 0x0C : 0x83;
    CHECK(ed->create(s, &l[i]).ok());
  }
  CHECK_EQ(l[0], 5u);
  CHECK_EQ(l[11], 16u);
  EditResult r = commit(*ed);
  CHECK(r.ok());
  PartitionTable t;
  CHECK(tb::parse(d->bytes(), &t).ok());
  CHECK(!t.has_errors());
  CHECK_EQ(t.kind == TableKind::Mbr, true);
  CHECK_EQ(t.partitions.size(), 14u);  // 主 1 + 拡張 + 論理 12
  size_t logical = 0;
  for (auto& p : t.partitions) logical += p.logical ? 1 : 0;
  CHECK_EQ(logical, 12u);
  CHECK(t.partitions[0].bootable);
  ed.reset();
  // 論理パーティションを真ん中で削除 → 連鎖が再構成される
  auto ed2 = open_ed(d);
  CHECK(ed2->remove(8).ok());
  CHECK(ed2->remove(5).ok());  // 先頭の論理 (EBR が拡張の先頭にある) も消せる
  CHECK(commit(*ed2).ok());
  PartitionTable t2;
  CHECK(tb::parse(d->bytes(), &t2).ok());
  CHECK(!t2.has_errors());
  size_t logical2 = 0;
  for (auto& p : t2.partitions) logical2 += p.logical ? 1 : 0;
  CHECK_EQ(logical2, 10u);
  ed2.reset();
  // 拡張に論理が残っていれば消せない。全部消せば消せる
  auto ed3 = open_ed(d);
  CHECK(ed3->remove(2).code == "extended-not-empty");
  for (auto& p : ed3->table().partitions)
    if (p.logical) CHECK(ed3->remove(p.index).ok());
  CHECK(ed3->remove(2).ok());
  CHECK(commit(*ed3).ok());
  PartitionTable t3;
  CHECK(tb::parse(d->bytes(), &t3).ok());
  CHECK_EQ(t3.partitions.size(), 1u);
  CHECK(!t3.has_errors());
}

TEST(partedit_mbr_modify_existing_and_limits) {
  auto d = wdev(mbr_disk(65536));
  auto ed = open_ed(d);
  CHECK_EQ(ed->table().partitions.size(), 5u);
  CHECK(ed->set_mbr_type(2, 0x07).ok());
  CHECK(ed->set_bootable(1, false).ok());
  CHECK(ed->set_bootable(2, true).ok());
  CHECK(ed->set_disk_id("0xDEADBEEF").ok());
  CHECK(ed->resize(1, 2048).ok());
  CHECK(ed->set_mbr_type(1, 0x05).code == "mbr-extended-type");  // 主 → 拡張には変えられない
  CHECK(ed->set_mbr_type(1, 0).code == "mbr-type-zero");
  CHECK(ed->set_mbr_type(1, 0xEE).code == "mbr-type-protective");
  CHECK(ed->set_bootable(3, true).code == "mbr-extended-active");
  CHECK(ed->set_disk_id("xyz").code == "bad-signature");
  CHECK(ed->set_disk_id("123456789").code == "bad-signature");
  CHECK(ed->resize(3, 4096).code == "extended-excludes-logical");  // 論理が収まらない
  CHECK(ed->move(5, 16384).code != "");                             // 拡張の先頭 (EBR の場所) には動かせない
  CHECK(ed->create(spec(0, 100, PartRole::Primary)).ok());
  CHECK(ed->create(spec(0, 100, PartRole::Primary)).code == "no-slot");  // 4 つ目の主は作れるが、5 つ目は作れない
  EditPlan plan;
  CHECK(ed->plan(&plan).ok());
  CHECK(plan.ok);
  CHECK(commit(*ed).ok());
  PartitionTable t;
  CHECK(tb::parse(d->bytes(), &t).ok());
  CHECK(t.mbr_disk_signature == 0xDEADBEEF);
  CHECK(t.partitions[0].sector_count == 2048 && !t.partitions[0].bootable);
  CHECK(t.partitions[1].mbr_type == 0x07 && t.partitions[1].bootable);
  CHECK_EQ(t.partitions.size(), 6u);
}

TEST(partedit_mbr_noop_plan_changes_nothing) {
  auto d = wdev(mbr_disk(65536));
  auto ed = open_ed(d);
  EditPlan plan;
  CHECK(ed->plan(&plan).ok());
  CHECK(!plan.ok);
  CHECK(has_problem(plan, "no-changes"));
  CHECK_EQ(plan.sector_count(), 0u);
  // 作って消すと元と同じ表になり、書き込みは発生しない
  uint32_t h;
  CHECK(ed->create(spec(0, 100, PartRole::Primary), &h).ok());
  CHECK(ed->remove(h).ok());
  CHECK(ed->plan(&plan).ok());
  CHECK(!plan.ok);
  CHECK(has_problem(plan, "no-changes"));
}

TEST(partedit_new_table_over_filesystem_requires_explicit_flag) {
  Bytes img = blank(8192);
  img[510] = 0x55; img[511] = 0xAA;  // NTFS 風の VBR (パーティション無しのボリューム)
  std::memcpy(&img[3], "NTFS    ", 8);
  auto d = wdev(img);
  auto ed = open_ed(d);
  CHECK(ed->table().whole_device_volume);
  CHECK(ed->new_table(TableKind::Gpt).ok());
  EditPlan plan;
  CHECK(ed->plan(&plan).ok());
  CHECK(!plan.ok);
  CHECK(has_problem(plan, "write-hits-data"));
  CHECK(ed->new_table(TableKind::Gpt, 128, true).ok());
  CHECK(ed->plan(&plan).ok());
  CHECK(plan.ok);
  CHECK(plan.destructive);
}

// ------------------------------------------------------------------ 確認トークン・安全ゲート

TEST(partedit_confirm_token_and_gates) {
  auto d = wdev(gpt_disk3(65536));
  Bytes original = d->bytes();
  auto ed = open_ed(d);
  CHECK(ed->set_name(1, "x").ok());
  EditPlan plan;
  CHECK(ed->plan(&plan).ok());
  CHECK(plan.ok);
  CHECK(plan.confirm_phrase.rfind("WRITE ", 0) == 0);
  ApplyOptions ao;
  ao.backup_sink = [](const Bytes&) { return Status::Ok(); };
  std::string good = PartitionEditor::make_confirm_token(plan, plan.confirm_phrase);
  // 誤った確認文字列 / 空 / 別の文字列
  CHECK(ed->apply(plan, PartitionEditor::make_confirm_token(plan, "WRITE 000000"), ao).code == "bad-token");
  CHECK(ed->apply(plan, PartitionEditor::make_confirm_token(plan, ""), ao).code == "bad-token");
  CHECK(ed->apply(plan, "", ao).code == "bad-token");
  CHECK(ed->apply(plan, "deadbeef", ao).code == "bad-token");
  // 計画の中身を 1 ビット変えるとトークンは無効
  EditPlan t1 = plan;
  t1.stages[0].writes[0].data[100] ^= 1;
  CHECK(ed->apply(t1, good, ao).code == "plan-tampered");
  EditPlan t2 = plan;
  t2.stages[0].writes[0].lba += 1;
  CHECK(ed->apply(t2, good, ao).code == "plan-tampered");
  EditPlan t3 = plan;
  t3.plan_hash[0] = t3.plan_hash[0] == 'a' ? 'b' : 'a';
  CHECK(ed->apply(t3, good, ao).code == "plan-tampered");
  EditPlan t4 = plan;
  t4.device_id = "other";
  CHECK(ed->apply(t4, good, ao).code != "ok");
  CHECK(d->bytes() == original);
  CHECK(d->writes.empty());
  // 別の計画のトークンは使えない
  CHECK(ed->set_name(2, "y").ok());
  EditPlan other;
  CHECK(ed->plan(&other).ok());
  CHECK(ed->apply(other, good, ao).code == "bad-token");
  // 計画後にディスクが変わったら拒否 (書き込みなし)
  CHECK(d->mem_->write_at(512 * 40, "zz", 2).ok());  // テーブル外なので before は同じ…
  Bytes tbl_changed = d->bytes();
  tbl_changed[512 * 2 + 5] ^= 0xFF;                  // プライマリのエントリ配列を直接書き換える
  d->mem_->write_at(512 * 2, &tbl_changed[512 * 2], 512);
  CHECK(ed->apply(other, PartitionEditor::make_confirm_token(other, other.confirm_phrase), ao).code == "device-changed");
  d->bytes() = original;
  CHECK(ed->apply(other, PartitionEditor::make_confirm_token(other, other.confirm_phrase), ao).ok());
}

TEST(partedit_gates_read_only_in_use_exclusive_and_backup_required) {
  Bytes img = gpt_disk3(65536);
  {  // 読み取り専用デバイス: 開けるが適用は拒否。計画・バックアップ・エクスポートは使える
    auto ro = std::make_shared<MemoryDevice>(img, true);
    std::unique_ptr<PartitionEditor> ed;
    CHECK(PartitionEditor::open(ro, EditorOptions{}, &ed).ok());
    CHECK(!ed->writable());
    CHECK(ed->set_name(1, "x").ok());
    EditPlan plan;
    CHECK(ed->plan(&plan).ok());
    CHECK(plan.ok);
    ApplyOptions ao;
    ao.backup_sink = [](const Bytes&) { return Status::Ok(); };
    CHECK(ed->apply(plan, PartitionEditor::make_confirm_token(plan, plan.confirm_phrase), ao).code == "device-read-only");
    Bytes bk;
    CHECK(ed->backup(0, &bk).ok());
    std::string sf;
    CHECK(ed->export_sfdisk(&sf).ok());
    // 読み取り専用のエディタは排他を取らない
    std::unique_ptr<PartitionEditor> ed2;
    CHECK(PartitionEditor::open(ro, EditorOptions{}, &ed2).ok());
  }
  auto d = wdev(img);
  Bytes original = d->bytes();
  {  // 呼び出し側が使用中と申告 → 拒否
    auto ed = open_ed(d, 0, true);
    CHECK(ed->set_name(1, "x").ok());
    EditResult r = commit(*ed);
    CHECK(r.code == "in-use");
    ed->set_in_use(false);
    EditPlan plan;
    CHECK(ed->plan(&plan).ok());
    ApplyOptions ao;  // バックアップ先なし (require_backup 既定 true) → 拒否
    CHECK(ed->apply(plan, PartitionEditor::make_confirm_token(plan, plan.confirm_phrase), ao).code == "backup-required");
    ao.backup_sink = [](const Bytes&) { return Status::Make(Errc::Io, 28); };  // バックアップ保存失敗 → 何も書かない
    CHECK(ed->apply(plan, PartitionEditor::make_confirm_token(plan, plan.confirm_phrase), ao).code == "backup-failed");
    CHECK(d->writes.empty() && d->bytes() == original);
    // 排他: 同じデバイスで 2 つ目の書き込み可エディタは開けない
    std::unique_ptr<PartitionEditor> second;
    EditResult r2 = PartitionEditor::open(d, EditorOptions{}, &second);
    CHECK(r2.code == "busy");
    CHECK(second == nullptr);
  }
  // 1 つ目を閉じれば開ける
  std::unique_ptr<PartitionEditor> again;
  CHECK(PartitionEditor::open(d, EditorOptions{}, &again).ok());
  // 閉じたデバイス / 読めないデバイス
  std::unique_ptr<PartitionEditor> bad;
  CHECK(PartitionEditor::open(nullptr, EditorOptions{}, &bad).code == "bad-argument");
  EditorOptions o;
  o.sector_size = 1000;
  CHECK(PartitionEditor::open(d, o, &bad).code == "bad-sector-size");
}

TEST(partedit_open_propagates_read_error) {
  auto inner = std::make_shared<MemoryDevice>(gpt_disk3(65536), false);
  auto f = std::make_shared<tb::FaultDevice>(inner);
  f->fail_after_reads = 0;
  std::unique_ptr<PartitionEditor> ed;
  EditResult r = PartitionEditor::open(f, EditorOptions{}, &ed);
  CHECK(!r.ok());
  CHECK(r.code == "io-error");
  CHECK(r.status.code == Errc::Io);
}

// ------------------------------------------------------------------ 書き込み順序

TEST(partedit_gpt_write_order_and_flush_per_stage) {
  auto d = wdev(gpt_disk3(65536));
  auto ed = open_ed(d);
  CHECK(ed->new_table(TableKind::Gpt).ok());
  uint32_t h;
  CHECK(ed->create(spec(0, 1000), &h).ok());
  EditPlan plan;
  CHECK(ed->plan(&plan).ok());
  CHECK(plan.ok);
  // 段階: backup-entries → backup-header → primary-entries → primary-header → protective-mbr (内容が同じなら省かれる)
  std::vector<std::string> names;
  for (auto& s : plan.stages) names.push_back(s.name);
  CHECK(names.size() >= 4);
  CHECK(names[0] == "gpt-backup-entries" && names[1] == "gpt-backup-header" && names[2] == "gpt-primary-entries" && names[3] == "gpt-primary-header");
  CHECK(commit(*ed).ok());
  // 実際の書き込み順 = 計画の順 (LBA 一致)
  std::vector<uint64_t> planned;
  for (auto& s : plan.stages) for (auto& w : s.writes) planned.push_back(w.lba);
  std::vector<uint64_t> done;
  for (auto& w : d->writes) done.push_back(w.first / 512);
  CHECK(planned == done);
  // flush は段階ごとに 1 回ずつ。flush 時点の累計書き込み数が段階境界と一致
  CHECK_EQ(d->flush_at.size(), plan.stages.size());
  size_t acc = 0;
  for (size_t i = 0; i < plan.stages.size(); i++) {
    acc += plan.stages[i].writes.size();
    CHECK_EQ(static_cast<size_t>(d->flush_at[i]), acc);
  }
}

// ------------------------------------------------------------------ バックアップ / 復元

TEST(partedit_backup_format_and_corruption_detection) {
  auto d = wdev(gpt_disk3(65536));
  auto ed = open_ed(d);
  Bytes bk;
  CHECK(ed->backup(1700000000ull, &bk).ok());
  BackupInfo bi;
  CHECK(parse_table_backup(bk, &bi).ok());
  CHECK(bi.version == 1 && bi.sector_size == 512 && bi.table_kind == 2 && bi.disk_bytes == 65536ull * 512 && bi.timestamp == 1700000000ull);
  CHECK(bi.disk_id == "04030201-0605-0807-090A-0B0C0D0E0F10" || !bi.disk_id.empty());
  // 末尾の CRC-32 と SHA-256 を独立に検証 (Python 側でも検証する)
  size_t n = bk.size();
  CHECK(crc32(bk.data(), n - 36) == (bk[n - 36] | (bk[n - 35] << 8) | (bk[n - 34] << 16) | (static_cast<uint32_t>(bk[n - 33]) << 24)));
  auto h = Sha256::hash(bk.data(), n - 32);
  CHECK(std::memcmp(h.data(), &bk[n - 32], 32) == 0);
  // 1 バイトのどこを壊しても検出される (全位置)
  size_t undetected = 0;
  for (size_t i = 0; i < bk.size(); i++) {
    Bytes c = bk;
    c[i] ^= 0x01;
    if (parse_table_backup(c, &bi).ok()) undetected++;
  }
  CHECK_EQ(undetected, 0u);
  // 切り詰め / 空 / 別形式
  for (size_t cut : {size_t(0), size_t(1), size_t(63), size_t(100), n - 1, n - 36, n - 37}) {
    Bytes c(bk.begin(), bk.begin() + static_cast<std::ptrdiff_t>(cut));
    CHECK(!parse_table_backup(c, &bi).ok());
  }
  CHECK(!parse_table_backup(Bytes(4096, 0xAA), &bi).ok());
  EditPlan plan;
  Bytes bad = bk;
  bad[200] ^= 0xFF;
  CHECK(ed->plan_restore(bad, &plan).code == "backup-corrupt");
}

TEST(partedit_backup_restore_roundtrip_gpt) {
  auto d = wdev(gpt_disk3(65536));
  Bytes original = d->bytes();
  Snap before = snap(*d);
  auto ed = open_ed(d);
  Bytes auto_backup;
  CHECK(ed->remove(1).ok());
  CHECK(ed->create(spec(0, 8000)).ok());
  CHECK(ed->set_name(2, "changed").ok());
  CHECK(commit(*ed, nullptr, &auto_backup).ok());  // 適用前に自動バックアップが渡される
  CHECK(snap(*d) != before);
  // 自動バックアップは「適用前」の状態を持つ
  BackupInfo bi;
  CHECK(parse_table_backup(auto_backup, &bi).ok());
  CHECK(bi.table_kind == 2);
  EditPlan rp;
  CHECK(ed->plan_restore(auto_backup, &rp).ok());
  CHECK(rp.ok);
  CHECK(!rp.requires_force);
  CHECK(rp.confirm_phrase.rfind("WRITE ", 0) == 0);
  CHECK(rp.destructive);
  Bytes undo_backup;
  ApplyOptions ao;
  ao.backup_sink = [&](const Bytes& b) { undo_backup = b; return Status::Ok(); };
  CHECK(ed->apply(rp, PartitionEditor::make_confirm_token(rp, rp.confirm_phrase), ao).ok());
  CHECK(snap(*d) == before);
  // テーブル領域のバイト列も完全一致 (保護 MBR・両ヘッダ・両配列)
  CHECK(d->bytes() == original);
  CHECK(!undo_backup.empty());  // 復元自体の前にも現状がバックアップされる
  // 元に戻した上で、さらにやり直し (undo の undo)
  EditPlan rp2;
  CHECK(ed->plan_restore(undo_backup, &rp2).ok());
  CHECK(rp2.ok);
}

TEST(partedit_restore_mbr_and_table_kind_switch) {
  auto d = wdev(mbr_disk(65536));
  Bytes original = d->bytes();
  auto ed = open_ed(d);
  Bytes bk;
  CHECK(ed->backup(0, &bk).ok());
  CHECK(ed->new_table(TableKind::Gpt).ok());
  CHECK(ed->create(spec(0, 0)).ok());
  CHECK(commit(*ed).ok());
  CHECK(snap(*d).kind == TableKind::Gpt);
  EditPlan rp;
  CHECK(ed->plan_restore(bk, &rp).ok());
  CHECK(rp.ok);
  // 復元後は GPT のヘッダが残らない (古いヘッダを消す段階がある)
  bool erase = false;
  for (auto& s : rp.stages) if (s.name.find("erase") != std::string::npos) erase = true;
  CHECK(erase);
  ApplyOptions ao;
  ao.backup_sink = [](const Bytes&) { return Status::Ok(); };
  CHECK(ed->apply(rp, PartitionEditor::make_confirm_token(rp, rp.confirm_phrase), ao).ok());
  Snap s = snap(*d);
  CHECK(s.kind == TableKind::Mbr);
  PartitionTable t;
  CHECK(tb::parse(d->bytes(), &t).ok());
  CHECK(!t.has_errors());
  CHECK_EQ(t.partitions.size(), 5u);
  GptInspection g;
  CHECK(inspect_gpt(*d, 512, &g).ok());
  CHECK(!g.any_present());
  // MBR → GPT → MBR でも、MBR の表の内容 (サイズ・種別) は元通り
  PartitionTable o;
  CHECK(tb::parse(original, &o).ok());
  for (size_t i = 0; i < 5; i++) CHECK(o.partitions[i].start_lba == t.partitions[i].start_lba && o.partitions[i].mbr_type == t.partitions[i].mbr_type);
}

TEST(partedit_restore_mismatch_requires_force) {
  auto small = wdev(gpt_disk3(65536));
  auto es = open_ed(small);
  Bytes bk;
  CHECK(es->backup(0, &bk).ok());
  es.reset();
  // 小さいバックアップを大きなディスクへ (サイズ違い → force)
  auto big = wdev(blank(131072));
  auto eb = open_ed(big);
  EditPlan rp;
  CHECK(eb->plan_restore(bk, &rp).ok());
  CHECK(rp.ok);
  CHECK(rp.requires_force);
  CHECK(rp.confirm_phrase.rfind("FORCE ", 0) == 0);
  CHECK(has_problem(rp, "restore-disk-size-mismatch"));
  ApplyOptions ao;
  ao.backup_sink = [](const Bytes&) { return Status::Ok(); };
  // WRITE 用の文字列では通らない
  std::string wrong = "WRITE " + rp.confirm_phrase.substr(6);
  CHECK(eb->apply(rp, PartitionEditor::make_confirm_token(rp, wrong), ao).code == "bad-token");
  CHECK(big->writes.empty());
  CHECK(eb->apply(rp, PartitionEditor::make_confirm_token(rp, rp.confirm_phrase), ao).ok());
  // 復元後は「バックアップが末尾にない」ことが見え、移動で直せる
  GptInspection g;
  CHECK(inspect_gpt(*big, 512, &g).ok());
  CHECK(g.disk_grew);
  CHECK(g.suggested_repairs.size() >= 1 && g.suggested_repairs[0] == GptRepairKind::MoveBackupToEnd);
  eb.reset();
  // 大きなバックアップを小さなディスクへは、範囲外なので強制でも不可
  auto ebig = open_ed(big);
  Bytes bk2;
  CHECK(ebig->repair_gpt(GptRepairKind::MoveBackupToEnd).ok());
  CHECK(commit(*ebig).ok());
  CHECK(ebig->backup(0, &bk2).ok());
  ebig.reset();
  auto tiny = wdev(blank(32768));
  auto et = open_ed(tiny);
  EditPlan rp2;
  CHECK(et->plan_restore(bk2, &rp2).ok());
  CHECK(!rp2.ok);
  CHECK(has_problem(rp2, "backup-sector-out-of-range"));
  // セクタサイズの違い
  auto s4k = wdev(blank(16384, 4096), 4096);
  auto e4 = open_ed(s4k);
  EditPlan rp3;
  CHECK(e4->plan_restore(bk, &rp3).ok());
  CHECK(has_problem(rp3, "restore-sector-size-mismatch"));
  CHECK(rp3.requires_force);
}

TEST(partedit_sfdisk_export_text) {
  auto d = wdev(gpt_disk3(65536));
  auto ed = open_ed(d);
  CHECK(ed->set_name(1, "a \"q\" \\b").ok());
  CHECK(ed->set_attrs(2, 0x1000000000000005ull).ok());
  CHECK(commit(*ed).ok());
  std::string t;
  CHECK(ed->export_sfdisk(&t, "/dev/sdx").ok());
  CHECK(t.find("label: gpt\n") == 0);
  CHECK(t.find("label-id: 13121110-1514-1716-1819-1A1B1C1D1E1F") != std::string::npos);  // make_gpt_disk のディスク GUID
  CHECK(t.find("first-lba: 34\nlast-lba: 65502\nsector-size: 512\n") != std::string::npos);
  CHECK(t.find("/dev/sdx1 : start=") != std::string::npos);
  CHECK(t.find("name=\"a \\\"q\\\" \\\\b\"") != std::string::npos);
  CHECK(t.find("attrs=\"RequiredPartition LegacyBIOSBootable GUID:60\"") != std::string::npos);
  auto m = wdev(mbr_disk(65536));
  auto em = open_ed(m);
  std::string u;
  CHECK(em->export_sfdisk(&u, "/dev/loop0").ok());
  CHECK(u.find("label: dos\nlabel-id: 0xc0ffee01\n") == 0);
  CHECK(u.find("/dev/loop0p1 : start=") != std::string::npos);
  CHECK(u.find("type=83, bootable") != std::string::npos);
  CHECK(u.find("type=c\n") != std::string::npos);
  CHECK(u.find("type=5\n") != std::string::npos);
  auto blk = wdev(blank(4096));
  auto eb = open_ed(blk);
  std::string v;
  CHECK(eb->export_sfdisk(&v).code == "no-table");
}

// ------------------------------------------------------------------ GPT 修復

TEST(partedit_gpt_repairs) {
  Bytes img = gpt_disk3(65536);
  {  // プライマリ破損 → バックアップから復元
    Bytes a = img;
    a[512 + 40] ^= 0xFF;
    auto d = wdev(a);
    auto ed = open_ed(d);
    CHECK(ed->table().used_backup_gpt);
    CHECK(ed->repair_gpt(GptRepairKind::CopyBackupToPrimary).ok());
    EditResult r = commit(*ed);
    CHECK(r.ok());
    GptInspection g;
    CHECK(inspect_gpt(*d, 512, &g).ok());
    CHECK(g.copies_agree);
    CHECK(snap(*d).parts.size() == 3);
    CHECK(d->bytes() == img);  // 元の健全な画像と同一
  }
  {  // バックアップ破損 → プライマリから複製
    Bytes b = img;
    b[(65536 - 1) * 512 + 7] ^= 0xFF;
    auto d = wdev(b);
    auto ed = open_ed(d);
    CHECK(ed->repair_gpt(GptRepairKind::CopyPrimaryToBackup).ok());
    CHECK(commit(*ed).ok());
    CHECK(d->bytes() == img);
  }
  {  // ディスクが大きくなった → バックアップを末尾へ
    Bytes c = img;
    c.resize(131072 * 512, 0);
    auto d = wdev(c);
    auto ed = open_ed(d);
    CHECK(ed->repair_gpt(GptRepairKind::MoveBackupToEnd).ok());
    EditPlan plan;
    CHECK(ed->plan(&plan).ok());
    CHECK(plan.ok);
    CHECK(!plan.destructive);
    CHECK(commit(*ed).ok());
    GptInspection g;
    CHECK(inspect_gpt(*d, 512, &g).ok());
    CHECK(g.copies_agree && g.backup_at_end && !g.disk_grew && g.mbr_protective_ok);
    CHECK_EQ(g.primary.last_usable, 131072u - 34u);
    PartitionTable t;
    CHECK(tb::parse(d->bytes(), &t).ok());
    CHECK(!t.has_errors());
    CHECK(t.diagnostics.empty() || !tb::has_diag(t, "gpt-backup-not-at-end"));
    // 古い位置のバックアップヘッダ / 配列は消えている
    CHECK(all_zero_range(d->bytes(), (65536 - 1) * 512, 512));
  }
  {  // 保護 MBR の再作成だけ
    Bytes e = img;
    tb::le32(e, 446 + 12, 100);
    auto d = wdev(e);
    auto ed = open_ed(d);
    CHECK(ed->repair_gpt(GptRepairKind::RebuildProtectiveMbr).ok());
    EditPlan plan;
    CHECK(ed->plan(&plan).ok());
    CHECK(plan.ok && plan.sector_count() == 1);
    CHECK(commit(*ed).ok());
    GptInspection g;
    CHECK(inspect_gpt(*d, 512, &g).ok());
    CHECK(g.mbr_protective_ok);
  }
  {  // 両方壊れている → 修復不能。新規作成か復元を案内する
    Bytes f = img;
    f[512 + 40] ^= 0xFF;
    f[(65536 - 1) * 512 + 7] ^= 0xFF;
    auto d = wdev(f);
    auto ed = open_ed(d);
    CHECK(ed->repair_gpt(GptRepairKind::MoveBackupToEnd).code == "gpt-unusable");
    CHECK(ed->create(spec(0, 100)).code == "gpt-unusable");
    CHECK(ed->new_table(TableKind::Gpt).ok());
  }
  {  // 修復は最初の操作としてのみ
    auto d = wdev(img);
    auto ed = open_ed(d);
    CHECK(ed->set_name(1, "z").ok());
    CHECK(ed->repair_gpt(GptRepairKind::CopyPrimaryToBackup).code == "repair-not-first");
  }
  {  // 縮んだイメージ: 末尾が欠けている (バックアップがディスクの外)
    Bytes g2 = img;
    g2.resize(40000 * 512);
    auto d = wdev(g2);
    auto ed = open_ed(d);
    GptInspection gi = ed->gpt_inspection();
    CHECK(gi.backup_beyond_disk);
    CHECK(ed->repair_gpt(GptRepairKind::MoveBackupToEnd).ok());
    EditPlan plan;
    CHECK(ed->plan(&plan).ok());
    CHECK(plan.ok);  // パーティションは 20479 まで: 縮んだ後の使用可能範囲に収まる
    CHECK(commit(*ed).ok());
    GptInspection g3;
    CHECK(inspect_gpt(*d, 512, &g3).ok());
    CHECK(g3.copies_agree && g3.backup_at_end);
  }
}
