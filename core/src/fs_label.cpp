// SPDX-License-Identifier: Apache-2.0
#include "usbmem/fs_label.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace usbmem {
namespace {

uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
uint64_t le64(const uint8_t* p) { return le32(p) | (static_cast<uint64_t>(le32(p + 4)) << 32); }
uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
bool is_pow2(uint64_t v) { return v && !(v & (v - 1)); }

void append_utf8(std::string& o, uint32_t c) {
  if (c < 0x80) o += static_cast<char>(c);
  else if (c < 0x800) { o += static_cast<char>(0xC0 | (c >> 6)); o += static_cast<char>(0x80 | (c & 0x3F)); }
  else if (c < 0x10000) {
    o += static_cast<char>(0xE0 | (c >> 12)); o += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
    o += static_cast<char>(0x80 | (c & 0x3F));
  } else {
    o += static_cast<char>(0xF0 | (c >> 18)); o += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
    o += static_cast<char>(0x80 | ((c >> 6) & 0x3F)); o += static_cast<char>(0x80 | (c & 0x3F));
  }
}

// UTF-16 (LE か BE) を UTF-8 へ。孤立サロゲートは U+FFFD。NUL で止める。
std::string utf16_to_utf8(const uint8_t* p, size_t units, bool big_endian) {
  auto u = [&](size_t i) -> uint32_t { return big_endian ? be16(p + 2 * i) : le16(p + 2 * i); };
  std::string out;
  for (size_t i = 0; i < units; i++) {
    uint32_t c = u(i);
    if (c == 0) break;
    if (c >= 0xD800 && c < 0xDC00 && i + 1 < units) {
      uint32_t d = u(i + 1);
      if (d >= 0xDC00 && d < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00); i++; }
      else c = 0xFFFD;
    } else if (c >= 0xD800 && c < 0xE000) {
      c = 0xFFFD;
    }
    append_utf8(out, c);
  }
  return out;
}

// 任意のバイト列 (OEM コードページ / 8 ビット文字) を安全な UTF-8 にする。
// 既に正しい UTF-8 ならそのまま、そうでなければ非 ASCII バイトを '?' にする。末尾の空白と NUL は落とし、制御文字は空白にする。
bool valid_utf8(const uint8_t* p, size_t len) {
  for (size_t i = 0; i < len;) {
    uint8_t c = p[i];
    size_t extra = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : 99;
    if (extra == 99) return false;
    for (size_t k = 1; k <= extra; k++)
      if (i + k >= len || (p[i + k] & 0xC0) != 0x80) return false;
    i += extra + 1;
  }
  return true;
}

std::string sanitize_bytes(const uint8_t* p, size_t n) {
  size_t len = 0;
  while (len < n && p[len]) len++;
  while (len > 0 && p[len - 1] == ' ') len--;
  std::string out;
  if (valid_utf8(p, len)) out.assign(reinterpret_cast<const char*>(p), len);
  else
    for (size_t i = 0; i < len; i++) out += p[i] < 0x80 ? static_cast<char>(p[i]) : '?';
  for (auto& ch : out)
    if (static_cast<unsigned char>(ch) < 0x20 || ch == 0x7F) ch = ' ';
  return out;
}

// GUID (先頭 3 フィールドがリトルエンディアンの Microsoft 形式) を小文字で
std::string ms_guid_str(const uint8_t* p) {
  char b[40];
  std::snprintf(b, sizeof b, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", p[3], p[2], p[1], p[0], p[5],
                p[4], p[7], p[6], p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
  return b;
}
std::string uuid_str(const uint8_t* p) {
  char b[40];
  std::snprintf(b, sizeof b, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", p[0], p[1], p[2],
                p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
  return b;
}
bool all_zero(const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; i++)
    if (p[i]) return false;
  return true;
}
std::string serial32(uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof b, "%04X-%04X", v >> 16, v & 0xFFFF);
  return b;
}

struct Reader {
  BlockDevice& dev;
  uint64_t off, len;
  Status* io;
  bool rd(uint64_t rel, void* buf, size_t n) {
    if (!range_ok(len, rel, n) || !range_ok(dev.size(), off, len)) return false;
    Status st = dev.read_at(off + rel, buf, n);
    if (!st.ok()) { if (io->ok()) *io = st; return false; }
    return true;
  }
};

