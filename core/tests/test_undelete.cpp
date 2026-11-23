// SPDX-License-Identifier: Apache-2.0
// 削除ファイルの一覧と救出 (undelete.h / c_api_undelete.h) の単体テスト。手で組んだ小さな FAT16 のイメージに、LFN 付き・8.3 のみ・上書きされた・生きている・空のファイルを置いて
// 削除し、一覧・確度・救出した内容の SHA-256・読み取り専用・取り消し・壊れた入力 2000 件 (ASan/UBSan で見る)・C API を確かめる。
// 本物の mkfs.* が作った FAT/exFAT/ext/NTFS との突き合わせは tools/check_undelete_tools.py (ctest の undelete_tools)。
#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <random>

#include "test_util.h"
#include "usbmem/c_api_undelete.h"
#include "usbmem/sha256.h"
#include "usbmem/undelete.h"

using namespace usbmem;
using Bytes = std::vector<uint8_t>;

namespace {

// ---- 最小の FAT16 のライタ (512 バイト/セクタ、4 セクタ/クラスタ、FAT 2 本、ルート 512 項目)
struct Fat16 {
  static constexpr uint32_t kBps = 512, kSpc = 4, kReserved = 1, kFatSz = 20, kRootEnts = 512, kTotal = 20480;
  Bytes img;
  uint32_t root_off() const { return (kReserved + 2 * kFatSz) * kBps; }
  uint32_t data_off() const { return root_off() + kRootEnts * 32; }
  uint32_t cluster_bytes() const { return kBps * kSpc; }
  uint32_t next_free = 2;
  uint32_t root_slot = 0;

  Fat16() : img(static_cast<size_t>(kTotal) * kBps, 0) {
    uint8_t* b = img.data();
    b[0] = 0xEB; b[1] = 0x3C; b[2] = 0x90;
    std::memcpy(b + 3, "MSDOS5.0", 8);
    tb::le16(img, 11, kBps); b[13] = kSpc; tb::le16(img, 14, kReserved); b[16] = 2; tb::le16(img, 17, kRootEnts);
    tb::le16(img, 19, kTotal); b[21] = 0xF8; tb::le16(img, 22, kFatSz); tb::le16(img, 24, 32); tb::le16(img, 26, 64);
    b[36] = 0x80; b[38] = 0x29; tb::le32(img, 39, 0x12345678);
    std::memcpy(b + 43, "UDTEST     ", 11);
    std::memcpy(b + 54, "FAT16   ", 8);
    b[510] = 0x55; b[511] = 0xAA;
    set_fat(0, 0xFFF8); set_fat(1, 0xFFFF);
  }
  void set_fat(uint32_t c, uint16_t v) {
    for (int f = 0; f < 2; f++) tb::le16(img, (kReserved + f * kFatSz) * kBps + c * 2, v);
  }
  uint32_t cluster_off(uint32_t c) const { return data_off() + (c - 2) * cluster_bytes(); }

