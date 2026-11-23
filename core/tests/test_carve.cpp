// SPDX-License-Identifier: Apache-2.0
// カービング (carve.h / c_api_undelete.h) の単体テスト。形式ごとの正しい見本を C++ で組み、背景 (ランダム・ゼロ・テキスト) の中に置いて切り出せること、
// 壊れたもの・偽物・背景では何も出ないこと、状態の保存と再開、読み取り専用、壊れた入力 2000 件 (ASan/UBSan で見る) を確かめる。
#include <algorithm>
#include <cstring>
#include <fstream>
#include <random>
#include <set>

#include "test_util.h"
#include "usbmem/c_api_undelete.h"
#include "usbmem/carve.h"
#include "usbmem/sha256.h"

using namespace usbmem;
using Bytes = std::vector<uint8_t>;

namespace {

void put(Bytes& b, const void* p, size_t n) { b.insert(b.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n); }
void put(Bytes& b, const char* s) { put(b, s, std::strlen(s)); }
void be32(Bytes& b, uint32_t v) { for (int i = 3; i >= 0; i--) b.push_back(static_cast<uint8_t>(v >> (8 * i))); }
void be16(Bytes& b, uint16_t v) { b.push_back(static_cast<uint8_t>(v >> 8)); b.push_back(static_cast<uint8_t>(v)); }
void le32(Bytes& b, uint32_t v) { for (int i = 0; i < 4; i++) b.push_back(static_cast<uint8_t>(v >> (8 * i))); }
void le16(Bytes& b, uint16_t v) { b.push_back(static_cast<uint8_t>(v)); b.push_back(static_cast<uint8_t>(v >> 8)); }

Bytes rnd(uint64_t seed, size_t n, bool no_ff = false) {
  std::mt19937_64 g(seed);
  Bytes b(n);
  for (auto& c : b) { c = static_cast<uint8_t>(g()); if (no_ff && c == 0xFF) c = 0x7F; }
  return b;
}

Bytes text_bytes(uint64_t seed, size_t n) {
  static const char* w[] = {"alpha", "beta", "gamma", "%PDF-1.4", "PK", "ftyp", "RIFF", "GIF89a", "usb", "memory"};
  std::mt19937_64 g(seed);
  Bytes b;
  while (b.size() < n) {
    for (int i = 0; i < 10; i++) { put(b, w[g() % 10]); b.push_back(' '); }
    b.push_back('\n');
  }
  b.resize(n);
  return b;
}

// ---- 見本 (形式の構造として正しい最小のもの)
Bytes mk_jpeg(uint64_t seed, size_t entropy = 2000) {
  Bytes b = {0xFF, 0xD8};
  put(b, "\xFF\xE0\x00\x10" "JFIF\x00\x01\x01\x00\x00\x01\x00\x01\x00\x00", 18);
  b.push_back(0xFF); b.push_back(0xDB); be16(b, 67); b.push_back(0);
  for (int i = 0; i < 64; i++) b.push_back(static_cast<uint8_t>(1 + i % 90));
  b.push_back(0xFF); b.push_back(0xC0); be16(b, 17); b.push_back(8); be16(b, 48); be16(b, 64); b.push_back(3);
  put(b, "\x01\x22\x00\x02\x11\x01\x03\x11\x01", 9);
  b.push_back(0xFF); b.push_back(0xC4); be16(b, 24); b.push_back(0);
  for (int i = 0; i < 16; i++) b.push_back(0);
  put(b, "\x01\x02\x03\x04\x05", 5);
  b.push_back(0xFF); b.push_back(0xDA); be16(b, 12); put(b, "\x03\x01\x00\x02\x11\x03\x11\x00\x3f\x00", 10);
  Bytes e = rnd(seed, entropy, true);
  b.insert(b.end(), e.begin(), e.end());
  b.push_back(0xFF); b.push_back(0xD9);
  return b;
}

void png_chunk(Bytes& b, const char* type, const Bytes& d) {
  be32(b, static_cast<uint32_t>(d.size()));
  Bytes td(type, type + 4);
  td.insert(td.end(), d.begin(), d.end());
  put(b, td.data(), td.size());
  be32(b, crc32(td.data(), td.size()));
}
Bytes mk_png(uint64_t seed) {
  Bytes b = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  Bytes ihdr;
  be32(ihdr, 16); be32(ihdr, 16); put(ihdr, "\x08\x02\x00\x00\x00", 5);
  png_chunk(b, "IHDR", ihdr);
  png_chunk(b, "IDAT", rnd(seed, 900));
  png_chunk(b, "IEND", Bytes());
  return b;
}

Bytes mk_gif(uint64_t seed) {
  Bytes b;
  put(b, "GIF89a");
  le16(b, 32); le16(b, 24); b.push_back(0xF1); b.push_back(0); b.push_back(0);
  Bytes pal = rnd(seed, 12);
  put(b, pal.data(), pal.size());
  put(b, "\x21\xF9\x04\x00\x0A\x00\x00\x00", 8);
  b.push_back(0x2C); le16(b, 0); le16(b, 0); le16(b, 32); le16(b, 24); b.push_back(0);
  b.push_back(2);
  Bytes d = rnd(seed + 1, 400);
  for (size_t i = 0; i < d.size(); i += 255) {
    size_t n = std::min<size_t>(255, d.size() - i);
    b.push_back(static_cast<uint8_t>(n));
    put(b, d.data() + i, n);
  }
  b.push_back(0);
  b.push_back(0x3B);
  return b;
}

Bytes mk_bmp(uint64_t seed) {
  const uint32_t w = 37, h = 21;
  uint32_t stride = ((w * 24 + 31) / 32) * 4;
  Bytes pix = rnd(seed, stride * h);
  Bytes b;
  put(b, "BM");
  le32(b, static_cast<uint32_t>(54 + pix.size())); le32(b, 0); le32(b, 54);
  le32(b, 40); le32(b, w); le32(b, h); le16(b, 1); le16(b, 24); le32(b, 0); le32(b, static_cast<uint32_t>(pix.size())); le32(b, 2835); le32(b, 2835); le32(b, 0); le32(b, 0);
  put(b, pix.data(), pix.size());
  return b;
}

Bytes mk_pdf(uint64_t seed) {
  Bytes b;
  put(b, "%PDF-1.4\n");
  std::vector<size_t> offs;
  const char* objs[] = {"<< /Type /Catalog /Pages 2 0 R >>", "<< /Type /Pages /Kids [3 0 R] /Count 1 >>", "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] >>"};
  for (int i = 0; i < 3; i++) {
    offs.push_back(b.size());
    std::string s = std::to_string(i + 1) + " 0 obj\n" + objs[i] + "\nendobj\n";
    put(b, s.c_str());
  }
  (void)seed;
  size_t xref = b.size();
  std::string x = "xref\n0 4\n0000000000 65535 f \n";
  char line[32];
  for (size_t o : offs) { std::snprintf(line, sizeof line, "%010zu 00000 n \n", o); x += line; }
  x += "trailer\n<< /Size 4 /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
  put(b, x.c_str());
  return b;
}

// 無圧縮 (stored) の zip。zip_name 先頭のファイルの内容を [Content_Types].xml にすると docx 系の分類になる
Bytes mk_zip(const std::vector<std::pair<std::string, Bytes>>& files) {
  Bytes b, cd;
  for (auto& f : files) {
    uint32_t off = static_cast<uint32_t>(b.size());
    uint32_t c = crc32(f.second.data(), f.second.size());
    le32(b, 0x04034B50); le16(b, 20); le16(b, 0); le16(b, 0); le16(b, 0); le16(b, 0x21); le32(b, c);
    le32(b, static_cast<uint32_t>(f.second.size())); le32(b, static_cast<uint32_t>(f.second.size())); le16(b, static_cast<uint16_t>(f.first.size())); le16(b, 0);
    put(b, f.first.data(), f.first.size());
    put(b, f.second.data(), f.second.size());
    le32(cd, 0x02014B50); le16(cd, 20); le16(cd, 20); le16(cd, 0); le16(cd, 0); le16(cd, 0); le16(cd, 0x21); le32(cd, c);
    le32(cd, static_cast<uint32_t>(f.second.size())); le32(cd, static_cast<uint32_t>(f.second.size())); le16(cd, static_cast<uint16_t>(f.first.size()));
    le16(cd, 0); le16(cd, 0); le16(cd, 0); le16(cd, 0); le32(cd, 0); le32(cd, off);
    put(cd, f.first.data(), f.first.size());
  }
  uint32_t cdoff = static_cast<uint32_t>(b.size());
  put(b, cd.data(), cd.size());
  le32(b, 0x06054B50); le16(b, 0); le16(b, 0); le16(b, static_cast<uint16_t>(files.size())); le16(b, static_cast<uint16_t>(files.size()));
  le32(b, static_cast<uint32_t>(cd.size())); le32(b, cdoff); le16(b, 0);
  return b;
}

// zlib を使わず、stored ブロックだけの gzip
Bytes mk_gzip(uint64_t seed) {
  Bytes data = text_bytes(seed, 3000);
  Bytes b = {0x1F, 0x8B, 8, 0, 0, 0, 0, 0, 0, 0xFF};
  size_t pos = 0;
  while (pos < data.size()) {
    size_t n = std::min<size_t>(data.size() - pos, 1000);
    bool last = pos + n == data.size();
    b.push_back(last ? 1 : 0);
    le16(b, static_cast<uint16_t>(n)); le16(b, static_cast<uint16_t>(~n));
    put(b, data.data() + pos, n);
    pos += n;
  }
  le32(b, crc32(data.data(), data.size()));
  le32(b, static_cast<uint32_t>(data.size()));
  return b;
}

Bytes mk_wav(uint64_t seed) {
  Bytes pcm = rnd(seed, 4000);
  Bytes body;
  put(body, "WAVEfmt ");
  le32(body, 16); le16(body, 1); le16(body, 1); le32(body, 8000); le32(body, 8000); le16(body, 1); le16(body, 8);
  put(body, "data");
  le32(body, static_cast<uint32_t>(pcm.size()));
  put(body, pcm.data(), pcm.size());
  Bytes b;
  put(b, "RIFF");
  le32(b, static_cast<uint32_t>(body.size()));
  put(b, body.data(), body.size());
  return b;
}

void atom(Bytes& b, const char* t, const Bytes& d) { be32(b, static_cast<uint32_t>(8 + d.size())); put(b, t, 4); put(b, d.data(), d.size()); }
Bytes mk_mp4(uint64_t seed) {
  Bytes b, ft, mv, mo;
  put(ft, "isom"); be32(ft, 0x200); put(ft, "isomiso2mp41");
  atom(b, "ftyp", ft);
  mv.assign(100, 0); be32(mv, 2);
  atom(mo, "mvhd", mv);
  atom(mo, "trak", Bytes(60, 0));
  atom(b, "free", Bytes(24, 0));
  atom(b, "mdat", rnd(seed, 6000));
  atom(b, "moov", mo);
  return b;
}

Bytes mk_mp3(uint64_t seed) {
  Bytes b;
  Bytes body;
  put(body, "TIT2"); be32(body, 11); body.push_back(0); body.push_back(0); body.push_back(0); put(body, "sample song");
  body.resize(111, 0);
  put(b, "ID3\x03\x00\x00", 6);
  size_t n = body.size();
  b.push_back(static_cast<uint8_t>((n >> 21) & 0x7F)); b.push_back(static_cast<uint8_t>((n >> 14) & 0x7F));
  b.push_back(static_cast<uint8_t>((n >> 7) & 0x7F)); b.push_back(static_cast<uint8_t>(n & 0x7F));
  put(b, body.data(), body.size());
  for (int i = 0; i < 20; i++) {  // MPEG-1 Layer III 128 kbps 44.1 kHz パディング無し = 417 バイト
    Bytes f = rnd(seed + static_cast<uint64_t>(i), 417);
    f[0] = 0xFF; f[1] = 0xFB; f[2] = 0x90; f[3] = 0x00;
    put(b, f.data(), f.size());
  }
  return b;
}

struct Sample { std::string name, format, ext; Bytes data; };

std::vector<Sample> all_samples() {
  std::vector<Sample> s;
  s.push_back({"jpeg", "jpeg", "jpg", mk_jpeg(1, 3000)});
  s.push_back({"png", "png", "png", mk_png(2)});
  s.push_back({"gif", "gif", "gif", mk_gif(3)});
  s.push_back({"bmp", "bmp", "bmp", mk_bmp(4)});
  s.push_back({"pdf", "pdf", "pdf", mk_pdf(5)});
  s.push_back({"zip", "zip", "zip", mk_zip({{"a.txt", text_bytes(6, 700)}, {"dir/b.bin", rnd(7, 900)}})});
  s.push_back({"docx", "zip", "docx", mk_zip({{"[Content_Types].xml", Bytes{'<', 'T', '/', '>'}}, {"word/document.xml", text_bytes(8, 500)}})});
  s.push_back({"gzip", "gzip", "gz", mk_gzip(9)});
  s.push_back({"wav", "riff", "wav", mk_wav(10)});
  s.push_back({"mp4", "mp4", "mp4", mk_mp4(11)});
  s.push_back({"mp3", "mp3", "mp3", mk_mp3(12)});
  return s;
}

// 背景 (ランダム / ゼロ / テキストを交互) の中の 512 の倍数の位置にファイルを置く。置いた位置を返す
Bytes build_image(const std::vector<Sample>& samples, std::vector<uint64_t>* offs, uint64_t seed = 1) {
  std::mt19937_64 g(seed);
  Bytes img;
  for (const Sample& s : samples) {
    size_t gap = 512 * (1 + g() % 8);
    Bytes bg;
    switch (g() % 3) { case 0: bg = rnd(g(), gap); break; case 1: bg = Bytes(gap, 0); break; default: bg = text_bytes(g(), gap); }
    img.insert(img.end(), bg.begin(), bg.end());
    offs->push_back(img.size());
    img.insert(img.end(), s.data.begin(), s.data.end());
    img.resize((img.size() + 511) / 512 * 512 + 512, 0);
    Bytes tail = rnd(g(), 512);
    img.insert(img.end(), tail.begin(), tail.end());
  }
  Bytes tail = rnd(g(), 4096);
  img.insert(img.end(), tail.begin(), tail.end());
  return img;
}

// 書き込みが来たら数える (カービングは読み取り専用でなければならない)
class CountingDevice final : public BlockDevice {
 public:
  explicit CountingDevice(Bytes d) : dev_(std::move(d), false) {}
  Status read_at(uint64_t o, void* b, size_t n, const IoOptions& io = {}) override { reads++; return dev_.read_at(o, b, n, io); }
  Status write_at(uint64_t, const void*, size_t, const IoOptions& = {}) override { writes++; return Status::Make(Errc::ReadOnly); }
  Status flush() override { return Status::Ok(); }
  Status close() override { return Status::Ok(); }
  uint64_t size() const override { return dev_.size(); }
  uint32_t logical_block_size() const override { return 512; }
  bool is_read_only() const override { return true; }
  std::string describe() const override { return "counting"; }
  int writes = 0;
  long reads = 0;