// ------------------------------------------------------------------ NTFS

void label_ntfs(Reader& r, const uint8_t* s, FsLabelInfo* o) {
  char b[24];
  std::snprintf(b, sizeof b, "%016llX", static_cast<unsigned long long>(le64(s + 0x48)));
  o->serial = b;
  o->uuid = b;
  uint32_t bps = le16(s + 11);
  uint32_t spc_raw = s[13];
  if (!is_pow2(bps) || bps < 512 || bps > 4096 || spc_raw == 0) return;
  uint64_t spc = spc_raw <= 0x80 ? spc_raw : (spc_raw >= 0xF9 ? (1ull << (256 - spc_raw)) : 0);  // 0xF4-0xF8 は異常
  if (!is_pow2(spc)) return;
  uint64_t cluster = bps * spc;
  int8_t cpr = static_cast<int8_t>(s[0x40]);
  uint64_t rec = cpr > 0 ? cluster * static_cast<uint64_t>(cpr) : (cpr >= -16 ? (1ull << (-cpr)) : 0);
  if (rec < 512 || rec > 65536 || !is_pow2(rec)) return;
  uint64_t mft_lcn = le64(s + 0x30);
  if (mft_lcn > UINT64_MAX / cluster) return;
  uint64_t mft_off = mft_lcn * cluster;
  if (mft_off > UINT64_MAX - 3 * rec) return;
  uint64_t rec3 = mft_off + 3 * rec;  // $Volume は MFT レコード 3 (MFT の先頭の連続領域に必ずある)
  if (!range_ok(r.len, rec3, rec)) return;
  std::vector<uint8_t> m(static_cast<size_t>(rec));
  if (!r.rd(rec3, m.data(), m.size())) return;
  if (std::memcmp(m.data(), "FILE", 4) != 0) return;
  // fixup (update sequence array) を適用
  uint32_t usa_ofs = le16(m.data() + 4), usa_cnt = le16(m.data() + 6);
  if (usa_cnt < 2 || usa_ofs < 42 || usa_ofs + 2ull * usa_cnt > m.size() || (usa_cnt - 1) * 512ull > m.size()) return;
  for (uint32_t i = 1; i < usa_cnt; i++) {
    size_t end = static_cast<size_t>(i) * 512 - 2;
    if (end + 2 > m.size()) return;
    if (std::memcmp(m.data() + end, m.data() + usa_ofs, 2) != 0) return;  // 破損 (トーン書き込み)
    std::memcpy(m.data() + end, m.data() + usa_ofs + 2 * i, 2);
  }
  uint32_t flags = le16(m.data() + 0x16);
  if (!(flags & 1)) return;  // 使用中のレコードのみ
  size_t pos = le16(m.data() + 0x14);
  size_t used = std::min<size_t>(le32(m.data() + 0x18), m.size());
  for (int guard = 0; guard < 64 && pos + 16 <= used; guard++) {
    uint32_t type = le32(m.data() + pos), alen = le32(m.data() + pos + 4);
    if (type == 0xFFFFFFFFu || alen < 24 || alen > used - pos || (alen & 7)) return;
    if (type == 0x60 && m[pos + 8] == 0) {  // $VOLUME_NAME (常駐)
      uint32_t vlen = le32(m.data() + pos + 0x10), voff = le16(m.data() + pos + 0x14);
      if (voff >= alen || vlen > alen - voff) return;
      o->label = utf16_to_utf8(m.data() + pos + voff, vlen / 2, false);
      return;
    }
    pos += alen;
  }
}

// ------------------------------------------------------------------ FAT