  // 連続した空きクラスタに data を書き、チェーンを張る。先頭クラスタを返す
  uint32_t alloc(const Bytes& data, int32_t force_first = -1) {
    uint32_t n = std::max<uint32_t>(1, (static_cast<uint32_t>(data.size()) + cluster_bytes() - 1) / cluster_bytes());
    uint32_t first = force_first >= 0 ? static_cast<uint32_t>(force_first) : next_free;
    for (uint32_t i = 0; i < n; i++) set_fat(first + i, i + 1 < n ? static_cast<uint16_t>(first + i + 1) : 0xFFFF);
    std::memcpy(&img[cluster_off(first)], data.data(), data.size());
    if (force_first < 0) next_free += n;
    return first;
  }
  static uint8_t lfn_sum(const uint8_t* n83) {
    uint8_t s = 0;
    for (int i = 0; i < 11; i++) s = static_cast<uint8_t>(((s & 1) ? 0x80 : 0) + (s >> 1) + n83[i]);
    return s;
  }
  // name83: 11 文字 (空白詰め)。lfn が空でなければ長い名前の項目も付ける (ASCII のみ)
  void add(const char* name83, const std::string& lfn, uint32_t first, uint32_t size, bool deleted) {
    uint8_t* d = &img[root_off() + root_slot * 32];
    if (!lfn.empty()) {
      size_t nent = (lfn.size() + 12) / 13;
      uint8_t sum = lfn_sum(reinterpret_cast<const uint8_t*>(name83));
      for (size_t k = nent; k >= 1; k--) {
        uint8_t* e = &img[root_off() + root_slot * 32];
        std::memset(e, 0, 32);
        e[0] = static_cast<uint8_t>(k == nent ? 0x40 | k : k);
        if (deleted) e[0] = 0xE5;
        e[11] = 0x0F; e[13] = sum;
        static const int pos[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
        for (int i = 0; i < 13; i++) {
          size_t ci = (k - 1) * 13 + static_cast<size_t>(i);
          uint16_t ch = ci < lfn.size() ? static_cast<uint16_t>(static_cast<uint8_t>(lfn[ci])) : ci == lfn.size() ? 0 : 0xFFFF;
          e[pos[i]] = static_cast<uint8_t>(ch); e[pos[i] + 1] = static_cast<uint8_t>(ch >> 8);
        }
        root_slot++;
      }
      d = &img[root_off() + root_slot * 32];
    }
    std::memset(d, 0, 32);
    std::memcpy(d, name83, 11);
    d[11] = 0x20;
    tb::le16(img, root_off() + root_slot * 32 + 16, 0x5821);   // 日付 2024-01-01 相当 (どの値でもよい)
    tb::le16(img, root_off() + root_slot * 32 + 24, 0x5821);
    tb::le16(img, root_off() + root_slot * 32 + 26, static_cast<uint16_t>(first));
    tb::le32(img, root_off() + root_slot * 32 + 28, size);
    if (deleted) d[0] = 0xE5;
    root_slot++;
  }
  // 削除: チェーンを 0 にする (Windows / Linux の vfat と同じ)
  void free_chain(uint32_t first, uint32_t size) {
    uint32_t n = std::max<uint32_t>(1, (size + cluster_bytes() - 1) / cluster_bytes());
    for (uint32_t i = 0; i < n; i++) set_fat(first + i, 0);
  }
};

std::string sha_of(const Bytes& b) { Sha256 h; h.update(b.data(), b.size()); return hex_encode(h.finish()); }

Bytes text_data(uint64_t seed, size_t n) {
  std::mt19937_64 g(seed);
  Bytes b;
  const char* w[] = {"alpha", "beta", "gamma", "delta", "report"};
  while (b.size() < n) { const char* s = w[g() % 5]; b.insert(b.end(), s, s + std::strlen(s)); b.push_back(g() % 7 ? ' ' : '\n'); }
  b.resize(n);
  return b;
}
Bytes jpeg_like(uint64_t seed, size_t n) {
  std::mt19937_64 g(seed);
  Bytes b = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0};
  while (b.size() < n) b.push_back(static_cast<uint8_t>(g()));
  b[n - 2] = 0xFF; b[n - 1] = 0xD9;
  return b;
}

struct Fixture {
  Fat16 fs;
  Bytes report, photo, victim, live, big;
  uint32_t victim_first = 0;
  Fixture() {
    report = text_data(1, 5000);        // LFN 付きの削除 (2 クラスタ)
    photo = jpeg_like(2, 3000);          // 8.3 のみの削除 (名前の先頭が失われる)
    victim = text_data(3, 9000);         // 削除後に別のファイルに上書きされる
    live = text_data(4, 4096);           // 生きている
    big = jpeg_like(5, 40000);           // 削除 (20 クラスタ)
    uint32_t c;
    c = fs.alloc(live);                  fs.add("LIVE    TXT", "", c, static_cast<uint32_t>(live.size()), false);
    c = fs.alloc(report);                fs.free_chain(c, static_cast<uint32_t>(report.size()));
    fs.add("QUARTE~1TXT", "Quarterly Report.txt", c, static_cast<uint32_t>(report.size()), true);
    c = fs.alloc(photo);                 fs.free_chain(c, static_cast<uint32_t>(photo.size()));
    fs.add("PHOTO   JPG", "", c, static_cast<uint32_t>(photo.size()), true);
    victim_first = fs.alloc(victim);     fs.free_chain(victim_first, static_cast<uint32_t>(victim.size()));
    fs.add("VICTIM  TXT", "", victim_first, static_cast<uint32_t>(victim.size()), true);
    c = fs.alloc(big);                   fs.free_chain(c, static_cast<uint32_t>(big.size()));
    fs.add("BIG     JPG", "", c, static_cast<uint32_t>(big.size()), true);
    // 空のファイル (削除済み)
    fs.add("EMPTY   TXT", "", 0, 0, true);
    // victim のクラスタの先頭 1 つを、新しい生きているファイルが使う (上書き)
    Bytes newer = text_data(6, 2000);
    fs.alloc(newer, static_cast<int32_t>(victim_first));
    fs.add("NEWER   TXT", "", victim_first, static_cast<uint32_t>(newer.size()), false);
  }
};

class CountingDevice final : public BlockDevice {
 public:
  explicit CountingDevice(Bytes d) : dev_(std::move(d), false) {}
  Status read_at(uint64_t o, void* b, size_t n, const IoOptions& io = {}) override { return dev_.read_at(o, b, n, io); }
  Status write_at(uint64_t, const void*, size_t, const IoOptions& = {}) override { writes++; return Status::Make(Errc::ReadOnly); }
  Status flush() override { return Status::Ok(); }
  Status close() override { return Status::Ok(); }
  uint64_t size() const override { return dev_.size(); }
  uint32_t logical_block_size() const override { return 512; }
  bool is_read_only() const override { return true; }
  std::string describe() const override { return "counting"; }
  int writes = 0;

