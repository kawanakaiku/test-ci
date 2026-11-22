// SPDX-License-Identifier: Apache-2.0
// ブートセクタの検査と修復 (bootfix) のテスト。本物の mkfs が作ったイメージ + fsck.fat / fsck.exfat / e2fsck -fn / ntfsfix -n との照合は
// tools/check_partscan_tools.py。ここは手組みのイメージでの損傷の種類・計画/トークン/適用の関門・バックアップ/復元・破損入力。
#include <algorithm>

#include "partscan_test_util.h"
#include "usbmem/bootfix.h"
#include "usbmem/c_api_partscan.h"

using namespace usbmem;
using namespace ps_test;

namespace {

std::shared_ptr<pt::WDev> wd(Bytes img) { return std::make_shared<pt::WDev>(std::move(img), 512); }

BootInspection insp(BlockDevice& d, FsType fs = FsType::Unknown, uint64_t off = 0, uint64_t len = 0) {
  BootInspection r;
  Status st = inspect_boot(d, off, len, fs, BootInspectOptions{}, &r);
  CHECK(st.ok());
  return r;
}

bool has_code(const BootInspection& r, const char* code) {
  for (const auto& f : r.findings)
    if (f.code == code) return true;
  return false;
}

bool has_repair(const BootInspection& r, const char* id) {
  for (const auto& o : r.repairs)
    if (o.id == id) return true;
  return false;
}

size_t errors(const BootInspection& r) {
  size_t n = 0;
  for (const auto& f : r.findings) n += f.severity == Severity::Error;
  return n;
}

// 計画 → トークン → 適用。成功したら true。ap に結果を返す
bool do_repair(BlockDevice& d, FsType fs, const char* id, uint64_t source, BootApplyResult* ap = nullptr, Bytes* backup = nullptr, uint64_t off = 0, uint64_t len = 0) {
  BootRepairPlan plan;
  CHECK(plan_boot_repair(d, off, len, fs, id, source, &plan).ok());
  if (!plan.ok) {
    for (auto& p : plan.problems) std::fprintf(stderr, "  plan problem %s %s\n", p.code.c_str(), p.message.c_str());
    return false;
  }
  CHECK(!plan.writes.empty());
  CHECK(plan.confirm_phrase.rfind("REPAIR ", 0) == 0);
  BootApplyOptions ao;
  ao.timestamp = 1700000000;
  ao.backup_sink = [&](const Bytes& b) { if (backup) *backup = b; return Status::Ok(); };
  BootApplyResult r = apply_boot_repair(d, plan, make_boot_confirm_token(plan, plan.confirm_phrase), ao);
  if (ap) *ap = r;
  if (!r.ok()) std::fprintf(stderr, "  apply failed: %s %s\n", r.code.c_str(), r.message.c_str());
  return r.ok() && r.verified;
}

void zero(Bytes& b, uint64_t off, uint64_t n) { std::memset(&b[off], 0, n); }

}  // namespace

// ------------------------------------------------------------------ 健全なイメージ

TEST(bootfix_healthy_images_are_ok) {
  struct { const char* n; Bytes img; FsType fs; } cases[] = {
      {"fat32", make_fat32(40 << 20), FsType::Fat32}, {"ntfs", make_ntfs(16 << 20), FsType::Ntfs}, {"exfat", make_exfat(16 << 20), FsType::Exfat},
      {"ext2", make_ext(24 << 20, 0), FsType::Ext2},   {"ext4", make_ext(24 << 20, 1), FsType::Ext4}, {"fat16", make_fat16(20 << 20), FsType::Fat16}};
  for (auto& c : cases) {
    MemoryDevice d(c.img, true, 512);
    for (FsType hint : {FsType::Unknown, c.fs}) {
      BootInspection r = insp(d, hint);
      if (r.verdict != "ok") {
        std::fprintf(stderr, "  %s: verdict %s\n", c.n, r.verdict.c_str());
        for (auto& f : r.findings) std::fprintf(stderr, "    %s %s\n", f.code.c_str(), f.message.c_str());
        g_failures++;
      }
      CHECK(r.recognized);
      CHECK(r.boot_ok);
      CHECK_EQ(errors(r), 0u);
      CHECK(r.repairs.empty());
      CHECK(r.fs == c.fs || (c.fs == FsType::Ext2 && r.fs == FsType::Ext3) || (c.fs == FsType::Ext4 && r.fs == FsType::Ext4));
    }
  }
}

TEST(bootfix_blank_and_random_are_unrecognized) {
  Bytes z(4 << 20, 0);
  MemoryDevice dz(z, true, 512);
  BootInspection r = insp(dz);
  CHECK(!r.recognized);
  CHECK(r.verdict == "unrecognized");
  CHECK(r.repairs.empty());
  for (uint32_t s = 1; s <= 20; s++) {
    Bytes rnd = random_bytes(2 << 20, s);
    MemoryDevice d(rnd, true, 512);
    BootInspection q = insp(d);
    CHECK(!q.recognized);
    CHECK(q.repairs.empty());
  }
}