void label_fat(Reader& r, const uint8_t* s, FsType fs, FsLabelInfo* o) {
  bool f32 = fs == FsType::Fat32;
  size_t sig = f32 ? 66 : 38;
  if (s[sig] == 0x29 || s[sig] == 0x28) {
    o->serial = serial32(le32(s + sig + 1));
    o->uuid = o->serial;
  }
  std::string boot;
  if (s[sig] == 0x29) {
    boot = sanitize_bytes(s + sig + 5, 11);
    if (boot == "NO NAME") boot.clear();
  }
  uint32_t bps = le16(s + 11), spc = s[13], rsvd = le16(s + 14), nfats = s[16];
  if (!is_pow2(bps) || bps < 512 || bps > 4096 || !is_pow2(spc) || spc > 128 || rsvd == 0 || nfats == 0 || nfats > 2) {
    o->label = boot;
    return;
  }
  uint32_t root_ents = le16(s + 17);
  uint32_t fatsz = le16(s + 22) ? le16(s + 22) : le32(s + 36);
  std::string root_label;
  auto scan = [&](const uint8_t* d, size_t n) -> int {  // 1 = 見つけた, -1 = ディレクトリ終端, 0 = 続く
    for (size_t i = 0; i + 32 <= n; i += 32) {
      if (d[i] == 0) return -1;
      if (d[i] == 0xE5) continue;
      uint8_t attr = d[i + 11];
      if ((attr & 0x0F) == 0x0F) continue;  // LFN
      if ((attr & 0x08) && !(attr & 0x10)) { root_label = sanitize_bytes(d + i, 11); return 1; }
    }
    return 0;
  };
  if (!f32) {
    uint64_t root_off = (static_cast<uint64_t>(rsvd) + static_cast<uint64_t>(nfats) * fatsz) * bps;
    uint64_t bytes = static_cast<uint64_t>(root_ents) * 32;
    if (bytes > 0 && bytes <= (1u << 20)) {
      std::vector<uint8_t> d(static_cast<size_t>(bytes));
      if (r.rd(root_off, d.data(), d.size())) scan(d.data(), d.size());
    }
  } else {
    uint64_t data_off = (static_cast<uint64_t>(rsvd) + static_cast<uint64_t>(nfats) * fatsz) * bps;
    uint32_t c = le32(s + 44) & 0x0FFFFFFF;
    uint64_t cbytes = static_cast<uint64_t>(bps) * spc;
    std::vector<uint8_t> d(static_cast<size_t>(cbytes));
    for (int hop = 0; hop < 64 && c >= 2 && c < 0x0FFFFFF8; hop++) {
      if (!r.rd(data_off + (static_cast<uint64_t>(c) - 2) * cbytes, d.data(), d.size())) break;
      int res = scan(d.data(), d.size());
      if (res != 0) break;
      uint8_t e[4];
      if (!r.rd(static_cast<uint64_t>(rsvd) * bps + static_cast<uint64_t>(c) * 4, e, 4)) break;
      c = le32(e) & 0x0FFFFFFF;
    }
  }
  o->label = !root_label.empty() ? root_label : boot;
}

// ------------------------------------------------------------------ exFAT

void label_exfat(Reader& r, const uint8_t* s, FsLabelInfo* o) {
  o->serial = serial32(le32(s + 100));
  o->uuid = o->serial;
  uint32_t bshift = s[108], sshift = s[109];
  if (bshift < 9 || bshift > 12 || sshift > 25 - bshift) return;
  uint64_t bps = 1ull << bshift, cbytes = bps << sshift;
  uint64_t fat_off = static_cast<uint64_t>(le32(s + 80)) * bps, heap_off = static_cast<uint64_t>(le32(s + 88)) * bps;
  uint32_t ccount = le32(s + 92), c = le32(s + 96);
  const size_t kMaxScan = 64 * 1024;
  std::vector<uint8_t> dir;
  for (int hop = 0; hop < 64 && dir.size() < kMaxScan && c >= 2 && c < ccount + 2; hop++) {
    size_t take = static_cast<size_t>(std::min<uint64_t>(cbytes, kMaxScan - dir.size()));
    size_t at = dir.size();
    dir.resize(at + take);
    if (!r.rd(heap_off + (static_cast<uint64_t>(c) - 2) * cbytes, dir.data() + at, take)) { dir.resize(at); break; }
    if (take < cbytes) break;
    uint8_t e[4];
    if (!r.rd(fat_off + static_cast<uint64_t>(c) * 4, e, 4)) break;
    c = le32(e);
  }
  for (size_t i = 0; i + 32 <= dir.size(); i += 32) {
    uint8_t t = dir[i];
    if (t == 0) break;
    if (t == 0x83) {  // ボリュームラベル
      size_t n = std::min<size_t>(dir[i + 1], 11);
      o->label = utf16_to_utf8(dir.data() + i + 2, n, false);
    } else if (t == 0xA0) {  // ボリューム GUID
      if (!all_zero(dir.data() + i + 6, 16)) o->guid = ms_guid_str(dir.data() + i + 6);
    }
  }
}

