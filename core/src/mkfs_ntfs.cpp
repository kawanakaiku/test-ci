// SPDX-License-Identifier: Apache-2.0
// NTFS (3.1) の mkfs。実験的。書き込む内容を組み立てるだけ。NTFS のオンディスク形式 (公開されている文書: Linux-NTFS の "NTFS Documentation" など) から自前で書く。
// ntfs-3g / mkntfs / Linux カーネルのコードは使わない (参照用に mkntfs が作ったイメージの「構造」を外から見て確認はした)。
//
// 作るもの: $MFT (0) / $MFTMirr (1) / $LogFile (2) / $Volume (3) / $AttrDef (4) / ルート (5) / $Bitmap (6) / $Boot (7) / $BadClus (8) / $Secure (9) / $UpCase (10) / $Extend (11) /
//   予約 (12〜23) / $Quota (24) / $ObjId (25) / $Reparse (26)。バックアップのブートセクタは最後のセクタ。
//   $LogFile は 0xFF で埋める (リセット済みのログ。Windows / ntfs-3g はこれを「空のログ」として受け入れ、初回のマウントで作り直す)。
//   セキュリティ記述子は $Secure ($SDS / $SDH / $SII) に 3 つ (0x100 = システムファイル、0x101 = $Secure と $Extend、0x102 = ルート)。
//   $UsnJrnl は作らない (Windows が必要なときに作る)。
#include <algorithm>
#include <cstdio>
#include <map>

#include "mkfs_internal.h"
#include "usbmem/partition_edit.h"  // make_random_guid

