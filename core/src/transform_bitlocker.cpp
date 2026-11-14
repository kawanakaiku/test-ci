// SPDX-License-Identifier: Apache-2.0
// BitLocker (Windows 7 以降の FVE メタデータ) の読み取り専用復号。仕様の調べ方と検証は docs/crypto-volumes.md。
//
//  ボリューム先頭 (ブートセクタ) の "-FVE-FS-" → 3 つの FVE メタデータブロックの位置 → エントリ列
//  → VMK 保護 (パスワード / 回復パスワード / スタートアップキー / クリアキー) → VMK → FVEK (どちらも AES-CCM)
//  → セクタ復号 (XTS-AES-128/256, AES-CBC-128/256 ± Elephant diffuser)。
#include <algorithm>
#include <cstring>
#include <memory>

#include "transform_internal.h"

namespace usbmem {
namespace tf {
namespace {

using crypto::Aes;
using crypto::Hash;
using crypto::Secret;

constexpr char kFveSig[8] = {'-', 'F', 'V', 'E', '-', 'F', 'S', '-'};
constexpr size_t kMaxMeta = 0x10000;           // メタデータブロックの上限 (Windows は 64 KiB 領域)
constexpr size_t kMaxEntries = 256;
constexpr uint32_t kStretchIterations = 0x100000;
constexpr uint64_t kHiddenSpan = 0x10000;  // FVE メタデータブロック 1 つ分の領域

// 値の型 (エントリヘッダの value_type)
enum : uint16_t { VT_KEY = 1, VT_STRING = 2, VT_STRETCH = 3, VT_USEKEY = 4, VT_CCM = 5, VT_TPM = 6, VT_VALIDATION = 7, VT_VMK = 8, VT_EXTERNAL = 9, VT_VOLHDR = 0xf };
// エントリの種類 (entry_type)
enum : uint16_t { ET_VMK = 2, ET_FVEK = 3, ET_VALIDATION = 4, ET_STARTUP = 6, ET_DESCRIPTION = 7, ET_VOLHDR = 0xf };
// 保護の種類
enum : uint16_t { PROT_CLEAR = 0x0000, PROT_TPM = 0x0100, PROT_STARTUP = 0x0200, PROT_TPM_STARTUP = 0x0300, PROT_TPM_PIN = 0x0500, PROT_RECOVERY = 0x0800, PROT_PASSWORD = 0x2000 };
// 暗号化方式
enum : uint16_t { M_CBC128_DIFF = 0x8000, M_CBC256_DIFF = 0x8001, M_CBC128 = 0x8002, M_CBC256 = 0x8003, M_XTS128 = 0x8004, M_XTS256 = 0x8005 };

struct Entry {
  uint16_t size = 0, type = 0, vtype = 0, ver = 0;
  const uint8_t* data = nullptr;
  size_t dlen = 0;
};

// [p, p+len) をエントリ列として解釈する。サイズ 0〜7 は終端 (パディング)。はみ出しは false。
bool parse_entries(const uint8_t* p, size_t len, std::vector<Entry>* out) {
  out->clear();
  size_t pos = 0;
  while (len - pos >= 8) {
    Entry e;
    e.size = le16(p + pos); e.type = le16(p + pos + 2); e.vtype = le16(p + pos + 4); e.ver = le16(p + pos + 6);
    if (e.size < 8) break;
    if (e.size > len - pos) return false;
    e.data = p + pos + 8;
    e.dlen = e.size - 8u;
    if (out->size() >= kMaxEntries) return false;
    out->push_back(e);
    pos += e.size;
  }
  return true;
}

const char* method_name(uint16_t m) {
  switch (m) {
    case M_CBC128_DIFF: return "AES-CBC-128 + Elephant diffuser";
    case M_CBC256_DIFF: return "AES-CBC-256 + Elephant diffuser";
    case M_CBC128: return "AES-CBC-128";
    case M_CBC256: return "AES-CBC-256";
    case M_XTS128: return "XTS-AES-128";
    case M_XTS256: return "XTS-AES-256";
  }
  return "unknown";
}

struct Meta {
  uint64_t offset = 0;
  std::vector<uint8_t> raw;  // ブロックヘッダ 64 + メタデータ
  uint16_t cur_state = 0, next_state = 0;
  uint64_t enc_size = 0;
  uint32_t vh_sectors = 0;
  uint64_t vh_off = 0;
  uint16_t method = 0;
  std::vector<Entry> entries;  // raw の中を指す (Meta は移動しない)
};

struct Volume {
  uint32_t sector = 512;
  std::vector<std::unique_ptr<Meta>> metas;  // 構造が正しく読めた (内容が異なる) コピー
  std::vector<uint64_t> offsets;             // メタデータブロックの位置 (読めなかったものを含む)
  int bad_copies = 0;
};

// ブートセクタの 8 バイト境界の値から、"-FVE-FS-" で始まるメタデータブロックを 3 つまで探す。
// Windows 7+ は 0xB0〜、BitLocker To Go は別の位置にあるため、位置を決め打ちせず走査する。
Status load_volume(BlockDevice& dev, Volume* v, std::string* why) {
  uint8_t sec[512];
  if (dev.size() < 512) return Status::Make(Errc::Unsupported);
  Status s = read_exact(dev, 0, sec, sizeof sec);
  if (!s) return s;
  if (std::memcmp(sec + 3, kFveSig, 8) != 0) return Status::Make(Errc::Unsupported);
  const uint32_t bps = le16(sec + 0x0b);
  v->sector = (bps == 512 || bps == 1024 || bps == 2048 || bps == 4096) ? bps : 512;
  const uint64_t dsize = dev.size();
  std::vector<uint64_t> cands;
  for (size_t pos = 0x80; pos + 8 <= 0x1f8; pos += 8) {
    uint64_t o = le64(sec + pos);
    if (o == 0 || (o % 512) != 0 || o > dsize || dsize - o < 112) continue;
    if (std::find(cands.begin(), cands.end(), o) != cands.end()) continue;
    uint8_t sig[8];
    if (!read_exact(dev, o, sig, 8)) continue;
    if (std::memcmp(sig, kFveSig, 8) != 0) continue;
    cands.push_back(o);
    if (cands.size() == 3) break;
  }
  if (cands.empty()) {
    if (why) *why = "BitLocker のメタデータ位置を特定できません (ヘッダ破損、または Vista 形式)";
    return Status::Make(Errc::Corrupt);
  }
  v->offsets = cands;
  for (uint64_t o : cands) {
    auto m = std::make_unique<Meta>();
    m->offset = o;
    uint8_t hdr[112];
    if (!read_exact(dev, o, hdr, sizeof hdr)) { v->bad_copies++; continue; }
    if (std::memcmp(hdr, kFveSig, 8) != 0 || le16(hdr + 10) != 2) { v->bad_copies++; continue; }  // version 1 = Vista (非対応)
    const uint32_t msize = le32(hdr + 64);
    if (le32(hdr + 68) != 1 || le32(hdr + 72) != 48 || le32(hdr + 76) != msize || msize < 48 || msize > kMaxMeta - 64) { v->bad_copies++; continue; }
    m->raw.resize(64 + size_t(msize));
    if (!read_exact(dev, o, m->raw.data(), m->raw.size())) { v->bad_copies++; continue; }
    const uint8_t* r = m->raw.data();
    m->cur_state = le16(r + 12); m->next_state = le16(r + 14);
    m->enc_size = le64(r + 16);
    m->vh_sectors = le32(r + 28);
    m->vh_off = le64(r + 56);
    m->method = le16(r + 64 + 36);
    if (!parse_entries(r + 112, m->raw.size() - 112, &m->entries)) { v->bad_copies++; continue; }
    // 内容が同じコピーは 1 つにまとめる (パスワード誤りの再試行で KDF を無駄に回さない)
    bool dup = false;
    for (auto& e : v->metas)
      if (e->raw.size() == m->raw.size() && std::memcmp(e->raw.data() + 64, m->raw.data() + 64, m->raw.size() - 64) == 0) dup = true;
    if (!dup) v->metas.push_back(std::move(m));
  }
  if (v->metas.empty()) {
    if (why) *why = "BitLocker のメタデータ 3 コピーすべてが読めません (破損または切り詰め)";
    return Status::Make(Errc::Corrupt);
  }
  return Status::Ok();
}

// ---- 鍵の導出
bool utf8_to_utf16le(const Secret& in, Secret* out) {
  std::vector<uint8_t> tmp;
  const uint8_t* p = in.data();
  size_t n = in.size(), i = 0;
  auto put16 = [&](uint32_t u) { tmp.push_back(uint8_t(u)); tmp.push_back(uint8_t(u >> 8)); };
  bool ok = true;
  while (i < n) {
    uint32_t c = p[i], cp;
    size_t extra;
    if (c < 0x80) { cp = c; extra = 0; }
    else if ((c & 0xe0) == 0xc0) { cp = c & 0x1f; extra = 1; }
    else if ((c & 0xf0) == 0xe0) { cp = c & 0x0f; extra = 2; }
    else if ((c & 0xf8) == 0xf0) { cp = c & 0x07; extra = 3; }
    else { ok = false; break; }
    if (extra && i + extra >= n) { ok = false; break; }  // 末尾で切れている
    for (size_t k = 1; k <= extra; k++) {
      if ((p[i + k] & 0xc0) != 0x80) { ok = false; break; }
      cp = (cp << 6) | (p[i + k] & 0x3f);
    }
    if (!ok) break;
    i += extra + 1;
    if (cp > 0x10ffff || (cp >= 0xd800 && cp < 0xe000)) { ok = false; break; }
    if (cp >= 0x10000) { cp -= 0x10000; put16(0xd800 + (cp >> 10)); put16(0xdc00 + (cp & 0x3ff)); }
    else put16(cp);
  }
  if (ok) out->assign(tmp.data(), tmp.size());
  crypto::secure_zero(tmp.data(), tmp.size());
  return ok;
}

// 回復パスワード (8 グループ × 6 桁、各グループは 11 の倍数で 11 で割ると 16 ビット) → 16 バイト
bool parse_recovery(const Secret& in, uint8_t out16[16]) {
  char digits[48];
  size_t nd = 0;
  for (size_t i = 0; i < in.size(); i++) {
    char c = char(in.data()[i]);
    if (c == '-' || c == ' ') continue;
    if (c < '0' || c > '9' || nd >= 48) return false;
    digits[nd++] = c;
  }
  if (nd != 48) return false;
  for (int g = 0; g < 8; g++) {
    uint32_t v = 0;
    for (int k = 0; k < 6; k++) v = v * 10 + uint32_t(digits[g * 6 + k] - '0');
    if (v % 11 != 0 || v / 11 > 0xffff) return false;
    uint32_t w = v / 11;
    out16[g * 2] = uint8_t(w); out16[g * 2 + 1] = uint8_t(w >> 8);
  }
  crypto::secure_zero(digits, sizeof digits);
  return true;
}

// stretch: last = SHA256(last || initial || salt || counter_u64_LE) を 0x100000 回
Status stretch_key(const uint8_t initial[32], const uint8_t salt[16], const CancelToken* cancel, uint8_t out[32]) {
  // 入力は 88 バイト固定なので、パディング込みの 128 バイト (2 ブロック) を最初に組み、毎回 last と counter だけ差し替えて圧縮関数を直接回す
  // (HashCtx の生成・パディング・コピーを 100 万回繰り返さない。SHA-256 命令があればそれを使う)。
  static const uint32_t kIv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  uint8_t last[32] = {0}, blk[128] = {0};
  uint32_t st[8];
  std::memcpy(blk + 32, initial, 32);
  std::memcpy(blk + 64, salt, 16);
  blk[88] = 0x80;
  blk[126] = uint8_t((88 * 8) >> 8);
  blk[127] = uint8_t((88 * 8) & 0xff);
  for (uint32_t i = 0; i < kStretchIterations; i++) {
    if ((i & 0x3fff) == 0 && cancel && cancel->cancelled()) {
      crypto::secure_zero(last, 32); crypto::secure_zero(blk, sizeof blk); crypto::secure_zero(st, sizeof st);
      return Status::Make(Errc::Cancelled);
    }
    std::memcpy(blk, last, 32);
    put_le64(blk + 80, i);
    std::memcpy(st, kIv, sizeof st);
    crypto::sha256_compress(st, blk, 2);
    for (int k = 0; k < 8; k++) { last[4 * k] = uint8_t(st[k] >> 24); last[4 * k + 1] = uint8_t(st[k] >> 16); last[4 * k + 2] = uint8_t(st[k] >> 8); last[4 * k + 3] = uint8_t(st[k]); }
  }
  std::memcpy(out, last, 32);
  crypto::secure_zero(last, sizeof last);
  crypto::secure_zero(blk, sizeof blk);
  crypto::secure_zero(st, sizeof st);
  return Status::Ok();
}

// AES-CCM エントリ (nonce12 | tag16 | ct) を key で復号して鍵エントリ (ヘッダ 8 + method 4 + 鍵) を得る。
bool ccm_unwrap(const Entry& e, const uint8_t key[32], Secret* blob) {
  if (e.vtype != VT_CCM || e.dlen < 12 + 16 + 12 + 16 || e.dlen > 12 + 16 + 256) return false;
  Aes a;
  if (!a.set_key(key, 32)) return false;
  const size_t clen = e.dlen - 28;
  Secret out(clen);
  if (!crypto::aes_ccm_decrypt(a, e.data, e.data + 28, clen, e.data + 12, out.data())) return false;
  const uint8_t* b = out.data();
  const uint16_t sz = le16(b), vt = le16(b + 4);
  if (sz < 12 || sz > clen || vt != VT_KEY) return false;
  blob->assign(b, sz);
  return true;
}

// 鍵エントリ (blob) から method と鍵データを取り出す。
bool key_of(const Secret& blob, uint32_t* method, const uint8_t** key, size_t* klen) {
  if (blob.size() < 12) return false;
  *method = le32(blob.data() + 8);
  *key = blob.data() + 12;
  *klen = blob.size() - 12;
  return true;
}

// ---- セクタ暗号
struct SectorCipher {
  uint16_t method = 0;
  uint32_t sector = 512;
  Aes k1, k2;  // XTS: データ鍵 / tweak 鍵。CBC: k1 = FVEK, k2 = Elephant の sector key 用 tweak 鍵