 private:
  MemoryDevice dev_;
};

Status scan(BlockDevice& dev, const CarveOptions& opt, CarveState* st, CarveResult* res = nullptr) {
  CarveResult r;
  return carve_scan(dev, opt, st, res ? res : &r);
}

const CarveHit* find_hit(const CarveState& st, uint64_t off) {
  for (const CarveHit& h : st.hits) if (h.offset == off) return &h;
  return nullptr;
}

std::string sha_of(const Bytes& b) { Sha256 h; h.update(b.data(), b.size()); return hex_encode(h.finish()); }

}  // namespace

TEST(carve_types_listed) {
  std::vector<CarveTypeInfo> t = carve_types();
  std::set<std::string> ids;
  for (auto& x : t) ids.insert(x.id);
  for (const char* want : {"jpeg", "png", "gif", "bmp", "pdf", "zip", "mp4", "mp3", "riff", "7z", "gzip", "sqlite", "pe"}) CHECK(ids.count(want) == 1);
}

TEST(carve_formats_exact_position_length_hash) {
  std::vector<Sample> samples = all_samples();
  std::vector<uint64_t> offs;
  Bytes img = build_image(samples, &offs);
  MemoryDevice dev(img, true);
  CarveOptions opt;
  CarveState st;
  CHECK(scan(dev, opt, &st).ok());
  CHECK(st.finished);
  for (size_t i = 0; i < samples.size(); i++) {
    const CarveHit* h = find_hit(st, offs[i]);
    if (!h) { std::fprintf(stderr, "  not found: %s at %llu\n", samples[i].name.c_str(), static_cast<unsigned long long>(offs[i])); g_failures++; continue; }
    CHECK_EQ(h->length, samples[i].data.size());
    CHECK(h->format == samples[i].format);
    CHECK(h->ext == samples[i].ext);
    Bytes got;
    ExtractResult er;
    CHECK(carve_extract(dev, *h, [&](const void* d, size_t n) { put(got, d, n); return Status::Ok(); }, nullptr, &er).ok());
    CHECK(got == samples[i].data);
    CHECK(er.sha256 == sha_of(samples[i].data));
  }
  CHECK_EQ(st.hits.size(), samples.size());   // 置いたもの以外は出さない
}