// ------------------------------------------------------------------ ext

bool label_ext(Reader& r, FsLabelInfo* o) {
  uint8_t sb[1024];
  if (!r.rd(1024, sb, sizeof sb) || le16(sb + 56) != 0xEF53) return false;
  o->label = sanitize_bytes(sb + 120, 16);
  if (!all_zero(sb + 104, 16)) o->uuid = uuid_str(sb + 104);
  return true;
}

// ------------------------------------------------------------------ ISO 9660 / UDF

void label_iso(Reader& r, FsLabelInfo* o) {
  uint8_t d[2048];
  for (uint32_t i = 0; i < 32; i++) {
    if (!r.rd(0x8000 + static_cast<uint64_t>(i) * 2048, d, sizeof d)) return;
    if (std::memcmp(d + 1, "CD001", 5) != 0) {
      if (std::memcmp(d + 1, "BEA01", 5) == 0 || std::memcmp(d + 1, "NSR02", 5) == 0 || std::memcmp(d + 1, "NSR03", 5) == 0) continue;
      return;
    }
    if (d[0] == 255) return;
    if (d[0] != 1) continue;  // Primary Volume Descriptor だけ
    o->label = sanitize_bytes(d + 40, 32);
    bool digits = true;
    for (int k = 0; k < 16; k++)
      if (d[813 + k] < '0' || d[813 + k] > '9') digits = false;
    if (digits) {
      char b[32];
      std::snprintf(b, sizeof b, "%.4s-%.2s-%.2s-%.2s-%.2s-%.2s-%.2s", d + 813, d + 817, d + 819, d + 821, d + 823,
                    d + 825, d + 827);
      o->uuid = b;
    }
    return;
  }
}

// UDF の dstring (固定長フィールド、最後のバイトが長さ)。
std::string udf_dstring(const uint8_t* p, size_t fieldlen) {
  if (fieldlen < 2) return {};
  size_t len = p[fieldlen - 1];
  if (len == 0 || len > fieldlen - 1) return {};
  std::string o;
  if (p[0] == 8) {
    for (size_t i = 1; i < len; i++) append_utf8(o, p[i]);
  } else if (p[0] == 16) {
    o = utf16_to_utf8(p + 1, (len - 1) / 2, true);
  } else {
    return {};
  }
  return sanitize_bytes(reinterpret_cast<const uint8_t*>(o.data()), o.size());
}

bool udf_tag_ok(const uint8_t* t, uint16_t* id, uint32_t* loc) {
  uint8_t sum = 0;
  for (int i = 0; i < 16; i++)
    if (i != 4) sum = static_cast<uint8_t>(sum + t[i]);
  if (sum != t[4]) return false;
  *id = le16(t);
  *loc = le32(t + 12);
  return true;
}

bool label_udf(Reader& r, FsLabelInfo* o) {
  for (uint32_t bs : {2048u, 512u, 4096u, 1024u}) {
    std::vector<uint8_t> b(bs);
    if (!r.rd(256ull * bs, b.data(), bs)) continue;
    uint16_t id; uint32_t loc;
    if (!udf_tag_ok(b.data(), &id, &loc) || id != 2 || loc != 256) continue;
    uint32_t vlen = le32(b.data() + 16), vloc = le32(b.data() + 20);
    uint32_t nsec = vlen / bs;
    if (nsec == 0 || nsec > 64) nsec = nsec ? 64 : 0;
    std::string lvd_label, pvd_label, vset;
    for (uint32_t i = 0; i < nsec; i++) {
      if (!r.rd((static_cast<uint64_t>(vloc) + i) * bs, b.data(), bs)) break;
      if (!udf_tag_ok(b.data(), &id, &loc)) continue;
      if (id == 1 && bs >= 200) { pvd_label = udf_dstring(b.data() + 24, 32); vset = udf_dstring(b.data() + 72, 128); }
      else if (id == 6 && bs >= 212) lvd_label = udf_dstring(b.data() + 84, 128);
      else if (id == 8) break;
    }
    o->label = !lvd_label.empty() ? lvd_label : pvd_label;
    if (vset.size() >= 16) o->uuid = vset.substr(0, 16);
    return true;
  }
  return false;
}

