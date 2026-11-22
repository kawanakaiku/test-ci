// SPDX-License-Identifier: Apache-2.0
// パーティション探索の FS 検出器。512 バイトのチャンクにシグネチャ (アンカー) が見つかったら「パーティションの開始位置の仮説」を作り、
// verify で FS の構造 (ヘッダの整合・大きさ・バックアップ) を確かめて Hit にする。Linux / 他のプロジェクトのコードは使っていない。
// オンディスク形式は各 FS の公開仕様 (Microsoft FAT / exFAT 仕様、NTFS の BPB、ext2/3/4・XFS・Btrfs・F2FS・squashfs・erofs・ISO 9660・ECMA-167・LUKS の公開された形式) から起こした。
#include <algorithm>

#include "partition_scan_internal.h"
#include "usbmem/crc32.h"
#include "usbmem/sha256.h"

namespace usbmem {
namespace ps {

bool Reader::read(uint64_t off, void* buf, size_t n) {
  if (!fatal.ok() || (cancel_ && cancel_->cancelled())) return false;
  if (!range_ok(disk_, off, n)) return false;
  Status st = dev_.read_at(off, buf, n);
  if (!st.ok()) {
    io_errors++;
    if (st.code == Errc::Disconnected || st.code == Errc::Closed) fatal = st;
    return false;
  }
  bytes_read += n;
  return true;
}

int family_of(FsType t) {
  switch (t) {
    case FsType::Fat12: case FsType::Fat16: case FsType::Fat32: return 1;
    case FsType::Ext2: case FsType::Ext3: case FsType::Ext4: return 2;
    case FsType::Iso9660: case FsType::Udf: return 3;
    default: return 100 + static_cast<int>(t);
  }
}

namespace {

constexpr uint64_t kBtrfsMirror = 0x4000000ull;
constexpr uint64_t kIsoPvd = 0x8000;

void note(Hit* h, const char* n) { h->notes.push_back(n); }

// ------------------------------------------------------------------ アンカー

bool plausible_bps(uint32_t b) { return b >= 512 && b <= 4096 && is_pow2(b); }

void push(std::vector<Hyp>* out, uint64_t start, uint32_t ss, Kind k, uint64_t aux = 0) {
  if (start % ss) return;
  out->push_back({start, k, aux});
}

}  // namespace

void match_anchors(const uint8_t* c, uint64_t addr, uint32_t ss, std::vector<Hyp>* out) {
  const bool sig = c[510] == 0x55 && c[511] == 0xAA;
  if (sig) {
    uint32_t bps = le16(c + 11);
    if (std::memcmp(c + 3, "NTFS    ", 8) == 0) {
      push(out, addr, ss, Kind::Ntfs);
      uint64_t total = le64(c + 0x28);
      uint64_t tail = 0;
      if (plausible_bps(bps) && total && total < (1ull << 48) && mul_ok(total, bps, &tail) && tail <= addr) push(out, addr - tail, ss, Kind::Ntfs, tail);
    } else if (std::memcmp(c + 3, "-FVE-FS-", 8) == 0) {
      push(out, addr, ss, Kind::Bitlocker);
    } else if ((c[0] == 0xEB || c[0] == 0xE9) && plausible_bps(bps) && c[16] && c[16] <= 2) {
      push(out, addr, ss, Kind::Fat);
      uint32_t bk = le16(c + 50);
      if (le16(c + 17) == 0 && le16(c + 22) == 0 && bk >= 1 && bk <= 64 && static_cast<uint64_t>(bk) * bps <= addr)
        push(out, addr - static_cast<uint64_t>(bk) * bps, ss, Kind::Fat);
    }
  }
  if (std::memcmp(c + 3, "EXFAT   ", 8) == 0) {
    push(out, addr, ss, Kind::Exfat);
    if (c[108] >= 9 && c[108] <= 12) {
      uint64_t bk = 12ull << c[108];
      if (bk <= addr) push(out, addr - bk, ss, Kind::Exfat);
    }
  }
  if (std::memcmp(c, "XFSB", 4) == 0) push(out, addr, ss, Kind::Xfs);
  if (std::memcmp(c, "LUKS\xba\xbe", 6) == 0) push(out, addr, ss, Kind::Luks);
  if (std::memcmp(c, "SKUL\xba\xbe", 6) == 0) {
    uint64_t off = be64(c + 256);
    if (off >= 16384 && off <= (4u << 20) && off <= addr) push(out, addr - off, ss, Kind::Luks, off);
  }
  if (std::memcmp(c, "hsqs", 4) == 0) push(out, addr, ss, Kind::Squashfs);
  uint32_t w0 = le32(c);
  if (w0 == 0xE0F5E1E2u && addr >= 1024) push(out, addr - 1024, ss, Kind::Erofs);
  if (w0 == 0xF2F52010u) {
    if (addr >= 1024) push(out, addr - 1024, ss, Kind::F2fs);
    if (addr >= 1024 + 4096) push(out, addr - 1024 - 4096, ss, Kind::F2fs);
  }
  if (le16(c + 56) == 0xEF53) {
    uint32_t lbs = le32(c + 24);
    if (lbs <= 6) {
      uint64_t bs = 1024ull << lbs, g = le16(c + 0x5A), bpg = le32(c + 32), fdb = le32(c + 20), off = 1024, blk = 0;
      if (g != 0 && (!mul_ok(g, bpg, &blk) || !add_ok(blk, fdb, &blk) || !mul_ok(blk, bs, &off))) off = UINT64_MAX;
      if (off <= addr) push(out, addr - off, ss, Kind::Ext, g == 0 ? 0 : off);
    }
  }
  if (std::memcmp(c + 0x40, "_BHRfS_M", 8) == 0) {
    uint64_t by = le64(c + 0x30);
    if ((by == 0x10000 || by == kBtrfsMirror || by == 0x4000000000ull) && by <= addr) push(out, addr - by, ss, Kind::Btrfs);
  }
  if (c[0] == 1 && std::memcmp(c + 1, "CD001", 5) == 0 && addr >= kIsoPvd) push(out, addr - kIsoPvd, ss, Kind::Iso);
  if (c[0] == 0 && (std::memcmp(c + 1, "NSR02", 5) == 0 || std::memcmp(c + 1, "NSR03", 5) == 0) && addr >= kIsoPvd + 2048)
    push(out, addr - kIsoPvd - 2048, ss, Kind::Iso);
}

namespace {

// ------------------------------------------------------------------ FAT

}  // namespace

bool parse_fat(const uint8_t* s, Fat* f) {
  if (s[510] != 0x55 || s[511] != 0xAA) return false;
  if (!((s[0] == 0xEB && s[2] == 0x90) || s[0] == 0xE9)) return false;
  Fat b;
  b.bps = le16(s + 11); b.spc = s[13]; b.rsvd = le16(s + 14); b.nfats = s[16]; b.root_ent = le16(s + 17); b.media = s[21];
  b.tot = le16(s + 19) ? le16(s + 19) : le32(s + 32);
  uint32_t fat16 = le16(s + 22);
  if (!plausible_bps(b.bps) || !is_pow2(b.spc) || b.spc > 128 || b.rsvd == 0 || b.nfats == 0 || b.nfats > 2) return false;
  if (b.media != 0xF0 && b.media < 0xF8) return false;
  b.fat32 = b.root_ent == 0 && fat16 == 0;
  b.fatsz = b.fat32 ? le32(s + 36) : fat16;
  if (b.fatsz == 0 || b.tot == 0) return false;
  uint64_t root_secs = (static_cast<uint64_t>(b.root_ent) * 32 + b.bps - 1) / b.bps;
  uint64_t meta = b.rsvd + b.nfats * b.fatsz + root_secs;
  if (meta >= b.tot) return false;
  if (b.fat32) {
    if (le16(s + 42) != 0) return false;  // FS バージョン
    b.root_clus = le32(s + 44); b.fsinfo = le16(s + 48); b.bk = le16(s + 50);
    if (b.root_clus < 2) return false;
    if (b.fsinfo >= b.rsvd && b.fsinfo != 0xFFFF && b.fsinfo != 0) return false;
    if (b.bk >= b.rsvd && b.bk != 0xFFFF && b.bk != 0) return false;
    b.type = FsType::Fat32;
  } else {
    if (b.root_ent == 0) return false;
    uint64_t clusters = (b.tot - meta) / b.spc;
    b.type = clusters < 4085 ? FsType::Fat12 : clusters < 65525 ? FsType::Fat16 : FsType::Fat32;
    if (b.type == FsType::Fat32) return false;  // 16 ビットの FAT サイズ・ルートエントリを持つ FAT32 は無い
  }
  *f = b;
  return true;
}

namespace {

bool fat_first_entry_ok(const uint8_t* e, uint32_t media) { return e[0] == media && e[1] == 0xFF && e[2] == 0xFF; }

bool verify_fat(Reader& rd, uint64_t S, uint32_t ss, Hit* hit) {
  uint8_t s[512];
  Fat f;
  bool prim = rd.read(S, s, 512) && parse_fat(s, &f);
  Hit h;
  h.start = S;
  if (!prim) {
    // FAT32 のバックアップブートセクタ (セクタ 6)。先頭が壊れた FS を探す。
    uint32_t cands[2] = {512, ss};
    for (int i = 0; i < (ss == 512 ? 1 : 2); i++) {
      uint64_t off = S + 6ull * cands[i];
      uint8_t b[512];
      if (!rd.read(off, b, 512) || !parse_fat(b, &f) || !f.fat32 || f.bps != cands[i] || f.bk != 6) continue;
      uint8_t e[16];
      if (!rd.read(S + static_cast<uint64_t>(f.rsvd) * f.bps, e, 16) || !fat_first_entry_ok(e, f.media)) return false;
      h.fs = FsType::Fat32;
      h.size = f.tot * f.bps;
      h.size_known = true;
      h.score = 62;
      h.backup_ok = true;
      note(&h, "primary-damaged");
      note(&h, "found-by-backup");
      *hit = h;
      return true;
    }
    return false;
  }
  h.fs = f.type;
  h.size = f.tot * f.bps;
  h.size_known = true;
  h.primary_ok = true;
  uint8_t e[16];
  bool ok1 = rd.read(S + static_cast<uint64_t>(f.rsvd) * f.bps, e, 16) && fat_first_entry_ok(e, f.media);
  bool ok2 = false;
  if (f.nfats == 2) {
    uint8_t e2[16];
    ok2 = rd.read(S + (f.rsvd + f.fatsz) * f.bps, e2, 16) && fat_first_entry_ok(e2, f.media);
  }
  bool fat_ok = ok1 || ok2;
  int score = 50;
  if (fat_ok) score += 30;
  else { score = 45; note(&h, "fat-first-entry-mismatch"); }
  if (f.fat32) {
    uint32_t bk = (f.bk >= 1 && f.bk != 0xFFFF) ? f.bk : 0;
    if (bk) {
      // これが別の FS のバックアップブートセクタなら、その FS の側で採用する
      if (S >= static_cast<uint64_t>(bk) * f.bps) {
        uint8_t o[512];
        Fat g;
        if (rd.read(S - static_cast<uint64_t>(bk) * f.bps, o, 512) && parse_fat(o, &g) && g.fat32 && g.bps == f.bps && g.tot == f.tot && g.rsvd == f.rsvd) return false;
      }
      uint8_t b[512];
      Fat g;
      if (rd.read(S + static_cast<uint64_t>(bk) * f.bps, b, 512) && parse_fat(b, &g) && g.fat32 && g.tot == f.tot && g.rsvd == f.rsvd && g.fatsz == f.fatsz) {
        h.backup_ok = true;
        score += 10;
      } else {
        note(&h, "backup-boot-damaged");
      }
    }
    if (f.fsinfo >= 1 && f.fsinfo != 0xFFFF) {
      uint8_t fi[512];
      if (rd.read(S + static_cast<uint64_t>(f.fsinfo) * f.bps, fi, 512) && le32(fi) == 0x41615252u && le32(fi + 484) == 0x61417272u && le32(fi + 508) == 0xAA550000u) score += 5;
    }
  } else if (f.nfats == 2) {
    uint8_t a[512], b2[512];
    if (rd.read(S + static_cast<uint64_t>(f.rsvd) * f.bps, a, 512) && rd.read(S + (f.rsvd + f.fatsz) * f.bps, b2, 512) && std::memcmp(a, b2, 512) == 0) score += 5;
  }
  h.score = score;
  *hit = h;
  return true;
}

// ------------------------------------------------------------------ exFAT

}  // namespace

bool parse_exfat(const uint8_t* s, Exfat* x) {
  if (std::memcmp(s + 3, "EXFAT   ", 8) != 0 || s[510] != 0x55 || s[511] != 0xAA) return false;
  if (!all_zero(s + 11, 53)) return false;
  uint32_t sh = s[108], sp = s[109];
  if (sh < 9 || sh > 12 || sp > 25 || sh + sp > 25) return false;
  if (s[110] < 1 || s[110] > 2) return false;
  if ((le16(s + 104) >> 8) != 1) return false;
  uint64_t vol = le64(s + 72);
  uint32_t fat_off = le32(s + 80), fat_len = le32(s + 84), heap_off = le32(s + 88), clusters = le32(s + 92), root = le32(s + 96);
  if (vol < 16 || vol >= (1ull << 48) || fat_off < 24 || fat_len == 0 || heap_off <= fat_off || clusters == 0 || root < 2 || root >= clusters + 2) return false;
  x->bps = 1u << sh;
  x->vol_len = vol;
  return true;
}

uint32_t exfat_boot_checksum(const uint8_t* r, uint32_t bps) {
  uint32_t cs = 0;
  for (size_t i = 0; i < 11ull * bps; i++) {
    if (i == 106 || i == 107 || i == 112) continue;
    cs = ((cs & 1) ? 0x80000000u : 0u) + (cs >> 1) + r[i];
  }
  return cs;
}

namespace {

// 12 セクタの領域 (ブート領域) の整合: セクタ 0 の構造、セクタ 11 のチェックサム
bool exfat_region_ok(Reader& rd, uint64_t base, uint32_t bps, Exfat* x, bool* checksum_ok) {
  std::vector<uint8_t> r(12ull * bps);
  if (!rd.read(base, r.data(), r.size())) return false;
  Exfat t;
  if (!parse_exfat(r.data(), &t) || t.bps != bps) return false;
  if (x) *x = t;
  uint32_t cs = exfat_boot_checksum(r.data(), bps);
  bool ok = true;
  for (size_t i = 0; i < bps; i += 4)
    if (le32(&r[11ull * bps + i]) != cs) { ok = false; break; }
  *checksum_ok = ok;
  return true;
}

bool verify_exfat(Reader& rd, uint64_t S, uint32_t, Hit* hit) {
  Hit h;
  h.start = S;
  uint8_t s[512];
  Exfat x, xb;
  bool p_struct = false, p_cs = false, b_struct = false, b_cs = false;
  uint32_t bps = 0;
  if (rd.read(S, s, 512) && parse_exfat(s, &x)) {
    bps = x.bps;
    p_struct = exfat_region_ok(rd, S, bps, &x, &p_cs);
  }
  if (p_struct) {
    b_struct = exfat_region_ok(rd, S + 12ull * bps, bps, &xb, &b_cs);
  } else {
    // 先頭が壊れている: バックアップ領域 (セクタ 12 から) を、ありうるセクタサイズで調べる
    for (uint32_t b = 512; b <= 4096 && !b_struct; b <<= 1)
      if (exfat_region_ok(rd, S + 12ull * b, b, &xb, &b_cs)) { b_struct = true; bps = b; x = xb; }
  }
  if (!p_struct && !b_struct) return false;
  h.fs = FsType::Exfat;
  h.size = x.vol_len * bps;
  h.size_known = true;
  int score = 55;
  if (p_struct) {
    h.primary_ok = p_cs;
    if (p_cs) score += 25; else note(&h, "exfat-checksum-bad");
  } else {
    note(&h, "primary-damaged");
    note(&h, "found-by-backup");
  }
  if (b_struct && b_cs) { h.backup_ok = true; score += 10; }
  else if (p_struct) note(&h, "backup-boot-damaged");
  if (!p_cs && !(b_struct && b_cs)) score = 45;
  h.score = score;
  *hit = h;
  return true;
}

// ------------------------------------------------------------------ NTFS

}  // namespace

bool parse_ntfs(const uint8_t* s, Ntfs* n) {
  if (std::memcmp(s + 3, "NTFS    ", 8) != 0 || s[510] != 0x55 || s[511] != 0xAA) return false;
  uint32_t bps = le16(s + 11), spc = s[13];
  if (!plausible_bps(bps)) return false;
  if (spc >= 0xF4) spc = 1u << (256 - spc);  // 負の値: 2^-n × ... (大きなクラスタ。1 << (256-v))
  if (!spc || !is_pow2(spc) || spc > 4096) return false;
  if (le16(s + 14) != 0 || s[16] != 0 || le16(s + 17) != 0 || le16(s + 19) != 0 || le16(s + 22) != 0 || le32(s + 32) != 0) return false;
  uint64_t total = le64(s + 0x28), mft = le64(s + 0x30), mirr = le64(s + 0x38);
  if (total < 16 || total >= (1ull << 48)) return false;
  uint64_t clusters = total / spc;
  if (mft >= clusters || mirr >= clusters) return false;
  int8_t rec = static_cast<int8_t>(s[0x40]);
  if (rec > 0 ? !is_pow2(static_cast<uint8_t>(rec)) : (-rec < 9 || -rec > 20)) return false;
  n->bps = bps; n->spc = spc; n->total = total; n->mft = mft; n->mirr = mirr;
  return true;
}

namespace {

bool verify_ntfs(Reader& rd, uint64_t S, uint64_t aux, Hit* hit) {
  uint8_t s[512];
  Ntfs n;
  Hit h;
  h.start = S;
  h.fs = FsType::Ntfs;
  bool prim = rd.read(S, s, 512) && parse_ntfs(s, &n);
  if (prim) {
    h.primary_ok = true;
    uint64_t tail_off = n.total * n.bps;
    uint8_t t[512];
    Ntfs m;
    h.size = tail_off + n.bps;
    h.size_known = true;
    int score = 60;
    if (rd.read(S + tail_off, t, 512) && parse_ntfs(t, &m) && m.total == n.total && m.bps == n.bps && m.mft == n.mft) { h.backup_ok = true; score += 15; }
    else note(&h, "backup-boot-damaged");
    uint8_t f[4];
    uint64_t mo;
    if (mul_ok(n.mft * n.spc, n.bps, &mo) && rd.read(S + mo, f, 4) && std::memcmp(f, "FILE", 4) == 0) score += 20;
    else note(&h, "ntfs-mft-missing");
    h.score = score;
    *hit = h;
    return true;
  }
  // バックアップだけ (最終セクタ)。aux = 開始から見たバックアップの位置
  if (!aux) return false;
  uint8_t t[512];
  if (!rd.read(S + aux, t, 512) || !parse_ntfs(t, &n) || n.total * n.bps != aux) return false;
  h.size = aux + n.bps;
  h.size_known = true;
  h.backup_ok = true;
  note(&h, "primary-damaged");
  note(&h, "found-by-backup");
  int score = 50;
  uint8_t f[4];
  uint64_t mo;
  if (mul_ok(n.mft * n.spc, n.bps, &mo) && rd.read(S + mo, f, 4) && std::memcmp(f, "FILE", 4) == 0) score += 20;
  h.score = score;
  *hit = h;
  return true;
}

// ------------------------------------------------------------------ ext2/3/4

}  // namespace

bool parse_ext(const uint8_t* sb, ExtSb* e) {
  if (le16(sb + 56) != 0xEF53) return false;
  ExtSb x;
  uint32_t lbs = le32(sb + 24);
  if (lbs > 6) return false;
  x.bs = 1024u << lbs;
  x.bpg = le32(sb + 32); x.ipg = le32(sb + 40); x.fdb = le32(sb + 20); x.group = le16(sb + 0x5A);
  x.compat = le32(sb + 0x5C); x.incompat = le32(sb + 0x60); x.ro = le32(sb + 0x64); x.state = le16(sb + 58);
  x.blocks = le32(sb + 4);
  if (x.incompat & 0x80) x.blocks |= static_cast<uint64_t>(le32(sb + 0x150)) << 32;
  bool bigalloc = x.ro & 0x200;
  if (x.bpg == 0 || x.bpg % 8 || x.ipg == 0 || x.ipg > 8u * x.bs) return false;
  if (!bigalloc && (x.bpg > 8u * x.bs || le32(sb + 36) != x.bpg)) return false;
  if (!bigalloc && x.fdb != (x.bs == 1024 ? 1u : 0u)) return false;
  if (x.state & ~7u) return false;
  if (le32(sb + 76) > 1) return false;  // rev_level
  if (le32(sb + 76) == 1) {
    uint32_t isz = le16(sb + 88);
    if (!is_pow2(isz) || isz < 128 || isz > x.bs || le32(sb + 84) < 11) return false;
  }
  if (x.blocks <= x.fdb || x.blocks >= (1ull << 48)) return false;
  x.groups = (x.blocks - x.fdb + x.bpg - 1) / x.bpg;
  if (x.groups == 0 || x.groups * x.ipg != le32(sb)) return false;
  std::memcpy(x.uuid, sb + 104, 16);
  if (x.ro & 0x400) {  // metadata_csum
    x.csum_present = true;
    x.csum_ok = ps::crc32c_ext_sb(sb) == le32(sb + 0x3FC);
  }
  *e = x;
  return true;
}

FsType ext_type(const ExtSb& e) {
  if (e.incompat & (0x40 | 0x80 | 0x200 | 0x400 | 0x1000)) return FsType::Ext4;
  if ((e.compat & ~0x3Fu) || (e.incompat & ~0x2u & ~0x4u) || (e.ro & ~0x7u)) return FsType::Ext4;
  if (e.compat & 0x4) return FsType::Ext3;
  return FsType::Ext2;
}

uint64_t ext_sb_pos(uint64_t group, const ExtSb& e) { return group == 0 ? 1024 : (group * e.bpg + e.fdb) * e.bs; }

namespace {

bool verify_ext(Reader& rd, uint64_t S, uint64_t aux, Hit* hit) {
  uint8_t sb[1024];
  ExtSb e;
  Hit h;
  h.start = S;
  bool prim = rd.read(S + 1024, sb, 1024) && parse_ext(sb, &e) && e.group == 0;
  bool any = prim;
  if (!prim) {
    // バックアップ (グループ 1 = ブロックサイズごとの位置、または見つけた位置) からスーパーブロックを読む
    std::vector<uint64_t> pos;
    if (aux) pos.push_back(aux);
    for (uint64_t bs = 1024; bs <= 4096; bs <<= 1) pos.push_back((8 * bs + (bs == 1024 ? 1 : 0)) * bs);
    for (uint64_t p : pos) {
      if (!rd.read(S + p, sb, 1024) || !parse_ext(sb, &e) || e.group == 0 || ext_sb_pos(e.group, e) != p) continue;
      any = true;
      break;
    }
    if (!any) return false;
    note(&h, "primary-damaged");
    note(&h, "found-by-backup");
  } else {
    h.primary_ok = true;
  }
  h.fs = ext_type(e);
  h.size = e.blocks * e.bs;
  h.size_known = true;
  int score = 70;
  bool uuid_nz = false;
  for (uint8_t b : e.uuid) uuid_nz = uuid_nz || b;
  if (uuid_nz) score += 5;
  if (e.csum_present) {
    if (e.csum_ok) score += 10;
    else { score -= 25; note(&h, "sb-checksum-bad"); }
  }
  if (prim && e.groups >= 2) {
    uint8_t b2[1024];
    ExtSb o;
    if (rd.read(S + ext_sb_pos(1, e), b2, 1024) && parse_ext(b2, &o) && o.group == 1 && o.blocks == e.blocks && o.ipg == e.ipg && std::memcmp(o.uuid, e.uuid, 16) == 0) {
      h.backup_ok = true;
      score += 10;
    } else {
      note(&h, "backup-sb-missing");
    }
  } else if (!prim) {
    h.backup_ok = true;
  }
  h.score = score;
  *hit = h;
  return true;
}

// ------------------------------------------------------------------ XFS

struct Xfs { uint32_t bs = 0, agblocks = 0, agcount = 0; uint64_t dblocks = 0; };

bool parse_xfs(const uint8_t* s, Xfs* x) {
  if (std::memcmp(s, "XFSB", 4) != 0) return false;
  uint32_t bs = be32(s + 4);
  uint64_t dblocks = be64(s + 8);
  uint32_t agblocks = be32(s + 84), agcount = be32(s + 88);
  uint32_t sect = be16(s + 102), isz = be16(s + 104), ver = be16(s + 100) & 0xF;
  if (!is_pow2(bs) || bs < 512 || bs > 65536 || (1u << s[120]) != bs) return false;
  if (!is_pow2(sect) || sect < 512 || sect > 32768 || (1u << s[121]) != sect || sect > bs) return false;
  if (!is_pow2(isz) || isz < 256 || isz > 2048) return false;
  if (ver != 4 && ver != 5) return false;
  if (dblocks == 0 || dblocks >= (1ull << 48) || agblocks == 0 || agcount == 0) return false;
  uint64_t lo = static_cast<uint64_t>(agblocks) * (agcount - 1), hi = lo + agblocks;
  if (dblocks <= lo || dblocks > hi) return false;
  if (s[126] > 1) return false;  // sb_inprogress
  x->bs = bs; x->agblocks = agblocks; x->agcount = agcount; x->dblocks = dblocks;
  return true;
}

bool verify_xfs(Reader& rd, uint64_t S, Hit* hit) {
  uint8_t s[512];
  Xfs x;
  if (!rd.read(S, s, 512) || !parse_xfs(s, &x)) return false;
  uint64_t agbytes = static_cast<uint64_t>(x.agblocks) * x.bs;
  uint8_t o[512];
  if (x.agcount >= 2 && S >= agbytes && rd.read(S - agbytes, o, 512) && std::memcmp(o, "XFSB", 4) == 0 && std::memcmp(o + 32, s + 32, 16) == 0)
    return false;  // 前の AG の先頭にも同じ UUID のスーパーブロックがある → これは AG の予備のコピー
  Hit h;
  h.start = S;
  h.fs = FsType::Xfs;
  h.size = x.dblocks * x.bs;
  h.size_known = true;
  h.primary_ok = true;
  int score = 75;
  if (x.agcount >= 2) {
    if (rd.read(S + agbytes, o, 512) && std::memcmp(o, "XFSB", 4) == 0 && std::memcmp(o + 32, s + 32, 16) == 0) { h.backup_ok = true; score += 10; }
    else note(&h, "backup-sb-missing");
  }
  h.score = score;
  *hit = h;
  return true;
}

// ------------------------------------------------------------------ Btrfs

bool verify_btrfs(Reader& rd, uint64_t S, Hit* hit) {
  static const uint64_t offs[2] = {0x10000, kBtrfsMirror};
  std::vector<uint8_t> sb[2];
  bool ok[2] = {false, false}, csum[2] = {false, false};
  for (int i = 0; i < 2; i++) {
    sb[i].resize(4096);
    if (!rd.read(S + offs[i], sb[i].data(), 4096)) continue;
    const uint8_t* p = sb[i].data();
    if (std::memcmp(p + 0x40, "_BHRfS_M", 8) != 0 || le64(p + 0x30) != offs[i]) continue;
    uint64_t total = le64(p + 0x70);
    uint32_t sect = le32(p + 0x90), node = le32(p + 0x94);
    if (total == 0 || !is_pow2(sect) || sect < 512 || sect > 65536 || !is_pow2(node) || node < 4096 || node > 65536 || node < sect) continue;
    ok[i] = true;
    csum[i] = le16(p + 196) != 0 || le32(p) == ~crc32c_raw(0xFFFFFFFFu, p + 32, 4096 - 32);
  }
  int k = ok[0] ? 0 : ok[1] ? 1 : -1;
  if (k < 0) return false;
  const uint8_t* p = sb[k].data();
  Hit h;
  h.start = S;
  h.fs = FsType::Btrfs;
  h.size = le64(p + 0x70);
  h.size_known = true;
  h.primary_ok = ok[0];
  h.backup_ok = ok[1];
  int score = 65;
  if (csum[k]) score += 20; else note(&h, "sb-checksum-bad");
  if (ok[0] && ok[1]) score += 5;
  if (!ok[0]) { note(&h, "primary-damaged"); note(&h, "found-by-backup"); score -= 5; }
  if (le64(p + 0x88) > 1) note(&h, "multi-device");
  h.score = score;
  *hit = h;
  return true;
}

// ------------------------------------------------------------------ F2FS

bool parse_f2fs(const uint8_t* s, uint64_t* blocks) {
  if (le32(s) != 0xF2F52010u) return false;
  if (le16(s + 4) != 1) return false;
  uint32_t lss = le32(s + 8), lspb = le32(s + 12), lbs = le32(s + 16), lbps = le32(s + 20);
  if (lss < 9 || lss > 12 || lbs != 12 || lspb != lbs - lss || lbps != 9) return false;
  if (le32(s + 24) == 0 || le32(s + 28) == 0) return false;
  uint64_t bc = le64(s + 36);
  if (bc < 64 || bc >= (1ull << 40)) return false;
  *blocks = bc;
  return true;
}

bool verify_f2fs(Reader& rd, uint64_t S, Hit* hit) {
  uint8_t a[512], b[512];
  uint64_t ba = 0, bb = 0;
  bool oka = rd.read(S + 1024, a, 512) && parse_f2fs(a, &ba);
  bool okb = rd.read(S + 4096 + 1024, b, 512) && parse_f2fs(b, &bb);
  if (!oka && !okb) return false;
  Hit h;
  h.start = S;
  h.fs = FsType::F2fs;
  h.size = (oka ? ba : bb) << 12;
  h.size_known = true;
  h.primary_ok = oka;
  h.backup_ok = okb;
  int score = 75;
  if (oka && okb && ba == bb) score += 10;
  if (!oka) { note(&h, "primary-damaged"); note(&h, "found-by-backup"); score -= 10; }
  h.score = score;
  *hit = h;
  return true;
}

// ------------------------------------------------------------------ squashfs / erofs

bool verify_squashfs(Reader& rd, uint64_t S, Hit* hit) {
  uint8_t s[96];
  if (!rd.read(S, s, 96) || std::memcmp(s, "hsqs", 4) != 0) return false;
  uint32_t bs = le32(s + 12), comp = le16(s + 20), log = le16(s + 22);
  if (le16(s + 28) != 4 || le16(s + 30) != 0 || !is_pow2(bs) || bs < 4096 || bs > (1u << 20) || (1u << log) != bs || comp < 1 || comp > 6) return false;
  uint64_t used = le64(s + 40);
  if (used < 96 || used >= (1ull << 50)) return false;
  Hit h;
  h.start = S;
  h.fs = FsType::Squashfs;
  h.size = used;
  h.size_known = true;
  h.size_min = true;  // mksquashfs は 4 KiB に切り上げる。パーティションはさらに大きいことがある
  h.primary_ok = true;
  h.score = 80;
  *hit = h;
  return true;
}

bool verify_erofs(Reader& rd, uint64_t S, Hit* hit) {
  uint8_t s[128];
  if (!rd.read(S + 1024, s, 128) || le32(s) != 0xE0F5E1E2u) return false;
  uint32_t bb = s[12];
  uint64_t blocks = le32(s + 36);
  if (bb < 9 || bb > 16 || blocks == 0 || le32(s + 40) >= blocks) return false;
  Hit h;
  h.start = S;
  h.fs = FsType::Erofs;
  h.size = blocks << bb;
  h.size_known = true;
  h.primary_ok = true;
  h.score = 72;
  *hit = h;
  return true;
}

// ------------------------------------------------------------------ ISO 9660 / UDF

bool udf_tag_ok(const uint8_t* t, uint16_t id) {
  if (le16(t) != id) return false;
  uint8_t sum = 0;
  for (int i = 0; i < 16; i++)
    if (i != 4) sum = static_cast<uint8_t>(sum + t[i]);
  return sum == t[4];
}

// UDF の大きさ: AVDP (セクタ 256) → 主ボリューム記述子列 → パーティション記述子 (開始 + 長さ)
bool udf_size(Reader& rd, uint64_t S, uint64_t* bytes) {
  uint8_t a[2048];
  if (!rd.read(S + 256ull * 2048, a, 2048) || !udf_tag_ok(a, 2)) return false;
  uint32_t len = le32(a + 16), loc = le32(a + 20);
  if (len < 2048 || len > 64 * 2048) return false;
  uint8_t d[2048];
  for (uint32_t i = 0; i < len / 2048 && i < 32; i++) {
    if (!rd.read(S + (static_cast<uint64_t>(loc) + i) * 2048, d, 2048)) return false;
    if (le16(d) == 0) break;
    if (udf_tag_ok(d, 5)) {
      uint64_t end = static_cast<uint64_t>(le32(d + 188)) + le32(d + 192);
      if (end == 0 || end >= (1ull << 32)) return false;
      *bytes = end * 2048;
      return true;
    }
  }
  return false;
}

bool verify_iso(Reader& rd, uint64_t S, Hit* hit) {
  uint8_t d[2048];
  bool pvd = false, udf = false, term = false, bea = false;
  uint64_t iso_bytes = 0;
  for (uint32_t i = 0; i < 24; i++) {
    if (!rd.read(S + kIsoPvd + static_cast<uint64_t>(i) * 2048, d, 2048)) break;
    if (std::memcmp(d + 1, "CD001", 5) == 0 && d[6] == 1) {
      if (d[0] == 1 && !pvd) {
        uint32_t vss = le32(d + 80), lbs = le16(d + 128);
        if (vss != be32(d + 84) || lbs != be16(d + 130) || vss == 0 || (lbs != 512 && lbs != 1024 && lbs != 2048 && lbs != 4096)) return false;
        iso_bytes = static_cast<uint64_t>(vss) * lbs;
        pvd = true;
      } else if (d[0] == 255) {
        term = true;
      }
    } else if (std::memcmp(d + 1, "BEA01", 5) == 0) {
      bea = true;
    } else if (std::memcmp(d + 1, "NSR02", 5) == 0 || std::memcmp(d + 1, "NSR03", 5) == 0) {
      udf = true;
    } else if (std::memcmp(d + 1, "TEA01", 5) != 0) {
      break;
    }
  }
  if (!pvd && !(bea && udf)) return false;
  Hit h;
  h.start = S;
  h.primary_ok = true;
  if (udf) {
    h.fs = FsType::Udf;
    if (pvd) { h.variant = "udf-bridge"; h.size = iso_bytes; h.size_known = true; h.score = 82; }
    else {
      uint64_t b = 0;
      if (udf_size(rd, S, &b)) { h.size = b; h.size_known = true; h.size_min = true; h.score = 80; }
      else { h.score = 60; note(&h, "size-unknown"); }
    }
  } else {
    h.fs = FsType::Iso9660;
    h.size = iso_bytes;
    h.size_known = true;
    h.score = term ? 90 : 78;
  }
  *hit = h;
  return true;
}

// ------------------------------------------------------------------ LUKS

bool luks_text_ok(const uint8_t* p, size_t n) {
  size_t i = 0;
  for (; i < n && p[i]; i++)
    if (p[i] < 0x20 || p[i] > 0x7E) return false;
  return i > 0;
}

bool luks2_hdr_ok(Reader& rd, uint64_t base, uint64_t hdr_size, uint64_t hdr_offset_expected, bool secondary, bool* csum_ok, std::vector<uint8_t>* json) {
  std::vector<uint8_t> h(hdr_size);
  if (!rd.read(base, h.data(), h.size())) return false;
  if (std::memcmp(h.data(), secondary ? "SKUL\xba\xbe" : "LUKS\xba\xbe", 6) != 0 || be16(h.data() + 6) != 2) return false;
  if (be64(h.data() + 8) != hdr_size || be64(h.data() + 256) != hdr_offset_expected) return false;
  uint8_t stored[64];
  std::memcpy(stored, &h[448], 64);
  std::memset(&h[448], 0, 64);
  std::array<uint8_t, 32> d = Sha256::hash(h.data(), h.size());
  *csum_ok = std::memcmp(d.data(), stored, 32) == 0 && std::strncmp(reinterpret_cast<const char*>(&h[72]), "sha256", 7) == 0;
  if (json && hdr_size > 4096) json->assign(h.begin() + 4096, h.end());
  return true;
}

// JSON 領域から segments の最初の暗号セグメントの offset / size を雑に取り出す (size が数字のときだけ大きさになる)
bool luks2_json_size(const std::vector<uint8_t>& js, uint64_t* total) {
  std::string t(reinterpret_cast<const char*>(js.data()), strnlen(reinterpret_cast<const char*>(js.data()), js.size()));
  size_t p = t.find("\"segments\"");
  if (p == std::string::npos) return false;
  auto num = [&](const char* key, size_t from, uint64_t* v) -> int {  // 1 = 数値 / 0 = 数値でない / -1 = 無い
    size_t k = t.find(key, from);
    if (k == std::string::npos) return -1;
    k += std::strlen(key);
    if (k >= t.size()) return -1;
    if (t[k] < '0' || t[k] > '9') return 0;
    uint64_t x = 0;
    int digits = 0;
    while (k < t.size() && t[k] >= '0' && t[k] <= '9' && digits < 18) { x = x * 10 + static_cast<uint64_t>(t[k] - '0'); k++; digits++; }
    *v = x;
    return 1;
  };
  uint64_t off = 0, sz = 0;
  if (num("\"offset\":\"", p, &off) != 1 || num("\"size\":\"", p, &sz) != 1) return false;
  if (off > (1ull << 50) || sz > (1ull << 50)) return false;
  *total = off + sz;
  return true;
}

bool verify_luks(Reader& rd, uint64_t S, uint64_t aux, Hit* hit) {
  uint8_t s[512];
  Hit h;
  h.start = S;
  h.fs = FsType::Luks;
  bool prim = rd.read(S, s, 512) && std::memcmp(s, "LUKS\xba\xbe", 6) == 0;
  if (prim) {
    uint32_t ver = be16(s + 6);
    if (ver == 1) {
      uint32_t kb = be32(s + 108), payload = be32(s + 104);
      if (!luks_text_ok(s + 8, 32) || !luks_text_ok(s + 40, 32) || !luks_text_ok(s + 72, 32) || payload == 0 || (kb != 16 && kb != 24 && kb != 32 && kb != 48 && kb != 64)) return false;
      if (s[168 + 8] != '-' || s[168 + 13] != '-' || s[168 + 18] != '-' || s[168 + 23] != '-') return false;
      h.variant = "luks1";
      h.size = static_cast<uint64_t>(payload) * 512;
      h.size_known = false;
      h.size_min = true;
      h.primary_ok = true;
      h.score = 78;
      *hit = h;
      return true;
    }
    if (ver == 2) {
      uint64_t hs = be64(s + 8);
      if (hs < 16384 || hs > (4u << 20) || !is_pow2(hs) || be64(s + 256) != 0) return false;
      bool cs = false;
      std::vector<uint8_t> js;
      if (!luks2_hdr_ok(rd, S, hs, 0, false, &cs, &js)) return false;
      h.variant = "luks2";
      h.primary_ok = cs;
      int score = cs ? 90 : 55;
      if (!cs) note(&h, "luks-checksum-bad");
      uint64_t total = 0;
      if (luks2_json_size(js, &total)) { h.size = total; h.size_known = true; h.size_min = true; }
      else { h.size = 0; h.size_known = false; }
      bool bcs = false;
      if (luks2_hdr_ok(rd, S + hs, hs, hs, true, &bcs, nullptr) && bcs) { h.backup_ok = true; score += 5; }
      h.score = score > 100 ? 100 : score;
      *hit = h;
      return true;
    }
    return false;
  }
  if (!aux) return false;
  bool bcs = false;
  std::vector<uint8_t> js;
  if (!luks2_hdr_ok(rd, S + aux, aux, aux, true, &bcs, &js) || !bcs) return false;
  h.variant = "luks2";
  h.backup_ok = true;
  h.score = 60;
  note(&h, "primary-damaged");
  note(&h, "found-by-backup");
  uint64_t total = 0;
  if (luks2_json_size(js, &total)) { h.size = total; h.size_known = true; h.size_min = true; }
  *hit = h;
  return true;
}

// ------------------------------------------------------------------ BitLocker

bool verify_bitlocker(Reader& rd, uint64_t S, Hit* hit) {
  uint8_t s[512];
  if (!rd.read(S, s, 512) || std::memcmp(s + 3, "-FVE-FS-", 8) != 0 || s[510] != 0x55 || s[511] != 0xAA) return false;
  uint32_t bps = le16(s + 11), spc = s[13];
  if (!plausible_bps(bps) || !is_pow2(spc) || spc > 128) return false;
  if (le16(s + 14) != 0 || s[16] != 0 || le16(s + 17) != 0 || le16(s + 19) != 0) return false;
  static const uint8_t guid[16] = {0x3b, 0xd6, 0x67, 0x49, 0x29, 0x2e, 0xd8, 0x4a, 0x83, 0x99, 0xf6, 0xa3, 0x39, 0xe3, 0xd0, 0x01};
  Hit h;
  h.start = S;
  h.fs = FsType::BitLocker;
  h.primary_ok = true;
  int score = 60;
  bool win7 = std::memcmp(s + 0xA0, guid, 16) == 0;
  uint64_t total = le64(s + 0x28), bytes = 0;
  if (total >= 16 && total < (1ull << 48) && mul_ok(total, bps, &bytes)) {
    h.size = bytes;
    h.size_known = true;
    h.size_min = true;  // 0x28 の意味 (NTFS と同じ「総数 - 1」か) を実機で確かめられていないので、下限として扱う
    score += 5;
    if (win7) {
      bool offs_ok = true;
      for (int i = 0; i < 3; i++) {
        uint64_t o = le64(s + 0xB0 + 8 * i);
        if (o % bps || o >= bytes) offs_ok = false;
      }
      if (offs_ok) score += 10;
    }
  }
  if (win7) score += 15;
  h.score = score;
  *hit = h;
  return true;
}

}  // namespace

bool verify(Reader& rd, const Hyp& y, uint32_t ss, Hit* hit) {
  switch (y.kind) {
    case Kind::Fat: return verify_fat(rd, y.start, ss, hit);
    case Kind::Exfat: return verify_exfat(rd, y.start, ss, hit);
    case Kind::Ntfs: return verify_ntfs(rd, y.start, y.aux, hit);
    case Kind::Ext: return verify_ext(rd, y.start, y.aux, hit);
    case Kind::Xfs: return verify_xfs(rd, y.start, hit);
    case Kind::Btrfs: return verify_btrfs(rd, y.start, hit);
    case Kind::F2fs: return verify_f2fs(rd, y.start, hit);
    case Kind::Squashfs: return verify_squashfs(rd, y.start, hit);
    case Kind::Erofs: return verify_erofs(rd, y.start, hit);
    case Kind::Iso: return verify_iso(rd, y.start, hit);
    case Kind::Luks: return verify_luks(rd, y.start, y.aux, hit);
    case Kind::Bitlocker: return verify_bitlocker(rd, y.start, hit);
  }
  return false;
}

uint32_t crc32c_ext_sb(const uint8_t* sb) { return crc32c_raw(0xFFFFFFFFu, sb, 1020); }

}  // namespace ps
}  // namespace usbmem