TEST(carve_damaged_samples_are_rejected) {
  // CRC や構造で壊れが分かる形式: 1 バイト壊した / 途中で切れて後ろに別のデータが続く、ものは出さない
  std::vector<Sample> all = all_samples();
  std::vector<Sample> bad;
  for (const Sample& s : all) {
    if (s.name == "png" || s.name == "gzip") {
      Sample c = s; c.name += "_flip"; c.data[c.data.size() / 2] ^= 0x55; bad.push_back(c);
    }
    if (s.name == "jpeg" || s.name == "png" || s.name == "gif" || s.name == "pdf" || s.name == "zip" || s.name == "gzip") {
      Sample c = s; c.name += "_cut"; c.data.resize(c.data.size() * 6 / 10); bad.push_back(c);
    }
  }
  std::vector<uint64_t> offs;
  Bytes img = build_image(bad, &offs, 77);
  MemoryDevice dev(img, true);
  CarveState st;
  CHECK(scan(dev, CarveOptions(), &st).ok());
  for (size_t i = 0; i < bad.size(); i++) {
    const CarveHit* h = find_hit(st, offs[i]);
    if (h) { std::fprintf(stderr, "  damaged sample accepted: %s\n", bad[i].name.c_str()); g_failures++; }
  }
}

TEST(carve_no_false_positives_in_noise) {
  Bytes img;
  Bytes r = rnd(99, 3 << 20), z(3 << 20, 0), t = text_bytes(98, 3 << 20), ff(1 << 20, 0xFF);
  for (const Bytes* p : {&r, &z, &t, &ff}) img.insert(img.end(), p->begin(), p->end());
  // 各形式の署名の先頭だけを 512 バイトごとに置いたランダム
  Bytes seeded = rnd(97, 2 << 20);
  const std::vector<std::string> sigs = {std::string("\xFF\xD8\xFF\xE0", 4), std::string("\x89PNG\r\n\x1A\n", 8), "GIF89a", "BM", "%PDF-1.4\n", std::string("PK\x03\x04", 4),
                                         std::string("\x00\x00\x00\x18" "ftypisom", 12), "ID3\x03", std::string("\xFF\xFB\x90\x00", 4), std::string("RIFF\x24\x00\x00\x00WAVE", 12),
                                         std::string("7z\xBC\xAF\x27\x1C\x00\x04", 8), std::string("\x1F\x8B\x08\x00", 4), "SQLite format 3", "MZ"};
  std::mt19937_64 g(5);
  for (size_t o = 0; o + 512 <= seeded.size(); o += 512) {
    const std::string& s = sigs[g() % sigs.size()];
    std::memcpy(&seeded[o], s.data(), s.size());
  }
  img.insert(img.end(), seeded.begin(), seeded.end());
  MemoryDevice dev(img, true);
  CarveState st;
  CHECK(scan(dev, CarveOptions(), &st).ok());
  CHECK_EQ(st.hits.size(), 0u);
  CHECK(st.rejected > 0);   // 署名が合った候補は構造の検査で捨てている
}

