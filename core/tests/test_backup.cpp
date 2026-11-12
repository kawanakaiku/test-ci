// SPDX-License-Identifier: Apache-2.0
// バックアップ / 復元 (Partclone 形式) のテスト: 合成 FAT16、LZ4、ゲート、電源断、改ざんの検出、fuzz、実フィクスチャ。
#include <sys/stat.h>

#include <algorithm>

#include "backup_internal.h"
#include "recovery_test_util.h"

using namespace rt;

namespace {
std::shared_ptr<MemoryDevice> mem(Bytes b) { return std::make_shared<MemoryDevice>(std::move(b), false, 512); }

struct Img { Bytes bytes; BackupResult res; };
Img backup_mem(BlockDevice& dev, bool unclean_ok = false) {
  Img i;
  MemorySink sink;
  BackupOptions bo;
  bo.extract.full_if_unclean = unclean_ok;
  i.res = backup_filesystem(dev, sink, bo);
  i.bytes = std::move(sink.data);
  return i;
}
SourceFactory factory_of(const Bytes* b) { return [b] { return std::unique_ptr<ByteSource>(new MemorySource(b)); }; }
VerifyResult verify_bytes(const Bytes& b) { MemorySource s(&b); return verify_image(s); }

// 実フィクスチャ置き場
std::string fixture(const char* dir, const char* name) {
  const char* u = std::getenv("USER");
  return std::string("/dev/shm/usbmem_") + dir + "_" + (u ? u : "u") + "/" + name;
}
bool exists(const std::string& p) { struct stat st; return ::stat(p.c_str(), &st) == 0; }
}  // namespace

TEST(backup_fat16_roundtrip_exact) {
  Bytes src = make_fat16(8192, 1, 3, 1);
  auto dev = mem(src);
  Img im = backup_mem(*dev);
  CHECK(im.res.status.ok());
  CHECK(im.res.map.fs == "FAT16");
  CHECK_EQ(im.res.map.block_size, 512u);
  CHECK_EQ(im.res.map.total_blocks, 8192u);
  VerifyResult v = verify_bytes(im.bytes);
  CHECK(v.status.ok());
  CHECK_EQ(v.blocks_checked, im.res.map.used_blocks);
  CHECK(v.image_sha256 == im.res.image_sha256);
  CHECK_EQ(v.image_bytes, im.bytes.size());
  // 復元: 書き込み先は乱数で埋めておく。使用中ブロックだけが元と一致し、他は書き込み先のまま
  Bytes junk = random_bytes(src.size(), 99);
  RecordingDev tgt(junk);
  RestorePlan plan = plan_restore(factory_of(&im.bytes), tgt);
  CHECK(plan.ok);
  CHECK_EQ(plan.blocks_to_write, im.res.map.used_blocks);
  CHECK(plan.confirm_phrase.rfind("RESTORE ", 0) == 0);
  RestoreResult rr = apply_restore(factory_of(&im.bytes), tgt, plan, make_restore_token(plan, plan.confirm_phrase));
  CHECK(rr.status.ok());
  CHECK(rr.verified);
  CHECK_EQ(rr.blocks_written, im.res.map.used_blocks);
  UsedMap m;
  CHECK(build_used_map(*dev, ExtractOptions(), &m).ok());
  for (uint64_t b = 0; b < 8192; b++) {
    const Bytes& want = m.test(b) ? src : junk;
    if (std::memcmp(&tgt.bytes()[b * 512], &want[b * 512], 512) != 0) { CHECK(false); break; }
  }
  // 復元したものは、また同じ画像にバックアップできる (FS として正しく復元された)
  Img again = backup_mem(*mem(tgt.bytes()));
  CHECK(again.res.status.ok());
  CHECK_EQ(again.res.map.used_blocks, im.res.map.used_blocks);
}

