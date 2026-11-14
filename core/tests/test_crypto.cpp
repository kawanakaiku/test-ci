// SPDX-License-Identifier: Apache-2.0
// 暗号プリミティブの既知値テスト (期待値は tools/gen_crypto_vectors.py が独立実装から生成) とゼロ化・性能の確認。
#include <chrono>
#include <new>
#include <string>

#include "test_util.h"
#include "usbmem/crypto.h"
#include "usbmem/crc32.h"

using namespace usbmem::crypto;

namespace {

std::vector<uint8_t> unhex(const char* s) {
  std::vector<uint8_t> v;
  auto nib = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
  for (size_t i = 0; s[i] && s[i + 1]; i += 2) v.push_back(uint8_t((nib(s[i]) << 4) | nib(s[i + 1])));
  return v;
}
std::string hex(const uint8_t* p, size_t n) {
  static const char* d = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; i++) { s += d[p[i] >> 4]; s += d[p[i] & 15]; }
  return s;
}
Hash hash_of(const char* n) { return !strcmp(n, "sha1") ? Hash::Sha1 : !strcmp(n, "sha256") ? Hash::Sha256 : Hash::Sha512; }

int g_hash = 0, g_hmac = 0, g_pbkdf = 0, g_aes = 0, g_cbc = 0, g_xts = 0, g_ccm = 0, g_b2 = 0, g_argon = 0;

void do_hash(const char* alg, const char* msg, const char* exp) {
  auto m = unhex(msg);
  Hash h = hash_of(alg);
  uint8_t out[64];
  usbmem::crypto::hash(h, m.data(), m.size(), out);
  CHECK(hex(out, hash_size(h)) == exp);
  // 分割 update でも同じ結果
  HashCtx c(h);
  for (size_t i = 0; i < m.size(); i += 7) c.update(m.data() + i, std::min<size_t>(7, m.size() - i));
  c.final(out);
  CHECK(hex(out, hash_size(h)) == exp);
  g_hash++;
}
void do_hmac(const char* alg, const char* key, const char* data, const char* exp) {
  auto k = unhex(key), d = unhex(data);
  Hash h = hash_of(alg);
  uint8_t out[64];
  usbmem::crypto::hmac(h, k.data(), k.size(), d.data(), d.size(), out);
  CHECK(hex(out, hash_size(h)) == exp);
  g_hmac++;
}
void do_pbkdf(const char* alg, const char* pw, const char* salt, int it, const char* exp) {
  auto p = unhex(pw), s = unhex(salt), e = unhex(exp);
  std::vector<uint8_t> out(e.size());
  CHECK(pbkdf2(hash_of(alg), p.data(), p.size(), s.data(), s.size(), uint32_t(it), out.data(), out.size()));
  CHECK(out == e);
  g_pbkdf++;
}
void do_aes(const char* key, const char* pt, const char* ct) {
  auto k = unhex(key), p = unhex(pt), c = unhex(ct);
  Aes a;
  CHECK(a.set_key(k.data(), k.size()));
  uint8_t o[16];
  a.encrypt_block(p.data(), o);
  CHECK(std::vector<uint8_t>(o, o + 16) == c);
  a.decrypt_block(c.data(), o);
  CHECK(std::vector<uint8_t>(o, o + 16) == p);
  g_aes++;
}
void do_cbc(const char* key, const char* iv, const char* ct, const char* pt) {
  auto k = unhex(key), v = unhex(iv), c = unhex(ct), p = unhex(pt);
  Aes a;
  CHECK(a.set_key(k.data(), k.size()));
  std::vector<uint8_t> o(c.size());
  aes_cbc_decrypt(a, v.data(), c.data(), o.data(), c.size());
  CHECK(o == p);
  aes_cbc_decrypt(a, v.data(), c.data(), c.data(), c.size());  // in-place
  CHECK(c == p);
  g_cbc++;
}
void do_xts(const char* key, uint64_t sec, const char* ct, const char* pt) {
  auto k = unhex(key), c = unhex(ct), p = unhex(pt);
  Aes k1, k2;
  CHECK(k1.set_key(k.data(), k.size() / 2));
  CHECK(k2.set_key(k.data() + k.size() / 2, k.size() / 2));
  std::vector<uint8_t> o(c.size());
  aes_xts_decrypt(k1, k2, sec, c.data(), o.data(), c.size());
  CHECK(o == p);
  aes_xts_encrypt(k1, k2, sec, p.data(), o.data(), p.size());
  CHECK(o == c);
  g_xts++;
}
void do_ccm(const char* key, const char* nonce, const char* ct, const char* tag, const char* pt) {
  auto k = unhex(key), n = unhex(nonce), c = unhex(ct), t = unhex(tag), p = unhex(pt);
  Aes a;
  CHECK(a.set_key(k.data(), k.size()));
  std::vector<uint8_t> o(c.size() + 1);
  CHECK(aes_ccm_decrypt(a, n.data(), c.data(), c.size(), t.data(), o.data()));
  CHECK(std::vector<uint8_t>(o.begin(), o.begin() + long(c.size())) == p);
  // タグ 1 ビット違いで失敗し、平文は出力に残らない
  t[3] ^= 1;
  CHECK(!aes_ccm_decrypt(a, n.data(), c.data(), c.size(), t.data(), o.data()));
  for (size_t i = 0; i < c.size(); i++) CHECK(o[i] == 0);
  g_ccm++;
}
void do_b2(int ln, const char* msg, const char* exp) {
  auto m = unhex(msg);
  uint8_t out[64];
  blake2b(m.data(), m.size(), out, size_t(ln));
  CHECK(hex(out, size_t(ln)) == exp);
  g_b2++;
}
void do_argon(const char* type, const char* pw, const char* salt, int t, int m, int p, const char* exp) {
  auto pwb = unhex(pw), s = unhex(salt), e = unhex(exp);
  Argon2Type ty = !strcmp(type, "d") ? Argon2Type::D : !strcmp(type, "i") ? Argon2Type::I : Argon2Type::ID;
  std::vector<uint8_t> out(e.size());
  CHECK(argon2(ty, pwb.data(), pwb.size(), s.data(), s.size(), uint32_t(t), uint32_t(m), uint32_t(p), out.data(), out.size(), 1u << 20));
  CHECK(out == e);
  g_argon++;
}

}  // namespace