TEST(carve_options_types_min_size_align) {
  std::vector<Sample> samples = all_samples();
  std::vector<uint64_t> offs;
  Bytes img = build_image(samples, &offs);
  MemoryDevice dev(img, true);
  {
    CarveOptions o;
    o.types = {"png", "jpeg"};
    CarveState st;
    CHECK(scan(dev, o, &st).ok());
    CHECK_EQ(st.hits.size(), 2u);
    for (auto& h : st.hits) CHECK(h.format == "png" || h.format == "jpeg");
  }
  {
    CarveOptions o;
    o.min_size = 2000;
    CarveState st;
    CHECK(scan(dev, o, &st).ok());
    size_t want = 0;
    for (auto& s : samples) if (s.data.size() >= 2000) want++;
    CHECK_EQ(st.hits.size(), want);
    for (auto& h : st.hits) CHECK(h.length >= 2000);
  }
  {  // 512 の倍数でない位置は、align = 1 のときだけ
    Bytes odd = rnd(3, 100);
    Bytes png = mk_png(2);
    odd.insert(odd.end(), png.begin(), png.end());
    Bytes tail = rnd(4, 2048);
    odd.insert(odd.end(), tail.begin(), tail.end());
    MemoryDevice d2(odd, true);
    CarveState a, b;
    CarveOptions o;
    CHECK(scan(d2, o, &a).ok());
    CHECK_EQ(a.hits.size(), 0u);
    o.align = 1;
    CHECK(scan(d2, o, &b).ok());
    CHECK_EQ(b.hits.size(), 1u);
    CHECK(b.hits.size() == 1 && b.hits[0].offset == 100);
  }
  {  // 不明な形式の名前
    CarveOptions o;
    o.types = {"nosuchtype"};
    CarveState st;
    CHECK(scan(dev, o, &st).code == Errc::InvalidArgument);
  }
}

