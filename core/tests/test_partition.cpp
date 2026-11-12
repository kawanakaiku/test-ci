// SPDX-License-Identifier: Apache-2.0
#include <random>

#include "test_util.h"

using namespace usbmem;
using namespace tb;

static const uint64_t kSecs = 8192;

static Bytes simple_gpt(uint32_t ss = 512) {
  return make_gpt_disk(ss, kSecs, {{1, 40, 999, u"EFI"}, {1, 1000, 4999, u"Data 日本"}, {1, 5000, 8000, u""}});
}

TEST(crc32_known_vector) {
  CHECK_EQ(crc32("123456789", 9), 0xCBF43926u);
  CHECK_EQ(crc32("", 0), 0u);
}

TEST(gpt_valid) {
  PartitionTable t;
  CHECK(parse(simple_gpt(), &t).ok());
  CHECK(t.kind == TableKind::Gpt);
  CHECK_EQ(t.partitions.size(), 3u);
  CHECK(!t.has_errors());
  CHECK(!t.used_backup_gpt);
  CHECK_EQ(t.partitions[1].start_lba, 1000u);
  CHECK_EQ(t.partitions[1].sector_count, 4000u);
  CHECK_EQ(t.partitions[1].size_bytes, 4000u * 512);
  CHECK(t.partitions[1].name == "Data \xE6\x97\xA5\xE6\x9C\xAC");  // UTF-16 → UTF-8
  CHECK_EQ(t.partitions[0].index, 1u);
  CHECK_EQ(t.partitions[2].index, 3u);
  CHECK(t.disk_guid == "13121110-1514-1716-1819-1A1B1C1D1E1F");  // 混合エンディアン
}

TEST(gpt_4k_sectors) {
  PartitionTable t;
  CHECK(parse(simple_gpt(4096), &t, 4096).ok());
  CHECK(t.kind == TableKind::Gpt);
  CHECK_EQ(t.sector_size, 4096u);
  CHECK_EQ(t.partitions.size(), 3u);
  CHECK_EQ(t.partitions[1].start_bytes, 1000u * 4096);
}

TEST(gpt_4k_autodetected_on_512_device) {
  PartitionTable t;
  CHECK(parse(simple_gpt(4096), &t, 512).ok());  // デバイスは 512 と申告
  CHECK_EQ(t.sector_size, 4096u);
  CHECK_EQ(t.partitions.size(), 3u);
}

TEST(gpt_primary_header_corrupt_uses_backup) {
  Bytes img = simple_gpt();
  img[512 + 20] ^= 0xFF;  // ヘッダ CRC 破壊
  PartitionTable t;
  CHECK(parse(img, &t).ok());
  CHECK(t.used_backup_gpt);
  CHECK_EQ(t.partitions.size(), 3u);
  CHECK(has_diag(t, "gpt-primary-header-invalid"));
  CHECK(has_diag(t, "gpt-using-backup"));
}

TEST(gpt_primary_entries_corrupt_uses_backup) {
  Bytes img = simple_gpt();
  img[2 * 512 + 40] ^= 0x01;
  PartitionTable t;
  CHECK(parse(img, &t).ok());
  CHECK(t.used_backup_gpt);
  CHECK(has_diag(t, "gpt-primary-entries-crc"));
  CHECK_EQ(t.partitions.size(), 3u);
}

TEST(gpt_backup_corrupt_primary_still_used) {
  Bytes img = simple_gpt();
  img[(kSecs - 1) * 512 + 20] ^= 0xFF;
  PartitionTable t;
  CHECK(parse(img, &t).ok());
  CHECK(!t.used_backup_gpt);
  CHECK(has_diag(t, "gpt-backup-header-invalid"));
  CHECK_EQ(t.partitions.size(), 3u);
}