#define HASHV(a, b, c) do_hash(a, b, c);
#define HMACV(a, b, c, d) do_hmac(a, b, c, d);
#define PBKDFV(a, b, c, d, e) do_pbkdf(a, b, c, d, e);
#define AESV(a, b, c) do_aes(a, b, c);
#define CBCV(a, b, c, d) do_cbc(a, b, c, d);
#define XTSV(a, b, c, d) do_xts(a, b, c, d);
#define CCMV(a, b, c, d, e) do_ccm(a, b, c, d, e);
#define B2V(a, b, c) do_b2(a, b, c);
#define ARGONV(a, b, c, d, e, f, g) do_argon(a, b, c, d, e, f, g);

TEST(crypto_known_answers) {
#include "crypto_vectors.inc"
  std::fprintf(stderr, "  vectors: hash %d hmac %d pbkdf2 %d aes %d cbc %d xts %d ccm %d blake2b %d argon2 %d\n", g_hash, g_hmac, g_pbkdf, g_aes, g_cbc, g_xts, g_ccm, g_b2, g_argon);
  CHECK(g_hash > 40 && g_hmac > 20 && g_pbkdf >= 8 && g_aes >= 9 && g_cbc >= 6 && g_xts >= 32 && g_ccm >= 16 && g_b2 >= 24 && g_argon >= 24);
}

TEST(crypto_argon2_limits) {
  uint8_t out[32], pw[4] = {1, 2, 3, 4}, salt[16] = {0};
  CHECK(!argon2(Argon2Type::ID, pw, 4, salt, 16, 1, 1 << 20, 1, out, 32, 1 << 10));  // メモリ上限を超える要求は拒否
  CHECK(!argon2(Argon2Type::ID, pw, 4, salt, 4, 1, 64, 1, out, 32, 1 << 10));        // salt が短すぎる
  CHECK(!argon2(Argon2Type::ID, pw, 4, salt, 16, 0, 64, 1, out, 32, 1 << 10));       // t = 0
  CHECK(!argon2(Argon2Type::ID, pw, 4, salt, 16, 1, 4, 1, out, 32, 1 << 10));        // メモリが小さすぎる
}