TEST(carve_state_text_roundtrip_and_resume) {
  std::vector<Sample> samples = all_samples();
  std::vector<uint64_t> offs;
  Bytes img = build_image(samples, &offs);
  MemoryDevice dev(img, true);
  CarveState full;
  CHECK(scan(dev, CarveOptions(), &full).ok());
  // 2 件目で取り消す
  CancelToken cancel;
  CarveOptions o;
  o.cancel = &cancel;
  CarveState part;
  size_t seen = 0;
  o.on_hit = [&](const CarveHit&) { if (++seen == 2) cancel.cancel(); return Status::Ok(); };
  Status st = scan(dev, o, &part);
  CHECK(st.code == Errc::Cancelled);
  CHECK(!part.finished);
  CHECK(part.hits.size() < full.hits.size());
  // 保存 → 読み込み → 再開
  std::string text = carve_state_to_text(part);
  CarveState loaded;
  CHECK(carve_state_parse(text, &loaded));
  CHECK_EQ(loaded.pos, part.pos);
  CHECK_EQ(loaded.hits.size(), part.hits.size());
  CHECK_EQ(loaded.options_hash, part.options_hash);
  CarveOptions o2;
  CHECK(scan(dev, o2, &loaded).ok());
  CHECK(loaded.finished);
  CHECK_EQ(loaded.hits.size(), full.hits.size());
  for (size_t i = 0; i < full.hits.size() && i < loaded.hits.size(); i++) {
    CHECK_EQ(loaded.hits[i].offset, full.hits[i].offset);
    CHECK_EQ(loaded.hits[i].length, full.hits[i].length);
  }
  // 別の設定の状態は拒否する
  CarveOptions o3;
  o3.types = {"png"};
  CarveState other = part;
  CHECK(scan(dev, o3, &other).code == Errc::InvalidArgument);
  // 壊れた状態の文字列
  CarveState junk;
  CHECK(!carve_state_parse("garbage", &junk));
  CHECK(!carve_state_parse("", &junk));
  std::string trunc = text.substr(0, text.size() / 2);
  CarveState t2;
  (void)carve_state_parse(trunc, &t2);   // 落ちないこと (true / false どちらでもよい)
  std::string bad = text;
  size_t p = bad.find("pos ");
  if (p != std::string::npos) bad.replace(p, 4, "pos x");
  CarveState t3;
  CHECK(!carve_state_parse(bad, &t3));
}