  bool setup(uint16_t m, const uint8_t* key, size_t klen) {
    method = m;
    size_t need;
    switch (m) {
      case M_CBC128_DIFF: need = 32; break;
      case M_CBC256_DIFF: need = 64; break;
      case M_CBC128: need = 16; break;
      case M_CBC256: need = 32; break;
      case M_XTS128: need = 32; break;
      case M_XTS256: need = 64; break;
      default: return false;
    }
    if (klen < need) return false;
    const size_t half = (m == M_CBC128 || m == M_CBC256) ? need : need / 2;
    if (!k1.set_key(key, half)) return false;
    // AES-128 + diffuser: 鍵データは 64 バイト (FVEK 16 + 詰め物 16 + tweak 16 + 詰め物 16。libbde と同じ解釈)。32 バイトだけなら 16 + 16 と見なす
    const size_t tw_off = (m == M_CBC128_DIFF && klen >= 64) ? 32 : half;
    if (need != half && !k2.set_key(key + tw_off, half)) return false;
    return true;
  }
  bool diffuser() const { return method == M_CBC128_DIFF || method == M_CBC256_DIFF; }
  bool xts() const { return method == M_XTS128 || method == M_XTS256; }
  void clear() { k1.clear(); k2.clear(); }

  // in -> out (in == out 可)。phys は暗号化されたデータの、ボリューム先頭からのバイト位置 (セクタ境界)。
  void decrypt_sector(uint64_t phys, const uint8_t* in, uint8_t* out) const {
    if (xts()) { crypto::aes_xts_decrypt(k1, k2, phys / sector, in, out, sector); return; }
    uint8_t blk[16] = {0}, iv[16];
    put_le64(blk, phys);
    k1.encrypt_block(blk, iv);
    crypto::aes_cbc_decrypt(k1, iv, in, out, sector);
    if (!diffuser()) return;
    // Elephant diffuser: B → A の順に外し、最後に sector key を XOR する
    const size_t n = sector / 4;
    std::vector<uint32_t> d(n);
    for (size_t i = 0; i < n; i++) d[i] = le32(out + 4 * i);
    static const int Rb[4] = {0, 10, 0, 25};
    static const int Ra[4] = {9, 0, 13, 0};
    auto rol = [](uint32_t x, int r) { return r ? (x << r) | (x >> (32 - r)) : x; };
    for (int c = 0; c < 3; c++)
      for (size_t i = 0; i < n; i++) d[i] = d[i] + (d[(i + 2) % n] ^ rol(d[(i + 5) % n], Rb[i % 4]));
    for (int c = 0; c < 5; c++)
      for (size_t i = 0; i < n; i++) d[i] = d[i] + (d[(i + n - 2) % n] ^ rol(d[(i + n - 5) % n], Ra[i % 4]));
    uint8_t skey[32];
    uint8_t bk[16] = {0};
    put_le64(bk, phys);
    k2.encrypt_block(bk, skey);
    bk[15] = 0x80;
    k2.encrypt_block(bk, skey + 16);
    for (size_t i = 0; i < n; i++) {
      uint32_t w = d[i];
      for (int k = 0; k < 4; k++) out[4 * i + size_t(k)] = uint8_t(uint8_t(w >> (8 * k)) ^ skey[(4 * i + size_t(k)) % 32]);
    }
    crypto::secure_zero(d.data(), d.size() * 4);
    crypto::secure_zero(skey, sizeof skey);
  }
};

class BitLockerDevice final : public DecryptingDevice {
 public:
  BitLockerDevice(BlockDevicePtr base, std::unique_ptr<SectorCipher> c, uint64_t size, uint32_t cache_chunks, uint32_t vh_sectors, uint64_t vh_off,
                  uint64_t plain_from, std::vector<uint64_t> hidden, std::string desc)
      : DecryptingDevice(std::move(base), size, c->sector, cache_chunks, std::move(desc)),
        c_(std::move(c)), vh_sectors_(vh_sectors), vh_off_(vh_off), plain_from_(plain_from), hidden_(std::move(hidden)) {}