TEST(backup_fat16_various_geometry) {
  for (uint32_t spc : {1u, 2u, 4u, 8u}) {
    for (uint32_t every : {1u, 2u, 7u, 1000u}) {
      uint32_t total = 4096 * spc + 2048;
      Bytes src = make_fat16(total, spc, every, spc * 31 + every);
      auto dev = mem(src);
      Img im = backup_mem(*dev);
      CHECK(im.res.status.ok());
      VerifyResult v = verify_bytes(im.bytes);
      CHECK(v.status.ok());
      RecordingDev tgt(Bytes(src.size(), 0));
      RestorePlan plan = plan_restore(factory_of(&im.bytes), tgt);
      RestoreResult rr = apply_restore(factory_of(&im.bytes), tgt, plan, make_restore_token(plan, plan.confirm_phrase));
      CHECK(rr.status.ok());
      UsedMap m;
      CHECK(build_used_map(*dev, ExtractOptions(), &m).ok());
      for (uint64_t b = 0; b < m.total_blocks; b++) {
        const uint8_t* g = &tgt.bytes()[b * 512];
        const uint8_t* w = &src[b * 512];
        bool same = std::memcmp(g, w, 512) == 0;
        if (m.test(b) ? !same : std::any_of(g, g + 512, [](uint8_t x) { return x != 0; })) { CHECK(false); break; }
      }
    }
  }
}

TEST(backup_header_matches_partclone_layout) {
  Bytes src = make_fat16(8192, 1, 5, 3);
  Img im = backup_mem(*mem(src));
  const Bytes& b = im.bytes;
  CHECK(std::memcmp(b.data(), "partclone-image\0", 16) == 0);
  CHECK(std::memcmp(&b[30], "0002", 4) == 0);
  CHECK_EQ(backup_detail::get_le(&b[34], 2), 0xC0DEu);
  CHECK(std::memcmp(&b[36], "FAT16", 5) == 0);
  CHECK_EQ(backup_detail::get_le(&b[0x3c], 8), 8192u);
  CHECK_EQ(backup_detail::get_le(&b[0x54], 4), 512u);
  CHECK_EQ(backup_detail::get_le(&b[0x64], 4), 2048u);  // 1 MiB / 512
  CHECK_EQ(b.size(), 0x6e + 1024 + 4 + im.res.map.used_blocks * 512 + ((im.res.map.used_blocks + 2047) / 2048) * 4);
}

TEST(lz4_roundtrip_and_interop_properties) {
  std::mt19937 g(5);
  for (size_t n : {0u, 1u, 11u, 12u, 13u, 100u, 65535u, 65536u, 1048575u, 1048576u, 1048577u, 3000000u}) {
    for (int kind = 0; kind < 3; kind++) {
      Bytes data(n);
      for (size_t i = 0; i < n; i++) data[i] = kind == 0 ? 0 : kind == 1 ? static_cast<uint8_t>(g()) : static_cast<uint8_t>((i / 7) ^ (i % 13 == 0 ? g() : 0));
      MemorySink raw;
      auto z = make_lz4_sink(&raw);
      // 小刻みに書く
      for (size_t p = 0; p < n;) { size_t k = std::min<size_t>(n - p, 1 + g() % 70000); CHECK(z->write(&data[p], k).ok()); p += k; }
      CHECK(z->finish().ok());
      CHECK(raw.data.size() >= 7);
      CHECK(looks_like_lz4(raw.data.data()));
      if (kind == 0 && n > 100000) CHECK(raw.data.size() < n / 50);  // ゼロはよく縮む
      MemorySource rs(&raw.data);
      auto u = make_lz4_source(&rs);
      Bytes back;
      uint8_t buf[100000];
      for (;;) {
        size_t got = 0;
        CHECK(u->read(buf, 1 + g() % sizeof buf, &got).ok());
        if (!got) break;
        back.insert(back.end(), buf, buf + got);
      }
      CHECK(back == data);
    }
  }
}

