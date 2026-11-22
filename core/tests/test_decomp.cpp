// SPDX-License-Identifier: Apache-2.0
// 展開器 (usbmem/decomp.h: zlib / LZ4 ブロック / LZO1X / Zstandard) の試験。docs/decomp.md
//   test_decomp <fixdir>               tools/gen_decomp_fixtures.py が作った manifest.txt の全項目: 本物の圧縮器 (zstd / lzop / zlib) の出力が元データへ展開されること、
//                                      出力の容量が足りないとき (ENOSPC)・ちょうどのとき・余裕があるときの挙動
//   test_decomp --fuzz <fixdir> <回数> <シード>   圧縮データを壊して (ビット反転・切り詰め・挿入・複製) 展開する。クラッシュ・範囲外の読み書き (ASan/UBSan) が無いこと、
//                                      展開が成功した場合は出力が容量に収まっていること
//   test_decomp --unit                 手で作った小さな入力 (LZO-RLE・LZ4 の重なる一致・zstd の RLE / raw ブロックなど)
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "usbmem/decomp.h"

namespace {
using Bytes = std::vector<uint8_t>;
int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "  CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

bool read_file(const std::string& p, Bytes* out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return true;
}

inline bool same(const uint8_t* a, const uint8_t* b, size_t n) { return n == 0 || std::memcmp(a, b, n) == 0; }

struct Case { std::string alg, cfile, rfile; };

int run(const std::string& alg, const Bytes& c, uint8_t* dst, size_t cap, size_t* outlen) {
  *outlen = 0;
  if (alg == "zlib") return usbmem_zlib_decompress(c.data(), c.size(), dst, cap, outlen);
  if (alg == "zstd") return usbmem_zstd_decompress(c.data(), c.size(), dst, cap, outlen);
  if (alg == "lzo") { *outlen = cap; int rc = usbmem_lzo1x_decompress(c.data(), c.size(), dst, outlen); return rc; }
  if (alg == "lz4") return usbmem_lz4_block_decompress(c.data(), c.size(), dst, 0, cap, outlen);
  return -EINVAL;
}

std::vector<Case> load_manifest(const std::string& dir) {
  std::vector<Case> v;
  std::ifstream f(dir + "/manifest.txt");
  std::string line;
  while (std::getline(f, line)) {
    std::istringstream is(line);
    Case c;
    if (is >> c.alg >> c.cfile >> c.rfile) { c.cfile = dir + "/" + c.cfile; c.rfile = dir + "/" + c.rfile; v.push_back(c); }
  }
  return v;
}

int cmd_verify(const std::string& dir) {
  auto cases = load_manifest(dir);
  if (cases.empty()) { std::fprintf(stderr, "manifest.txt が空 / 無い: %s\n", dir.c_str()); return 1; }
  size_t ok = 0;
  std::map<std::string, size_t> per;
  for (const auto& c : cases) {
    Bytes comp, raw;
    if (!read_file(c.cfile, &comp) || !read_file(c.rfile, &raw)) { std::fprintf(stderr, "読めない: %s\n", c.cfile.c_str()); g_fail++; continue; }
    // ちょうどの容量
    Bytes out(raw.size() + 64, 0xAA);
    size_t n = 0;
    int rc = run(c.alg, comp, out.data(), raw.size(), &n);
    if (rc != 0 || n != raw.size() || !same(out.data(), raw.data(), raw.size())) {
      std::fprintf(stderr, "  不一致: %s %s rc=%d n=%zu (期待 %zu)\n", c.alg.c_str(), c.cfile.c_str(), rc, n, raw.size());
      g_fail++;
      continue;
    }
    for (size_t i = raw.size(); i < out.size(); i++) if (out[i] != 0xAA) { std::fprintf(stderr, "  容量の外へ書いた: %s\n", c.cfile.c_str()); g_fail++; break; }
    // 余裕のある容量
    Bytes big(raw.size() + 100000, 0);
    rc = run(c.alg, comp, big.data(), big.size(), &n);
    if (rc != 0 || n != raw.size() || !same(big.data(), raw.data(), raw.size())) { std::fprintf(stderr, "  大きい容量で不一致: %s rc=%d\n", c.cfile.c_str(), rc); g_fail++; continue; }
    // 足りない容量: ENOSPC (LZ4 / LZO / zstd / zlib とも)。書いた分は容量内
    if (raw.size() > 0) {
      Bytes small(raw.size() - 1 + 16, 0x55);
      rc = run(c.alg, comp, small.data(), raw.size() - 1, &n);
      if (rc == 0 || n > raw.size() - 1) { std::fprintf(stderr, "  足りない容量で成功 / 超過: %s rc=%d n=%zu\n", c.cfile.c_str(), rc, n); g_fail++; continue; }
      for (size_t i = raw.size() - 1; i < small.size(); i++) if (small[i] != 0x55) { std::fprintf(stderr, "  足りない容量の外へ書いた: %s\n", c.cfile.c_str()); g_fail++; break; }
    }
    ok++;
    per[c.alg]++;
  }
  for (const auto& kv : per) std::printf("  %s: %zu 件 OK\n", kv.first.c_str(), kv.second);
  std::printf("decomp: %zu / %zu 件 OK\n", ok, cases.size());
  return g_fail ? 1 : 0;
}

int cmd_fuzz(const std::string& dir, unsigned iters, unsigned seed) {
  auto cases = load_manifest(dir);
  if (cases.empty()) return 1;
  std::mt19937_64 rng(seed);
  size_t succeeded = 0, runs = 0;
  for (unsigned it = 0; it < iters; it++) {
    const Case& c = cases[rng() % cases.size()];
    Bytes comp, raw;
    if (!read_file(c.cfile, &comp) || !read_file(c.rfile, &raw)) continue;
    if (comp.empty()) continue;
    unsigned nmut = 1 + static_cast<unsigned>(rng() % 4);
    for (unsigned m = 0; m < nmut && !comp.empty(); m++) {
      switch (rng() % 6) {
        case 0: comp[rng() % comp.size()] ^= static_cast<uint8_t>(1u << (rng() % 8)); break;
        case 1: comp[rng() % comp.size()] = static_cast<uint8_t>(rng()); break;
        case 2: comp.resize(rng() % comp.size()); break;   // 切り詰め
        case 3: comp.insert(comp.begin() + static_cast<long>(rng() % (comp.size() + 1)), static_cast<uint8_t>(rng())); break;
        case 4: if (comp.size() > 1) comp.erase(comp.begin() + static_cast<long>(rng() % comp.size())); break;
        default: { size_t a = rng() % comp.size(), l = std::min<size_t>(comp.size() - a, 1 + rng() % 64); Bytes seg(comp.begin() + static_cast<long>(a), comp.begin() + static_cast<long>(a + l)); comp.insert(comp.begin() + static_cast<long>(rng() % (comp.size() + 1)), seg.begin(), seg.end()); }
      }
    }
    size_t cap = (rng() % 4 == 0) ? static_cast<size_t>(rng() % (raw.size() + 1)) : raw.size() + (rng() % 3) * 100;
    Bytes out(cap + 32, 0x77);
    size_t n = 0;
    int rc = run(c.alg, comp, out.data(), cap, &n);
    runs++;
    if (n > cap) { std::fprintf(stderr, "  出力が容量を超えた: %s n=%zu cap=%zu\n", c.alg.c_str(), n, cap); g_fail++; }
    for (size_t i = cap; i < out.size(); i++) if (out[i] != 0x77) { std::fprintf(stderr, "  容量の外へ書いた: %s\n", c.alg.c_str()); g_fail++; break; }
    if (rc == 0) succeeded++;
  }
  std::printf("decomp fuzz: %zu 回 (成功 %zu / 失敗 %zu)、異常 %d\n", runs, succeeded, runs - succeeded, g_fail);
  return g_fail ? 1 : 0;
}

// 手で作った小さな入力
int cmd_unit() {
  size_t n = 0;
  uint8_t out[4096];
  // ---- LZ4: リテラル 1 + 距離 1 の一致 (重なる) = "aaaaaaa" (1 + 6)
  {
    const uint8_t d[] = {0x12, 'a', 0x01, 0x00, 0x00};   // lit=1, ml=2+4=6, offset=1、続いて token 0 (リテラル 0) で終わり
    int rc = usbmem_lz4_block_decompress(d, sizeof d, out, 0, sizeof out, &n);
    CHECK(rc == 0); CHECK(n == 7); CHECK(std::memcmp(out, "aaaaaaa", 7) == 0);
    // 距離 0 / 履歴より遠い距離は拒否
    const uint8_t bad0[] = {0x12, 'a', 0x00, 0x00, 0x00};
    CHECK(usbmem_lz4_block_decompress(bad0, sizeof bad0, out, 0, sizeof out, &n) == -EIO);
    const uint8_t bad2[] = {0x12, 'a', 0x05, 0x00, 0x00};
    CHECK(usbmem_lz4_block_decompress(bad2, sizeof bad2, out, 0, sizeof out, &n) == -EIO);
    CHECK(usbmem_lz4_block_decompress(d, sizeof d, out, 0, 5, &n) == -ENOSPC);
  }
  // ---- LZO: 先頭のリテラル列 (17 + 5 バイト) + 終端 (0x11 0x00 0x00)
  {
    const uint8_t c[] = {22, 'h', 'e', 'l', 'l', 'o', 0x11, 0x00, 0x00};
    n = sizeof out;
    CHECK(usbmem_lzo1x_decompress(c, sizeof c, out, &n) == 0);
    CHECK(n == 5); CHECK(std::memcmp(out, "hello", 5) == 0);
    // 終端が無い
    n = sizeof out;
    CHECK(usbmem_lzo1x_decompress(c, sizeof c - 3, out, &n) == -EIO);
    // 終端の後ろに余分なバイト
    const uint8_t extra[] = {22, 'h', 'e', 'l', 'l', 'o', 0x11, 0x00, 0x00, 0x00};
    n = sizeof out;
    CHECK(usbmem_lzo1x_decompress(extra, sizeof extra, out, &n) == -EIO);
    // リテラル 5 + M2 の一致 (長さ 3、距離 5 = 先頭へ): 0x40 | (L=0)<<5 | D=(5-1)&7 <<2 → b = 0x40 | (4<<2) = 0x50、H = 0
    const uint8_t m2[] = {22, 'a', 'b', 'c', 'd', 'e', 0x50, 0x00, 0x11, 0x00, 0x00};
    n = sizeof out;
    int rc = usbmem_lzo1x_decompress(m2, sizeof m2, out, &n);
    CHECK(rc == 0); CHECK(n == 8); CHECK(std::memcmp(out, "abcdeabc", 8) == 0);
  }
  // ---- LZO-RLE: 17, 1 の後ろにリテラル 1 バイト ('x') と 0 の連続 (長さ = ((X << 3) | L) + 4 = 12) と終端
  {
    // 先頭 (17, 1) + 先頭リテラル列 (18 + 1 → 1 バイトのリテラル 'x') + RLE (0x18 | L=0, 0xFC | trail=0, 0xFF, X=1 → ((1<<3)|0)+4 = 12) + 終端
    const uint8_t r[] = {17, 1, 18, 'x', 0x18, 0xFC, 0xFF, 0x01, 0x11, 0x00, 0x00};
    n = sizeof out;
    int rc = usbmem_lzo1x_decompress(r, sizeof r, out, &n);
    CHECK(rc == 0); CHECK(n == 13); CHECK(out[0] == 'x');
    bool zeros = true;
    for (int i = 1; i < 13; i++) zeros = zeros && out[i] == 0;
    CHECK(zeros);
  }
  // ---- LZO-RLE: 本物のカーネルの圧縮器 (lzorle1x_1_compress) の出力。先頭 (17, 1)、0 の連続の命令 (0x1f 0xfc 0xff + X) を含む
  {
    static const uint8_t kRle[] = {
        0x11, 0x01, 0x02, 0x41, 0x41, 0x41, 0x41, 0x41, 0x90, 0x00, 0x1f, 0xfc, 0xff, 0xff, 0x1f, 0xfc,
        0xff, 0xff, 0x1e, 0xfc, 0xff, 0x6f, 0x02, 0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x18, 0xff, 0xff, 0x0c,
        0x78, 0x79, 0x7a, 0x2a, 0x08, 0x00, 0x1f, 0xfc, 0xff, 0xff, 0x1f, 0xfc, 0xff, 0xff, 0x1f, 0xfc,
        0xff, 0xff, 0x1f, 0xfc, 0xff, 0xff, 0x1f, 0xfc, 0xff, 0xff, 0x1f, 0xfc, 0xff, 0xff, 0x1f, 0xfc,
        0xff, 0xff, 0x1f, 0xfc, 0xff, 0xff, 0x1f, 0xfc, 0xff, 0xff, 0x1f, 0xfc, 0xff, 0xff, 0x1f, 0xfc,
        0xff, 0xff, 0x1f, 0xfc, 0xff, 0xff, 0x1f, 0xfc, 0xff, 0xff, 0x1f, 0xfc, 0xff, 0xff, 0x1f, 0xfc,
        0xff, 0xff, 0x1f, 0xfc, 0xff, 0xff, 0x1f, 0xfc, 0xff, 0xff, 0x1f, 0xfc, 0xff, 0xff, 0x1f, 0xfc,
        0xff, 0xff, 0x1f, 0xfc, 0xff, 0xff, 0x1f, 0xfc, 0xff, 0xff, 0x1e, 0xfc, 0xff, 0x73, 0x00, 0x03,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x1f, 0xfc, 0xff, 0xff, 0x1f, 0xfc, 0xff, 0xff, 0x1f, 0xfc, 0xff,
        0xff, 0x1f, 0xfc, 0xff, 0xff, 0x1f, 0xfc, 0xff, 0xff, 0x1f, 0xfc, 0xff, 0xff, 0x1f, 0xfc, 0xff,
        0xff, 0x1f, 0xfc, 0xff, 0xff, 0x1f, 0xfc, 0xff, 0xff, 0x1f, 0xfc, 0xff, 0xff, 0x1f, 0xfc, 0xff,
        0xff, 0x1f, 0xfc, 0xff, 0xff, 0x19, 0xfc, 0xff, 0xa8, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x65, 0x6e, 0x64, 0x11,
        0x00, 0x00,
    };
    Bytes expect(10, 'A');
    expect.insert(expect.end(), 5000, 0);
    expect.insert(expect.end(), {'h', 'e', 'l', 'l', 'o'});
    expect.insert(expect.end(), 100, 0);
    for (int i = 0; i < 5; i++) expect.insert(expect.end(), {'x', 'y', 'z'});
    expect.insert(expect.end(), 70000, 0);
    expect.insert(expect.end(), {'e', 'n', 'd'});
    Bytes big(expect.size() + 8, 0xEE);
    n = big.size() - 8;
    int rc = usbmem_lzo1x_decompress(kRle, sizeof kRle, big.data(), &n);
    CHECK(rc == 0); CHECK(n == expect.size());
    CHECK(std::memcmp(big.data(), expect.data(), expect.size()) == 0);
    // 版 0 として読むと (先頭 2 バイトを落とす) RLE の命令は距離 0xbfff の一致になり、履歴が足りず拒否される
    n = big.size() - 8;
    CHECK(usbmem_lzo1x_decompress(kRle + 2, sizeof kRle - 2, big.data(), &n) != 0);
  }
  // ---- Zstandard: RLE ブロック 1 つ (マジック + 単一セグメント・内容サイズ 10 + ブロックヘッダ(last, RLE, size 10) + 1 バイト)
  {
    uint32_t bh = (10u << 3) | (1u << 1) | 1u;
    const uint8_t zr[] = {0x28, 0xB5, 0x2F, 0xFD, 0x20, 0x0A, static_cast<uint8_t>(bh), static_cast<uint8_t>(bh >> 8), static_cast<uint8_t>(bh >> 16), 'q'};
    int rc = usbmem_zstd_decompress(zr, sizeof zr, out, sizeof out, &n);
    CHECK(rc == 0); CHECK(n == 10); CHECK(std::memcmp(out, "qqqqqqqqqq", 10) == 0);
    // 内容サイズの不一致
    uint8_t bad[sizeof zr];
    std::memcpy(bad, zr, sizeof zr);
    bad[5] = 9;
    CHECK(usbmem_zstd_decompress(bad, sizeof bad, out, sizeof out, &n) == -EIO);
    // 容量不足 (入った分だけ書く)
    rc = usbmem_zstd_decompress(zr, sizeof zr, out, 4, &n);
    CHECK(rc == -ENOSPC); CHECK(n == 4);
    // raw ブロック
    uint32_t rh = (3u << 3) | (0u << 1) | 1u;
    const uint8_t zraw[] = {0x28, 0xB5, 0x2F, 0xFD, 0x20, 0x03, static_cast<uint8_t>(rh), static_cast<uint8_t>(rh >> 8), static_cast<uint8_t>(rh >> 16), 'a', 'b', 'c'};
    rc = usbmem_zstd_decompress(zraw, sizeof zraw, out, sizeof out, &n);
    CHECK(rc == 0); CHECK(n == 3); CHECK(std::memcmp(out, "abc", 3) == 0);
    // スキップフレームが先頭にあっても読める
    Bytes sk = {0x50, 0x2A, 0x4D, 0x18, 0x02, 0x00, 0x00, 0x00, 0xDE, 0xAD};
    sk.insert(sk.end(), zraw, zraw + sizeof zraw);
    rc = usbmem_zstd_decompress(sk.data(), sk.size(), out, sizeof out, &n);
    CHECK(rc == 0); CHECK(n == 3);
    // マジックが違う / 空
    CHECK(usbmem_zstd_decompress(zraw + 1, sizeof zraw - 1, out, sizeof out, &n) == -EIO);
    CHECK(usbmem_zstd_decompress(zraw, 0, out, sizeof out, &n) == -EIO);
  }
  // ---- zlib: 空でない入力 / 壊れた入力
  {
    const uint8_t zl[] = {0x78, 0x9c, 0x4b, 0x4c, 0x4a, 0x06, 0x00, 0x02, 0x4d, 0x01, 0x27};   // "abc"
    int rc = usbmem_zlib_decompress(zl, sizeof zl, out, sizeof out, &n);
    CHECK(rc == 0); CHECK(n == 3); CHECK(std::memcmp(out, "abc", 3) == 0);
    CHECK(usbmem_zlib_decompress(zl, sizeof zl, out, 2, &n) == -ENOSPC);
    const uint8_t bad[] = {0x00, 0x01, 0x02};
    CHECK(usbmem_zlib_decompress(bad, sizeof bad, out, sizeof out, &n) == -EIO);
  }
  std::printf("decomp unit: %s\n", g_fail ? "失敗あり" : "OK");
  return g_fail ? 1 : 0;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc >= 2 && std::string(argv[1]) == "--unit") return cmd_unit();
  if (argc >= 5 && std::string(argv[1]) == "--fuzz") return cmd_fuzz(argv[2], static_cast<unsigned>(std::atoi(argv[3])), static_cast<unsigned>(std::atoi(argv[4])));
  if (argc >= 2) return cmd_verify(argv[1]);
  std::fprintf(stderr, "使い方: test_decomp <fixdir> | --fuzz <fixdir> <回数> <シード> | --unit\n");
  return 2;
}