namespace usbmem {
namespace mkfsimpl {

namespace {

#include "mkfs_upcase.inc"

constexpr uint64_t kFiletimeEpoch = 11644473600ull;   // 1601-01-01 から 1970-01-01 までの秒

std::vector<uint16_t> expand_upcase() {
  std::vector<uint16_t> t(65536);
  for (uint32_t i = 0; i < 65536; i++) t[i] = static_cast<uint16_t>(i);
  size_t idx = 0;
  const size_t n = sizeof(kUpcaseCompressed) / sizeof(kUpcaseCompressed[0]);
  for (size_t i = 0; i < n; i++) {
    if (kUpcaseCompressed[i] == 0xFFFF && i + 1 < n) { idx += kUpcaseCompressed[i + 1]; i++; }
    else if (idx < 65536) t[idx++] = kUpcaseCompressed[i];
  }
  return t;
}

bool utf8_to_utf16(const std::string& s, std::vector<uint16_t>* out) {
  for (size_t i = 0; i < s.size();) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    uint32_t cp;
    int n;
    if (c < 0x80) { cp = c; n = 0; }
    else if ((c >> 5) == 6) { cp = c & 0x1F; n = 1; }
    else if ((c >> 4) == 14) { cp = c & 0x0F; n = 2; }
    else if ((c >> 3) == 30) { cp = c & 0x07; n = 3; }
    else return false;
    if (i + static_cast<size_t>(n) >= s.size() + (n ? 0 : 1) && n) return false;
    for (int k = 1; k <= n; k++) {
      if (i + static_cast<size_t>(k) >= s.size()) return false;
      unsigned char d = static_cast<unsigned char>(s[i + static_cast<size_t>(k)]);
      if ((d >> 6) != 2) return false;
      cp = (cp << 6) | (d & 0x3F);
    }
    static const uint32_t minv[4] = {0, 0x80, 0x800, 0x10000};
    if (cp < minv[n] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
    i += static_cast<size_t>(n) + 1;
    if (cp >= 0x10000) {
      cp -= 0x10000;
      out->push_back(static_cast<uint16_t>(0xD800 + (cp >> 10)));
      out->push_back(static_cast<uint16_t>(0xDC00 + (cp & 0x3FF)));
    } else out->push_back(static_cast<uint16_t>(cp));
  }
  return true;
}

std::vector<uint16_t> ascii16(const char* s) {
  std::vector<uint16_t> v;
  for (; *s; s++) v.push_back(static_cast<uint8_t>(*s));
  return v;
}

void put_le(std::vector<uint8_t>& v, size_t at, uint64_t val, int n) {
  for (int i = 0; i < n; i++) v[at + static_cast<size_t>(i)] = static_cast<uint8_t>(val >> (8 * i));
}
void push_le(std::vector<uint8_t>& v, uint64_t val, int n) {
  for (int i = 0; i < n; i++) v.push_back(static_cast<uint8_t>(val >> (8 * i)));
}
void pad8(std::vector<uint8_t>& v) { while (v.size() % 8) v.push_back(0); }

// 符号つきの最小バイト数 (mapping pairs の長さ / 差分の符号化)
int signed_bytes(int64_t v) {
  int n = 1;
  while (n < 8) {
    int64_t lo = -(int64_t(1) << (8 * n - 1)), hi = (int64_t(1) << (8 * n - 1)) - 1;
    if (v >= lo && v <= hi) break;
    n++;
  }
  return n;
}

struct Run { int64_t lcn; uint64_t len; };   // lcn < 0 = 疎

std::vector<uint8_t> mapping_pairs(const std::vector<Run>& runs) {
  std::vector<uint8_t> o;
  int64_t prev = 0;
  for (auto& r : runs) {
    int ln = signed_bytes(static_cast<int64_t>(r.len));
    int64_t delta = r.lcn < 0 ? 0 : r.lcn - prev;
    int on = r.lcn < 0 ? 0 : signed_bytes(delta);
    o.push_back(static_cast<uint8_t>(ln | (on << 4)));
    push_le(o, r.len, ln);
    if (on) { push_le(o, static_cast<uint64_t>(delta), on); prev = r.lcn; }
  }
  o.push_back(0);
  return o;
}

// MFT レコードの組み立て
struct Rec {
  uint32_t rs = 1024;
  std::vector<uint8_t> b;
  size_t pos = 0;
  uint16_t inst = 0;
  uint32_t num = 0;
  void init(uint32_t n, uint16_t seq, uint16_t flags, uint16_t links, uint32_t recsize) {
    rs = recsize;
    num = n;
    b.assign(rs, 0);
    std::memcpy(&b[0], "FILE", 4);
    uint16_t usa_cnt = static_cast<uint16_t>(1 + rs / 512);
    put_le(b, 4, 0x30, 2);
    put_le(b, 6, usa_cnt, 2);
    put_le(b, 16, seq, 2);
    put_le(b, 18, links, 2);
    size_t ao = align_up(0x30 + 2u * usa_cnt, 8);
    put_le(b, 20, ao, 2);
    put_le(b, 22, flags, 2);
    put_le(b, 28, rs, 4);
    put_le(b, 44, n, 4);
    pos = ao;
    inst = 0;
  }
  void add(std::vector<uint8_t> a) {
    pad8(a);
    put_le(a, 4, a.size(), 4);
    put_le(a, 14, inst++, 2);
    if (pos + a.size() + 8 > rs) { overflow = true; return; }
    std::memcpy(&b[pos], a.data(), a.size());
    pos += a.size();
  }
  bool overflow = false;
  // 常駐属性
  void resident(uint32_t type, const std::vector<uint16_t>& name, const std::vector<uint8_t>& value, uint8_t rflags = 0) {
    std::vector<uint8_t> a(24, 0);
    put_le(a, 0, type, 4);
    a[9] = static_cast<uint8_t>(name.size());
    put_le(a, 10, 24, 2);
    for (uint16_t c : name) push_le(a, c, 2);
    pad8(a);
    size_t vo = a.size();
    put_le(a, 16, value.size(), 4);
    put_le(a, 20, vo, 2);
    a[22] = rflags;
    a.insert(a.end(), value.begin(), value.end());
    add(std::move(a));
  }
  // 非常駐属性
  void nonresident(uint32_t type, const std::vector<uint16_t>& name, const std::vector<Run>& runs, uint64_t vcns, uint64_t alloc, uint64_t data, uint64_t init, uint16_t aflags = 0) {
    std::vector<uint8_t> a(64, 0);
    put_le(a, 0, type, 4);
    a[8] = 1;
    a[9] = static_cast<uint8_t>(name.size());
    put_le(a, 10, 64, 2);
    put_le(a, 12, aflags, 2);
    for (uint16_t c : name) push_le(a, c, 2);
    pad8(a);
    size_t mp = a.size();
    std::vector<uint8_t> m = mapping_pairs(runs);
    a.insert(a.end(), m.begin(), m.end());
    put_le(a, 16, 0, 8);
    put_le(a, 24, vcns ? vcns - 1 : 0, 8);
    put_le(a, 32, mp, 2);
    put_le(a, 40, alloc, 8);
    put_le(a, 48, data, 8);
    put_le(a, 56, init, 8);
    add(std::move(a));
  }
  std::vector<uint8_t> finish() {
    // 終端
    put_le(b, pos, 0xFFFFFFFFu, 4);
    put_le(b, 24, pos + 8, 4);
    put_le(b, 40, inst, 2);
    apply_usa(b, 0x30, 4);
    return b;
  }
  // 更新シーケンス配列 (512 バイトごとの末尾 2 バイトを USN と入れ替える)
  static void apply_usa(std::vector<uint8_t>& v, size_t usa_off, uint16_t usn) {
    size_t cnt = 1 + v.size() / 512;
    put_le(v, usa_off, usn, 2);
    for (size_t i = 1; i < cnt; i++) {
      size_t end = i * 512;
      v[usa_off + 2 * i] = v[end - 2];
      v[usa_off + 2 * i + 1] = v[end - 1];
      put_le(v, end - 2, usn, 2);
    }
  }
};

uint64_t mref(uint64_t n, uint64_t seq) { return n | (seq << 48); }

struct Times { uint64_t t; };

std::vector<uint8_t> std_info(uint64_t ft, uint32_t fattr, uint32_t sec_id) {
  std::vector<uint8_t> v(72, 0);
  for (int i = 0; i < 4; i++) put_le(v, static_cast<size_t>(8 * i), ft, 8);
  put_le(v, 32, fattr, 4);
  put_le(v, 52, sec_id, 4);
  return v;
}

std::vector<uint8_t> file_name_value(uint64_t parent, uint64_t ft, uint64_t alloc, uint64_t data, uint32_t fattr, const std::vector<uint16_t>& name) {
  std::vector<uint8_t> v(66, 0);
  put_le(v, 0, parent, 8);
  for (int i = 0; i < 4; i++) put_le(v, static_cast<size_t>(8 + 8 * i), ft, 8);
  put_le(v, 40, alloc, 8);
  put_le(v, 48, data, 8);
  put_le(v, 56, fattr, 4);
  v[64] = static_cast<uint8_t>(name.size());
  v[65] = 3;   // Win32 と DOS 共通
  for (uint16_t c : name) push_le(v, c, 2);
  return v;
}

// ファイル名インデックスのエントリ (8 バイト整列)
std::vector<uint8_t> fn_entry(uint64_t ref, const std::vector<uint8_t>& fnv, uint16_t flags = 0, uint64_t vcn = 0) {
  std::vector<uint8_t> e(16, 0);
  put_le(e, 0, ref, 8);
  put_le(e, 10, fnv.size(), 2);
  e.insert(e.end(), fnv.begin(), fnv.end());
  pad8(e);
  if (flags & 1) push_le(e, vcn, 8);
  put_le(e, 8, e.size(), 2);
  put_le(e, 12, flags, 2);
  return e;
}
std::vector<uint8_t> end_entry(bool has_sub = false, uint64_t vcn = 0) {
  std::vector<uint8_t> e(16, 0);
  uint16_t fl = 2;
  if (has_sub) { fl |= 1; push_le(e, vcn, 8); }
  put_le(e, 8, e.size(), 2);
  put_le(e, 12, fl, 2);
  return e;
}

// INDEX_ROOT の値: type (0x30 or 0)、照合規則、インデックスブロックの大きさ、エントリの並び (終端を含む)
std::vector<uint8_t> index_root(uint32_t attr_type, uint32_t collation, uint32_t ib_size, uint8_t cpb, const std::vector<uint8_t>& entries, bool large) {
  std::vector<uint8_t> v(32, 0);
  put_le(v, 0, attr_type, 4);
  put_le(v, 4, collation, 4);
  put_le(v, 8, ib_size, 4);
  v[12] = cpb;
  put_le(v, 16, 16, 4);                         // エントリの開始 (インデックスヘッダ先頭から)
  put_le(v, 20, 16 + entries.size(), 4);        // 使用バイト数
  put_le(v, 24, 16 + entries.size(), 4);        // 確保バイト数
  v[28] = large ? 1 : 0;
  v.insert(v.end(), entries.begin(), entries.end());
  return v;
}

// ビュー用のエントリ (キー + データ)。キーの後ろにデータを置く
std::vector<uint8_t> view_entry(const std::vector<uint8_t>& key, const std::vector<uint8_t>& data, bool pad_ii = false) {
  std::vector<uint8_t> e(16, 0);
  size_t koff = 16;
  e.insert(e.end(), key.begin(), key.end());
  size_t doff = e.size();
  // データの位置は 8 バイト境界ではなくキーの直後 (4 バイト整列)
  while (doff % 4) { e.push_back(0); doff++; }
  e.insert(e.end(), data.begin(), data.end());
  if (pad_ii) { push_le(e, 0x00490049, 4); }
  pad8(e);
  put_le(e, 0, doff, 2);
  put_le(e, 2, data.size(), 2);
  put_le(e, 8, e.size(), 2);
  put_le(e, 10, key.size(), 2);
  (void)koff;
  return e;
}

// セキュリティ記述子 (自己相対)
std::vector<uint8_t> sid(uint8_t auth, std::initializer_list<uint32_t> sub) {
  std::vector<uint8_t> s = {1, static_cast<uint8_t>(sub.size()), 0, 0, 0, 0, 0, auth};
  for (uint32_t x : sub) push_le(s, x, 4);
  return s;
}
struct Ace { std::vector<uint8_t> s; uint32_t mask; uint8_t flags; };
std::vector<uint8_t> make_sd(const std::vector<Ace>& aces, const std::vector<uint8_t>& owner, const std::vector<uint8_t>& group) {
  std::vector<uint8_t> acl(8, 0);
  acl[0] = 2;
  for (auto& a : aces) {
    std::vector<uint8_t> e = {0, a.flags, 0, 0};
    push_le(e, a.mask, 4);
    e.insert(e.end(), a.s.begin(), a.s.end());
    put_le(e, 2, e.size(), 2);
    acl.insert(acl.end(), e.begin(), e.end());
  }
  put_le(acl, 2, acl.size(), 2);
  put_le(acl, 4, aces.size(), 2);
  std::vector<uint8_t> sd(20, 0);
  sd[0] = 1;
  put_le(sd, 2, 0x8004, 2);
  put_le(sd, 16, 20, 4);   // DACL
  sd.insert(sd.end(), acl.begin(), acl.end());
  put_le(sd, 4, sd.size(), 4);   // owner
  sd.insert(sd.end(), owner.begin(), owner.end());
  put_le(sd, 8, sd.size(), 4);   // group
  sd.insert(sd.end(), group.begin(), group.end());
  return sd;
}
uint32_t sd_hash(const std::vector<uint8_t>& sd) {
  uint32_t h = 0;
  for (size_t i = 0; i + 4 <= sd.size(); i += 4) h = ((h << 3) | (h >> 29)) + g32(&sd[i]);
  return h;
}

}  // namespace

uint64_t ntfs_default_cluster(uint64_t bytes, uint32_t sector) {
  uint64_t c = bytes < (7ull << 20) ? 512 : 4096;
  if (bytes > (16ull << 40)) c = 8192;
  if (bytes > (32ull << 40)) c = 16384;
  if (bytes > (64ull << 40)) c = 32768;
  if (bytes > (128ull << 40)) c = 65536;
  return std::max<uint64_t>(c, sector);
}

void build_ntfs(const Params& p, Build* b) {
  const uint32_t S = p.sector_size;
  if (S < 512 || S > 4096 || !pow2(S)) { b->error("bad-sector-size", "the sector size must be 512, 1024, 2048 or 4096"); return; }
  const uint64_t V = p.length / S;
  if (V < 4) { b->error("too-small", "the volume is too small for NTFS"); return; }
  uint64_t cs = p.opt.cluster_size ? p.opt.cluster_size : ntfs_default_cluster(V * S, S);
  if (!pow2(cs) || cs < S || cs < 512 || cs > 65536) { b->error("bad-cluster-size", "the NTFS cluster size must be a power of two from 512 bytes (and at least the sector size) to 64 KiB"); return; }
  const uint64_t spc = cs / S;
  if (spc > 128) { b->error("bad-cluster-size", "too many sectors per cluster"); return; }
  const uint64_t ncl = (V - 1) / spc;   // バックアップのブートセクタは最後のセクタ (BPB の総セクタ数 = V - 1)
  if (ncl > 0xFFFFFFFEull) { b->error("too-large", "NTFS cannot have more than 2^32 - 2 clusters; use a larger cluster size (at most 64 KiB, so about 256 TiB)"); return; }
  std::vector<uint16_t> label;
  if (!utf8_to_utf16(p.opt.label, &label) || label.size() > 32) { b->error("bad-label", "the NTFS label must be valid UTF-8 and at most 32 UTF-16 characters"); return; }
  for (uint16_t c : label) if (c < 0x20 || c == '\\' || c == '/' || c == '"' || c == '*' || c == '?' || c == '<' || c == '>' || c == '|' || c == ':') { b->error("bad-label", "the label has a character NTFS does not allow in a volume name"); return; }

  const uint32_t rs = std::max<uint32_t>(1024, S);       // MFT レコードの大きさ (4Kn は 4096)
  const uint32_t ibs = std::max<uint32_t>(4096, S);      // インデックスブロック
  auto cl = [&](uint64_t bytes) { return ceil_div(bytes, cs); };
  const uint64_t ft = (p.now + kFiletimeEpoch) * 10000000ull;

  auto id = make_random_guid(p.opt.seed);
  if (!p.opt.uuid.empty() && !parse_uuid(p.opt.uuid, id.data())) { b->error("bad-uuid", "the volume serial must look like xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx (the first 8 bytes are used)"); return; }
  uint64_t serial = 0;
  for (int i = 7; i >= 0; i--) serial = (serial << 8) | id[static_cast<size_t>(i)];

  // ---- 大きさの見積り
  const uint32_t kSys = 27;   // 使用するレコード (0〜26)
  const uint64_t mft_alloc_clusters = ceil_div(static_cast<uint64_t>(kSys) * rs, cs);
  const uint32_t kRecs = static_cast<uint32_t>(mft_alloc_clusters * cs / rs);   // 確保した領域に入るだけ全部のレコードを作る (残りは未使用)
  const uint64_t mft_bytes = static_cast<uint64_t>(kRecs) * rs;
  const uint64_t mftbm_bytes = align_up(ceil_div(kRecs, 8), 8);
  const uint64_t bm_bytes = align_up(ceil_div(ncl, 8), 8);
  const uint64_t log_bytes = std::min<uint64_t>(64ull << 20, std::max<uint64_t>(256ull << 10, align_up(V * S / 32, 64ull << 10)));
  const uint64_t attrdef_bytes = 2560;
  const uint64_t upcase_bytes = 131072;
  // セキュリティ記述子
  auto admins = sid(5, {32, 544}), system = sid(5, {18}), everyone = sid(1, {0}), authusers = sid(5, {11}), users = sid(5, {32, 545});
  std::vector<std::vector<uint8_t>> sds_list = {
      make_sd({{everyone, 0x00120089, 0}, {admins, 0x00120089, 0}}, admins, admins),
      make_sd({{everyone, 0x0012019F, 0}, {admins, 0x0012019F, 0}}, admins, admins),
      make_sd({{system, 0x001F01FF, 3}, {admins, 0x001F01FF, 3}, {authusers, 0x001301BF, 3}, {users, 0x001200A9, 3}}, admins, system)};
  struct SdsEnt { uint32_t hash, id; uint64_t off; uint32_t len; };
  std::vector<SdsEnt> sdse;
  std::vector<uint8_t> sds_first;   // $SDS の先頭 (最初のコピー)
  for (size_t i = 0; i < sds_list.size(); i++) {
    SdsEnt e;
    e.hash = sd_hash(sds_list[i]);
    e.id = 0x100 + static_cast<uint32_t>(i);
    e.off = sds_first.size();
    e.len = static_cast<uint32_t>(20 + sds_list[i].size());
    std::vector<uint8_t> en(20, 0);
    put_le(en, 0, e.hash, 4);
    put_le(en, 4, e.id, 4);
    put_le(en, 8, e.off, 8);
    put_le(en, 16, e.len, 4);
    en.insert(en.end(), sds_list[i].begin(), sds_list[i].end());
    sds_first.insert(sds_first.end(), en.begin(), en.end());
    while (sds_first.size() % 16) sds_first.push_back(0);
    sdse.push_back(e);
  }
  const uint64_t sds_used = sdse.back().off + sdse.back().len;
  const uint64_t sds_bytes = 0x40000 + sds_used;

  // ---- クラスタの割り当て (先頭から順に)
  struct Alloc { const char* what; uint64_t lcn, n; };
  std::vector<Alloc> allocs;
  uint64_t cur = 0;
  auto take = [&](const char* what, uint64_t n) { Alloc a{what, cur, n}; allocs.push_back(a); cur += n; return a.lcn; };
  const uint64_t boot_lcn = take("boot", cl(8192));
  const uint64_t mftbm_lcn = take("mft-bitmap", 1);
  const uint64_t mft_lcn = take("mft", cl(mft_bytes));
  const uint64_t attrdef_lcn = take("attrdef", cl(attrdef_bytes));
  const uint64_t bitmap_lcn = take("bitmap", cl(bm_bytes));
  const uint64_t upcase_lcn = take("upcase", cl(upcase_bytes));
  const uint64_t log_lcn = take("logfile", cl(log_bytes));
  const uint64_t sds_lcn = take("sds", cl(sds_bytes));
  const uint64_t rootidx_lcn = take("root-index", cl(ibs));
  const uint64_t mirr_n = cl(4ull * rs);
  uint64_t mirr_lcn = ncl / 2;
  if (mirr_lcn < cur) mirr_lcn = cur;
  if (mirr_lcn + mirr_n > ncl) {
    b->error("too-small", "the volume is too small for NTFS with a " + std::to_string(cs) + "-byte cluster (needs about " + std::to_string((cur + mirr_n + 1) * cs / 1024) + " KiB); use a smaller cluster size");
    return;
  }
  allocs.push_back({"mft-mirror", mirr_lcn, mirr_n});
  (void)boot_lcn;

  // ---- $Bitmap の中身 (使用中のクラスタ + 末尾の余白ビット)
  std::map<uint64_t, uint8_t> bm;   // バイト位置 → 値
  auto setbit = [&](uint64_t c) { bm[c / 8] |= static_cast<uint8_t>(1u << (c % 8)); };
  for (auto& a : allocs) for (uint64_t c = a.lcn; c < a.lcn + a.n; c++) setbit(c);
  for (uint64_t c = ncl; c < bm_bytes * 8; c++) setbit(c);

  // ---- 属性・レコード
  const uint64_t root_ref = mref(5, 5);
  auto fa_sys = 0x06u;
  struct Sys { uint32_t n; const char* name; uint64_t alloc, data; uint32_t fa; uint32_t sec; };
  // ルートのインデックス用の FILE_NAME (ファイル名の大文字小文字を無視した並び順)
  const uint64_t mft_alloc = cl(mft_bytes) * cs;
  std::vector<Sys> sys = {
      {4, "$AttrDef", cl(attrdef_bytes) * cs, attrdef_bytes, fa_sys, 0x100},
      {8, "$BadClus", 0, 0, fa_sys, 0x100},
      {6, "$Bitmap", cl(bm_bytes) * cs, bm_bytes, fa_sys, 0x100},
      {7, "$Boot", cl(8192) * cs, 8192, fa_sys, 0x100},
      {11, "$Extend", 0, 0, 0x10000000u | fa_sys, 0x101},
      {2, "$LogFile", cl(log_bytes) * cs, log_bytes, fa_sys, 0x100},
      {0, "$MFT", mft_alloc, mft_bytes, fa_sys, 0x100},
      {1, "$MFTMirr", mirr_n * cs, 4ull * rs, fa_sys, 0x100},
      {9, "$Secure", 0, 0, 0x20000000u | fa_sys, 0x101},
      {10, "$UpCase", cl(upcase_bytes) * cs, upcase_bytes, fa_sys, 0x100},
      {3, "$Volume", 0, 0, fa_sys, 0x100},
  };
  auto seq_of = [](uint32_t n) { return static_cast<uint16_t>(n == 0 ? 1 : n < 24 ? n : 1); };
  auto fnv_of = [&](const Sys& s, uint64_t parent) { return file_name_value(parent, ft, s.alloc, s.data, s.fa, ascii16(s.name)); };
  std::vector<std::vector<uint8_t>> rec(kRecs);
  Rec r;
  auto std_attr = [&](Rec& rr, uint32_t fa, uint32_t sec) { rr.resident(0x10, {}, std_info(ft, fa, sec)); };
  auto sys_fn = [&](Rec& rr, const Sys& s) { rr.resident(0x30, {}, fnv_of(s, root_ref), 1); };
  auto find_sys = [&](uint32_t n) -> const Sys& { for (auto& s : sys) if (s.n == n) return s; return sys[0]; };

  // 0: $MFT
  {
    r.init(0, seq_of(0), 1, 1, rs);
    std_attr(r, fa_sys, 0x100);
    sys_fn(r, find_sys(0));
    r.nonresident(0x80, {}, {{static_cast<int64_t>(mft_lcn), cl(mft_bytes)}}, cl(mft_bytes), mft_alloc, mft_bytes, mft_bytes);
    r.nonresident(0xB0, {}, {{static_cast<int64_t>(mftbm_lcn), 1}}, 1, cs, mftbm_bytes, mftbm_bytes);
    rec[0] = r.finish();
  }
  // 1: $MFTMirr
  {
    r.init(1, seq_of(1), 1, 1, rs);
    std_attr(r, fa_sys, 0x100);
    sys_fn(r, find_sys(1));
    r.nonresident(0x80, {}, {{static_cast<int64_t>(mirr_lcn), mirr_n}}, mirr_n, mirr_n * cs, 4ull * rs, 4ull * rs);
    rec[1] = r.finish();
  }
  // 2: $LogFile
  {
    r.init(2, seq_of(2), 1, 1, rs);
    std_attr(r, fa_sys, 0x100);
    sys_fn(r, find_sys(2));
    r.nonresident(0x80, {}, {{static_cast<int64_t>(log_lcn), cl(log_bytes)}}, cl(log_bytes), cl(log_bytes) * cs, log_bytes, log_bytes);
    rec[2] = r.finish();
  }
  // 3: $Volume
  {
    r.init(3, seq_of(3), 1, 1, rs);
    std_attr(r, fa_sys, 0x100);
    sys_fn(r, find_sys(3));
    if (!label.empty()) { std::vector<uint8_t> v; for (uint16_t c : label) push_le(v, c, 2); r.resident(0x60, {}, v); }
    std::vector<uint8_t> vi(12, 0);
    vi[8] = 3; vi[9] = 1;   // NTFS 3.1
    r.resident(0x70, {}, vi);
    r.resident(0x80, {}, {});
    rec[3] = r.finish();
  }
  // 4: $AttrDef
  {
    r.init(4, seq_of(4), 1, 1, rs);
    std_attr(r, fa_sys, 0x100);
    sys_fn(r, find_sys(4));
    r.nonresident(0x80, {}, {{static_cast<int64_t>(attrdef_lcn), cl(attrdef_bytes)}}, cl(attrdef_bytes), cl(attrdef_bytes) * cs, attrdef_bytes, attrdef_bytes);
    rec[4] = r.finish();
  }
  // 5: ルートディレクトリ ($I30: 索引ブロック 1 つに全エントリ)
  std::vector<uint8_t> root_indx;
  {
    std::vector<uint8_t> ents;
    for (auto& s : sys) { auto e = fn_entry(mref(s.n, seq_of(s.n)), fnv_of(s, root_ref)); ents.insert(ents.end(), e.begin(), e.end()); }
    {
      Sys dot{5, ".", 0, 0, 0x10000000u | fa_sys, 0x102};
      auto e = fn_entry(root_ref, fnv_of(dot, root_ref));
      ents.insert(ents.end(), e.begin(), e.end());
    }
    auto e = end_entry();
    ents.insert(ents.end(), e.begin(), e.end());
    // INDX ブロック
    root_indx.assign(ibs, 0);
    std::memcpy(&root_indx[0], "INDX", 4);
    uint16_t ucnt = static_cast<uint16_t>(1 + ibs / 512);
    put_le(root_indx, 4, 0x28, 2);
    put_le(root_indx, 6, ucnt, 2);
    size_t es = align_up(0x28 + 2u * ucnt, 8);
    put_le(root_indx, 0x18, es - 0x18, 4);
    put_le(root_indx, 0x1C, es - 0x18 + ents.size(), 4);
    put_le(root_indx, 0x20, ibs - 0x18, 4);
    if (es + ents.size() > ibs) { b->error("internal-layout", "the root directory index does not fit in one block"); return; }
    std::memcpy(&root_indx[es], ents.data(), ents.size());
    Rec::apply_usa(root_indx, 0x28, 1);

    r.init(5, seq_of(5), 3, 1, rs);
    r.resident(0x10, {}, std_info(ft, 0x26, 0x102));
    r.resident(0x30, {}, file_name_value(root_ref, ft, 0, 0, 0x10000000u | fa_sys, {'.'}), 1);
    std::vector<uint8_t> sub = end_entry(true, 0);
    uint8_t cpb = static_cast<uint8_t>(ibs >= cs ? ibs / cs : ibs / S);
    r.resident(0x90, ascii16("$I30"), index_root(0x30, 1, ibs, cpb, sub, true));
    r.nonresident(0xA0, ascii16("$I30"), {{static_cast<int64_t>(rootidx_lcn), cl(ibs)}}, cl(ibs), cl(ibs) * cs, ibs, ibs);
    r.resident(0xB0, ascii16("$I30"), {1, 0, 0, 0, 0, 0, 0, 0});
    rec[5] = r.finish();
  }
  // 6: $Bitmap
  {
    r.init(6, seq_of(6), 1, 1, rs);
    std_attr(r, fa_sys, 0x100);
    sys_fn(r, find_sys(6));
    r.nonresident(0x80, {}, {{static_cast<int64_t>(bitmap_lcn), cl(bm_bytes)}}, cl(bm_bytes), cl(bm_bytes) * cs, bm_bytes, bm_bytes);
    rec[6] = r.finish();
  }
  // 7: $Boot
  {
    r.init(7, seq_of(7), 1, 1, rs);
    std_attr(r, fa_sys, 0x100);
    sys_fn(r, find_sys(7));
    r.nonresident(0x80, {}, {{0, cl(8192)}}, cl(8192), cl(8192) * cs, 8192, 8192);
    rec[7] = r.finish();
  }
  // 8: $BadClus ($Bad は全体が疎)
  {
    r.init(8, seq_of(8), 1, 1, rs);
    std_attr(r, fa_sys, 0x100);
    sys_fn(r, find_sys(8));
    r.resident(0x80, {}, {});
    r.nonresident(0x80, ascii16("$Bad"), {{-1, ncl}}, ncl, ncl * cs, ncl * cs, 0);
    rec[8] = r.finish();
  }
  // 9: $Secure ($SDS / $SDH / $SII)
  {
    std::vector<SdsEnt> by_hash = sdse;
    std::sort(by_hash.begin(), by_hash.end(), [](const SdsEnt& a, const SdsEnt& c) { return a.hash != c.hash ? a.hash < c.hash : a.id < c.id; });
    auto data_of = [](const SdsEnt& e) { std::vector<uint8_t> d(20, 0); put_le(d, 0, e.hash, 4); put_le(d, 4, e.id, 4); put_le(d, 8, e.off, 8); put_le(d, 16, e.len, 4); return d; };
    std::vector<uint8_t> sdh, sii;
    for (auto& e : by_hash) {
      std::vector<uint8_t> key(8, 0);
      put_le(key, 0, e.hash, 4);
      put_le(key, 4, e.id, 4);
      auto en = view_entry(key, data_of(e), true);
      sdh.insert(sdh.end(), en.begin(), en.end());
    }
    for (auto& e : sdse) {
      std::vector<uint8_t> key(4, 0);
      put_le(key, 0, e.id, 4);
      auto en = view_entry(key, data_of(e));
      sii.insert(sii.end(), en.begin(), en.end());
    }
    auto ee = end_entry();
    sdh.insert(sdh.end(), ee.begin(), ee.end());
    sii.insert(sii.end(), ee.begin(), ee.end());
    r.init(9, seq_of(9), 9, 1, rs);
    std_attr(r, 0x20000000u | fa_sys, 0x101);
    sys_fn(r, find_sys(9));
    r.nonresident(0x80, ascii16("$SDS"), {{static_cast<int64_t>(sds_lcn), cl(sds_bytes)}}, cl(sds_bytes), cl(sds_bytes) * cs, sds_bytes, sds_bytes);
    uint8_t cpb = static_cast<uint8_t>(ibs >= cs ? ibs / cs : ibs / S);
    r.resident(0x90, ascii16("$SDH"), index_root(0, 0x12, ibs, cpb, sdh, false));
    r.resident(0x90, ascii16("$SII"), index_root(0, 0x10, ibs, cpb, sii, false));
    rec[9] = r.finish();
  }
  // 10: $UpCase
  {
    r.init(10, seq_of(10), 1, 1, rs);
    std_attr(r, fa_sys, 0x100);
    sys_fn(r, find_sys(10));
    r.nonresident(0x80, {}, {{static_cast<int64_t>(upcase_lcn), cl(upcase_bytes)}}, cl(upcase_bytes), cl(upcase_bytes) * cs, upcase_bytes, upcase_bytes);
    rec[10] = r.finish();
  }
  // 24..26: $Quota / $ObjId / $Reparse ($Extend の索引に載る)
  struct Child { uint32_t n; const char* name; };
  std::vector<Child> kids = {{25, "$ObjId"}, {24, "$Quota"}, {26, "$Reparse"}};   // 名前順
  const uint64_t ext_ref = mref(11, 11);
  uint8_t cpb_ibs = static_cast<uint8_t>(ibs >= cs ? ibs / cs : ibs / S);
  auto kid_fnv = [&](const Child& k) { return file_name_value(ext_ref, ft, 0, 0, 0x20000000u | 0x26u, ascii16(k.name)); };
  {
    // $Quota: $O (SID → 所有者 ID) と $Q (所有者 ID → 制限)。既定の 2 エントリ (所有者 ID 1 = 既定の制限、0x100 = Administrators)
    std::vector<uint8_t> o_idx, q_idx;
    {
      auto adm = sid(5, {32, 544});
      std::vector<uint8_t> data(4, 0);
      put_le(data, 0, 0x100, 4);
      auto en = view_entry(adm, data);
      o_idx.insert(o_idx.end(), en.begin(), en.end());
      auto ee = end_entry();
      o_idx.insert(o_idx.end(), ee.begin(), ee.end());
    }
    auto qdata = [&](uint32_t, const std::vector<uint8_t>& s) {
      std::vector<uint8_t> d(48, 0);
      put_le(d, 0, 2, 4);              // バージョン
      put_le(d, 4, 1, 4);   // 既定の制限を使う
      put_le(d, 16, ft, 8);
      put_le(d, 24, ~0ull, 8);         // 警告しきい値 = 無制限
      put_le(d, 32, ~0ull, 8);         // 上限 = 無制限
      d.insert(d.end(), s.begin(), s.end());
      return d;
    };
    {
      std::vector<uint8_t> key(4, 0);
      put_le(key, 0, 1, 4);
      auto en = view_entry(key, qdata(1, {}));
      q_idx.insert(q_idx.end(), en.begin(), en.end());
      put_le(key, 0, 0x100, 4);
      en = view_entry(key, qdata(0x100, sid(5, {32, 544})));
      q_idx.insert(q_idx.end(), en.begin(), en.end());
      auto ee = end_entry();
      q_idx.insert(q_idx.end(), ee.begin(), ee.end());
    }
    r.init(24, 1, 0x0D, 1, rs);
    r.resident(0x10, {}, std_info(ft, 0x20000026u, 0x101));
    r.resident(0x30, {}, kid_fnv(kids[1]), 1);
    r.resident(0x90, ascii16("$O"), index_root(0, 0x11, ibs, cpb_ibs, o_idx, false));
    r.resident(0x90, ascii16("$Q"), index_root(0, 0x10, ibs, cpb_ibs, q_idx, false));
    rec[24] = r.finish();
    // $ObjId: $O (オブジェクト ID → ファイル)
    r.init(25, 1, 0x0D, 1, rs);
    r.resident(0x10, {}, std_info(ft, 0x20000026u, 0x101));
    r.resident(0x30, {}, kid_fnv(kids[0]), 1);
    r.resident(0x90, ascii16("$O"), index_root(0, 0x13, ibs, cpb_ibs, end_entry(), false));
    rec[25] = r.finish();
    // $Reparse: $R
    r.init(26, 1, 0x0D, 1, rs);
    r.resident(0x10, {}, std_info(ft, 0x20000026u, 0x101));
    r.resident(0x30, {}, kid_fnv(kids[2]), 1);
    r.resident(0x90, ascii16("$R"), index_root(0, 0x13, ibs, cpb_ibs, end_entry(), false));
    rec[26] = r.finish();
  }
  // 11: $Extend
  {
    std::vector<uint8_t> ents;
    for (auto& k : kids) { auto e = fn_entry(mref(k.n, 1), kid_fnv(k)); ents.insert(ents.end(), e.begin(), e.end()); }
    auto ee = end_entry();
    ents.insert(ents.end(), ee.begin(), ee.end());
    r.init(11, seq_of(11), 3, 1, rs);
    r.resident(0x10, {}, std_info(ft, fa_sys, 0x101));
    r.resident(0x30, {}, fnv_of(find_sys(11), root_ref), 1);
    r.resident(0x90, ascii16("$I30"), index_root(0x30, 1, ibs, cpb_ibs, ents, false));
    rec[11] = r.finish();
  }
  // 12..15: 予約 (使用中・空の $DATA)、16..23: 未使用 (標準情報だけ)
  for (uint32_t n = 12; n <= 23; n++) {
    r.init(n, seq_of(n), n <= 15 ? 1 : 0, 0, rs);
    r.resident(0x10, {}, std_info(ft, fa_sys, 0x100));
    if (n <= 15) r.resident(0x80, {}, {});
    rec[n] = r.finish();
  }
  for (uint32_t n = kSys; n < kRecs; n++) { r.init(n, 0, 0, 0, rs); rec[n] = r.finish(); }
  for (uint32_t n = 0; n < kRecs; n++) if (rec[n].size() != rs) { b->error("internal-layout", "an MFT record was not built"); return; }
  if (r.overflow) { b->error("internal-layout", "an MFT record overflowed"); return; }

  // ---- ブートセクタ
  std::vector<uint8_t> boot(S, 0);
  boot[0] = 0xEB; boot[1] = 0x52; boot[2] = 0x90;
  std::memcpy(&boot[3], "NTFS    ", 8);
  put_le(boot, 11, S, 2);
  boot[13] = static_cast<uint8_t>(spc);
  boot[21] = 0xF8;
  put_le(boot, 24, 63, 2);
  put_le(boot, 26, 255, 2);
  uint32_t hidden = p.opt.hidden_sectors != 0xFFFFFFFFu ? p.opt.hidden_sectors : static_cast<uint32_t>(p.opt.offset / S);
  put_le(boot, 28, hidden, 4);
  put_le(boot, 36, 0x00800080, 4);
  put_le(boot, 40, V - 1, 8);
  put_le(boot, 48, mft_lcn, 8);
  put_le(boot, 56, mirr_lcn, 8);
  boot[64] = rs >= cs ? static_cast<uint8_t>(rs / cs) : static_cast<uint8_t>(256 - ilog2(rs));
  boot[68] = static_cast<uint8_t>(ibs >= cs ? ibs / cs : 256 - ilog2(ibs));
  put_le(boot, 72, serial, 8);
  boot[0x54] = 0xFA; boot[0x55] = 0xF4; boot[0x56] = 0xEB; boot[0x57] = 0xFD;   // 起動しない (cli; hlt; jmp $-1)
  boot[510] = 0x55; boot[511] = 0xAA;
  if (S > 512) { boot[S - 2] = 0x55; boot[S - 1] = 0xAA; }
  boot[510] = 0x55; boot[511] = 0xAA;

  // ---- 書く領域 (順序: 本体 → ミラー・控え → 最後に主ブートセクタ)
  const std::string st_data = "ntfs-metadata", st_mirror = "mft-mirror", st_backup = "backup-boot-sector", st_boot = "boot-sector";
  b->stage(st_data);
  b->stage(st_mirror);
  b->stage(st_backup);
  b->stage(st_boot);
  auto at = [&](uint64_t lcn) { return lcn * cs; };
  // $MFT: 27 レコード + 残りのゼロ
  {
    std::vector<uint8_t> all;
    for (auto& rc : rec) all.insert(all.end(), rc.begin(), rc.end());
    b->data(st_data, at(mft_lcn), std::move(all), "mft-records");
  }
  // MFT のビットマップ (0〜15 と 24〜26 が使用中)
  {
    std::vector<uint8_t> v(static_cast<size_t>(cs), 0);
    for (uint32_t n = 0; n < kSys; n++) if (n < 16 || n >= 24) v[n / 8] |= static_cast<uint8_t>(1u << (n % 8));   // 16〜23 は予約だが未使用 (mkntfs と同じ)
    b->data(st_data, at(mftbm_lcn), std::move(v), "mft-bitmap");
  }
  // $MFTMirr: レコード 0〜3
  {
    std::vector<uint8_t> v;
    for (int i = 0; i < 4; i++) v.insert(v.end(), rec[static_cast<size_t>(i)].begin(), rec[static_cast<size_t>(i)].end());
    b->data(st_mirror, at(mirr_lcn), std::move(v), "mft-mirror");
    b->zero(st_mirror, at(mirr_lcn) + 4ull * rs, mirr_n * cs - 4ull * rs, "mft-mirror-tail");
  }
  // $AttrDef
  {
    struct Def { const char* name; uint32_t type, flags; uint64_t minv, maxv; };
    const uint64_t M = ~0ull;
    std::vector<Def> defs = {
        {"$STANDARD_INFORMATION", 0x10, 0x40, 0x30, 0x48}, {"$ATTRIBUTE_LIST", 0x20, 0x80, 0, M},          {"$FILE_NAME", 0x30, 0x42, 0x44, 0x242},
        {"$OBJECT_ID", 0x40, 0x40, 0, 0x100},              {"$SECURITY_DESCRIPTOR", 0x50, 0x80, 0, M},     {"$VOLUME_NAME", 0x60, 0x40, 0x2, 0x100},
        {"$VOLUME_INFORMATION", 0x70, 0x40, 0xC, 0xC},     {"$DATA", 0x80, 0, 0, M},                       {"$INDEX_ROOT", 0x90, 0x40, 0, M},
        {"$INDEX_ALLOCATION", 0xA0, 0x80, 0, M},           {"$BITMAP", 0xB0, 0x80, 0, M},                  {"$REPARSE_POINT", 0xC0, 0x80, 0, 0x4000},
        {"$EA_INFORMATION", 0xD0, 0x40, 0x8, 0x8},         {"$EA", 0xE0, 0, 0, 0x10000},                   {"$PROPERTY_SET", 0xF0, 0x40, 0, M},
        {"$LOGGED_UTILITY_STREAM", 0x100, 0x80, 0, 0x10000}};
    std::vector<uint8_t> v(attrdef_bytes, 0);
    for (size_t i = 0; i < defs.size(); i++) {
      size_t o = i * 160;
      for (size_t k = 0; defs[i].name[k]; k++) put_le(v, o + 2 * k, static_cast<uint8_t>(defs[i].name[k]), 2);
      put_le(v, o + 128, defs[i].type, 4);
      put_le(v, o + 136, defs[i].type == 0x30 ? 0x1 : 0, 4);   // 照合規則: $FILE_NAME だけ COLLATION_FILE_NAME
      put_le(v, o + 140, defs[i].flags, 4);
      put_le(v, o + 144, defs[i].minv, 8);
      put_le(v, o + 152, defs[i].maxv, 8);
    }
    b->data(st_data, at(attrdef_lcn), std::move(v), "attrdef");
    b->zero(st_data, at(attrdef_lcn) + attrdef_bytes, cl(attrdef_bytes) * cs - attrdef_bytes, "attrdef-tail");
  }
  // $Bitmap: 使用中のバイトを書き、あいだはゼロ
  {
    uint64_t pos = 0;
    auto it = bm.begin();
    while (it != bm.end()) {
      uint64_t s = it->first;
      std::vector<uint8_t> run;
      while (it != bm.end() && it->first == s + run.size()) { run.push_back(it->second); ++it; }
      if (s > pos) b->zero(st_data, at(bitmap_lcn) + pos, s - pos, "bitmap-free");
      uint64_t n = run.size();
      b->data(st_data, at(bitmap_lcn) + s, std::move(run), "bitmap-used");
      pos = s + n;
    }
    uint64_t alloc_end = cl(bm_bytes) * cs;
    if (pos < alloc_end) b->zero(st_data, at(bitmap_lcn) + pos, alloc_end - pos, "bitmap-free");
  }
  // $UpCase
  {
    auto up = expand_upcase();
    std::vector<uint8_t> v(upcase_bytes, 0);
    for (size_t i = 0; i < 65536; i++) put_le(v, 2 * i, up[i], 2);
    b->data(st_data, at(upcase_lcn), std::move(v), "upcase");
    b->zero(st_data, at(upcase_lcn) + upcase_bytes, cl(upcase_bytes) * cs - upcase_bytes, "upcase-tail");
  }
  // $LogFile: 0xFF 埋め
  {
    MkfsRegion lr;
    lr.offset = at(log_lcn);
    lr.length = log_bytes;
    lr.fill = 0xFF;
    lr.what = "logfile-reset";
    b->stage(st_data).regions.push_back(std::move(lr));
    b->zero(st_data, at(log_lcn) + log_bytes, cl(log_bytes) * cs - log_bytes, "logfile-tail");
  }
  // $SDS (2 つのコピー)
  {
    b->data(st_data, at(sds_lcn), sds_first, "sds");
    b->zero(st_data, at(sds_lcn) + sds_first.size(), 0x40000 - sds_first.size(), "sds-gap");
    std::vector<uint8_t> second(sds_first.begin(), sds_first.begin() + static_cast<std::ptrdiff_t>(sds_used));
    b->data(st_data, at(sds_lcn) + 0x40000, std::move(second), "sds-mirror");
    b->zero(st_data, at(sds_lcn) + sds_bytes, cl(sds_bytes) * cs - sds_bytes, "sds-tail");
  }
  // ルートのインデックスブロック
  b->data(st_data, at(rootidx_lcn), root_indx, "root-index-block");
  if (cl(ibs) * cs > ibs) b->zero(st_data, at(rootidx_lcn) + ibs, cl(ibs) * cs - ibs, "root-index-tail");
  // $Boot の 2 つ目以降 (ゼロ)
  if (cl(8192) * cs > S) b->zero(st_data, S, cl(8192) * cs - S, "boot-rest");
  // バックアップのブートセクタ (最後のセクタ)
  b->data(st_backup, (V - 1) * S, boot, "backup-boot-sector");
  b->data(st_boot, 0, boot, "boot-sector");

  auto n = [](uint64_t v) { return std::to_string(v); };
  char ser[20];
  std::snprintf(ser, sizeof ser, "%016llX", static_cast<unsigned long long>(serial));
  b->layout = {{"sectorSize", n(S)}, {"clusterSize", n(cs)}, {"sectorsPerCluster", n(spc)}, {"clusterCount", n(ncl)}, {"totalSectors", n(V - 1)},
               {"mftLcn", n(mft_lcn)}, {"mftMirrorLcn", n(mirr_lcn)}, {"mftRecordSize", n(rs)}, {"indexBlockSize", n(ibs)}, {"logFileBytes", n(log_bytes)},
               {"ntfsVersion", "3.1"}, {"volumeSerial", ser}};
  if ((V - 1) % spc) b->warn("unused-tail", n(((V - 1) % spc) * S) + " bytes before the backup boot sector do not fill a whole cluster and are not used");
  if (p.length > V * S) b->warn("unused-tail", n(p.length - V * S) + " bytes at the end of the range do not fill a whole sector and are not used");
  b->warn("experimental", "NTFS creation is experimental: it was checked with ntfs-3g and this app's drivers, not with Windows chkdsk");
  if (bm_bytes > (256ull << 20)) b->warn("slow", "the cluster bitmap is " + n(bm_bytes >> 20) + " MiB of zeros; writing it can take a long time. Use a larger cluster size to make it shorter");
  b->changes.push_back("create an NTFS file system (experimental): " + n(ncl) + " clusters of " + n(cs) + " bytes, MFT record " + n(rs) + " bytes");
}

}  // namespace mkfsimpl
}  // namespace usbmem