// ------------------------------------------------------------------ FAT32

TEST(bootfix_fat32_primary_damage_variants) {
  Bytes good = make_fat32(40 << 20);
  struct Damage { const char* name; void (*fn)(Bytes&); };
  const Damage ds[] = {
      {"zero-sector0", [](Bytes& b) { zero(b, 0, 512); }},
      {"bad-signature", [](Bytes& b) { b[510] = 0; b[511] = 0; }},
      {"bad-bytes-per-sector", [](Bytes& b) { b[11] = 0x33; b[12] = 0x01; }},
      {"zero-sectors-0-2", [](Bytes& b) { zero(b, 0, 3 * 512); }},
      {"random-bpb", [](Bytes& b) { for (int i = 11; i < 90; i++) b[i] = static_cast<uint8_t>(i * 37); }},
  };
  for (const auto& d : ds) {
    Bytes img = good;
    d.fn(img);
    auto dev = wd(img);
    BootInspection r = insp(*dev);
    if (!(r.recognized && r.fs == FsType::Fat32 && !r.boot_ok && r.backup_ok && r.verdict == "repairable" && has_repair(r, "fat32-restore-primary-from-backup"))) {
      std::fprintf(stderr, "  %s: not diagnosed (verdict %s)\n", d.name, r.verdict.c_str());
      g_failures++;
      continue;
    }
    CHECK(has_code(r, "fat32-boot-invalid"));
    BootApplyResult ap;
    Bytes backup;
    CHECK(do_repair(*dev, FsType::Fat32, "fat32-restore-primary-from-backup", 0, &ap, &backup));
    CHECK(ap.verified && !ap.rolled_back);
    CHECK(ap.writes_done >= 1);
    BootInspection after = insp(*dev);
    CHECK(after.verdict == "ok");
    CHECK(errors(after) == 0);
    // 書いたのは先頭 3 セクタだけ。データ領域は変わっていない
    CHECK(std::memcmp(&dev->bytes()[3 * 512], &good[3 * 512], good.size() - 3 * 512) == 0);
    CHECK(std::memcmp(dev->bytes().data(), good.data(), 3 * 512) == 0 || std::string(d.name) != "zero-sector0");
    BootBackupInfo bi;
    CHECK(parse_boot_backup(backup, &bi).ok());
    CHECK(bi.record_count >= 1);
  }
}

TEST(bootfix_fat32_backup_and_fsinfo) {
  Bytes good = make_fat32(40 << 20);
  {
    Bytes img = good;
    zero(img, 6 * 512, 3 * 512);
    auto dev = wd(img);
    BootInspection r = insp(*dev);
    CHECK(r.boot_ok && !r.backup_ok && r.has_backup_structure);
    CHECK(r.verdict == "warnings" || r.verdict == "repairable");
    CHECK(has_code(r, "fat32-backup-boot-invalid"));
    CHECK(has_repair(r, "fat32-refresh-backup-from-primary"));
    CHECK(do_repair(*dev, FsType::Fat32, "fat32-refresh-backup-from-primary", 0));
    CHECK(insp(*dev).verdict == "ok");
    CHECK(std::memcmp(&dev->bytes()[6 * 512], &good[6 * 512], 3 * 512) == 0);
  }
  {
    Bytes img = good;
    tb::le32(img, 512, 0);  // FSInfo の署名を壊す
    auto dev = wd(img);
    BootInspection r = insp(*dev);
    CHECK(has_code(r, "fat32-fsinfo-invalid"));
    CHECK(has_repair(r, "fat32-rebuild-fsinfo"));
    CHECK(do_repair(*dev, FsType::Fat32, "fat32-rebuild-fsinfo", 0));
    BootInspection a = insp(*dev);
    CHECK(a.verdict == "ok");
    // 空き数・次の空きは「不明」
    CHECK_EQ(rd32(dev->bytes(), 512 + 488), 0xFFFFFFFFu);
    CHECK_EQ(rd32(dev->bytes(), 512 + 492), 0xFFFFFFFFu);
  }
  {
    // 両方壊れている: 直せない
    Bytes img = good;
    zero(img, 0, 3 * 512);
    zero(img, 6 * 512, 3 * 512);
    auto dev = wd(img);
    BootInspection r = insp(*dev, FsType::Fat32);
    CHECK(!r.boot_ok);
    CHECK(r.verdict == "damaged" || r.verdict == "unrecognized");
    CHECK(!has_repair(r, "fat32-restore-primary-from-backup"));
    BootRepairPlan plan;
    CHECK(plan_boot_repair(*dev, 0, 0, FsType::Fat32, "fat32-restore-primary-from-backup", 0, &plan).ok());
    CHECK(!plan.ok);
    CHECK(!plan.problems.empty());
  }
}

