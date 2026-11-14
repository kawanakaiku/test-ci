// SPDX-License-Identifier: Apache-2.0
// 暗号プリミティブ (BitLocker / LUKS の読み取り復号用)。外部ライブラリに依存しない小さな自前実装 (docs/crypto-volumes.md)。
// 正しさは tests/crypto_vectors.inc (Python cryptography / hashlib / argon2-cffi / openssl の出力) との一致で固めている。
// 秘密データを扱う型は破棄時にゼロ化する (Secret)。AES は表引きの基準実装に加え、CPU が持てば AES-NI / ARMv8 Crypto Extensions を使う
// (起動時の自己テストで表引き版と一致したときだけ。どの ABI でも動く。速度は docs/crypto-volumes.md 6.3 / 6.4)。
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace usbmem {
namespace crypto {

// コンパイラに消されないゼロ化。
void secure_zero(void* p, size_t n);

// 破棄時にゼロ化するバイト列 (コピー不可・ムーブ可)。鍵・パスワード・復号途中のバッファに使う。
class Secret {
 public:
  Secret() = default;
  explicit Secret(size_t n) : v_(n, 0) {}
  Secret(const uint8_t* p, size_t n) : v_(p, p + n) {}
  Secret(const Secret&) = delete;
  Secret& operator=(const Secret&) = delete;
  Secret(Secret&& o) noexcept : v_(std::move(o.v_)) { o.v_.clear(); }
  Secret& operator=(Secret&& o) noexcept {
    if (this != &o) { wipe(); v_ = std::move(o.v_); o.v_.clear(); }
    return *this;
  }
  ~Secret() { wipe(); }
  uint8_t* data() { return v_.data(); }
  const uint8_t* data() const { return v_.data(); }
  size_t size() const { return v_.size(); }
  bool empty() const { return v_.empty(); }
  void resize(size_t n) {  // 縮めるときも、捨てる部分をゼロ化してから (再確保で古い領域が残らないように)
    if (n < v_.size()) secure_zero(v_.data() + n, v_.size() - n);
    if (n > v_.capacity()) { std::vector<uint8_t> nv(n, 0); std::memcpy(nv.data(), v_.data(), v_.size()); wipe(); v_ = std::move(nv); }
    else v_.resize(n, 0);
  }
  void assign(const uint8_t* p, size_t n) { wipe(); v_.assign(p, p + n); }
  void wipe() { if (!v_.empty()) secure_zero(v_.data(), v_.size()); v_.clear(); }

 private:
  std::vector<uint8_t> v_;
};

// ---- ハッシュ / HMAC / PBKDF2
// SHA-256 の圧縮関数は、CPU に SHA 命令 (x86 SHA-NI / ARMv8 SHA2) があり自己テストに通れば命令版を使う。
bool sha256_hw_supported();
void sha256_set_hw_enabled(bool on);  // 診断・比較用 (既定 true)
// SHA-256 の圧縮関数を直接呼ぶ (st = 状態 8 ワード、blocks = 64 バイト × n。パディングは呼び出し側)。
void sha256_compress(uint32_t st[8], const uint8_t* blocks, size_t n);
enum class Hash { Sha1, Sha256, Sha512 };
size_t hash_size(Hash h);
size_t hash_block_size(Hash h);

class HashCtx {
 public:
  explicit HashCtx(Hash h);
  void update(const void* data, size_t n);
  void final(uint8_t* out);  // hash_size(h) バイト。以後 update 不可
  Hash kind() const { return kind_; }
 private:
  void block(const uint8_t* p);
  Hash kind_;
  uint64_t st64_[8];
  uint32_t st32_[8];
  uint8_t buf_[128];
  size_t buf_len_ = 0;
  uint64_t total_ = 0;
};

void hash(Hash h, const void* data, size_t n, uint8_t* out);
void hmac(Hash h, const uint8_t* key, size_t klen, const uint8_t* data, size_t dlen, uint8_t* out);
// 成功で true。iterations==0 や出力長 0 は false。
bool pbkdf2(Hash h, const uint8_t* pw, size_t pwlen, const uint8_t* salt, size_t slen, uint32_t iterations, uint8_t* out, size_t outlen);

// ---- AES (鍵長 16/24/32 バイト)
struct AesHw;
class Aes {
 public:
  Aes() = default;
  ~Aes() { secure_zero(this, sizeof *this); }
  Aes(const Aes&) = delete;
  Aes& operator=(const Aes&) = delete;
  // 不正な鍵長は false。CPU に AES 命令があり自己テストに通っていれば、以後の暗号化・復号はハードウェア命令で行う。
  bool set_key(const uint8_t* key, size_t len);
  bool set_key_sw(const uint8_t* key, size_t len);  // 常に表引き実装 (テスト・ベンチ・自己テスト用)
  bool ready() const { return rounds_ != 0; }
  bool hw() const { return hw_; }  // この鍵がハードウェア命令を使うか
  void clear() { secure_zero(this, sizeof *this); }  // 鍵表をゼロ化 (ready() == false に戻る)
  void encrypt_block(const uint8_t in[16], uint8_t out[16]) const;
  void decrypt_block(const uint8_t in[16], uint8_t out[16]) const;
  // このビルド・CPU でハードウェア命令が使え、起動時の自己テストにも通ったか (set_key が使う条件)
  static bool hw_supported();
  // false にすると、以後の set_key はハードウェア命令を使わない (診断・比較用。設定済みの鍵には影響しない)
  static void set_hw_enabled(bool on);

 private:
  friend struct AesHw;
  enum HwMode { kSwOnly = 0, kAuto = 1, kForce = 2 };
  bool set_key_impl(const uint8_t* key, size_t len, HwMode mode);
  uint32_t ek_[60];
  uint32_t dk_[60];
  int rounds_ = 0;
  bool hw_ = false;
  alignas(16) uint8_t hek_[16 * 15];  // ハードウェア用の暗号側ラウンド鍵 (バイト列)
  alignas(16) uint8_t hdk_[16 * 15];  // 同じく復号側 (等価逆暗号の鍵: 中間は InvMixColumns 済み)
};

// CBC 復号 (len は 16 の倍数)。in == out 可。
void aes_cbc_decrypt(const Aes& k, const uint8_t iv[16], const uint8_t* in, uint8_t* out, size_t len);
// XTS 復号 (IEEE 1619)。データ単位 = len バイト (16 の倍数)。tweak は 128 ビット LE のセクタ番号 (上位 64 ビットは 0)。in == out 可。
void aes_xts_decrypt(const Aes& k1, const Aes& k2, uint64_t tweak, const uint8_t* in, uint8_t* out, size_t len);
void aes_xts_encrypt(const Aes& k1, const Aes& k2, uint64_t tweak, const uint8_t* in, uint8_t* out, size_t len);  // テスト用
// CCM 復号 (ノンス 12 バイト、タグ 16 バイト、追加データなし)。タグ不一致は false (out は消す)。
bool aes_ccm_decrypt(const Aes& k, const uint8_t nonce[12], const uint8_t* ct, size_t len, const uint8_t tag[16], uint8_t* out);

// ---- Argon2 (RFC 9106, version 0x13)
enum class Argon2Type { D = 0, I = 1, ID = 2 };
// memory_kib 上限を超える要求は false (呼び出し側が上限を決める)。成功で true。
bool argon2(Argon2Type type, const uint8_t* pw, size_t pwlen, const uint8_t* salt, size_t slen, uint32_t t_cost, uint32_t memory_kib,
            uint32_t lanes, uint8_t* out, size_t outlen, uint32_t max_memory_kib);

// BLAKE2b (Argon2 用。key なし)
void blake2b(const uint8_t* in, size_t inlen, uint8_t* out, size_t outlen);

}  // namespace crypto
}  // namespace usbmem