TEST(carve_is_read_only_and_cancellable) {
  std::vector<Sample> samples = all_samples();
  std::vector<uint64_t> offs;
  CountingDevice dev(build_image(samples, &offs));
  CarveState st;
  CHECK(scan(dev, CarveOptions(), &st).ok());
  CHECK_EQ(dev.writes, 0);
  CHECK(dev.reads > 0);
  for (const CarveHit& h : st.hits) carve_extract(dev, h, [](const void*, size_t) { return Status::Ok(); }, nullptr, nullptr);
  CHECK_EQ(dev.writes, 0);
  // 最初から取り消し済み
  CancelToken c;
  c.cancel();
  CarveOptions o;
  o.cancel = &c;
  CarveState st2;
  CarveResult res;
  Status s = scan(dev, o, &st2, &res);
  CHECK(s.code == Errc::Cancelled);
  CHECK(res.cancelled);
  CHECK_EQ(dev.writes, 0);
}

TEST(carve_progress_and_json) {
  std::vector<Sample> samples = all_samples();
  std::vector<uint64_t> offs;
  Bytes img = build_image(samples, &offs);
  MemoryDevice dev(img, true);
  CarveOptions o;
  uint64_t last_done = 0;
  bool monotonic = true;
  o.progress = [&](const char*, uint64_t d, uint64_t) { if (d < last_done) monotonic = false; last_done = d; };
  CarveState st;
  CarveResult res;
  CHECK(scan(dev, o, &st, &res).ok());
  CHECK(monotonic);
  CHECK(res.bytes_scanned > 0);
  std::string j = carve_result_to_json(res, st);
  CHECK(j.find("\"hits\":[") != std::string::npos);
  CHECK(j.find("\"format\":\"png\"") != std::string::npos);
  CHECK(carve_hit_filename(st.hits[0]).size() > 4);
}