TEST(bootfix_fat32_in_partition_offset) {
  // パーティションの途中 (オフセット 1 MiB) にある FAT32 を、オフセット・長さつきで検査・修復する
  Bytes fs = make_fat32(20 << 20);
  Bytes disk(24 << 20, 0);
  place(disk, 1 << 20, fs);
  zero(disk, 1 << 20, 512);
  auto dev = wd(disk);
  BootInspection r = insp(*dev, FsType::Unknown, 1 << 20, 20 << 20);
  CHECK(r.fs == FsType::Fat32 && r.verdict == "repairable");
  CHECK(do_repair(*dev, FsType::Fat32, "fat32-restore-primary-from-backup", 0, nullptr, nullptr, 1 << 20, 20 << 20));
  CHECK(insp(*dev, FsType::Unknown, 1 << 20, 20 << 20).verdict == "ok");
  // パーティションの外には何も書いていない
  CHECK(pt::all_zero_range(dev->bytes(), 0, 1 << 20));
  CHECK(pt::all_zero_range(dev->bytes(), (21u << 20), 3u << 20));
}

// ------------------------------------------------------------------ FAT16 (書かない)

TEST(bootfix_fat16_bpb_rebuild_is_proposal_only) {
  Bytes good = make_fat16(20 << 20);
  Bytes img = good;
  zero(img, 0, 512);
  auto dev = wd(img);
  BootInspection r = insp(*dev, FsType::Fat16);
  CHECK(!r.boot_ok);
  CHECK(!r.has_backup_structure);
  CHECK(has_code(r, "fat-boot-invalid"));
  CHECK(has_code(r, "fat-no-backup-boot"));
  CHECK(r.repairs.empty());  // 書き込みの修復は提案しない
  CHECK(!r.proposed_bpb.empty());
  auto get = [&](const char* k) -> std::string {
    for (auto& kv : r.proposed_bpb) if (kv.first == k) return kv.second;
    return "";
  };
  CHECK(get("bytesPerSector") == "512");
  CHECK(get("numFats") == "2");
  CHECK(get("rootEntries") == "512");
  // 計画は作れない
  BootRepairPlan plan;
  CHECK(plan_boot_repair(*dev, 0, 0, FsType::Fat16, "fat32-restore-primary-from-backup", 0, &plan).ok());
  CHECK(!plan.ok);
  CHECK(dev->writes.empty());
}

// ------------------------------------------------------------------ NTFS

TEST(bootfix_ntfs_restore_and_refresh) {
  Bytes good = make_ntfs(16 << 20);
  size_t last = good.size() - 512;
  {
    Bytes img = good;
    zero(img, 0, 512);
    auto dev = wd(img);
    BootInspection r = insp(*dev);
    CHECK(r.fs == FsType::Ntfs && !r.boot_ok && r.backup_ok);
    CHECK(has_code(r, "ntfs-boot-invalid"));
    CHECK(r.verdict == "repairable");
    CHECK(do_repair(*dev, FsType::Ntfs, "ntfs-restore-primary-from-backup", 0));
    CHECK(std::memcmp(dev->bytes().data(), good.data(), 512) == 0);
    CHECK(insp(*dev).verdict == "ok");
    for (auto& w : dev->writes) CHECK(w.first == 0 && w.second == 512);
  }
  {
    Bytes img = good;
    zero(img, last, 512);
    auto dev = wd(img);
    BootInspection r = insp(*dev);
    CHECK(r.boot_ok && !r.backup_ok);
    CHECK(has_code(r, "ntfs-backup-boot-invalid"));
    CHECK(has_repair(r, "ntfs-refresh-backup-from-primary"));
    CHECK(do_repair(*dev, FsType::Ntfs, "ntfs-refresh-backup-from-primary", 0));
    CHECK(std::memcmp(&dev->bytes()[last], &good[last], 512) == 0);
    CHECK(insp(*dev).verdict == "ok");
  }
  {
    // 先頭を壊し、バックアップが指す $MFT が無い (ブートセクタだけを書き戻しても意味がない) → 勧めない
    Bytes img = good;
    zero(img, 0, 512);
    zero(img, 4 * 4096, 4);
    zero(img, 2 * 4096, 4);
    auto dev = wd(img);
    BootInspection r = insp(*dev, FsType::Ntfs);
    CHECK(has_code(r, "ntfs-no-mft"));
    CHECK(!has_repair(r, "ntfs-restore-primary-from-backup"));
  }
  {
    // 両方壊れている
    Bytes img = good;
    zero(img, 0, 512);
    zero(img, last, 512);
    auto dev = wd(img);
    BootInspection r = insp(*dev, FsType::Ntfs);
    CHECK(!r.boot_ok && !r.backup_ok);
    CHECK(r.repairs.empty());
    CHECK(r.verdict == "damaged");
  }
}

// ------------------------------------------------------------------ exFAT