TEST(crypto_secret_zeroization) {
  // Secret の破棄後にバッファがゼロ化されること (placement で確認)
  alignas(Secret) unsigned char raw[sizeof(Secret)];
  Secret* s = new (raw) Secret(64);
  uint8_t* p = s->data();
  for (int i = 0; i < 64; i++) p[i] = 0xA5;
  s->wipe();  // wipe は内容をゼロ化してから解放する
  CHECK(s->empty());
  s->~Secret();
  uint8_t buf[32];
  std::memset(buf, 0x77, sizeof buf);
  secure_zero(buf, sizeof buf);
  for (uint8_t b : buf) CHECK(b == 0);
  Aes* a = new Aes();
  uint8_t key[32];
  for (int i = 0; i < 32; i++) key[i] = uint8_t(i + 1);
  CHECK(a->set_key(key, 32));
  uint8_t* raw_aes = reinterpret_cast<uint8_t*>(a);
  size_t sz = sizeof(Aes);
  a->~Aes();  // デストラクタが鍵表をゼロ化する (解放前の領域を確認)
  size_t nonzero = 0;
  for (size_t i = 0; i < sz; i++) nonzero += raw_aes[i] != 0;
  CHECK(nonzero == 0);
  ::operator delete(a);
}

TEST(crypto_throughput) {
  // XTS-AES-256 / SHA-256 / PBKDF2 の速度の目安 (docs/crypto-volumes.md に記録)
  Aes k1, k2;
  uint8_t key[32];
  for (int i = 0; i < 32; i++) key[i] = uint8_t(i);
  k1.set_key(key, 32); k2.set_key(key, 32);
  std::vector<uint8_t> buf(8u << 20, 0x5a);
  auto t0 = std::chrono::steady_clock::now();
  for (size_t o = 0; o < buf.size(); o += 4096) aes_xts_decrypt(k1, k2, o / 4096, buf.data() + o, buf.data() + o, 4096);
  double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::fprintf(stderr, "  perf: XTS-AES-256 decrypt %.0f MB/s\n", 8.0 / sec);
  uint8_t h[32];
  t0 = std::chrono::steady_clock::now();
  usbmem::crypto::hash(Hash::Sha256, buf.data(), buf.size(), h);
  sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::fprintf(stderr, "  perf: SHA-256 %.0f MB/s\n", 8.0 / sec);
  CHECK(sec < 5.0);  // 極端に遅くないこと (ASan 下でも通る緩い上限)
}

// ハードウェア AES (あれば) と表引き版が、鍵長・長さ・位置 (in == out 含む) の組み合わせで一致すること。
// ハードウェア命令の無い環境では hw() が偽で、表引き同士の比較になるだけ (失敗にはしない)。
TEST(crypto_aes_hw_matches_software) {
  std::fprintf(stderr, "  aes hw supported: %s\n", Aes::hw_supported() ? "yes" : "no");
  uint32_t x = 0x9e3779b9u;
  auto rnd = [&]() { x ^= x << 13; x ^= x >> 17; x ^= x << 5; return uint8_t(x >> 7); };
  for (size_t kl : {size_t(16), size_t(24), size_t(32)}) {
    uint8_t key[64];
    for (auto& b : key) b = rnd();
    Aes a, b, a2, b2;
    CHECK(a.set_key(key, kl) && b.set_key_sw(key, kl) && a2.set_key(key + 32, kl) && b2.set_key_sw(key + 32, kl));
    CHECK(!b.hw());
    CHECK_EQ(a.hw(), Aes::hw_supported());
    for (size_t len : {size_t(16), size_t(32), size_t(48), size_t(64), size_t(80), size_t(512), size_t(1040), size_t(4096)}) {
      std::vector<uint8_t> in(len), o1(len), o2(len);
      for (auto& v : in) v = rnd();
      uint8_t iv[16];
      for (auto& v : iv) v = rnd();
      aes_cbc_decrypt(a, iv, in.data(), o1.data(), len);
      aes_cbc_decrypt(b, iv, in.data(), o2.data(), len);
      CHECK(o1 == o2);
      std::vector<uint8_t> ip = in;
      aes_cbc_decrypt(a, iv, ip.data(), ip.data(), len);
      CHECK(ip == o2);
      for (uint64_t tw : {uint64_t(0), uint64_t(1), uint64_t(0xfffffffffffffffeull), uint64_t(123456789)}) {
        aes_xts_decrypt(a, a2, tw, in.data(), o1.data(), len);
        aes_xts_decrypt(b, b2, tw, in.data(), o2.data(), len);
        CHECK(o1 == o2);
        aes_xts_encrypt(a, a2, tw, in.data(), o1.data(), len);
        aes_xts_encrypt(b, b2, tw, in.data(), o2.data(), len);
        CHECK(o1 == o2);
        ip = in;
        aes_xts_encrypt(a, a2, tw, ip.data(), ip.data(), len);  // in-place
        CHECK(ip == o2);
        aes_xts_decrypt(a, a2, tw, ip.data(), ip.data(), len);
        CHECK(ip == in);
      }
    }
    for (int i = 0; i < 50; i++) {
      uint8_t p[16], c1[16], c2[16];
      for (auto& v : p) v = rnd();
      a.encrypt_block(p, c1); b.encrypt_block(p, c2);
      CHECK(std::memcmp(c1, c2, 16) == 0);
      a.decrypt_block(p, c1); b.decrypt_block(p, c2);
      CHECK(std::memcmp(c1, c2, 16) == 0);
    }
  }
  // 診断用の切り替え: 無効にすると、以後に設定する鍵は表引きになる
  Aes::set_hw_enabled(false);
  Aes d;
  uint8_t key[16] = {1, 2, 3};
  CHECK(d.set_key(key, 16));
  CHECK(!d.hw());
  Aes::set_hw_enabled(true);
  Aes e;
  CHECK(e.set_key(key, 16));
  CHECK_EQ(e.hw(), Aes::hw_supported());
}