 protected:
  Status read_plain(uint64_t off, uint8_t* buf, size_t len, const IoOptions& opt) override {
    const uint32_t ss = c_->sector;
    uint64_t pos = off;
    size_t left = len;
    while (left) {
      const uint64_t s = pos / ss;
      uint64_t src;
      size_t n;
      bool in_hdr = s < vh_sectors_;
      if (in_hdr) {  // 先頭セクタ群の本体は volume header offset に退避されている
        src = vh_off_ + s * ss;
        n = std::min<uint64_t>(left, (uint64_t(vh_sectors_) - s) * ss);
      } else {
        src = pos;
        n = left;
      }
      Status st = base_->read_at(src, buf, n, opt);
      if (!st) return st;
      for (size_t o = 0; o < n; o += ss) {
        const uint64_t phys = src + o;
        uint8_t* sec = buf + o;
        if (!in_hdr) {
          // FVE メタデータ領域と退避済みボリュームヘッダ領域は、Windows の BitLocker ドライバと同様に 0 を返す (暗号文を見せない)
          bool hide = false;
          for (uint64_t h : hidden_) if (phys >= h && phys - h < kHiddenSpan) { hide = true; break; }
          if (!hide && phys >= vh_off_ && phys - vh_off_ < uint64_t(vh_sectors_) * ss) hide = true;
          if (hide) { std::memset(sec, 0, ss); continue; }
          if (phys >= plain_from_) continue;  // 暗号化がまだ及んでいない領域は平文のまま
        }
        c_->decrypt_sector(phys, sec, sec);
      }
      pos += n; buf += n; left -= n;
    }
    return Status::Ok();
  }
  void wipe_keys() override { c_->clear(); }