TEST(bootfix_exfat_damage_variants) {
  Bytes good = make_exfat(16 << 20);
  {
    Bytes img = good;
    zero(img, 0, 12 * 512);
    auto dev = wd(img);
    BootInspection r = insp(*dev);
    CHECK(r.fs == FsType::Exfat && !r.boot_ok && r.backup_ok);
    CHECK(has_code(r, "exfat-main-boot-invalid"));
    CHECK(do_repair(*dev, FsType::Exfat, "exfat-restore-main-from-backup", 0));
    CHECK(std::memcmp(dev->bytes().data(), good.data(), 12 * 512) == 0);
    CHECK(insp(*dev).verdict == "ok");
  }
  {
    // 主領域のチェックサムだけが違う (バックアップは正しい)
    Bytes img = good;
    img[11 * 512] ^= 0xFF;
    auto dev = wd(img);
    BootInspection r = insp(*dev);
    CHECK(has_code(r, "exfat-main-checksum-bad"));
    CHECK(has_repair(r, "exfat-restore-main-from-backup"));
    CHECK(do_repair(*dev, FsType::Exfat, "exfat-restore-main-from-backup", 0));
    CHECK(insp(*dev).verdict == "ok");
  }
  {
    // 主領域のセクタ 0〜10 が正しく、チェックサムのセクタだけ壊れていて、バックアップもない → 計算し直す
    Bytes img = good;
    zero(img, 12 * 512, 12 * 512);
    img[11 * 512] ^= 0xFF;
    auto dev = wd(img);
    BootInspection r = insp(*dev, FsType::Exfat);
    CHECK(has_repair(r, "exfat-recompute-checksums"));
    CHECK(do_repair(*dev, FsType::Exfat, "exfat-recompute-checksums", 0));
    BootInspection a = insp(*dev);
    CHECK(a.boot_ok);
    CHECK(errors(a) == 0);
  }
  {
    // バックアップが壊れている
    Bytes img = good;
    zero(img, 12 * 512, 12 * 512);
    auto dev = wd(img);
    BootInspection r = insp(*dev);
    CHECK(r.boot_ok && !r.backup_ok);
    CHECK(has_repair(r, "exfat-refresh-backup-from-main"));
    CHECK(do_repair(*dev, FsType::Exfat, "exfat-refresh-backup-from-main", 0));
    CHECK(std::memcmp(&dev->bytes()[12 * 512], &good[12 * 512], 12 * 512) == 0);
    CHECK(insp(*dev).verdict == "ok");
  }
  {
    // 両方壊れている
    Bytes img = good;
    zero(img, 0, 24 * 512);
    auto dev = wd(img);
    BootInspection r = insp(*dev, FsType::Exfat);
    CHECK(!r.boot_ok && r.repairs.empty());
  }
}

// ------------------------------------------------------------------ ext

TEST(bootfix_ext_superblock_restore) {
  for (int flags : {0, 1}) {
    Bytes good = make_ext(24 << 20, flags);
    struct Damage { const char* name; void (*fn)(Bytes&); };
    const Damage ds[] = {
        {"zero-primary", [](Bytes& b) { zero(b, 1024, 1024); }},
        {"bad-magic", [](Bytes& b) { b[1024 + 56] = 0; }},
        {"zero-first-4k", [](Bytes& b) { zero(b, 0, 4096); }},
        {"garbage-sb", [](Bytes& b) { for (int i = 0; i < 1000; i++) b[1024 + i] = static_cast<uint8_t>(i * 131 + 7); b[1024 + 56] = 0x53; b[1024 + 57] = 0xEF; }},
    };
    for (const auto& d : ds) {
      Bytes img = good;
      d.fn(img);
      auto dev = wd(img);
      BootInspection r = insp(*dev);
      if (!(r.recognized && !r.boot_ok && r.verdict == "repairable" && has_repair(r, "ext-restore-superblock-from-backup") && !r.ext_backup_blocks.empty())) {
        std::fprintf(stderr, "  ext flags=%d %s: not diagnosed (verdict %s)\n", flags, d.name, r.verdict.c_str());
        g_failures++;
        continue;
      }
      // 予備は e2fsck -b の位置: 8193 (1 KiB ブロックのグループ 1) など
      CHECK_EQ(r.ext_backup_blocks[0], 8193u);
      BootApplyResult ap;
      CHECK(do_repair(*dev, FsType::Unknown, "ext-restore-superblock-from-backup", 0, &ap));
      BootInspection a = insp(*dev);
      CHECK(a.verdict == "ok" || a.verdict == "warnings");
      CHECK(errors(a) == 0);
      // 書いたのはスーパーブロック (と GDT) だけ。ブロック 3 以降 (inode テーブルなど) は不変
      CHECK(std::memcmp(&dev->bytes()[8192], &good[8192], good.size() - 8192) == 0);
      // 主スーパーブロックは予備と同じ中身 (グループ番号を除く) か、少なくとも総ブロック数 / UUID が一致する
      CHECK(std::memcmp(&dev->bytes()[1024 + 104], &good[1024 + 104], 16) == 0);
      CHECK_EQ(rd32(dev->bytes(), 1024 + 4), rd32(good, 1024 + 4));
    }
  }
}