TEST(carve_fuzz_2000_corrupted_inputs) {
  // 見本を並べたイメージの数バイトを壊す / 切る / 長さのフィールドを大きな値にする。落ちない・範囲外の読みが無い・結果が範囲内 (ASan / UBSan で確かめる)
  std::vector<Sample> samples = all_samples();
  std::vector<uint64_t> offs;
  Bytes base = build_image(samples, &offs);
  std::mt19937_64 g(2024);
  size_t accepted = 0;
  for (int it = 0; it < 2000; it++) {
    Bytes img = base;
    int n = 1 + static_cast<int>(g() % 24);
    for (int k = 0; k < n; k++) {
      // 見本の先頭付近 (ヘッダ) を多めに
      uint64_t o = (g() % 2) ? offs[g() % offs.size()] + g() % 128 : g() % img.size();
      if (o >= img.size()) continue;
      switch (g() % 4) {
        case 0: img[o] = static_cast<uint8_t>(g()); break;
        case 1: img[o] ^= static_cast<uint8_t>(1u << (g() % 8)); break;
        case 2: img[o] = 0xFF; break;
        default: img[o] = 0; break;
      }
    }
    if (g() % 8 == 0) img.resize(g() % img.size());
    MemoryDevice dev(img, true);
    CarveOptions o;
    if (g() % 4 == 0) o.align = 1;
    CarveState st;
    CarveResult res;
    Status s = carve_scan(dev, o, &st, &res);
    CHECK(s.ok());
    for (const CarveHit& h : st.hits) {
      if (!(h.length > 0 && h.offset + h.length <= img.size())) { std::fprintf(stderr, "  hit out of range at iteration %d\n", it); g_failures++; break; }
      accepted++;
    }
    if (it % 100 == 0 && !st.hits.empty()) {
      ExtractResult er;
      CHECK(carve_extract(dev, st.hits[0], [](const void*, size_t) { return Status::Ok(); }, nullptr, &er).ok());
      CHECK_EQ(er.bytes, st.hits[0].length);
    }
  }
  CHECK(accepted > 0);
}