 private:
  std::unique_ptr<SectorCipher> c_;
  uint32_t vh_sectors_;
  uint64_t vh_off_;
  uint64_t plain_from_;
  std::vector<uint64_t> hidden_;  // FVE メタデータブロックの位置
};

// ---- 保護の分類
struct ProtectorSummary {
  bool password = false, recovery = false, startup = false, clear = false, tpm_related = false;
  int total = 0;
};

ProtectorSummary summarize(const Meta& m) {
  ProtectorSummary ps;
  for (const Entry& e : m.entries) {
    if (e.type != ET_VMK || e.vtype != VT_VMK || e.dlen < 28) continue;
    ps.total++;
    switch (le16(e.data + 26)) {
      case PROT_PASSWORD: ps.password = true; break;
      case PROT_RECOVERY: ps.recovery = true; break;
      case PROT_STARTUP: ps.startup = true; break;
      case PROT_CLEAR: ps.clear = true; break;
      default: ps.tpm_related = true; break;
    }
  }
  return ps;
}

void fill_info(const Volume& v, VolumeInfo* info) {
  const Meta& m = *v.metas[0];
  ProtectorSummary ps = summarize(m);
  info->kind = CryptoKind::BitLocker;
  info->format = "BitLocker (Windows 7+)";
  info->cipher = method_name(m.method);
  info->can_password = ps.password;
  info->can_recovery_key = ps.recovery;
  info->can_key_file = ps.startup;
  info->unlocked_without_secret = ps.clear;
  info->tpm_only = ps.tpm_related && !ps.password && !ps.recovery && !ps.startup && !ps.clear;
  info->encryption_in_progress = (m.cur_state != 4 || m.next_state != 4);
  const bool method_ok = method_name(m.method)[0] != 'u';
  info->supported = method_ok;
  info->note.clear();
  if (!method_ok) info->note = "未対応の暗号化方式です";
  else if (info->tpm_only) info->note = "TPM のみで保護されています。このアプリでは解錠できません (回復パスワードがあれば入力してください)";
  else if (info->encryption_in_progress) info->note = "暗号化または復号が途中のボリュームです (変換が完了するまで開けません)";
  else if (v.bad_copies) info->note = "メタデータの一部のコピーが壊れています (残りのコピーで読めます)";
}

}  // namespace

Status bitlocker_probe(BlockDevice& dev, VolumeInfo* info) {
  Volume v;
  std::string why;
  Status s = load_volume(dev, &v, &why);
  if (s.code == Errc::Unsupported) return s;
  if (!s) {
    if (s.code == Errc::Corrupt) {  // BitLocker と分かるがメタデータが読めない
      info->kind = CryptoKind::BitLocker;
      info->format = "BitLocker";
      info->supported = false;
      info->note = why;
    }
    return s;
  }
  fill_info(v, info);
  return Status::Ok();
}

namespace {

// 1 つの VMK 保護を資格情報で解く。成功なら VMK(32 バイト) を返す。
//  戻り値: Ok = 解けた / AuthFailed = 資格情報が合わない / Unsupported = 試せない保護 / Cancelled など
Status unlock_protector(const Entry& vmk, const Credentials& cred, const OpenOptions& opt, uint8_t vmk_out[32]) {
  const uint16_t prot = le16(vmk.data + 26);
  std::vector<Entry> nested;
  if (!parse_entries(vmk.data + 28, vmk.dlen - 28, &nested)) return Status::Make(Errc::Corrupt);
  const Entry *stretch = nullptr, *ccm = nullptr, *clearkey = nullptr;
  for (const Entry& n : nested) {
    if (n.vtype == VT_STRETCH && !stretch) stretch = &n;
    else if (n.vtype == VT_CCM && !ccm) ccm = &n;
    else if (n.vtype == VT_KEY && !clearkey) clearkey = &n;
  }
  if (!ccm) return Status::Make(Errc::Corrupt);
  uint8_t key[32];
  Status s = Status::Make(Errc::Unsupported);
  if (prot == PROT_CLEAR) {
    if (!clearkey || clearkey->dlen < 4 + 32) return Status::Make(Errc::Corrupt);
    std::memcpy(key, clearkey->data + 4, 32);
    s = Status::Ok();
  } else if (prot == PROT_PASSWORD || prot == PROT_RECOVERY) {
    const bool pw = prot == PROT_PASSWORD;
    const Secret& in = pw ? cred.password : cred.recovery_key;
    if (in.empty()) return Status::Make(Errc::Unsupported);  // この種の資格情報が渡されていない
    if (!stretch || stretch->dlen < 20 || (le32(stretch->data) != 0x1001 && le32(stretch->data) != 0x1000)) return Status::Make(Errc::Corrupt);
    uint8_t init[32];
    if (pw) {
      Secret u16;
      if (!utf8_to_utf16le(in, &u16)) return Status::Make(Errc::InvalidArgument);
      uint8_t h1[32];
      crypto::hash(Hash::Sha256, u16.data(), u16.size(), h1);
      crypto::hash(Hash::Sha256, h1, 32, init);
      crypto::secure_zero(h1, 32);
    } else {
      uint8_t r16[16];
      if (!parse_recovery(in, r16)) return Status::Make(Errc::InvalidArgument);
      crypto::hash(Hash::Sha256, r16, 16, init);
      crypto::secure_zero(r16, 16);
    }
    s = stretch_key(init, stretch->data + 4, opt.cancel, key);
    crypto::secure_zero(init, 32);
  } else if (prot == PROT_STARTUP) {
    if (cred.key_file.empty()) return Status::Make(Errc::Unsupported);
    // .BEK: 48 バイトのメタデータヘッダ + 外部鍵エントリ (GUID 16 + FILETIME 8 + 入れ子の鍵エントリ)
    const uint8_t* f = cred.key_file.data();
    size_t fl = cred.key_file.size();
    bool found = false;
    if (fl >= 48 + 8 && le32(f + 8) == 48 && le32(f) <= fl) {
      std::vector<Entry> top;
      if (parse_entries(f + 48, std::min<size_t>(fl, le32(f)) - 48, &top)) {
        for (const Entry& e : top) {
          if (e.vtype != VT_EXTERNAL || e.dlen < 24 + 8) continue;
          if (std::memcmp(e.data, vmk.data, 16) != 0) continue;  // この保護の GUID の鍵ファイルか
          std::vector<Entry> in2;
          if (!parse_entries(e.data + 24, e.dlen - 24, &in2)) continue;
          for (const Entry& k : in2)
            if (k.vtype == VT_KEY && k.dlen >= 4 + 32) { std::memcpy(key, k.data + 4, 32); found = true; break; }
          if (found) break;
        }
      }
    }
    if (!found) return Status::Make(Errc::AuthFailed);
    s = Status::Ok();
  } else {
    return Status::Make(Errc::Unsupported);  // TPM 系など
  }
  if (!s) { crypto::secure_zero(key, 32); return s; }
  Secret blob;
  bool ok = ccm_unwrap(*ccm, key, &blob);
  crypto::secure_zero(key, 32);
  if (!ok) return Status::Make(Errc::AuthFailed);
  uint32_t method;
  const uint8_t* k;
  size_t kl;
  if (!key_of(blob, &method, &k, &kl) || kl < 32) return Status::Make(Errc::Corrupt);
  std::memcpy(vmk_out, k, 32);
  return Status::Ok();
}

}  // namespace

Status bitlocker_open(BlockDevicePtr base, const Credentials& cred, const OpenOptions& opt, BlockDevicePtr* out, VolumeInfo* info) {
  Volume v;
  std::string why;
  Status s = load_volume(*base, &v, &why);
  if (!s) {
    if (info) { info->kind = CryptoKind::BitLocker; info->note = s.code == Errc::Unsupported ? "BitLocker ではありません" : why; }
    return s;
  }
  fill_info(v, info);
  const Meta& m0 = *v.metas[0];
  if (!info->supported) return Status::Make(Errc::Unsupported);
  if (info->encryption_in_progress && !opt.allow_partial_encryption) return Status::Make(Errc::Unsupported);

  bool any_auth_failed = false, any_tried = false, bad_input = false;
  Status last_hard = Status::Ok();
  for (auto& mp : v.metas) {
    const Meta& m = *mp;
    for (const Entry& e : m.entries) {
      if (e.type != ET_VMK || e.vtype != VT_VMK || e.dlen < 28) continue;
      uint8_t vmk[32];
      Status r = unlock_protector(e, cred, opt, vmk);
      if (r.code == Errc::Unsupported) continue;
      if (r.code == Errc::InvalidArgument) { bad_input = true; continue; }
      if (r.code == Errc::AuthFailed) { any_tried = true; any_auth_failed = true; continue; }
      if (!r) { if (r.code == Errc::Cancelled) return r; last_hard = r; continue; }
      any_tried = true;
      // FVEK
      const Entry* fe = nullptr;
      for (const Entry& x : m.entries)
        if (x.type == ET_FVEK && x.vtype == VT_CCM) { fe = &x; break; }
      Secret blob;
      if (!fe || !ccm_unwrap(*fe, vmk, &blob)) { crypto::secure_zero(vmk, 32); last_hard = Status::Make(Errc::Corrupt); continue; }
      crypto::secure_zero(vmk, 32);
      uint32_t method;
      const uint8_t* k;
      size_t kl;
      auto scp = std::make_unique<SectorCipher>();
      SectorCipher& sc = *scp;
      if (!key_of(blob, &method, &k, &kl) || method > 0xffff || !sc.setup(uint16_t(method), k, kl)) {
        last_hard = Status::Make(Errc::Unsupported);
        continue;
      }
      sc.sector = v.sector;
      uint64_t bsize = base->size();
      // 暗号化が完了していれば、メタデータの「暗号化済みボリュームサイズ」までをボリュームとして見せる (パーティションより後ろの余りを含めない。libbde と同じ)
      if (!info->encryption_in_progress && m.enc_size >= v.sector && m.enc_size < bsize) bsize = m.enc_size / v.sector * v.sector;
      // ボリュームヘッダ退避領域の妥当性
      if (m.vh_sectors == 0 || m.vh_sectors > 1024 || m.vh_off % v.sector != 0 || m.vh_off < uint64_t(m.vh_sectors) * v.sector ||
          !range_ok(bsize, m.vh_off, uint64_t(m.vh_sectors) * v.sector)) {
        if (info) info->note = "ボリュームヘッダの退避位置が不正です";
        return Status::Make(Errc::Corrupt);
      }
      std::vector<uint64_t> hidden = v.offsets;
      uint64_t plain_from = ~uint64_t(0);
      if (info->encryption_in_progress) plain_from = m.enc_size;
      info->cipher = method_name(sc.method);
      std::string desc = std::string("bitlocker(") + method_name(sc.method) + ", " + base->describe() + ")";
      *out = std::make_shared<BitLockerDevice>(base, std::move(scp), bsize, opt.cache_chunks, m.vh_sectors, m.vh_off, plain_from, hidden, std::move(desc));
      return Status::Ok();
    }
  }
  (void)m0;
  if (any_auth_failed) { if (info) info->note = "パスワード / 回復パスワード / キーファイルが違います"; return Status::Make(Errc::AuthFailed); }
  if (bad_input) { if (info) info->note = "入力された資格情報の形式が不正です"; return Status::Make(Errc::InvalidArgument); }
  if (!last_hard) return last_hard;
  if (!any_tried) {
    const bool anycred = !cred.password.empty() || !cred.recovery_key.empty() || !cred.key_file.empty();
    if (info) {
      if (info->tpm_only) info->note = "TPM のみで保護されています。このアプリでは解錠できません";
      else info->note = anycred ? "渡された種類の資格情報で解錠できる保護がありません" : "資格情報が必要です";
    }
    return Status::Make(anycred && !info->tpm_only ? Errc::AuthFailed : (info->tpm_only ? Errc::Unsupported : Errc::AuthFailed));
  }
  return Status::Make(Errc::AuthFailed);
}

}  // namespace tf
}  // namespace usbmem
