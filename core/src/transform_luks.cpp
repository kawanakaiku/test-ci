// SPDX-License-Identifier: Apache-2.0
// LUKS1 / LUKS2 の読み取り専用復号 (dm-crypt 互換のデータ層)。仕様: cryptsetup の docs/on-disk-format.pdf, docs/on-disk-format-luks2.pdf。
//
//  ヘッダ (LUKS1: 592 バイト固定 / LUKS2: 4096 バイトのバイナリ + JSON、2 コピー・チェックサム) を読み、
//  keyslot ごとに: パスフレーズ → KDF (PBKDF2 / argon2i / argon2id) → keyslot 領域を復号 → AF-merge → ボリュームキー候補
//  → ダイジェスト (PBKDF2) と一致すれば成功。データ層は aes-xts-plain64 を優先し、aes-cbc-{essiv:sha256,plain64,plain} も扱う。
//  serpent / twofish、dm-integrity、再暗号化途中、分離ヘッダは明示的に拒否する (黙って誤動作しない)。
#include <algorithm>
#include <cstring>
#include <memory>

#include "mini_json.h"
#include "transform_internal.h"

namespace usbmem {
namespace tf {
namespace {

using crypto::Aes;
using crypto::Hash;
using crypto::Secret;

const uint8_t kMagic1[6] = {'L', 'U', 'K', 'S', 0xba, 0xbe};
const uint8_t kMagic2[6] = {'S', 'K', 'U', 'L', 0xba, 0xbe};
constexpr size_t kLuks1HeaderSize = 592;
constexpr uint32_t kLuks1KeyEnabled = 0x00AC71F3;
constexpr size_t kLuks2BinSize = 4096;
constexpr uint64_t kLuks2MaxHdr = 4u << 20;
constexpr uint32_t kMaxKeyBytes = 128;
constexpr uint32_t kMaxStripes = 10000;

bool hash_by_name(const std::string& n, Hash* h) {
  if (n == "sha1") { *h = Hash::Sha1; return true; }
  if (n == "sha256") { *h = Hash::Sha256; return true; }
  if (n == "sha512") { *h = Hash::Sha512; return true; }
  return false;
}

std::string cstr(const uint8_t* p, size_t max) {
  size_t n = 0;
  while (n < max && p[n]) n++;
  return std::string(reinterpret_cast<const char*>(p), n);
}

bool ct_equal(const uint8_t* a, const uint8_t* b, size_t n) {
  uint8_t d = 0;
  for (size_t i = 0; i < n; i++) d |= uint8_t(a[i] ^ b[i]);
  return d == 0;
}

// ---- データ層 / keyslot 領域の暗号 (dm-crypt の cipher-mode-iv)
struct CryptCipher {
  enum Mode { None, XtsPlain64, XtsPlain, CbcPlain64, CbcPlain, CbcEssiv } mode = None;
  Aes k1, k2;
  uint32_t sector = 512;

  // "aes" + "xts-plain64" のような組。unsupported なら理由を why に入れて false。
  static bool parse(const std::string& cipher, const std::string& iv_mode, Mode* m, std::string* why) {
    if (cipher != "aes") { *why = "未対応の暗号 (" + cipher + "): このアプリは AES のみ対応です (serpent / twofish は非対応)"; return false; }
    if (iv_mode == "xts-plain64") *m = XtsPlain64;
    else if (iv_mode == "xts-plain") *m = XtsPlain;
    else if (iv_mode == "cbc-plain64") *m = CbcPlain64;
    else if (iv_mode == "cbc-plain") *m = CbcPlain;
    else if (iv_mode == "cbc-essiv:sha256") *m = CbcEssiv;
    else { *why = "未対応の暗号モード (" + iv_mode + "): xts-plain64 / cbc-essiv:sha256 / cbc-plain64 のみ対応です"; return false; }
    return true;
  }
  static bool xts(Mode m) { return m == XtsPlain64 || m == XtsPlain; }
  static bool key_size_ok(Mode m, size_t n) { return xts(m) ? (n == 32 || n == 64) : (n == 16 || n == 24 || n == 32); }

  bool setup(Mode m, const uint8_t* key, size_t n, uint32_t sector_size) {
    mode = m;
    sector = sector_size;
    if (!key_size_ok(m, n)) return false;
    if (xts(m)) return k1.set_key(key, n / 2) && k2.set_key(key + n / 2, n / 2);
    if (!k1.set_key(key, n)) return false;
    if (m == CbcEssiv) {
      uint8_t h[32];
      crypto::hash(Hash::Sha256, key, n, h);
      bool ok = k2.set_key(h, 32);
      crypto::secure_zero(h, sizeof h);
      return ok;
    }
    return true;
  }
  void clear() { k1.clear(); k2.clear(); }