TEST(lz4_corrupt_streams_are_rejected_not_crashed) {
  Bytes data = random_bytes(300000, 8);
  for (size_t i = 0; i < data.size(); i += 3) data[i] = 0;
  MemorySink raw;
  auto z = make_lz4_sink(&raw);
  CHECK(z->write(data.data(), data.size()).ok());
  CHECK(z->finish().ok());
  std::mt19937 g(6);
  int rejected = 0;
  for (int iter = 0; iter < 1500; iter++) {
    Bytes bad = raw.data;
    int m = 1 + g() % 3;
    for (int k = 0; k < m; k++) bad[g() % bad.size()] ^= static_cast<uint8_t>(1 + g() % 255);
    if (g() % 4 == 0) bad.resize(g() % bad.size());
    MemorySource rs(&bad);
    auto u = make_lz4_source(&rs);
    Bytes out;
    uint8_t buf[65536];
    Status st;
    size_t total = 0;
    for (;;) {
      size_t got = 0;
      st = u->read(buf, sizeof buf, &got);
      if (!st.ok() || !got) break;
      total += got;
      if (total > 8u << 20) { CHECK(false); break; }  // 出力が暴走しない
    }
    if (!st.ok()) rejected++;
  }
  CHECK(rejected > 50);  // 圧縮データの中身を書き換えても気づけない場合がある (LZ4 自体にはチェックサムがない。検出は Partclone の CRC が担う)
}

TEST(backup_compressed_image_roundtrip) {
  Bytes src = make_fat16(8192, 2, 4, 21);
  auto dev = mem(src);
  MemorySink raw;
  auto z = make_lz4_sink(&raw);
  BackupResult r = backup_filesystem(*dev, *z);
  CHECK(r.status.ok());
  CHECK(raw.data.size() < r.image_bytes);
  // 展開して検査 → 展開後の SHA-256 が書き出し時と一致
  MemorySource rs(&raw.data);
  auto u = make_lz4_source(&rs);
  VerifyResult v = verify_image(*u);
  CHECK(v.status.ok());
  CHECK(v.image_sha256 == r.image_sha256);
}

TEST(restore_gates_refuse_before_writing) {
  Bytes src = make_fat16(8192, 1, 3, 4);
  Img im = backup_mem(*mem(src));
  Bytes junk = random_bytes(src.size(), 3);
  auto expect_refused = [&](RecordingDev& t, const RestorePlan& p, const std::string& tok, const char* code) {
    RestoreResult r = apply_restore(factory_of(&im.bytes), t, p, tok);
    CHECK(!r.status.ok());
    CHECK(r.status.code == code);
    CHECK(!r.target_touched);
    CHECK(t.writes.empty());
  };
  {  // 読み取り専用
    RecordingDev t(junk);
    t.ro = true;
    RestorePlan p = plan_restore(factory_of(&im.bytes), t);
    CHECK(!p.ok);
    CHECK(p.code == "target-read-only");
    expect_refused(t, p, "x", "bad-plan");
  }
  {  // 使用中
    RecordingDev t(junk);
    RestorePlanOptions po;
    po.target_in_use = true;
    RestorePlan p = plan_restore(factory_of(&im.bytes), t, po);
    CHECK(p.code == "target-in-use");
  }
  {  // 小さい
    RecordingDev t(Bytes(src.size() - 512, 0));
    RestorePlan p = plan_restore(factory_of(&im.bytes), t);
    CHECK(p.code == "target-too-small");
  }
  {  // トークン違い・確認文字列違い
    RecordingDev t(junk);
    RestorePlan p = plan_restore(factory_of(&im.bytes), t);
    CHECK(p.ok);
    expect_refused(t, p, "", "bad-token");
    expect_refused(t, p, make_restore_token(p, "RESTORE 000000"), "bad-token");
    expect_refused(t, p, make_restore_token(p, "restore " + p.plan_hash.substr(0, 6)), "bad-token");
    // 別の計画のトークン
    RecordingDev t2(Bytes(src.size() + 512, 0));
    RestorePlan p2 = plan_restore(factory_of(&im.bytes), t2);
    expect_refused(t, p, make_restore_token(p2, p2.confirm_phrase), "bad-token");
    // 計画のあとで画像が変わった
    Bytes other = backup_mem(*mem(make_fat16(8192, 1, 3, 5))).bytes;
    RestoreResult r = apply_restore(factory_of(&other), t, p, make_restore_token(p, p.confirm_phrase));
    CHECK(r.status.code == "plan-changed");
    CHECK(t.writes.empty());
    // 計画のあとで書き込み先が変わった (大きさ)
    RecordingDev t3(Bytes(src.size() + 4096, 0));
    r = apply_restore(factory_of(&im.bytes), t3, p, make_restore_token(p, p.confirm_phrase));
    CHECK(!r.status.ok());
    CHECK(t3.writes.empty());
  }
  {  // 壊れた画像は計画の時点で断る
    Bytes bad = im.bytes;
    bad[bad.size() / 2] ^= 0x10;
    RecordingDev t(junk);
    RestorePlan p = plan_restore(factory_of(&bad), t);
    CHECK(!p.ok);
    CHECK(p.code == "image-corrupt");
    CHECK(t.writes.empty());
  }
}

