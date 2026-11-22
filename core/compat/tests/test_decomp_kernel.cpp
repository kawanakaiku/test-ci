// SPDX-License-Identifier: GPL-2.0-or-later
// 展開器の差分試験 (個人版だけ。docs/decomp.md): 自前の展開器 (core/src/decomp*.cpp) と、カーネルの展開ライブラリ (usbmem_kdecomp) の出力を突き合わせる。
//   test_decomp_kernel <fixdir>                  tools/gen_decomp_fixtures.py の manifest.txt の lzo / lz4 / zstd の全項目: 両方が元データへ展開すること
//   test_decomp_kernel --fuzz <fixdir> <回数> <seed>   壊した入力を両方に通して、両方が成功したときは出力が一致すること (片方だけ成功するのは許す: 検査の厳しさが違ってよい)
// 受け入れの厳しさの違いは数えて表示する (自前だけ失敗 / カーネルだけ失敗)。出力が食い違ったら失敗。
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "usbmem/decomp.h"

extern "C" {
int kref_lzo(const void* src, size_t clen, void* dst, size_t* dstlen);
int kref_lz4(const void* src, size_t clen, void* dst, size_t cap, size_t* dstlen);
int kref_zstd(const void* src, size_t clen, void* dst, size_t cap, size_t* dstlen);
}

namespace {
using Bytes = std::vector<uint8_t>;
struct Case { std::string alg, cfile, rfile; };

bool read_file(const std::string& p, Bytes* out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return true;
}
std::vector<Case> load(const std::string& dir) {
  std::vector<Case> v;
  std::ifstream f(dir + "/manifest.txt");
  std::string line;
  while (std::getline(f, line)) {
    std::istringstream is(line);
    Case c;
    if ((is >> c.alg >> c.cfile >> c.rfile) && (c.alg == "lzo" || c.alg == "lz4" || c.alg == "zstd")) { c.cfile = dir + "/" + c.cfile; c.rfile = dir + "/" + c.rfile; v.push_back(c); }
  }
  return v;
}
// 自前 / カーネルで 1 回ずつ展開する。戻り値は成功か
bool ours(const Case& c, const Bytes& comp, uint8_t* dst, size_t cap, size_t* n) {
  *n = 0;
  int rc;
  if (c.alg == "lzo") { *n = cap; rc = usbmem_lzo1x_decompress(comp.data(), comp.size(), dst, n); }
  else if (c.alg == "lz4") rc = usbmem_lz4_block_decompress(comp.data(), comp.size(), dst, 0, cap, n);
  else rc = usbmem_zstd_decompress(comp.data(), comp.size(), dst, cap, n);
  return rc == 0;
}
bool kernel(const Case& c, const Bytes& comp, uint8_t* dst, size_t cap, size_t* n) {
  *n = 0;
  int rc;
  if (c.alg == "lzo") { *n = cap; rc = kref_lzo(comp.data(), comp.size(), dst, n); }
  else if (c.alg == "lz4") rc = kref_lz4(comp.data(), comp.size(), dst, cap, n);
  else rc = kref_zstd(comp.data(), comp.size(), dst, cap, n);
  return rc == 0;
}

int cmd_verify(const std::string& dir) {
  auto cases = load(dir);
  if (cases.empty()) { std::fprintf(stderr, "manifest.txt に lzo / lz4 / zstd が無い: %s\n", dir.c_str()); return 1; }
  int fail = 0;
  for (const Case& c : cases) {
    Bytes comp, raw;
    if (!read_file(c.cfile, &comp) || !read_file(c.rfile, &raw)) { std::fprintf(stderr, "読めない: %s\n", c.cfile.c_str()); fail++; continue; }
    Bytes a(raw.size() + 64, 0), b(raw.size() + 64, 0);
    size_t na = 0, nb = 0;
    bool oa = ours(c, comp, a.data(), raw.size(), &na), ob = kernel(c, comp, b.data(), raw.size(), &nb);
    if (!oa || !ob || na != raw.size() || nb != raw.size() || (!raw.empty() && (std::memcmp(a.data(), raw.data(), raw.size()) || std::memcmp(b.data(), raw.data(), raw.size())))) {
      std::fprintf(stderr, "FAIL %s %s: 自前 %s / カーネル %s\n", c.alg.c_str(), c.cfile.c_str(), oa ? "ok" : "失敗", ob ? "ok" : "失敗");
      fail++;
    }
  }
  std::printf("decomp vs kernel: %zu 件、失敗 %d\n", cases.size(), fail);
  return fail ? 1 : 0;
}

int cmd_fuzz(const std::string& dir, unsigned iters, unsigned seed) {
  auto cases = load(dir);
  if (cases.empty()) return 1;
  std::mt19937_64 rng(seed);
  size_t both = 0, oursonly_fail = 0, kernelonly_fail = 0, bothfail = 0;
  int fail = 0;
  for (unsigned it = 0; it < iters; it++) {
    const Case& c = cases[rng() % cases.size()];
    Bytes comp, raw;
    if (!read_file(c.cfile, &comp) || !read_file(c.rfile, &raw) || comp.empty()) continue;
    unsigned nm = static_cast<unsigned>(rng() % 4);
    for (unsigned m = 0; m < nm && !comp.empty(); m++) {
      switch (rng() % 4) {
        case 0: comp[rng() % comp.size()] ^= static_cast<uint8_t>(1u << (rng() % 8)); break;
        case 1: comp[rng() % comp.size()] = static_cast<uint8_t>(rng()); break;
        case 2: comp.resize(rng() % comp.size() + 1); break;
        default: comp.insert(comp.begin() + static_cast<long>(rng() % (comp.size() + 1)), static_cast<uint8_t>(rng()));
      }
    }
    const size_t cap = raw.size() + (rng() % 2 ? 100000 : 100);
    Bytes a(cap + 64, 0), b(cap + 64, 0);
    size_t na = 0, nb = 0;
    bool oa = ours(c, comp, a.data(), cap, &na), ob = kernel(c, comp, b.data(), cap, &nb);
    if (oa && ob) {
      both++;
      if (na != nb || (na && std::memcmp(a.data(), b.data(), na))) { std::fprintf(stderr, "FAIL 出力が違う: %s %s (変異 %u)\n", c.alg.c_str(), c.cfile.c_str(), nm); fail++; }
    } else if (!oa && ob) oursonly_fail++;
    else if (oa && !ob) kernelonly_fail++;
    else bothfail++;
  }
  std::printf("decomp vs kernel fuzz: 両方成功 %zu (出力の違い %d)、両方失敗 %zu、自前だけ失敗 %zu、カーネルだけ失敗 %zu\n", both, fail, bothfail, oursonly_fail, kernelonly_fail);
  return fail ? 1 : 0;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc >= 5 && std::string(argv[1]) == "--fuzz") return cmd_fuzz(argv[2], static_cast<unsigned>(std::atoi(argv[3])), static_cast<unsigned>(std::atoi(argv[4])));
  if (argc == 2) return cmd_verify(argv[1]);
  std::fprintf(stderr, "使い方: %s <fixdir> | --fuzz <fixdir> <回数> <seed>\n", argv[0]);
  return 2;
}