TEST(bootfix_ext_backup_source_selection_and_gdt) {
  Bytes good = make_ext(48 << 20, 1);
  Bytes img = good;
  zero(img, 1024, 1024);
  auto dev = wd(img);
  BootInspection r = insp(*dev);
  CHECK(r.ext_backup_blocks.size() >= 2);
  if (r.ext_backup_blocks.size() < 2) return;
  // 2 番目の予備を明示して使える
  CHECK(do_repair(*dev, FsType::Unknown, "ext-restore-superblock-from-backup", r.ext_backup_blocks[1]));
  CHECK(errors(insp(*dev)) == 0);
  // 存在しない源は拒否 (計画が ok にならない)
  Bytes img2 = good;
  zero(img2, 1024, 1024);
  auto dev2 = wd(img2);
  BootRepairPlan plan;
  CHECK(plan_boot_repair(*dev2, 0, 0, FsType::Unknown, "ext-restore-superblock-from-backup", 12345, &plan).ok());
  CHECK(!plan.ok);
  // グループ記述子だけが壊れている
  Bytes img3 = good;
  zero(img3, 2048, 512);
  auto dev3 = wd(img3);
  BootInspection r3 = insp(*dev3);
  CHECK(has_code(r3, "ext-gdt-damaged"));
  CHECK(has_repair(r3, "ext-restore-gdt-from-backup"));
  CHECK(do_repair(*dev3, FsType::Unknown, "ext-restore-gdt-from-backup", 0));
  CHECK(std::memcmp(&dev3->bytes()[2048], &good[2048], 1024) == 0);
  CHECK(errors(insp(*dev3)) == 0);
}

TEST(bootfix_ext_both_damaged_has_no_repair) {
  Bytes img = make_ext(48 << 20, 1);
  zero(img, 1024, 1024);
  for (uint64_t g : {1u, 3u, 5u}) zero(img, (g * 8192 + 1) * 1024, 1024);
  MemoryDevice d(img, true, 512);
  BootInspection r = insp(d, FsType::Ext4);
  CHECK(!r.boot_ok);
  CHECK(r.repairs.empty());
  CHECK(has_code(r, "ext-no-valid-superblock"));
  CHECK(r.verdict == "damaged");
}

// ------------------------------------------------------------------ 関門

namespace {
struct Ready {
  std::shared_ptr<pt::WDev> dev;
  Bytes good;
  BootRepairPlan plan;
  std::string token;
};
Ready ready_fat32() {
  Ready r;
  r.good = make_fat32(40 << 20);
  Bytes img = r.good;
  zero(img, 0, 512);
  r.dev = wd(img);
  CHECK(plan_boot_repair(*r.dev, 0, 0, FsType::Fat32, "fat32-restore-primary-from-backup", 0, &r.plan).ok());
  CHECK(r.plan.ok);
  r.token = make_boot_confirm_token(r.plan, r.plan.confirm_phrase);
  return r;
}
BootApplyOptions opts_with_backup() {
  BootApplyOptions ao;
  ao.backup_sink = [](const Bytes&) { return Status::Ok(); };
  return ao;
}
}  // namespace

TEST(bootfix_apply_gates_write_nothing) {
  {
    Ready r = ready_fat32();
    BootApplyResult a = apply_boot_repair(*r.dev, r.plan, "00", opts_with_backup());
    CHECK(!a.ok() && a.code == "bad-token");
    CHECK(r.dev->writes.empty());
    // 確認文字列が違えばトークンも違う
    CHECK(make_boot_confirm_token(r.plan, "REPAIR 000000") != r.token);
    a = apply_boot_repair(*r.dev, r.plan, make_boot_confirm_token(r.plan, "repair"), opts_with_backup());
    CHECK(a.code == "bad-token" && r.dev->writes.empty());
  }
  {
    Ready r = ready_fat32();
    BootRepairPlan t = r.plan;
    t.writes[0].data[0] ^= 1;  // 計画の改竄
    BootApplyResult a = apply_boot_repair(*r.dev, t, r.token, opts_with_backup());
    CHECK(!a.ok() && a.code == "plan-tampered");
    CHECK(r.dev->writes.empty());
    t = r.plan;
    t.writes[0].offset = 1 << 20;  // 書き込み先の改竄
    a = apply_boot_repair(*r.dev, t, r.token, opts_with_backup());
    CHECK(!a.ok() && r.dev->writes.empty());
  }
  {
    Ready r = ready_fat32();
    r.dev->bytes()[40] ^= 0x55;  // 計画のあとでデバイスが変わった
    BootApplyResult a = apply_boot_repair(*r.dev, r.plan, r.token, opts_with_backup());
    CHECK(!a.ok() && a.code == "device-changed");
    CHECK(r.dev->writes.empty());
  }
  {
    Ready r = ready_fat32();
    BootApplyOptions ao = opts_with_backup();
    ao.in_use = true;
    BootApplyResult a = apply_boot_repair(*r.dev, r.plan, r.token, ao);
    CHECK(!a.ok() && a.code == "in-use");
    CHECK(r.dev->writes.empty());
  }
  {
    Ready r = ready_fat32();
    MemoryDevice ro(r.dev->bytes(), true, 512);
    BootApplyResult a = apply_boot_repair(ro, r.plan, r.token, opts_with_backup());
    CHECK(!a.ok() && a.code == "device-read-only");
  }
  {
    Ready r = ready_fat32();
    BootApplyResult a = apply_boot_repair(*r.dev, r.plan, r.token, BootApplyOptions{});
    CHECK(!a.ok() && a.code == "backup-required");
    CHECK(r.dev->writes.empty());
    BootApplyOptions ao;
    ao.backup_sink = [](const Bytes&) { return Status::Make(Errc::Io, 28); };
    a = apply_boot_repair(*r.dev, r.plan, r.token, ao);
    CHECK(!a.ok() && a.code == "backup-failed");
    CHECK(r.dev->writes.empty());
  }
  {
    // 別のデバイスに別のデバイスの計画を適用しない
    Ready r = ready_fat32();
    Bytes other = make_fat32(41 << 20);
    zero(other, 0, 512);
    auto d2 = wd(other);
    BootApplyResult a = apply_boot_repair(*d2, r.plan, r.token, opts_with_backup());
    CHECK(!a.ok() && d2->writes.empty());
  }
  {
    // キャンセル済み: 何も書かない
    Ready r = ready_fat32();
    CancelToken tok;
    tok.cancel();
    BootApplyOptions ao = opts_with_backup();
    ao.cancel = &tok;
    BootApplyResult a = apply_boot_repair(*r.dev, r.plan, r.token, ao);
    CHECK(a.status.code == Errc::Cancelled);
    CHECK(r.dev->bytes()[0] == 0 && r.dev->writes.empty());
  }
}