  // iv_sector: 512 バイト単位のセクタ番号 (dm-crypt は sector_size > 512 でも 512 単位で数える)
  void decrypt_sector(uint64_t iv_sector, const uint8_t* in, uint8_t* out) const {
    switch (mode) {
      case XtsPlain64: crypto::aes_xts_decrypt(k1, k2, iv_sector, in, out, sector); break;
      case XtsPlain: crypto::aes_xts_decrypt(k1, k2, iv_sector & 0xffffffffu, in, out, sector); break;
      case CbcPlain64:
      case CbcPlain: {
        uint8_t iv[16] = {0};
        put_le64(iv, mode == CbcPlain ? (iv_sector & 0xffffffffu) : iv_sector);
        crypto::aes_cbc_decrypt(k1, iv, in, out, sector);
        break;
      }
      case CbcEssiv: {
        uint8_t blk[16] = {0}, iv[16];
        put_le64(blk, iv_sector);
        k2.encrypt_block(blk, iv);
        crypto::aes_cbc_decrypt(k1, iv, in, out, sector);
        break;
      }
      default: break;
    }
  }
  // 暗号化 (書き込み用)。IV の作り方は復号と同じ (dm-crypt の cipher-mode-iv)。in == out 可。
  void encrypt_sector(uint64_t iv_sector, const uint8_t* in, uint8_t* out) const {
    switch (mode) {
      case XtsPlain64: crypto::aes_xts_encrypt(k1, k2, iv_sector, in, out, sector); break;
      case XtsPlain: crypto::aes_xts_encrypt(k1, k2, iv_sector & 0xffffffffu, in, out, sector); break;
      case CbcPlain64:
      case CbcPlain: {
        uint8_t iv[16] = {0};
        put_le64(iv, mode == CbcPlain ? (iv_sector & 0xffffffffu) : iv_sector);
        crypto::aes_cbc_encrypt(k1, iv, in, out, sector);
        break;
      }
      case CbcEssiv: {
        uint8_t blk[16] = {0}, iv[16];
        put_le64(blk, iv_sector);
        k2.encrypt_block(blk, iv);
        crypto::aes_cbc_encrypt(k1, iv, in, out, sector);
        break;
      }
      default: break;
    }
  }
  void encrypt_range(uint64_t first, uint8_t* buf, size_t len) const {
    for (size_t o = 0; o < len; o += sector) encrypt_sector(first + o / 512, buf + o, buf + o);
  }
  // 書き込みに対応する方式か。32 ビット IV の plain 方式 (xts-plain / cbc-plain) は実カーネルでの照合をしていないので書かない。
  static bool write_ok(Mode m) { return m == XtsPlain64 || m == CbcPlain64 || m == CbcEssiv; }
  // buf (len は sector の倍数) を、先頭の 512 単位セクタ番号 first から順に復号
  void decrypt_range(uint64_t first, uint8_t* buf, size_t len) const {
    for (size_t o = 0; o < len; o += sector) decrypt_sector(first + o / 512, buf + o, buf + o);
  }
};

// ---- AF (anti-forensic) 合成: LUKS1 / LUKS2 共通
void af_diffuse(Hash h, uint8_t* buf, size_t size) {
  const size_t ds = crypto::hash_size(h);
  const size_t blocks = size / ds, pad = size % ds;
  uint8_t out[64], iv[4];
  for (size_t i = 0; i < blocks + (pad ? 1 : 0); i++) {
    const size_t n = i < blocks ? ds : pad;
    iv[0] = uint8_t(i >> 24); iv[1] = uint8_t(i >> 16); iv[2] = uint8_t(i >> 8); iv[3] = uint8_t(i);
    crypto::HashCtx c(h);
    c.update(iv, 4);
    c.update(buf + i * ds, n);
    c.final(out);
    std::memcpy(buf + i * ds, out, n);
  }
}
void af_merge(Hash h, const uint8_t* src, uint8_t* dst, size_t block, size_t n) {
  std::vector<uint8_t> acc(block, 0);
  for (size_t i = 0; i + 1 < n; i++) {
    for (size_t j = 0; j < block; j++) acc[j] ^= src[i * block + j];
    af_diffuse(h, acc.data(), block);
  }
  for (size_t j = 0; j < block; j++) dst[j] = uint8_t(acc[j] ^ src[(n - 1) * block + j]);
  crypto::secure_zero(acc.data(), acc.size());
}

// ---- keyslot / digest / volume の共通表現
struct Kdf {
  enum { Pbkdf2, Argon2 } type = Pbkdf2;
  Hash hash = Hash::Sha256;
  uint32_t iterations = 0;
  crypto::Argon2Type atype = crypto::Argon2Type::ID;
  uint32_t time = 0, memory_kib = 0, cpus = 1;
  std::vector<uint8_t> salt;
};
struct Slot {
  int index = 0;
  int priority = 1;  // LUKS2: 0 = ignore, 1 = normal, 2 = high
  Kdf kdf;
  uint32_t key_size = 0;      // ボリュームキー長
  uint32_t area_key_size = 0; // keyslot 領域の鍵長 (KDF 出力長)
  uint32_t stripes = 4000;
  Hash af_hash = Hash::Sha256;
  uint64_t area_offset = 0;
  uint64_t area_size = 0;
  CryptCipher::Mode area_mode = CryptCipher::XtsPlain64;
  int digest = 0;  // digests[] の添字
};
struct Digest {
  Hash hash = Hash::Sha1;
  uint32_t iterations = 0;
  std::vector<uint8_t> salt, value;
};
struct Luks {
  int version = 0;
  std::string uuid;
  uint64_t data_offset = 0;
  uint64_t data_size = 0;   // 0 = 残り全部
  uint32_t sector = 512;
  uint64_t iv_tweak = 0;
  CryptCipher::Mode mode = CryptCipher::None;
  std::string cipher_desc;
  std::vector<Slot> slots;
  std::vector<Digest> digests;
  std::string unsupported;  // 非空ならデータ層を開けない理由
};

// ---- LUKS1
bool parse_luks1(const uint8_t* h, uint64_t dev_size, Luks* l, const OpenOptions& opt, std::string* why) {
  l->version = 1;
  if (be16(h + 6) != 1) { *why = "LUKS1 ヘッダのバージョンが不正です"; return false; }
  std::string cipher = cstr(h + 8, 32), mode = cstr(h + 40, 32), hashspec = cstr(h + 72, 32);
  l->uuid = cstr(h + 168, 40);
  const uint32_t payload = be32(h + 104), key_bytes = be32(h + 108);
  if (key_bytes < 16 || key_bytes > kMaxKeyBytes) { *why = "LUKS1 のキー長が不正です"; return false; }
  if (uint64_t(payload) * 512 > dev_size) { *why = "LUKS1 のペイロード位置がデバイスの外です (切り詰め?)"; return false; }
  l->data_offset = uint64_t(payload) * 512;
  l->data_size = 0;
  l->sector = 512;
  l->cipher_desc = cipher + "-" + mode;
  Hash hh;
  if (!hash_by_name(hashspec, &hh)) l->unsupported = "未対応のハッシュ (" + hashspec + ")";
  std::string cw;
  if (!CryptCipher::parse(cipher, mode, &l->mode, &cw)) l->unsupported = cw;
  else if (!CryptCipher::key_size_ok(l->mode, key_bytes)) l->unsupported = "キー長が暗号モードと合いません";
  Digest d;
  d.hash = hh;
  d.value.assign(h + 112, h + 132);
  d.salt.assign(h + 132, h + 164);
  d.iterations = be32(h + 164);
  if (d.iterations == 0 || d.iterations > opt.max_pbkdf2_iterations) { if (l->unsupported.empty()) l->unsupported = "ダイジェストの反復回数が上限を超えています"; }
  l->digests.push_back(d);
  bool over_cap = false;
  for (int i = 0; i < 8; i++) {
    const uint8_t* s = h + 208 + 48 * i;
    if (be32(s) != kLuks1KeyEnabled) continue;
    Slot sl;
    sl.index = i;
    sl.kdf.type = Kdf::Pbkdf2;
    sl.kdf.hash = hh;
    sl.kdf.iterations = be32(s + 4);
    sl.kdf.salt.assign(s + 8, s + 40);
    sl.key_size = sl.area_key_size = key_bytes;
    sl.stripes = be32(s + 44);
    sl.af_hash = hh;
    sl.area_offset = uint64_t(be32(s + 40)) * 512;
    sl.area_mode = l->mode;
    if (sl.stripes == 0 || sl.stripes > kMaxStripes) continue;
    sl.area_size = (uint64_t(key_bytes) * sl.stripes + 511) / 512 * 512;
    if (!range_ok(dev_size, sl.area_offset, sl.area_size) || sl.kdf.iterations == 0) continue;
    // 壊れた/悪意あるヘッダの反復回数で何分も固まらない (上限超えの keyslot は試さない)
    if (sl.kdf.iterations > opt.max_pbkdf2_iterations) { over_cap = true; continue; }
    l->slots.push_back(std::move(sl));
  }
  if (l->slots.empty()) { *why = over_cap ? "keyslot の PBKDF2 反復回数が上限を超えています (壊れたヘッダ?)" : "有効な keyslot がありません (壊れたヘッダ)"; return false; }
  return true;
}

// ---- LUKS2
const JsonValue* jget(const JsonValue* o, const char* k) { return o ? o->get(k) : nullptr; }
bool jstr(const JsonValue* o, const char* k, std::string* out) {
  const JsonValue* v = jget(o, k);
  if (!v || !v->is_str()) return false;
  *out = v->s;
  return true;
}
bool ju64(const JsonValue* o, const char* k, uint64_t* out) {
  const JsonValue* v = jget(o, k);
  return v && v->as_u64(out);
}

bool parse_luks2_json(const JsonValue& root, uint64_t dev_size, Luks* l, const OpenOptions& opt, std::string* why) {
  l->version = 2;
  if (!root.is_obj()) { *why = "LUKS2 JSON が不正です"; return false; }
  // 要件 (再暗号化途中・OPAL・未知の機能) があれば拒否する
  if (const JsonValue* cfg = root.get("config")) {
    if (const JsonValue* req = cfg->get("requirements")) {
      if (const JsonValue* ra = req->get("mandatory")) {
        if (ra->is_arr() && !ra->arr.empty()) {
          std::string names;
          for (auto& r : ra->arr) if (r.is_str()) { if (!names.empty()) names += ","; names += r.s.substr(0, 40); }
          l->unsupported = "LUKS2 の必須要件 (" + names + ") は非対応です (再暗号化途中のボリュームなど)";
        }
      }
    }
  }
  // segments
  const JsonValue* segs = root.get("segments");
  if (!segs || !segs->is_obj() || segs->obj.empty()) { *why = "LUKS2 に segment がありません"; return false; }
  const JsonValue* seg = nullptr;
  int crypt_count = 0;
  for (auto& kv : segs->obj) {
    std::string t;
    if (jstr(&kv.second, "type", &t) && t == "crypt") { crypt_count++; if (!seg) seg = &kv.second; }
  }
  std::string seg_id = "0";
  if (segs->obj.size() != 1 || crypt_count != 1) {
    if (l->unsupported.empty()) l->unsupported = "複数 segment / 暗号化以外の segment (再暗号化途中など) は非対応です";
  }
  if (!seg) { *why = "LUKS2 に暗号 segment がありません"; return false; }
  for (auto& kv : segs->obj) if (&kv.second == seg) seg_id = kv.first;
  uint64_t off = 0;
  if (!ju64(seg, "offset", &off)) { *why = "LUKS2 segment の offset が不正です"; return false; }
  l->data_offset = off;
  const JsonValue* sz = seg->get("size");
  if (sz && sz->is_str() && sz->s == "dynamic") l->data_size = 0;
  else if (!sz || !sz->as_u64(&l->data_size) || l->data_size == 0) { *why = "LUKS2 segment の size が不正です"; return false; }
  uint64_t ss = 512, ivt = 0;
  if (jget(seg, "sector_size") && !ju64(seg, "sector_size", &ss)) { *why = "LUKS2 の sector_size が不正です"; return false; }
  if (ss != 512 && ss != 1024 && ss != 2048 && ss != 4096) { *why = "LUKS2 の sector_size が不正です"; return false; }
  l->sector = uint32_t(ss);
  if (jget(seg, "iv_tweak") && !ju64(seg, "iv_tweak", &ivt)) { *why = "LUKS2 の iv_tweak が不正です"; return false; }
  l->iv_tweak = ivt;
  if (seg->get("integrity")) { if (l->unsupported.empty()) l->unsupported = "dm-integrity (認証付き暗号) は非対応です"; }
  std::string enc;
  if (!jstr(seg, "encryption", &enc)) { *why = "LUKS2 segment の encryption が不正です"; return false; }
  l->cipher_desc = enc;
  {
    size_t dash = enc.find('-');
    std::string cw;
    if (dash == std::string::npos) { if (l->unsupported.empty()) l->unsupported = "暗号指定が不正です"; }
    else if (!CryptCipher::parse(enc.substr(0, dash), enc.substr(dash + 1), &l->mode, &cw)) { if (l->unsupported.empty()) l->unsupported = cw; }
  }
  if (l->data_offset > dev_size) { *why = "LUKS2 のデータ位置がデバイスの外です (切り詰め?)"; return false; }
  // digests
  const JsonValue* digs = root.get("digests");
  if (!digs || !digs->is_obj()) { *why = "LUKS2 に digest がありません"; return false; }
  std::vector<std::string> digest_ids;
  for (auto& kv : digs->obj) {
    std::string t, hs;
    if (!jstr(&kv.second, "type", &t) || t != "pbkdf2" || !jstr(&kv.second, "hash", &hs)) continue;
    Digest d;
    uint64_t it = 0;
    std::string salt, dig;
    if (!hash_by_name(hs, &d.hash) || !ju64(&kv.second, "iterations", &it) || it == 0 || it > opt.max_pbkdf2_iterations) continue;
    if (!jstr(&kv.second, "salt", &salt) || !jstr(&kv.second, "digest", &dig)) continue;
    if (!base64_decode(salt, &d.salt, 256) || !base64_decode(dig, &d.value, 128) || d.value.empty()) continue;
    d.iterations = uint32_t(it);
    // この digest が当該 segment を指しているものだけ
    bool refers = false;
    if (const JsonValue* sa = kv.second.get("segments")) for (auto& x : sa->arr) if (x.is_str() && x.s == seg_id) refers = true;
    if (!refers) continue;
    l->digests.push_back(std::move(d));
    digest_ids.push_back(kv.first);
  }
  if (l->digests.empty()) { *why = "LUKS2 の使える digest がありません"; return false; }
  // keyslots
  const JsonValue* kss = root.get("keyslots");
  if (!kss || !kss->is_obj()) { *why = "LUKS2 に keyslot がありません"; return false; }
  for (auto& kv : kss->obj) {
    const JsonValue& k = kv.second;
    std::string t;
    if (!jstr(&k, "type", &t) || t != "luks2") continue;  // reencrypt 用などは無視
    Slot s;
    uint64_t v = 0;
    char* endp = nullptr;
    s.index = int(std::strtol(kv.first.c_str(), &endp, 10));
    if (!ju64(&k, "key_size", &v) || v < 16 || v > kMaxKeyBytes) continue;
    s.key_size = uint32_t(v);
    if (const JsonValue* pr = k.get("priority")) { uint64_t p; if (pr->as_u64(&p)) s.priority = int(p); }
    const JsonValue* af = k.get("af");
    std::string aft, afh;
    if (!af || !jstr(af, "type", &aft) || aft != "luks1" || !jstr(af, "hash", &afh) || !hash_by_name(afh, &s.af_hash) || !ju64(af, "stripes", &v) || v == 0 || v > kMaxStripes) continue;
    s.stripes = uint32_t(v);
    const JsonValue* area = k.get("area");
    std::string at, ae;
    if (!area || !jstr(area, "type", &at) || at != "raw" || !jstr(area, "encryption", &ae) || !ju64(area, "offset", &s.area_offset) || !ju64(area, "size", &s.area_size) ||
        !ju64(area, "key_size", &v) || v < 16 || v > kMaxKeyBytes)
      continue;
    s.area_key_size = uint32_t(v);
    {
      size_t dash = ae.find('-');
      std::string cw;
      if (dash == std::string::npos || !CryptCipher::parse(ae.substr(0, dash), ae.substr(dash + 1), &s.area_mode, &cw) || !CryptCipher::key_size_ok(s.area_mode, s.area_key_size)) continue;
    }
    const uint64_t need = (uint64_t(s.key_size) * s.stripes + 511) / 512 * 512;
    if (s.area_size < need || !range_ok(dev_size, s.area_offset, need)) continue;
    s.area_size = need;
    const JsonValue* kdf = k.get("kdf");
    std::string kt, salt;
    if (!kdf || !jstr(kdf, "type", &kt) || !jstr(kdf, "salt", &salt) || !base64_decode(salt, &s.kdf.salt, 256)) continue;
    if (kt == "pbkdf2") {
      std::string hs;
      uint64_t it;
      if (!jstr(kdf, "hash", &hs) || !hash_by_name(hs, &s.kdf.hash) || !ju64(kdf, "iterations", &it) || it == 0 || it > opt.max_pbkdf2_iterations) continue;
      s.kdf.type = Kdf::Pbkdf2;
      s.kdf.iterations = uint32_t(it);
    } else if (kt == "argon2i" || kt == "argon2id") {
      uint64_t tm, mem, cpus;
      if (!ju64(kdf, "time", &tm) || !ju64(kdf, "memory", &mem) || !ju64(kdf, "cpus", &cpus) || tm == 0 || tm > opt.max_argon2_time_cost || cpus == 0 || cpus > 64 ||
          mem < 8 || mem > 0xffffffffull)
        continue;
      s.kdf.type = Kdf::Argon2;
      s.kdf.atype = kt == "argon2i" ? crypto::Argon2Type::I : crypto::Argon2Type::ID;
      s.kdf.time = uint32_t(tm); s.kdf.memory_kib = uint32_t(mem); s.kdf.cpus = uint32_t(cpus);
    } else continue;
    // この keyslot を指す digest
    s.digest = -1;
    for (auto& dk : digs->obj) {
      auto it = std::find(digest_ids.begin(), digest_ids.end(), dk.first);
      if (it == digest_ids.end()) continue;
      if (const JsonValue* ka = dk.second.get("keyslots")) for (auto& x : ka->arr) if (x.is_str() && x.s == kv.first) s.digest = int(it - digest_ids.begin());
      if (s.digest >= 0) break;
    }
    if (s.digest < 0) continue;
    l->slots.push_back(std::move(s));
  }
  if (l->slots.empty()) { *why = "使える keyslot がありません (壊れた/非対応のヘッダ)"; return false; }
  if (!CryptCipher::key_size_ok(l->mode, l->slots[0].key_size) && l->unsupported.empty() && l->mode != CryptCipher::None) l->unsupported = "キー長が暗号モードと合いません";
  return true;
}

// LUKS2 ヘッダ 1 コピーを読んで検証する。成功で *hdr (バイナリ 4096 + JSON) に全体を入れる。
bool read_luks2_copy(BlockDevice& dev, uint64_t at, bool secondary, std::vector<uint8_t>* hdr, uint64_t* seqid, uint64_t* hdr_size_out) {
  uint8_t bin[kLuks2BinSize];
  if (!range_ok(dev.size(), at, sizeof bin) || !read_exact(dev, at, bin, sizeof bin)) return false;
  if (std::memcmp(bin, secondary ? kMagic2 : kMagic1, 6) != 0 || be16(bin + 6) != 2) return false;
  const uint64_t hsz = be64(bin + 8);
  if (hsz < 16384 || hsz > kLuks2MaxHdr || (hsz & (hsz - 1)) != 0) return false;
  if (be64(bin + 256) != at) return false;  // hdr_offset が実際の位置と一致
  if (!range_ok(dev.size(), at, hsz)) return false;
  hdr->resize(size_t(hsz));
  if (!read_exact(dev, at, hdr->data(), hdr->size())) return false;
  std::string alg = cstr(bin + 72, 32);
  Hash ch;
  if (!hash_by_name(alg, &ch)) return false;
  uint8_t stored[64];
  std::memcpy(stored, hdr->data() + 448, 64);
  std::memset(hdr->data() + 448, 0, 64);
  uint8_t calc[64];
  crypto::hash(ch, hdr->data(), hdr->size(), calc);
  std::memcpy(hdr->data() + 448, stored, 64);
  if (!ct_equal(calc, stored, crypto::hash_size(ch))) return false;
  *seqid = be64(bin + 16);
  *hdr_size_out = hsz;
  return true;
}

Status load_luks(BlockDevice& dev, Luks* l, const OpenOptions& opt, std::string* why, bool* is_luks) {
  *is_luks = false;
  uint8_t head[8];
  if (dev.size() < kLuks1HeaderSize) return Status::Make(Errc::Unsupported);
  Status s = read_exact(dev, 0, head, sizeof head);
  if (!s) return s;
  if (std::memcmp(head, kMagic1, 6) != 0) return Status::Make(Errc::Unsupported);
  *is_luks = true;
  const uint16_t ver = be16(head + 6);
  if (ver == 1) {
    uint8_t h[kLuks1HeaderSize];
    s = read_exact(dev, 0, h, sizeof h);
    if (!s) return s;
    if (!parse_luks1(h, dev.size(), l, opt, why)) return Status::Make(Errc::Corrupt);
    return Status::Ok();
  }
  if (ver != 2) { *why = "未対応の LUKS バージョンです"; return Status::Make(Errc::Unsupported); }
  // 2 コピーのうちチェックサムが合うもの。seqid が大きい方を採用
  std::vector<uint8_t> a, b;
  uint64_t sa = 0, sb = 0, ha = 0, hb = 0;
  bool oka = read_luks2_copy(dev, 0, false, &a, &sa, &ha);
  bool okb = false;
  if (oka) okb = read_luks2_copy(dev, ha, true, &b, &sb, &hb);
  else {
    // 先頭が壊れている: 副ヘッダの位置を既知の候補 (16K..4M) から探す
    for (uint64_t cand = 16384; cand <= kLuks2MaxHdr && !okb; cand <<= 1) okb = read_luks2_copy(dev, cand, true, &b, &sb, &hb);
  }
  if (!oka && !okb) { *why = "LUKS2 ヘッダの 2 コピーともチェックサムが合いません (破損または切り詰め)"; return Status::Make(Errc::Corrupt); }
  // JSON を読む: 新しい方を先に試し、パースできなければもう一方
  struct Cand { const std::vector<uint8_t>* h; uint64_t seq; };
  std::vector<Cand> order;
  if (oka) order.push_back({&a, sa});
  if (okb) order.push_back({&b, sb});
  std::sort(order.begin(), order.end(), [](const Cand& x, const Cand& y) { return x.seq > y.seq; });
  for (auto& c : order) {
    const std::vector<uint8_t>& h = *c.h;
    const char* js = reinterpret_cast<const char*>(h.data()) + kLuks2BinSize;
    size_t jl = h.size() - kLuks2BinSize;
    size_t n = 0;
    while (n < jl && js[n]) n++;
    JsonValue root;
    std::string jerr;
    if (!json_parse(js, n, &root, &jerr)) continue;
    Luks tmp;
    tmp.uuid = cstr(h.data() + 168, 40);
    std::string w;
    if (!parse_luks2_json(root, dev.size(), &tmp, opt, &w)) { *why = w; continue; }
    *l = std::move(tmp);
    return Status::Ok();
  }
  if (why->empty()) *why = "LUKS2 の JSON メタデータを解釈できません";
  return Status::Make(Errc::Corrupt);
}

// ---- keyslot の解錠
// 戻り値: Ok = ボリュームキーが得られダイジェストが一致 / AuthFailed = パスフレーズ違い / Unsupported = KDF の資源上限など / Cancelled
Status try_slot(BlockDevice& dev, const Luks& l, const Slot& s, const Secret& pass, const OpenOptions& opt, Secret* mk) {
  if (opt.cancel && opt.cancel->cancelled()) return Status::Make(Errc::Cancelled);
  Secret dk(s.area_key_size);
  if (s.kdf.type == Kdf::Pbkdf2) {
    if (!crypto::pbkdf2(s.kdf.hash, pass.data(), pass.size(), s.kdf.salt.data(), s.kdf.salt.size(), s.kdf.iterations, dk.data(), dk.size()))
      return Status::Make(Errc::Corrupt);
  } else {
    if (!crypto::argon2(s.kdf.atype, pass.data(), pass.size(), s.kdf.salt.data(), s.kdf.salt.size(), s.kdf.time, s.kdf.memory_kib, s.kdf.cpus, dk.data(), dk.size(),
                        opt.max_kdf_memory_kib))
      return Status::Make(Errc::Unsupported);  // メモリ上限超過・確保失敗・パラメータ不正
  }
  if (opt.cancel && opt.cancel->cancelled()) return Status::Make(Errc::Cancelled);
  Secret area(size_t(s.area_size));
  Status st = read_exact(dev, s.area_offset, area.data(), area.size());
  if (!st) return st;
  CryptCipher cc;
  if (!cc.setup(s.area_mode, dk.data(), dk.size(), 512)) return Status::Make(Errc::Corrupt);
  cc.decrypt_range(0, area.data(), area.size());
  cc.clear();
  Secret key(s.key_size);
  af_merge(s.af_hash, area.data(), key.data(), s.key_size, s.stripes);
  const Digest& d = l.digests[size_t(s.digest)];
  std::vector<uint8_t> calc(d.value.size());
  if (!crypto::pbkdf2(d.hash, key.data(), key.size(), d.salt.data(), d.salt.size(), d.iterations, calc.data(), calc.size())) return Status::Make(Errc::Corrupt);
  if (!ct_equal(calc.data(), d.value.data(), calc.size())) return Status::Make(Errc::AuthFailed);
  *mk = std::move(key);
  return Status::Ok();
}

class LuksDevice final : public DecryptingDevice {
 public:
  LuksDevice(BlockDevicePtr base, std::unique_ptr<CryptCipher> c, uint64_t data_off, uint64_t size, uint64_t iv_tweak, uint32_t cache_chunks, std::string desc,
             bool writable)
      : DecryptingDevice(std::move(base), size, c->sector, cache_chunks, std::move(desc)), c_(std::move(c)), data_off_(data_off), iv_tweak_(iv_tweak) {
    set_writable(writable);
  }