 private:
  MemoryDevice dev_;
};

const DeletedFile* by_name(const UndeleteResult& r, const std::string& n) {
  for (const DeletedFile& f : r.files) if (f.name == n) return &f;
  return nullptr;
}

std::string extract_sha(BlockDevice& dev, const DeletedFile& f, Bytes* out = nullptr) {
  ExtractResult er;
  Bytes got;
  Status s = extract_deleted(dev, f, [&](const void* d, size_t n) { got.insert(got.end(), static_cast<const uint8_t*>(d), static_cast<const uint8_t*>(d) + n); return Status::Ok(); }, nullptr, &er);
  if (!s.ok()) return "error";
  if (out) *out = got;
  return er.sha256;
}

}  // namespace

TEST(undelete_fat16_lists_and_recovers) {
  Fixture fx;
  MemoryDevice dev(fx.fs.img, true);
  UndeleteResult r;
  CHECK(list_deleted(dev, UndeleteOptions(), &r).ok());
  CHECK(r.fs == "fat16");
  CHECK(r.allocation_known);
  // LFN 付き: 名前・パス・内容
  const DeletedFile* rep = by_name(r, "Quarterly Report.txt");
  CHECK(rep != nullptr);
  if (rep) {
    CHECK(rep->path == "/Quarterly Report.txt");
    CHECK_EQ(rep->size, fx.report.size());
    CHECK(!rep->name_partial);
    CHECK(rep->level == RecoverLevel::High);
    CHECK_EQ(rep->reused_blocks, 0u);
    CHECK(extract_sha(dev, *rep) == sha_of(fx.report));
  }
  // 8.3 のみ: 先頭 1 文字は失われる
  const DeletedFile* photo = nullptr;
  for (const DeletedFile& f : r.files) if (f.size == fx.photo.size()) photo = &f;
  CHECK(photo != nullptr);
  if (photo) {
    CHECK(photo->name_partial);
    CHECK(photo->name.size() >= 7 && photo->name.substr(1) == "HOTO.JPG");
    CHECK(photo->signature == "jpeg");
    CHECK(extract_sha(dev, *photo) == sha_of(fx.photo));
  }
  // 大きいファイル (複数クラスタ)
  const DeletedFile* big = nullptr;
  for (const DeletedFile& f : r.files) if (f.size == fx.big.size()) big = &f;
  CHECK(big != nullptr);
  if (big) {
    CHECK(extract_sha(dev, *big) == sha_of(fx.big));
    CHECK(big->level != RecoverLevel::Low && big->level != RecoverLevel::None);
  }
  // 生きているファイルと空のファイルは出ない
  for (const DeletedFile& f : r.files) { CHECK(f.size != fx.live.size() || f.name != "LIVE.TXT"); CHECK(f.size != 0); }
  // 空のファイルは include_empty のときだけ
  UndeleteOptions o;
  o.include_empty = true;
  UndeleteResult r2;
  CHECK(list_deleted(dev, o, &r2).ok());
  CHECK(r2.files.size() == r.files.size() + 1);
}