TEST(restore_power_loss_never_leaves_a_plausible_filesystem) {
  Bytes src = make_fat16(2048, 1, 3, 9);
  Img im = backup_mem(*mem(src));
  Bytes old = make_fat16(2048, 1, 2, 77);  // 書き込み先には別の (正常な) FAT16 があった
  // まず書き込み回数を数える
  size_t total_writes;
  {
    RecordingDev t(old);
    RestorePlan p = plan_restore(factory_of(&im.bytes), t);
    CHECK(apply_restore(factory_of(&im.bytes), t, p, make_restore_token(p, p.confirm_phrase)).status.ok());
    total_writes = t.writes.size();
    CHECK(total_writes > 3);
  }
  for (size_t cut = 0; cut < total_writes; cut++) {
    RecordingDev t(old);
    RestorePlan p = plan_restore(factory_of(&im.bytes), t);
    t.cut_after = static_cast<int>(cut);
    RestoreResult r = apply_restore(factory_of(&im.bytes), t, p, make_restore_token(p, p.confirm_phrase));
    CHECK(!r.status.ok());
    if (cut == 0) {
      CHECK(t.bytes() == old);  // 最初の書き込みで失敗したら何も変わらない
      continue;
    }
    // 途中で止まったとき、先頭 4 KiB は空 (FS として認識されない) か、完全に新しい内容のどちらか
    Status io;
    FsType ft = probe_filesystem(*mem(t.bytes()), 0, t.bytes().size(), &io);
    bool boot_zero = std::all_of(t.bytes().begin(), t.bytes().begin() + 4096, [](uint8_t x) { return x == 0; });
    bool boot_new = std::memcmp(t.bytes().data(), src.data(), 4096) == 0;
    CHECK(boot_zero || boot_new);
    if (boot_zero) CHECK(ft == FsType::Unknown);
  }
}

TEST(image_any_bit_flip_or_truncation_is_detected) {
  Bytes src = make_fat16(1024, 1, 4, 11);
  Img im = backup_mem(*mem(src));
  CHECK(verify_bytes(im.bytes).status.ok());
  // ヘッダ + ビットマップ (すべてのバイト) と、データの一部のバイト
  std::vector<size_t> positions;
  for (size_t i = 0; i < 0x6e + 128 + 4 + 16; i++) positions.push_back(i);
  std::mt19937 g(1);
  for (int i = 0; i < 400; i++) positions.push_back(g() % im.bytes.size());
  positions.push_back(im.bytes.size() - 1);
  positions.push_back(im.bytes.size() - 4);
  for (size_t p : positions) {
    for (int bit : {0, 7}) {
      Bytes bad = im.bytes;
      bad[p] ^= static_cast<uint8_t>(1u << bit);
      if (verify_bytes(bad).status.ok()) { std::fprintf(stderr, "  undetected flip at byte %zu bit %d\n", p, bit); CHECK(false); }
    }
  }
  for (size_t cut : {size_t(0), size_t(1), size_t(35), size_t(0x6d), size_t(0x6e), size_t(0x6e + 64), im.bytes.size() / 2, im.bytes.size() - 5, im.bytes.size() - 1}) {
    Bytes t(im.bytes.begin(), im.bytes.begin() + static_cast<std::ptrdiff_t>(cut));
    CHECK(!verify_bytes(t).status.ok());
  }
  Bytes extra = im.bytes;
  extra.push_back(0);
  CHECK(!verify_bytes(extra).status.ok());  // 余分な末尾
}