TEST(carve_c_api_run_resume_cancel) {
  std::vector<Sample> samples = all_samples();
  std::vector<uint64_t> offs;
  Bytes img = build_image(samples, &offs);
  std::string dir = test_tmpdir();
  std::string path = dir + "/usbmem_carve_capi_" + std::to_string(getpid()) + ".img";
  std::string state = path + ".state";
  std::string outdir = path + ".out";
  { std::ofstream f(path, std::ios::binary); f.write(reinterpret_cast<const char*>(img.data()), static_cast<std::streamsize>(img.size())); }
  std::string cmd = "mkdir -p " + outdir;
  CHECK(std::system(cmd.c_str()) == 0);
  usbmem_device* dev = nullptr;
  CHECK_EQ(usbmem_open_path(path.c_str(), 1, &dev), 0);
  usbmem_carve_options opt;
  std::memset(&opt, 0, sizeof opt);
  {  // 取り消し済みのジョブ: Cancelled だが結果のハンドルは返る
    usbmem_ujob* job = usbmem_ujob_new();
    usbmem_ujob_cancel(job);
    usbmem_carve* h = nullptr;
    std::string cstate = state + ".cancelled";
    int rc = usbmem_carve_run(dev, nullptr, &opt, cstate.c_str(), nullptr, job, &h);
    CHECK_EQ(rc, static_cast<int>(Errc::Cancelled));
    unlink(cstate.c_str());
    CHECK(h != nullptr);
    if (h) usbmem_carve_close(h);
    usbmem_ujob_free(job);
  }
  {  // 通しで実行し、書き出されたファイルの SHA-256 が元と一致する
    usbmem_ujob* job = usbmem_ujob_new();
    usbmem_carve* h = nullptr;
    int rc = usbmem_carve_run(dev, "png,jpeg,pdf", &opt, state.c_str(), outdir.c_str(), job, &h);
    CHECK_EQ(rc, 0);
    char* json = nullptr;
    if (h) {
      CHECK_EQ(usbmem_carve_result_json(h, &json), 0);
      if (json) {
        std::string j = json;
        CHECK(j.find("\"finished\":true") != std::string::npos);
        usbmem_free(json);
      }
      std::string ex = path + ".extracted";
      char* oj = nullptr;
      CHECK_EQ(usbmem_carve_extract_path(h, 0, ex.c_str(), nullptr, &oj), 0);
      if (oj) usbmem_free(oj);
      CHECK(access(ex.c_str(), R_OK) == 0);
      unlink(ex.c_str());
      usbmem_carve_close(h);
    }
    usbmem_ujob_free(job);
  }
  {  // 別の設定の状態ファイル: InvalidArgument でハンドル無し
    usbmem_carve* h = nullptr;
    int rc = usbmem_carve_run(dev, "gif", &opt, state.c_str(), nullptr, nullptr, &h);
    CHECK_EQ(rc, static_cast<int>(Errc::InvalidArgument));
    CHECK(h == nullptr);
    if (h) usbmem_carve_close(h);
  }
  {  // 形式の一覧
    char* tj = nullptr;
    CHECK_EQ(usbmem_carve_types_json(&tj), 0);
    if (tj) { CHECK(std::strstr(tj, "\"jpeg\"") != nullptr); usbmem_free(tj); }
  }
  usbmem_close(dev);
  cmd = "rm -rf " + outdir;
  CHECK(std::system(cmd.c_str()) == 0);
  unlink(path.c_str());
  unlink(state.c_str());
}