TEST(gpt_both_corrupt) {
  Bytes img = simple_gpt();
  img[512 + 20] ^= 0xFF;
  img[(kSecs - 1) * 512 + 20] ^= 0xFF;
  PartitionTable t;
  CHECK(parse(img, &t).ok());
  CHECK(t.has_errors());
  CHECK_EQ(t.partitions.size(), 0u);
  CHECK(has_diag(t, "gpt-unusable"));
}

TEST(gpt_truncated_image_backup_missing) {
  Bytes img = simple_gpt();
  img.resize((kSecs - 100) * 512);  // 末尾が切れた(拡張前のイメージを縮めた等)
  PartitionTable t;
  CHECK(parse(img, &t).ok());
  CHECK(t.kind == TableKind::Gpt);
  // プライマリの usable 範囲がディスク外になるため不正扱い → 安全に拒否される
  CHECK(t.partitions.empty() || t.has_errors() || has_diag(t, "gpt-backup-header-missing"));
}

TEST(gpt_entry_out_of_disk_skipped) {
  Bytes img = make_gpt_disk(512, kSecs, {{1, 40, 999, u"ok"}, {1, 100, 99999, u"bad"}});
  PartitionTable t;
  CHECK(parse(img, &t).ok());
  CHECK_EQ(t.partitions.size(), 1u);
  CHECK(has_diag(t, "gpt-entry-invalid-range"));
}

TEST(gpt_overlap_detected) {
  Bytes img = make_gpt_disk(512, kSecs, {{1, 40, 999, u"a"}, {1, 500, 2000, u"b"}});
  PartitionTable t;
  CHECK(parse(img, &t).ok());
  CHECK(has_diag(t, "partitions-overlap"));
}

TEST(gpt_hostile_header_fields) {
  // 巨大な num_entries / entry_size / entries_lba を CRC 付きで仕込んでも安全に拒否する
  for (int variant = 0; variant < 4; variant++) {
    Bytes img = simple_gpt();
    uint8_t* h = img.data() + 512;
    if (variant == 0) le32(img, 512 + 80, 0xFFFFFFFFu);
    if (variant == 1) le32(img, 512 + 84, 1u << 30);
    if (variant == 2) le64(img, 512 + 72, UINT64_MAX - 1);
    if (variant == 3) le32(img, 512 + 12, 0xFFFFFFFFu);
    if (variant != 3) le32(img, 512 + 16, 0), le32(img, 512 + 16, crc32(h, 92));
    PartitionTable t;
    CHECK(parse(img, &t).ok());
    CHECK(t.used_backup_gpt);  // プライマリは拒否され、バックアップが使われる
    CHECK_EQ(t.partitions.size(), 3u);
  }
}

TEST(mbr_primary_only) {
  Bytes img(kSecs * 512, 0);
  put_mbr_entry(img, 0, {0x80, 0x07, 2048, 4000});
  put_mbr_entry(img, 1, {0x00, 0x0C, 6048, 2000});
  put_mbr_entry(img, 2, {0x00, 0x83, 8048, 100});
  le32(img, 440, 0xDEADBEEF);
  sign55aa(img);
  PartitionTable t;
  CHECK(parse(img, &t).ok());
  CHECK(t.kind == TableKind::Mbr);
  CHECK_EQ(t.partitions.size(), 3u);
  CHECK(t.partitions[0].bootable);
  CHECK(!t.partitions[1].bootable);
  CHECK_EQ(t.mbr_disk_signature, 0xDEADBEEFu);
  CHECK(t.partitions[2].type_name == "Linux");
  CHECK(!t.has_errors());
}