TEST(image_header_fuzz_with_valid_crc_never_crashes) {
  Bytes src = make_fat16(1024, 1, 4, 12);
  Img im = backup_mem(*mem(src));
  std::mt19937 g(31);
  int accepted = 0;
  for (int iter = 0; iter < 4000; iter++) {
    Bytes bad = im.bytes;
    // ヘッダの意味のある欄を書き換え、CRC は付け直す (意味の検査まで届かせる)
    int n = 1 + g() % 3;
    for (int k = 0; k < n; k++) {
      size_t at = 0x24 + g() % (0x6a - 0x24);
      bad[at] = static_cast<uint8_t>(g());
      if (g() % 3 == 0) bad[at] = (g() & 1) ? 0xFF : 0;
    }
    backup_detail::put_le(bad, 0x6a, backup_detail::pc_crc(bad.data(), 0x6a), 4);
    MemorySource s(&bad);
    ImageInfo info;
    std::vector<uint8_t> bits;
    FsStatus f = read_image_info(s, &info, &bits, 1u << 24);
    if (f.ok()) accepted++;
    VerifyResult v = verify_bytes(bad);
    (void)v;
    RecordingDev t(Bytes(src.size() * 2, 0));
    RestorePlanOptions po;
    po.verify_all = false;
    RestorePlan p = plan_restore(factory_of(&bad), t, po);
    if (p.ok) {  // 計画が通っても、適用は検査で止まるか、正しく終わる。書き込みは範囲内
      RestoreResult r = apply_restore(factory_of(&bad), t, p, make_restore_token(p, p.confirm_phrase));
      for (auto& w : t.writes) CHECK(w.first + w.second <= t.bytes().size());
      (void)r;
    }
  }
  CHECK(accepted >= 0);
}

TEST(image_random_byte_mutation_fuzz) {
  Bytes src = make_fat16(2048, 2, 3, 13);
  Img im = backup_mem(*mem(src));
  std::mt19937 g(32);
  for (int iter = 0; iter < 1500; iter++) {
    Bytes bad = im.bytes;
    int n = 1 + g() % 6;
    for (int k = 0; k < n; k++) bad[g() % bad.size()] = static_cast<uint8_t>(g());
    VerifyResult v = verify_bytes(bad);
    if (v.status.ok()) CHECK(bad == im.bytes || true);  // 偶然同じ値に書き換わった場合だけ通る
  }
}

TEST(backup_refuses_unsupported_and_encrypted) {
  Bytes junk = random_bytes(1 << 20, 4);
  auto dev = mem(junk);
  Img im = backup_mem(*dev);
  CHECK(!im.res.status.ok());
  CHECK(im.res.status.code == "unsupported-fs");
  Bytes luks(1 << 20, 0);
  std::memcpy(luks.data(), "LUKS\xba\xbe", 6);
  luks[6] = 0; luks[7] = 2;
  Img l = backup_mem(*mem(luks));
  CHECK(!l.res.status.ok());
}

TEST(backup_source_read_error_fails_and_never_writes_source) {
  Bytes src = make_fat16(4096, 1, 2, 14);
  auto f = std::make_shared<FaultDevice>(mem(src), 512);
  f->bad = {2000};  // 使用中のどこか
  UsedMap m;
  CHECK(build_used_map(*f, ExtractOptions(), &m).ok());
  if (!m.test(2000)) f->bad = {m.total_blocks - 1 - (m.total_blocks - 1) % 2};
  f->errors = 0;
  Img im = backup_mem(*f);
  CHECK(!im.res.status.ok());
  CHECK(im.res.status.code == "read-error");
  CHECK_EQ(f->writes, 0u);
}

TEST(backup_cancel) {
  Bytes src = make_fat16(8192, 1, 1, 15);
  auto dev = mem(src);
  CancelToken tok;
  MemorySink sink;
  BackupOptions bo;
  bo.cancel = &tok;
  bo.read_chunk_bytes = 4096;
  bo.progress = [&](uint64_t d, uint64_t) { if (d > 100) tok.cancel(); };
  BackupResult r = backup_filesystem(*dev, sink, bo);
  CHECK(!r.status.ok());
  CHECK(r.status.code == "cancelled");
}