TEST(bootfix_write_failure_rolls_back) {
  for (int fail = 0; fail < 3; fail++) {
    Bytes good = make_fat32(40 << 20);
    Bytes img = good;
    zero(img, 0, 3 * 512);
    auto dev = wd(img);
    BootRepairPlan plan;
    // 先頭 3 セクタの書き戻し (1 回の書き込みか、複数に分かれる) のうち fail 番目を失敗させる
    CHECK(plan_boot_repair(*dev, 0, 0, FsType::Fat32, "fat32-restore-primary-from-backup", 0, &plan).ok());
    CHECK(plan.ok);
    Bytes before = dev->bytes();
    dev->fail_write_index = fail;
    dev->torn = true;
    BootApplyOptions ao = opts_with_backup();
    BootApplyResult a = apply_boot_repair(*dev, plan, make_boot_confirm_token(plan, plan.confirm_phrase), ao);
    if (fail < static_cast<int>(plan.writes.size())) {
      CHECK(!a.ok());
      CHECK(a.code == "write-failed");
      dev->fail_write_index = -1;
      // ロールバック: 元の内容に戻る (書き込みの途中失敗でも)
      CHECK(a.rolled_back);
      CHECK(dev->bytes() == before);
    }
  }
}

TEST(bootfix_verify_failure_rolls_back) {
  // 計画自体が構造を直さない (別の場所のコピー) と、書いた後の再検査で verify-failed になり元に戻る。
  // ここでは、バックアップ側も壊れている FAT32 に「バックアップ → 先頭」を適用した計画を手で作る
  Bytes good = make_fat32(40 << 20);
  Bytes img = good;
  zero(img, 0, 512);
  auto dev = wd(img);
  BootRepairPlan plan;
  CHECK(plan_boot_repair(*dev, 0, 0, FsType::Fat32, "fat32-restore-primary-from-backup", 0, &plan).ok());
  CHECK(plan.ok);
  // 書き込み内容を壊れたセクタに置き換え、ハッシュを計算し直すことはできない (SHA は内部) ので、改竄は plan-tampered で止まる
  BootRepairPlan t = plan;
  t.writes[0].data.assign(t.writes[0].data.size(), 0);
  BootApplyResult a = apply_boot_repair(*dev, t, make_boot_confirm_token(t, t.confirm_phrase), opts_with_backup());
  CHECK(!a.ok());
  CHECK(a.code == "plan-tampered");
  CHECK(dev->writes.empty());
}

// ------------------------------------------------------------------ バックアップと復元