// ------------------------------------------------------------------ その他

void label_luks(const uint8_t* h, FsLabelInfo* o) {
  uint16_t ver = be16(h + 6);
  if (ver == 2) o->label = sanitize_bytes(h + 24, 48);
  size_t n = 0;
  while (n < 40 && h[168 + n]) n++;
  std::string u(reinterpret_cast<const char*>(h + 168), n);
  for (auto& c : u)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || c == '-')) { u.clear(); break; }
  o->uuid = u;
}

}  // namespace

Status read_fs_label(BlockDevice& dev, uint64_t offset, uint64_t length, FsType fs, FsLabelInfo* out) {
  if (!out) return Status::Make(Errc::InvalidArgument);
  *out = FsLabelInfo{};
  Status io = Status::Ok();
  Reader r{dev, offset, length, &io};
  std::vector<uint8_t> head(4096);
  size_t hn = static_cast<size_t>(length < head.size() ? length : head.size());
  if (hn < 512) return Status::Ok();
  if (!r.rd(0, head.data(), hn)) return io;
  const uint8_t* s = head.data();

  switch (fs) {
    case FsType::Ntfs: label_ntfs(r, s, out); break;
    case FsType::Fat12: case FsType::Fat16: case FsType::Fat32: label_fat(r, s, fs, out); break;
    case FsType::Exfat: label_exfat(r, s, out); break;
    case FsType::Ext2: case FsType::Ext3: case FsType::Ext4: label_ext(r, out); break;
    case FsType::Iso9660: label_iso(r, out); break;
    case FsType::Udf: label_udf(r, out); break;
    case FsType::Luks: if (hn >= 208) label_luks(s, out); break;
    case FsType::Btrfs: {
      uint8_t sb[512];
      if (r.rd(0x10000, sb, sizeof sb) && std::memcmp(sb + 64, "_BHRfS_M", 8) == 0) {
        if (!all_zero(sb + 32, 16)) out->uuid = uuid_str(sb + 32);
        uint8_t lb[256];
        if (r.rd(0x10000 + 299, lb, sizeof lb)) out->label = sanitize_bytes(lb, sizeof lb);
      }
      break;
    }
    case FsType::Xfs:
      if (hn >= 120 && std::memcmp(s, "XFSB", 4) == 0) {
        if (!all_zero(s + 32, 16)) out->uuid = uuid_str(s + 32);
        out->label = sanitize_bytes(s + 108, 12);
      }
      break;
    case FsType::Squashfs: case FsType::Erofs: case FsType::Unknown: {
      // squashfs / erofs: fs_probe が分類済みでも未分類でも、魔法値で判別してラベル/UUID を読む (squashfs にはラベルがない)
      uint8_t sb[128];
      if (std::memcmp(s, "hsqs", 4) == 0) out->detected = "squashfs";
      else if (r.rd(1024, sb, sizeof sb) && le32(sb) == 0xE0F5E1E2u) {
        out->detected = "erofs";
        if (!all_zero(sb + 48, 16)) out->uuid = uuid_str(sb + 48);
        out->label = sanitize_bytes(sb + 64, 16);
      } else if (std::memcmp(s, "LUKS\xba\xbe", 6) == 0 && hn >= 208) { out->detected = "luks"; label_luks(s, out); }
      break;
    }
    default: break;
  }
  if (!io.ok()) return io;
  return Status::Ok();
}

}  // namespace usbmem