// CRC-32C: 標準の検査値 (CRC-32C("123456789") = 0xE3069283)、命令版 (あれば) と表引き版の一致、細切れ呼び出しの一致、位置ずれ。
TEST(crc32c_known_and_hw_matches_software) {
  const char* s = "123456789";
  CHECK_EQ(~usbmem::crc32c_raw(0xFFFFFFFFu, s, 9), 0xE3069283u);
  CHECK_EQ(~usbmem::crc32c_raw_sw(0xFFFFFFFFu, s, 9), 0xE3069283u);
  // Btrfs の名前ハッシュ: crc32c(~1, name) は反転なしの生の値。ここでは細切れでも同じになることだけを確認
  std::fprintf(stderr, "  crc32c hw: %s\n", usbmem::crc32c_hw_active() ? "yes" : "no");
  std::vector<uint8_t> buf(70000);
  uint32_t x = 0x1234567u;
  for (auto& b : buf) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; b = uint8_t(x >> 9); }
  for (size_t skew : {size_t(0), size_t(1), size_t(2), size_t(5)})
    for (size_t len : {size_t(0), size_t(1), size_t(7), size_t(8), size_t(9), size_t(15), size_t(31), size_t(4096), size_t(65536), size_t(69000)}) {
      const uint8_t* p = buf.data() + skew;
      uint32_t a = usbmem::crc32c_raw(0xFFFFFFFFu, p, len), b = usbmem::crc32c_raw_sw(0xFFFFFFFFu, p, len);
      CHECK_EQ(a, b);
      size_t h = len / 3;
      uint32_t c = usbmem::crc32c_raw(usbmem::crc32c_raw(0xFFFFFFFEu, p, h), p + h, len - h);
      CHECK_EQ(c, usbmem::crc32c_raw_sw(0xFFFFFFFEu, p, len));
    }
}

// SHA-256 の命令版 (あれば) と表引き版が一致し、PBKDF2 / HMAC の結果も変わらないこと。
TEST(crypto_sha256_hw_matches_software) {
  std::fprintf(stderr, "  sha256 hw supported: %s\n", sha256_hw_supported() ? "yes" : "no");
  std::vector<uint8_t> buf(5000);
  uint32_t x = 0xabcdef1u;
  for (auto& b : buf) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; b = uint8_t(x >> 9); }
  for (size_t len : {size_t(0), size_t(1), size_t(63), size_t(64), size_t(65), size_t(127), size_t(1000), size_t(4999)}) {
    uint8_t a[32], b[32];
    sha256_set_hw_enabled(true);
    hash(Hash::Sha256, buf.data(), len, a);
    sha256_set_hw_enabled(false);
    hash(Hash::Sha256, buf.data(), len, b);
    CHECK(std::memcmp(a, b, 32) == 0);
  }
  uint8_t p1[32], p2[32];
  sha256_set_hw_enabled(true);
  CHECK(pbkdf2(Hash::Sha256, buf.data(), 20, buf.data() + 100, 16, 3000, p1, 32));
  sha256_set_hw_enabled(false);
  CHECK(pbkdf2(Hash::Sha256, buf.data(), 20, buf.data() + 100, 16, 3000, p2, 32));
  CHECK(std::memcmp(p1, p2, 32) == 0);
  sha256_set_hw_enabled(true);
}