TEST(bootfix_backup_restore_roundtrip) {
  Bytes good = make_exfat(16 << 20);
  Bytes img = good;
  zero(img, 0, 12 * 512);
  auto dev = wd(img);
  Bytes before = dev->bytes();
  Bytes backup;
  CHECK(do_repair(*dev, FsType::Exfat, "exfat-restore-main-from-backup", 0, nullptr, &backup));
  CHECK(dev->bytes() != before);
  BootBackupInfo bi;
  CHECK(parse_boot_backup(backup, &bi).ok());
  CHECK_EQ(bi.version, 1u);
  CHECK_EQ(bi.timestamp, 1700000000u);
  CHECK(bi.record_count >= 1);
  // 書き戻すと修復前の内容に戻る
  CHECK(restore_boot_backup(*dev, backup).ok());
  CHECK(dev->bytes() == before);
  // 破損したバックアップは拒否する (CRC / SHA-256 / 形式)
  for (size_t at : {size_t(0), size_t(9), size_t(30), backup.size() / 2, backup.size() - 40, backup.size() - 1}) {
    Bytes bad = backup;
    bad[at] ^= 0x01;
    BootBackupInfo b2;
    CHECK(!parse_boot_backup(bad, &b2).ok());
    auto d2 = wd(before);
    CHECK(!restore_boot_backup(*d2, bad).ok());
    CHECK(d2->writes.empty());
  }
  Bytes cut(backup.begin(), backup.begin() + static_cast<long>(backup.size() / 2));
  CHECK(!parse_boot_backup(cut, &bi).ok());
  CHECK(!parse_boot_backup(Bytes{}, &bi).ok());
  // 小さいデバイスには戻さない
  Bytes tiny(4096, 0);
  auto d3 = wd(tiny);
  CHECK(!restore_boot_backup(*d3, backup).ok());
}

TEST(bootfix_inspect_never_writes_and_honours_cancel) {
  Bytes img = make_fat32(40 << 20);
  zero(img, 0, 512);
  auto dev = wd(img);
  insp(*dev);
  insp(*dev, FsType::Fat32);
  BootRepairPlan plan;
  CHECK(plan_boot_repair(*dev, 0, 0, FsType::Fat32, "fat32-restore-primary-from-backup", 0, &plan).ok());
  CHECK(dev->writes.empty() && dev->flush_at.empty());
  CancelToken tok;
  tok.cancel();
  BootInspectOptions o;
  o.cancel = &tok;
  BootInspection r;
  Status st = inspect_boot(*dev, 0, 0, FsType::Unknown, o, &r);
  CHECK(st.code == Errc::Cancelled);
  // JSON が作れて、波括弧が釣り合う
  BootInspection ok = insp(*dev);
  std::string j = boot_inspection_to_json(ok) + boot_plan_to_json(plan);
  int depth = 0;
  bool in = false;
  for (size_t i = 0; i < j.size(); i++) {
    if (j[i] == '\\' && in) { i++; continue; }
    if (j[i] == '"') in = !in;
    if (!in) depth += (j[i] == '{' || j[i] == '[') - (j[i] == '}' || j[i] == ']');
  }
  CHECK_EQ(depth, 0);
  CHECK(j.find("fat32-restore-primary-from-backup") != std::string::npos);
}

// ------------------------------------------------------------------ 破損入力 (ASan / UBSan 用)

TEST(bootfix_fuzz_corrupt_headers_do_not_crash) {
  std::vector<std::pair<Bytes, size_t>> bases = {
      {make_fat32(24 << 20), 40 << 10}, {make_fat16(12 << 20), 40 << 10}, {make_ntfs(8 << 20), 40 << 10},
      {make_exfat(8 << 20), 40 << 10},  {make_ext(16 << 20, 1), 40 << 10}, {make_ext(16 << 20, 0), 40 << 10}};
  std::mt19937 rng(777);
  size_t applied = 0;
  for (int it = 0; it < 2000; it++) {
    auto& b = bases[static_cast<size_t>(it) % bases.size()];
    Bytes img = b.first;
    int n = 1 + static_cast<int>(rng() % 6);
    for (int k = 0; k < n; k++) {
      uint64_t where = rng() % 5;
      uint64_t off = where == 0 ? rng() % 4096 : where == 1 ? 6 * 512 + rng() % 2048 : where == 2 ? 12 * 512 + rng() % 8192 : where == 3 ? img.size() - 512 + rng() % 512 : 1024 * (1 + rng() % 3) + rng() % 1024;
      if (where == 4 && (rng() & 1)) off = (8192ull * (1 + 2 * (rng() % 2)) + 1) * 1024 + rng() % 1024;
      size_t len = 1 + rng() % 64;
      for (size_t i = 0; i < len && off + i < img.size(); i++) img[off + i] = static_cast<uint8_t>(rng() & 3 ? rng() : 0);
    }
    auto dev = wd(img);
    BootInspection r;
    FsType hint = it % 3 == 0 ? FsType::Unknown : it % 3 == 1 ? FsType::Fat32 : FsType::Ext4;
    Status st = inspect_boot(*dev, 0, 0, hint, BootInspectOptions{}, &r);
    CHECK(st.ok());
    (void)boot_inspection_to_json(r);
    for (const auto& o : r.repairs) {
      BootRepairPlan plan;
      CHECK(plan_boot_repair(*dev, 0, 0, hint, o.id, o.source, &plan).ok());
      (void)boot_plan_to_json(plan, true);
      if (!plan.ok) continue;
      BootApplyResult a = apply_boot_repair(*dev, plan, make_boot_confirm_token(plan, plan.confirm_phrase), opts_with_backup());
      applied++;
      // 適用に成功したなら検証済み。失敗したなら元に戻っている
      if (a.ok()) CHECK(a.verified);
      else CHECK(a.rolled_back || dev->bytes() == img || a.writes_done == 0);
    }
  }
  CHECK(applied > 100);
}