static Bytes mbr_with_ebr_chain() {
  Bytes img(40000 * 512, 0);
  put_mbr_entry(img, 0, {0x80, 0x07, 2048, 4000});
  put_mbr_entry(img, 1, {0, 0x0F, 10000, 20000});  // 拡張 (LBA 10000..29999)
  sign55aa(img);
  // EBR1 @10000: 論理 [10000+63, 1000]、次 EBR は ext 先頭から +5000
  auto ebr = [&](uint64_t at, uint32_t rel_start, uint32_t cnt, uint32_t next_rel, uint32_t next_cnt) {
    Bytes s(512, 0);
    put_mbr_entry(s, 0, {0, 0x83, rel_start, cnt});
    if (next_cnt) put_mbr_entry(s, 1, {0, 0x05, next_rel, next_cnt});
    sign55aa(s);
    std::memcpy(img.data() + at * 512, s.data(), 512);
  };
  ebr(10000, 63, 1000, 5000, 3000);
  ebr(15000, 63, 2000, 9000, 4000);
  ebr(19000, 63, 3000, 0, 0);
  return img;
}

TEST(mbr_extended_chain) {
  PartitionTable t;
  CHECK(parse(mbr_with_ebr_chain(), &t).ok());
  size_t logical = 0;
  for (auto& p : t.partitions) if (p.logical) logical++;
  CHECK_EQ(logical, 3u);
  CHECK_EQ(t.partitions.size(), 5u);  // 主 1 + 拡張枠 + 論理 3
  const Partition* l1 = nullptr; const Partition* l3 = nullptr;
  for (auto& p : t.partitions) { if (p.index == 5) l1 = &p; if (p.index == 7) l3 = &p; }
  CHECK(l1 && l3);
  if (l1) { CHECK_EQ(l1->start_lba, 10063u); CHECK_EQ(l1->sector_count, 1000u); }
  if (l3) { CHECK_EQ(l3->start_lba, 19063u); CHECK_EQ(l3->sector_count, 3000u); }
  CHECK(!t.has_errors());
}

TEST(mbr_ebr_loop_terminates) {
  Bytes img = mbr_with_ebr_chain();
  // 3 つ目の EBR が最初の EBR (ext 先頭 +0) を指す → ループ
  tb::put_mbr_entry(img, 0, {0x80, 0x07, 2048, 4000});
  Bytes s(512, 0);
  put_mbr_entry(s, 0, {0, 0x83, 63, 3000});
  put_mbr_entry(s, 1, {0, 0x05, 0, 3000});
  sign55aa(s);
  std::memcpy(img.data() + 19000 * 512, s.data(), 512);
  PartitionTable t;
  CHECK(parse(img, &t).ok());
  CHECK(has_diag(t, "ebr-loop"));
  CHECK(t.has_errors());
}

TEST(mbr_ebr_self_loop_and_outside) {
  Bytes img(40000 * 512, 0);
  put_mbr_entry(img, 0, {0, 0x05, 100, 30000});
  sign55aa(img);
  Bytes s(512, 0);
  put_mbr_entry(s, 1, {0, 0x05, 0, 100});  // 自分自身を指す
  sign55aa(s);
  std::memcpy(img.data() + 100 * 512, s.data(), 512);
  PartitionTable t;
  CHECK(parse(img, &t).ok());
  CHECK(has_diag(t, "ebr-loop") || t.partitions.size() == 1);
  // 拡張が device 外
  Bytes img2(1000 * 512, 0);
  put_mbr_entry(img2, 0, {0, 0x05, 5000000, 100});
  sign55aa(img2);
  PartitionTable t2;
  CHECK(parse(img2, &t2).ok());
  CHECK(has_diag(t2, "ebr-outside-disk"));
}

TEST(mbr_partition_beyond_disk_and_overflow) {
  Bytes img(1000 * 512, 0);
  put_mbr_entry(img, 0, {0, 0x83, 0xFFFFFF00u, 0xFFFFFFFFu});
  put_mbr_entry(img, 1, {0, 0x83, 10, 5000});
  sign55aa(img);
  PartitionTable t;
  CHECK(parse(img, &t).ok());
  CHECK(has_diag(t, "mbr-partition-exceeds-disk"));
  CHECK_EQ(t.partitions[0].size_bytes, 0xFFFFFFFFull * 512);
}