 protected:
  Status read_plain(uint64_t off, uint8_t* buf, size_t len, const IoOptions& opt) override {
    Status st = base_->read_at(data_off_ + off, buf, len, opt);
    if (!st) return st;
    c_->decrypt_range(iv_tweak_ + off / 512, buf, len);
    return Status::Ok();
  }
  // 書き込み: ヘッダには触れない。データ領域 [data_off_, data_off_+size) の暗号化したセクタだけを下位へ書く。
  Status write_plain(uint64_t off, const uint8_t* buf, size_t len, const IoOptions& opt) override {
    crypto::Secret enc(buf, len);
#ifdef USBMEM_MUTATE_CW_LUKS_IV
    c_->encrypt_range(iv_tweak_ + off / 512 + 1, enc.data(), len);  // 突然変異: IV が 1 ずれる
#else
    c_->encrypt_range(iv_tweak_ + off / 512, enc.data(), len);
#endif
    return base_->write_at(data_off_ + off, enc.data(), len, opt);
  }
  void wipe_keys() override { c_->clear(); }

 private:
  std::unique_ptr<CryptCipher> c_;
  uint64_t data_off_;
  uint64_t iv_tweak_;
};

void fill_info(const Luks& l, VolumeInfo* info) {
  info->kind = CryptoKind::Luks;
  info->format = l.version == 1 ? "LUKS1" : "LUKS2";
  info->cipher = l.cipher_desc;
  info->supported = l.unsupported.empty() && l.mode != CryptCipher::None;
  info->can_password = true;
  info->can_key_file = true;
  info->note = l.unsupported;
  info->write_note.clear();
  if (!info->supported) { info->write_supported = false; info->write_note = "読み取りでも非対応の構成です"; return; }
  info->write_supported = true;
#ifndef USBMEM_CRYPT_WRITE
  info->write_supported = false;
  info->write_note = "このビルドは暗号ボリュームの書き込みに対応していません (USBMEM_CRYPT_WRITE=OFF)";
#else
  if (!CryptCipher::write_ok(l.mode)) {
    info->write_supported = false;
    info->write_note = "この IV 方式 (32 ビット plain) は書き込みを検証していないため、読み取り専用です";
  }
#endif
}

}  // namespace

Status luks_probe(BlockDevice& dev, VolumeInfo* info) {
  Luks l;
  std::string why;
  bool is_luks = false;
  OpenOptions opt;
  Status s = load_luks(dev, &l, opt, &why, &is_luks);
  if (!is_luks) return s.code == Errc::Unsupported || s.ok() ? Status::Make(Errc::Unsupported) : s;
  info->kind = CryptoKind::Luks;
  info->format = "LUKS";
  if (!s) {
    info->supported = false;
    info->note = why;
    return s.code == Errc::Corrupt ? Status::Ok() : s;  // LUKS と分かった (壊れていても分類は返す)
  }
  fill_info(l, info);
  return Status::Ok();
}

Status luks_open(BlockDevicePtr base, const Credentials& cred, const OpenOptions& opt, BlockDevicePtr* out, VolumeInfo* info) {
  Luks l;
  std::string why;
  bool is_luks = false;
  Status s = load_luks(*base, &l, opt, &why, &is_luks);
  if (!is_luks) { if (info) info->note = "LUKS ではありません"; return s.ok() ? Status::Make(Errc::Unsupported) : s; }
  if (!s) {
    if (info) { info->kind = CryptoKind::Luks; info->format = "LUKS"; info->supported = false; info->note = why; }
    return s;
  }
  VolumeInfo local;
  VolumeInfo* inf = info ? info : &local;
  fill_info(l, inf);
  if (!inf->supported) return Status::Make(Errc::Unsupported);

  std::vector<const Secret*> passes;
  if (!cred.password.empty()) passes.push_back(&cred.password);
  if (!cred.key_file.empty()) passes.push_back(&cred.key_file);
  if (passes.empty()) { inf->note = "パスフレーズまたはキーファイルが必要です"; return Status::Make(Errc::AuthFailed); }

  std::vector<const Slot*> order;
  for (auto& sl : l.slots) if (sl.priority != 0) order.push_back(&sl);
  std::stable_sort(order.begin(), order.end(), [](const Slot* a, const Slot* b) { return a->priority > b->priority; });
  bool auth_failed = false, resource = false;
  for (const Slot* sl : order) {
    for (const Secret* pw : passes) {
      Secret mk;
      Status r = try_slot(*base, l, *sl, *pw, opt, &mk);
      if (r.code == Errc::Cancelled) return r;
      if (r.code == Errc::AuthFailed) { auth_failed = true; continue; }
      if (r.code == Errc::Unsupported) { resource = true; continue; }
      if (!r) continue;
      // 解錠成功: データ層
      auto cc = std::make_unique<CryptCipher>();
      if (!cc->setup(l.mode, mk.data(), mk.size(), l.sector)) { inf->note = "ボリュームキー長が暗号と合いません"; return Status::Make(Errc::Corrupt); }
      const uint64_t avail = base->size() - l.data_offset;
      uint64_t size = l.data_size ? l.data_size : avail;
      if (l.data_size && size > avail) { inf->note = "デバイスがヘッダの記録より小さいです (切り詰め)"; return Status::Make(Errc::Corrupt); }
      if (size < l.sector) { inf->note = "データ領域が空です"; return Status::Make(Errc::Corrupt); }
      std::string desc = std::string("luks") + (l.version == 1 ? "1" : "2") + "(" + l.cipher_desc + ", " + base->describe() + ")";
      bool writable = false;
#ifdef USBMEM_MUTATE_CW_ALWAYS_WRITABLE
      if (true) {  // 突然変異: open 時の明示 (opt.writable) を無視して、常に書き込み可にする
#else
      if (opt.writable) {
#endif
        if (!inf->write_supported) { /* 理由は fill_info が write_note に入れてある */ }
        else if (base->is_read_only()) inf->write_note = "下位デバイスが読み取り専用です";
        else writable = true;
      }
      inf->writable = writable;
      *out = std::make_shared<LuksDevice>(base, std::move(cc), l.data_offset, size, l.iv_tweak, opt.cache_chunks, std::move(desc), writable);
      return Status::Ok();
    }
  }
  if (auth_failed) { inf->note = "パスフレーズまたはキーファイルが違います"; return Status::Make(Errc::AuthFailed); }
  if (resource) { inf->note = "この keyslot の KDF が必要とするメモリ/時間が上限を超えています"; return Status::Make(Errc::Unsupported); }
  inf->note = "使える keyslot がありません";
  return Status::Make(Errc::Corrupt);
}

}  // namespace tf
}  // namespace usbmem