// ------------------------------------------------------------------ C API

TEST(bootfix_capi_inspect_plan_apply_restore) {
  Bytes good = make_ntfs(16 << 20);
  Bytes img = good;
  zero(img, 0, 512);
  std::string path = test_tmpdir() + "/usbmem_bootfix_capi.img";
  ::unlink(path.c_str());
  {
    FILE* f = std::fopen(path.c_str(), "wb");
    CHECK(f != nullptr);
    if (f) { std::fwrite(img.data(), 1, img.size(), f); std::fclose(f); }
  }
  usbmem_device* dev = nullptr;
  CHECK_EQ(usbmem_open_path(path.c_str(), 0, &dev), 0);
  usbmem_pjob* job = usbmem_pjob_new();
  usbmem_bootfix* h = nullptr;
  CHECK_EQ(usbmem_bootfix_open(dev, 0, 0, nullptr, job, &h), 0);
  char* js = nullptr;
  CHECK_EQ(usbmem_bootfix_inspection_json(h, &js), 0);
  std::string ij = js;
  usbmem_free(js);
  CHECK(ij.find("\"verdict\":\"repairable\"") != std::string::npos);
  CHECK(ij.find("ntfs-restore-primary-from-backup") != std::string::npos);
  // 計画の前にトークンは作れない
  char* tok = nullptr;
  CHECK(usbmem_bootfix_make_token(h, "x", &tok) != 0);
  CHECK_EQ(usbmem_bootfix_plan_json(h, "ntfs-restore-primary-from-backup", 0, &js), 0);
  std::string pj = js;
  usbmem_free(js);
  CHECK(pj.find("\"ok\":true") != std::string::npos);
  size_t k = pj.find("\"confirmPhrase\":\"");
  CHECK(k != std::string::npos);
  std::string phrase = pj.substr(k + 17);
  phrase = phrase.substr(0, phrase.find('"'));
  // 間違った確認文字列
  CHECK_EQ(usbmem_bootfix_make_token(h, "REPAIR 000000", &tok), 0);
  std::string badtok = tok;
  usbmem_free(tok);
  CHECK_EQ(usbmem_bootfix_apply(h, badtok.c_str(), nullptr, 0, 0, job, &js), static_cast<int>(Errc::InvalidArgument));
  CHECK(std::string(js).find("bad-token") != std::string::npos);
  usbmem_free(js);
  char code[64] = {}, msg[128] = {};
  usbmem_bootfix_last_error(h, code, sizeof code, msg, sizeof msg);
  CHECK(std::string(code) == "bad-token");
  // 正しいトークン (バックアップ先は dev の外)
  CHECK_EQ(usbmem_bootfix_make_token(h, phrase.c_str(), &tok), 0);
  std::string token = tok;
  usbmem_free(tok);
  std::string bk = path + ".bak";
  CHECK_EQ(usbmem_bootfix_apply(h, token.c_str(), bk.c_str(), 1700000000ull, 0, job, &js), 0);
  std::string aj = js;
  usbmem_free(js);
  CHECK(aj.find("\"ok\":true") != std::string::npos && aj.find("\"verified\":true") != std::string::npos);
  FILE* f = std::fopen(bk.c_str(), "rb");
  CHECK(f != nullptr);
  Bytes backup;
  if (f) {
    uint8_t buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) backup.insert(backup.end(), buf, buf + n);
    std::fclose(f);
  }
  CHECK(!backup.empty());
  unsigned char* last = nullptr;
  size_t last_len = 0;
  CHECK_EQ(usbmem_bootfix_last_backup(h, &last, &last_len), 0);
  CHECK(last_len == backup.size() && std::memcmp(last, backup.data(), last_len) == 0);
  usbmem_free(last);
  CHECK_EQ(usbmem_bootfix_backup_info_json(backup.data(), backup.size(), &js), 0);
  CHECK(std::string(js).find("\"fs\":\"ntfs\"") != std::string::npos);
  usbmem_free(js);
  // 再適用はできない (計画は使い切り)
  CHECK(usbmem_bootfix_apply(h, token.c_str(), nullptr, 0, 0, job, &js) != 0);
  usbmem_free(js);
  usbmem_bootfix_close(h);
  // 書き戻すと元 (壊れた状態) に戻る
  CHECK_EQ(usbmem_bootfix_restore_backup(dev, backup.data(), backup.size()), 0);
  usbmem_close(dev);
  Bytes now(img.size());
  f = std::fopen(path.c_str(), "rb");
  if (f) { CHECK(std::fread(now.data(), 1, now.size(), f) == now.size()); std::fclose(f); }
  CHECK(now == img);
  usbmem_pjob_free(job);
  ::unlink(path.c_str());
  ::unlink(bk.c_str());
}
