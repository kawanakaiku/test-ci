// SPDX-License-Identifier: Apache-2.0
// 暗号プリミティブの既知値テスト (期待値は tools/gen_crypto_vectors.py が独立実装から生成) とゼロ化・性能の確認。
#include <chrono>
#include <new>
#include <string>

#include "test_util.h"
#include "usbmem/crypto.h"

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