TEST(undelete_fat16_overwritten_is_low_never_high) {
  Fixture fx;
  MemoryDevice dev(fx.fs.img, true);
  UndeleteResult r;
  CHECK(list_deleted(dev, UndeleteOptions(), &r).ok());
  const DeletedFile* v = nullptr;
  for (const DeletedFile& f : r.files) if (f.size == fx.victim.size()) v = &f;
  CHECK(v != nullptr);
  if (v) {
    CHECK(v->level == RecoverLevel::Low);
    CHECK(v->reused_blocks > 0);
    CHECK(v->confidence < 50);
    // 内容は元と違う (上書きされている) ので、確度が高いはずがない
    CHECK(extract_sha(dev, *v) != sha_of(fx.victim));
  }
  // 安全の不変条件: 内容が元と一致しないものを 高 / 中 としない (このイメージでは victim だけが壊れている)
  for (const DeletedFile& f : r.files)
    if (f.size == fx.victim.size()) CHECK(f.level == RecoverLevel::Low || f.level == RecoverLevel::None);
}

TEST(undelete_is_read_only_and_cancellable) {
  Fixture fx;
  CountingDevice dev(fx.fs.img);
  UndeleteResult r;
  CHECK(list_deleted(dev, UndeleteOptions(), &r).ok());
  for (const DeletedFile& f : r.files) extract_sha(dev, f);
  CHECK_EQ(dev.writes, 0);
  CancelToken c;
  c.cancel();
  UndeleteOptions o;
  o.cancel = &c;
  UndeleteResult r2;
  Status s = list_deleted(dev, o, &r2);
  CHECK(s.code == Errc::Cancelled);
  CHECK(r2.cancelled);
  // 取り消しは抽出にも効く
  ExtractResult er;
  if (!r.files.empty()) CHECK(extract_deleted(dev, r.files[0], [](const void*, size_t) { return Status::Ok(); }, &c, &er).code == Errc::Cancelled);
  CHECK_EQ(dev.writes, 0);
}

TEST(undelete_unsupported_and_garbage_devices) {
  std::mt19937_64 g(5);
  Bytes junk(1 << 20);
  for (auto& b : junk) b = static_cast<uint8_t>(g());
  MemoryDevice d1(junk, true);
  UndeleteResult r;
  CHECK(list_deleted(d1, UndeleteOptions(), &r).code == Errc::Unsupported);
  MemoryDevice d2(Bytes(1 << 20, 0), true);
  CHECK(list_deleted(d2, UndeleteOptions(), &r).code == Errc::Unsupported);
}

TEST(undelete_json_has_documented_keys) {
  Fixture fx;
  MemoryDevice dev(fx.fs.img, true);
  UndeleteResult r;
  CHECK(list_deleted(dev, UndeleteOptions(), &r).ok());
  std::string j = undelete_result_to_json(r);
  for (const char* k : {"\"fs\"", "\"allocationKnown\"", "\"files\"", "\"name\"", "\"path\"", "\"size\"", "\"level\"", "\"confidence\"", "\"reusedBlocks\"", "\"totalBlocks\"",
                        "\"layoutExact\"", "\"signatureCheck\"", "\"namePartial\"", "\"inDeletedDir\"", "\"notes\""})
    CHECK(j.find(k) != std::string::npos);
}