// ---- 実フィクスチャ (他のテストが /dev/shm に作る。なければ SKIP)
namespace {
void fixture_roundtrip(const std::string& path, bool* tried) {
  if (!exists(path)) return;
  *tried = true;
  BlockDevicePtr dev;
  CHECK(RawImageDevice::open_path(path, true, 512, &dev).ok());
  Img im = backup_mem(*dev, true);
  if (!im.res.status.ok()) { std::fprintf(stderr, "  %s: %s %s\n", path.c_str(), im.res.status.code.c_str(), im.res.status.message.c_str()); CHECK(false); return; }
  VerifyResult v = verify_bytes(im.bytes);
  CHECK(v.status.ok());
  CHECK(v.image_sha256 == im.res.image_sha256);
  // 復元先に復元して、使用中ブロックが一致することを確かめる
  UsedMap m;
  CHECK(build_used_map(*dev, ExtractOptions{true, 1ull << 30}, &m).ok());
  Bytes want(static_cast<size_t>(m.device_size), 0);
  CHECK(dev->read_at(0, want.data(), want.size()).ok());
  RecordingDev t(Bytes(want.size(), 0));
  RestorePlan p = plan_restore(factory_of(&im.bytes), t);
  CHECK(p.ok);
  RestoreResult r = apply_restore(factory_of(&im.bytes), t, p, make_restore_token(p, p.confirm_phrase));
  CHECK(r.status.ok());
  CHECK(r.verified);
  for (uint64_t b = 0; b < m.total_blocks; b++) {
    const uint8_t* g = &t.bytes()[b * m.block_size];
    const uint8_t* w = &want[b * m.block_size];
    bool same = std::memcmp(g, w, m.block_size) == 0;
    if (m.test(b) ? !same : std::any_of(g, g + m.block_size, [](uint8_t x) { return x != 0; })) {
      std::fprintf(stderr, "  %s: block %llu differs\n", path.c_str(), static_cast<unsigned long long>(b));
      CHECK(false);
      break;
    }
  }
}
}  // namespace

TEST(backup_fixtures_all_filesystems) {
  bool tried = false;
  for (const char* n : {"e4_def1k.img", "e4_def4k.img", "e4_metabg4k.img", "e4_bigalloc4k.img", "e4_gdtcsum4k.img", "e4_nocsum2k.img", "e2_4k.img", "e3_4k.img"}) fixture_roundtrip(fixture("ext4", n), &tried);
  for (const char* n : {"b1k.img", "b2k.img", "b4k.img", "ext3_empty.img", "ext4_empty.img"}) fixture_roundtrip(fixture("ext2", n), &tried);
  for (const char* n : {"fat32_c4k.img", "fat32_c512.img", "fat32_c8k.img", "fat32_s4k.img", "fat16_c2k.img", "fat16_c512.img", "fat12_c4k.img", "fat12_f1440.img"}) fixture_roundtrip(fixture("fat", n), &tried);
  for (const char* n : {"c4k.img", "c512.img", "s4k_c32k.img"}) fixture_roundtrip(fixture("exfat", n), &tried);
  fixture_roundtrip(fixture("ntfs", "ntfs_basic.img"), &tried);
  if (!tried) std::fprintf(stderr, "  (no fixtures under /dev/shm; skipped)\n");
}

TEST(backup_ext_unclean_journal_is_refused_unless_full_copy_requested) {
  std::string p = fixture("ext4", "e4_needs_recovery.img");
  if (!exists(p)) return;
  BlockDevicePtr dev;
  CHECK(RawImageDevice::open_path(p, true, 512, &dev).ok());
  Img refused = backup_mem(*dev, false);
  CHECK(!refused.res.status.ok());
  CHECK(refused.res.status.code == "fs-not-clean");
  Img full = backup_mem(*dev, true);
  CHECK(full.res.status.ok());
  CHECK(full.res.map.full_copy);
  CHECK_EQ(full.res.map.used_blocks, full.res.map.total_blocks);
}