TEST(protective_mbr_without_gpt) {
  Bytes img(1000 * 512, 0);
  put_mbr_entry(img, 0, {0, 0xEE, 1, 999});
  sign55aa(img);
  PartitionTable t;
  CHECK(parse(img, &t).ok());
  CHECK(has_diag(t, "gpt-protective-without-gpt"));
}

TEST(hybrid_mbr_flagged) {
  Bytes img = simple_gpt();
  put_mbr_entry(img, 1, {0, 0x07, 1000, 100});
  PartitionTable t;
  CHECK(parse(img, &t).ok());
  CHECK(t.kind == TableKind::Gpt);
  CHECK(has_diag(t, "hybrid-mbr"));
}

static Bytes fat32_vbr(size_t total_sectors) {
  Bytes s(512, 0);
  s[0] = 0xEB; s[1] = 0x58; s[2] = 0x90;
  std::memcpy(&s[3], "MSDOS5.0", 8);
  le16(s, 11, 512); s[13] = 1; le16(s, 14, 32); s[16] = 2;
  le32(s, 32, (uint32_t)total_sectors); le32(s, 36, 1000);
  std::memcpy(&s[82], "FAT32   ", 8);
  sign55aa(s);
  return s;
}

TEST(superfloppy_fat32) {
  Bytes img(300000 * 512, 0);
  Bytes v = fat32_vbr(300000);
  std::memcpy(img.data(), v.data(), 512);
  PartitionTable t;
  CHECK(parse(img, &t).ok());
  CHECK(t.whole_device_volume);
  CHECK(t.whole_device_fs == FsType::Fat32);
  CHECK_EQ(t.partitions.size(), 1u);
  CHECK_EQ(t.partitions[0].sector_count, 300000u);
}

TEST(superfloppy_ntfs_and_exfat_and_bitlocker) {
  struct { const char* oem; FsType fs; } cases[] = {
      {"NTFS    ", FsType::Ntfs}, {"EXFAT   ", FsType::Exfat}, {"-FVE-FS-", FsType::BitLocker}};
  for (auto& c : cases) {
    Bytes img(4096 * 512, 0);
    std::memcpy(&img[3], c.oem, 8);
    sign55aa(img);
    PartitionTable t;
    CHECK(parse(img, &t).ok());
    CHECK(t.whole_device_fs == c.fs);
  }
}

TEST(fat_boot_sector_not_mistaken_for_mbr_and_vice_versa) {
  // 通常の MBR (ブートコード + パーティション) は FAT と誤認されない
  Bytes img(8192 * 512, 0);
  img[0] = 0xEB; img[1] = 0x3C; img[2] = 0x90;  // 見かけは FAT 風のジャンプ
  put_mbr_entry(img, 0, {0x80, 0x0C, 2048, 4000});
  sign55aa(img);
  PartitionTable t;
  CHECK(parse(img, &t).ok());
  CHECK(!t.whole_device_volume);
  CHECK(t.kind == TableKind::Mbr);
  CHECK_EQ(t.partitions.size(), 1u);
}

TEST(partition_fs_probe_inside_mbr) {
  Bytes img(8192 * 512, 0);
  put_mbr_entry(img, 0, {0, 0x83, 2048, 4000});
  sign55aa(img);
  // パーティション内に ext4 superblock
  size_t sb = 2048 * 512 + 1024;
  le16(img, sb + 56, 0xEF53);
  le32(img, sb + 0x60, 0x40);  // extents
  PartitionTable t;
  CHECK(parse(img, &t).ok());
  CHECK(t.partitions[0].fs == FsType::Ext4);
}

TEST(blank_and_tiny_and_garbage) {
  PartitionTable t;
  CHECK(parse(Bytes(100 * 512, 0), &t).ok());
  CHECK(t.kind == TableKind::None && has_diag(t, "blank-device"));
  CHECK(parse(Bytes(100, 1), &t).ok());
  CHECK(has_diag(t, "device-too-small"));
  CHECK(parse(Bytes(), &t).ok());
  Bytes junk(100 * 512, 0x5A);
  CHECK(parse(junk, &t).ok());
  CHECK(t.partitions.empty());
}