TEST(undelete_fuzz_2000_corrupted_inputs) {
  // 管理情報 (空でないセクタ) の数バイトを壊して list + extract。落ちない・範囲外の読みが無い・書き込みが無い・極端な大きさで暴走しない (ASan / UBSan で確かめる)
  Fixture fx;
  std::vector<uint64_t> hot;
  for (uint64_t o = 0; o + 512 <= fx.fs.img.size(); o += 512) {
    bool nz = false;
    for (uint32_t i = 0; i < 512 && !nz; i++) nz = fx.fs.img[o + i] != 0;
    if (nz) hot.push_back(o);
  }
  std::mt19937_64 g(777);
  size_t listed = 0;
  for (int it = 0; it < 2000; it++) {
    Bytes img = fx.fs.img;
    int n = 1 + static_cast<int>(g() % 16);
    for (int k = 0; k < n; k++) {
      uint64_t o = hot[g() % hot.size()] + g() % 512;
      switch (g() % 4) {
        case 0: img[o] = static_cast<uint8_t>(g()); break;
        case 1: img[o] ^= static_cast<uint8_t>(1u << (g() % 8)); break;
        case 2: img[o] = 0xFF; break;
        default: img[o] = 0; break;
      }
    }
    CountingDevice dev(img);
    UndeleteResult r;
    Status s = list_deleted(dev, UndeleteOptions(), &r);
    if (s.ok()) {
      listed++;
      int did = 0;
      for (const DeletedFile& f : r.files) {
        if (f.level == RecoverLevel::None || f.size > (16u << 20) || did >= 6) continue;
        did++;
        ExtractResult er;
        extract_deleted(dev, f, [](const void*, size_t) { return Status::Ok(); }, nullptr, &er);
      }
      (void)undelete_result_to_json(r);
    }
    CHECK_EQ(dev.writes, 0);
  }
  CHECK(listed > 1000);
}

TEST(undelete_c_api_scan_extract_cancel) {
  Fixture fx;
  std::string path = test_tmpdir() + "/usbmem_ud_capi_" + std::to_string(getpid()) + ".img";
  { std::ofstream f(path, std::ios::binary); f.write(reinterpret_cast<const char*>(fx.fs.img.data()), static_cast<std::streamsize>(fx.fs.img.size())); }
  usbmem_device* dev = nullptr;
  CHECK_EQ(usbmem_open_path(path.c_str(), 1, &dev), 0);
  usbmem_ujob* job = usbmem_ujob_new();
  usbmem_undelete* h = nullptr;
  CHECK_EQ(usbmem_undelete_scan(dev, 0, job, &h), 0);
  CHECK(h != nullptr);
  if (h) {
    char* j = nullptr;
    CHECK_EQ(usbmem_undelete_result_json(h, &j), 0);
    std::string json = j ? j : "";
    if (j) usbmem_free(j);
    CHECK(json.find("Quarterly Report.txt") != std::string::npos);
    // 「Quarterly Report.txt」の id を探して、パスへ救出する (新規作成。既にあれば失敗)
    size_t p = json.find("\"Quarterly Report.txt\"");
    size_t q = json.rfind("\"id\":", p);
    unsigned id = q == std::string::npos ? 0 : static_cast<unsigned>(std::strtoul(json.c_str() + q + 5, nullptr, 10));
    std::string out = path + ".rescued";
    unlink(out.c_str());
    char* oj = nullptr;
    CHECK_EQ(usbmem_undelete_extract_path(h, id, out.c_str(), job, &oj), 0);
    std::string o = oj ? oj : "";
    if (oj) usbmem_free(oj);
    CHECK(o.find(sha_of(fx.report)) != std::string::npos);
    char* oj2 = nullptr;
    CHECK(usbmem_undelete_extract_path(h, id, out.c_str(), job, &oj2) != 0);   // 既存のファイルは上書きしない
    if (oj2) usbmem_free(oj2);
    unlink(out.c_str());
    usbmem_undelete_close(h);
  }
  usbmem_ujob_cancel(job);
  usbmem_undelete* h2 = nullptr;
  int rc = usbmem_undelete_scan(dev, 0, job, &h2);
  CHECK_EQ(rc, static_cast<int>(Errc::Cancelled));
  if (h2) usbmem_undelete_close(h2);
  usbmem_ujob_free(job);
  usbmem_close(dev);
  unlink(path.c_str());
}