TEST(iso9660_whole_device) {
  Bytes img(40 * 2048, 0);
  img[0x8000] = 1;
  std::memcpy(&img[0x8001], "CD001", 5);
  PartitionTable t;
  CHECK(parse(img, &t, 2048).ok());
  CHECK(t.whole_device_fs == FsType::Iso9660);
}

TEST(udf_whole_device_and_bridge) {
  // UDF: BEA01 / NSR02 / TEA01 (UDF 単独)
  Bytes img(40 * 2048, 0);
  std::memcpy(&img[0x8001], "BEA01", 5);
  std::memcpy(&img[0x8000 + 2048 + 1], "NSR02", 5);
  std::memcpy(&img[0x8000 + 2 * 2048 + 1], "TEA01", 5);
  PartitionTable t;
  CHECK(parse(img, &t, 2048).ok());
  CHECK(t.whole_device_volume && t.whole_device_fs == FsType::Udf);
  // ISO 9660 + UDF のブリッジ (CD001 の後ろに BEA01 / NSR02 / TEA01): UDF を優先して報告する (マウント側は UDF を試し、だめなら ISO 9660)
  Bytes br(40 * 2048, 0);
  br[0x8000] = 1; std::memcpy(&br[0x8001], "CD001", 5);
  br[0x8000 + 2048] = 255; std::memcpy(&br[0x8000 + 2048 + 1], "CD001", 5);
  std::memcpy(&br[0x8000 + 2 * 2048 + 1], "BEA01", 5);
  std::memcpy(&br[0x8000 + 3 * 2048 + 1], "NSR02", 5);
  std::memcpy(&br[0x8000 + 4 * 2048 + 1], "TEA01", 5);
  CHECK(parse(br, &t, 2048).ok());
  CHECK(t.whole_device_volume && t.whole_device_fs == FsType::Udf);
  // isohybrid 風: 先頭に MBR (0x55AA) があり、パーティション 1 が LBA 0 から始まる → パーティション内の ISO 9660 を判別する
  Bytes hy(128 * 512, 0);
  hy[0x8000] = 1; std::memcpy(&hy[0x8001], "CD001", 5);
  hy[510] = 0x55; hy[511] = 0xAA;
  uint8_t* e = &hy[446];
  e[0] = 0x80; e[4] = 0x17; e[8] = 0; e[9] = 0; e[10] = 0; e[11] = 0;   // start LBA 0
  e[12] = 100; e[13] = 0; e[14] = 0; e[15] = 0;                          // 100 セクタ
  CHECK(parse(hy, &t).ok());
  bool found = false;
  for (auto& p : t.partitions) if (p.fs == FsType::Iso9660) found = true;
  CHECK(found);
}

TEST(io_errors_are_propagated) {
  for (int n = 0; n < 6; n++) {
    auto mem = std::make_shared<MemoryDevice>(simple_gpt(), true);
    FaultDevice f(mem);
    f.fail_after_reads = n;
    PartitionTable t;
    Status st = read_partition_table(f, &t);
    CHECK(!st.ok());
    CHECK(st.code == Errc::Io);
    CHECK_EQ(st.sys_errno, 5);
  }
  auto mem = std::make_shared<MemoryDevice>(simple_gpt(), true);
  FaultDevice f(mem);
  f.fail_after_reads = 1000;  // 十分大きい → 成功
  PartitionTable t;
  CHECK(read_partition_table(f, &t).ok());
}

TEST(disconnect_mid_scan) {
  auto mem = std::make_shared<MemoryDevice>(mbr_with_ebr_chain(), true);
  FaultDevice f(mem);
  f.fail_after_reads = 3;
  f.fail_code = Errc::Disconnected;
  PartitionTable t;
  CHECK(read_partition_table(f, &t).code == Errc::Disconnected);
}

TEST(json_output_escaping_and_shape) {
  Bytes img = make_gpt_disk(512, kSecs, {{1, 40, 999, u"a\"b\\c\n"}});
  PartitionTable t;
  CHECK(parse(img, &t).ok());
  std::string j = partition_table_to_json(t);
  CHECK(j.find("a\\\"b\\\\c\\n") != std::string::npos);
  CHECK(j.find("\"sizeBytes\":\"491520\"") != std::string::npos);
}

// ランダム破壊: 有効なテーブルの任意バイトを壊しても落ちず、無限ループしない。
// ASan/UBSan ビルド(USBMEM_SANITIZE=ON)で実行すること。
TEST(fuzz_mutations_do_not_crash) {
  std::mt19937 rng(12345);
  std::vector<Bytes> seeds = {simple_gpt(), make_gpt_disk(4096, 1024, {{1, 6, 400, u"a"}, {1, 401, 900, u"b"}}),
                              mbr_with_ebr_chain()};
  for (int iter = 0; iter < 1500; iter++) {
    Bytes img = seeds[iter % seeds.size()];
    int n = 1 + rng() % 8;
    for (int k = 0; k < n; k++) {
      // 変異は先頭/末尾セクタ付近に集中させて構造部分を狙う
      size_t region = rng() % 4;
      size_t pos;
      switch (region) {
        case 0: pos = rng() % 1024; break;
        case 1: pos = 512 + rng() % 512; break;
        case 2: pos = img.size() - 1 - rng() % 1024; break;
        default: pos = rng() % img.size();
      }
      img[pos] = (uint8_t)rng();
    }
    PartitionTable t;
    CHECK(parse(img, &t, (iter % seeds.size() == 1) ? 4096 : 512).ok());
    (void)partition_table_to_json(t);
  }
  // 完全ランダム
  for (int iter = 0; iter < 300; iter++) {
    Bytes img(64 * 512);
    for (auto& b : img) b = (uint8_t)rng();
    if (iter % 2) { img[510] = 0x55; img[511] = 0xAA; }
    PartitionTable t;
    CHECK(parse(img, &t).ok());
  }
}

// ---- レビュー指摘(codex)に対する回帰テスト
TEST(device_reports_bad_block_size_does_not_crash) {
  for (uint32_t bs : {0u, 1u, 100u, 3u * 512u}) {
    usbmem::MemoryDevice dev(simple_gpt(), true, bs);
    PartitionTable t;
    CHECK(read_partition_table(dev, &t).ok());
    CHECK(t.kind == TableKind::Gpt && t.partitions.size() == 3u);  // 512/4096 候補で読める
  }
}

TEST(gpt_invalid_signature_at_512_does_not_hide_valid_4k_gpt) {
  Bytes img = simple_gpt(4096);
  std::memcpy(img.data() + 512, "EFI PART", 8);  // 512 候補側に CRC 不正の署名だけを置く
  PartitionTable t;
  CHECK(parse(img, &t, 512).ok());
  CHECK_EQ(t.sector_size, 4096u);
  CHECK_EQ(t.partitions.size(), 3u);
  CHECK(!t.has_errors());
}

TEST(fat_bpb_claiming_more_than_device_is_not_superfloppy) {
  Bytes img(2048 * 512, 0);
  Bytes v = fat32_vbr(10000000);                          // 実容量(2048 セクタ)より遥かに大きい申告
  std::memcpy(img.data(), v.data(), 512);
  put_mbr_entry(img, 0, {0x80, 0x0C, 64, 1000});          // 実際は MBR
  PartitionTable t;
  CHECK(parse(img, &t).ok());
  CHECK(!t.whole_device_volume);
  CHECK(t.kind == TableKind::Mbr);
}
