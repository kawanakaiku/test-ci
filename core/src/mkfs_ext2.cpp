// SPDX-License-Identifier: Apache-2.0
// ext2 の mkfs (書き込む内容を組み立てるだけ)。ジャーナル無し・リサイズ用予約無し・flex_bg 無しの素直な構成:
//   互換 = dir_index / 非互換 = filetype / 読み取り専用互換 = sparse_super + large_file (+ extra_isize: 256 バイト iノードのとき)
// 各グループ: [スーパーブロック + GDT の控え (0, 1, 3 / 5 / 7 のべき乗)] [ブロックビットマップ] [iノードビットマップ] [iノードテーブル] [データ]
// グループ 0 のデータ領域の先頭に ルート (iノード 2) と lost+found (iノード 11)。
#include <algorithm>
#include <cstdio>

#include "mkfs_internal.h"
#include "usbmem/partition_edit.h"  // make_random_guid

namespace usbmem {
namespace mkfsimpl {

namespace {

bool has_backup(uint64_t g) {
  if (g <= 1) return true;
  for (uint64_t base : {3ull, 5ull, 7ull}) {
    uint64_t v = base;
    while (v < g) v *= base;
    if (v == g) return true;
  }
  return false;
}

}  // namespace

void build_ext2(const Params& p, Build* b) {
  const uint32_t B = p.opt.cluster_size ? p.opt.cluster_size : (p.length < (512ull << 20) ? 1024u : 4096u);
  if (B != 1024 && B != 2048 && B != 4096) { b->error("bad-block-size", "the ext2 block size must be 1024, 2048 or 4096"); return; }
  const uint32_t isz = p.opt.inode_size ? p.opt.inode_size : 256;
  if (isz != 128 && isz != 256) { b->error("bad-inode-size", "the inode size must be 128 or 256"); return; }
  const uint32_t ratio = p.opt.inode_ratio ? p.opt.inode_ratio : 16384;
  if (ratio < B || ratio > (64u << 20)) { b->error("bad-inode-ratio", "the bytes-per-inode ratio must be between the block size and 64 MiB"); return; }
  if (p.opt.reserved_percent > 50) { b->error("bad-reserved", "the reserved block percentage must be 0 to 50"); return; }
  if (p.opt.label.size() > 16 || p.opt.label.find('\0') != std::string::npos) { b->error("bad-label", "the label must be at most 16 bytes"); return; }
  uint64_t N = p.length / B;
  if (N > 0xFFFFFFFFull) { b->error("too-large", "ext2 without the 64bit feature cannot address more than 2^32 blocks; use a larger block size"); return; }
  const uint32_t fdb = B == 1024 ? 1 : 0;
  const uint64_t bpg = 8ull * B;
  if (N < 64) { b->error("too-small", "the volume is too small for ext2 (needs at least 64 blocks)"); return; }

  uint64_t G = 0, ipg = 0, gdb = 0, itb = 0;
  const uint64_t ipb = B / isz;   // ブロックあたりの iノード数
  for (int guard = 0; guard < 8; guard++) {
    G = ceil_div(N - fdb, bpg);
    uint64_t target = ceil_div(N * B, ratio);
    ipg = ceil_div(target, G);
    ipg = align_up(std::max<uint64_t>(ipg, 16), std::max<uint64_t>(ipb, 8));
    ipg = std::min<uint64_t>(ipg, 8ull * B);
    ipg -= ipg % std::max<uint64_t>(ipb, 8);
    itb = ipg / ipb;
    gdb = ceil_div(G * 32, B);
    // 最後のグループが小さすぎれば切り捨てる
    uint64_t last = (N - fdb) - (G - 1) * bpg;
    uint64_t ovh = (has_backup(G - 1) ? 1 + gdb : 0) + 2 + itb;
    if (G > 1 && last < ovh + 50) { N = fdb + (G - 1) * bpg; continue; }
    break;
  }
  if (ipg < 16 || ipg * G < 16) { b->error("too-small", "too few inodes"); return; }
  if (ipg * G > 0xFFFFFFFFull) { b->error("too-large", "too many inodes"); return; }
  const uint32_t nlf = static_cast<uint32_t>(std::min<uint64_t>(12, 16384 / B));
  const uint64_t ovh0 = 1 + gdb + 2 + itb;   // グループ 0 のメタデータ (sb + GDT + ビットマップ x2 + iノードテーブル)
  if (N < fdb + ovh0 + 1 + nlf + 8) { b->error("too-small", "the volume is too small for ext2 with this block size"); return; }

  auto id = make_random_guid(p.opt.seed);
  auto hs = make_random_guid(p.opt.seed ? p.opt.seed ^ 0x5bd1e995u : 0);
  const uint32_t now = static_cast<uint32_t>(p.now);
  const bool big = isz > 128;
  const uint32_t first_ino = 11;

  // 使用ブロック数の計算
  std::vector<uint32_t> free_blocks(G), free_inodes(G);
  std::vector<uint64_t> gsize(G), gstart(G);
  uint64_t total_free = 0;
  for (uint64_t g = 0; g < G; g++) {
    gstart[g] = fdb + g * bpg;
    gsize[g] = g + 1 == G ? (N - fdb) - g * bpg : bpg;
    uint64_t used = (has_backup(g) ? 1 + gdb : 0) + 2 + itb + (g == 0 ? 1 + nlf : 0);
    free_blocks[g] = static_cast<uint32_t>(gsize[g] - used);
    free_inodes[g] = static_cast<uint32_t>(ipg - (g == 0 ? first_ino : 0));
    total_free += free_blocks[g];
  }
  // レイアウト (グループ g)
  auto bb_of = [&](uint64_t g) { return gstart[g] + (has_backup(g) ? 1 + gdb : 0); };
  auto ib_of = [&](uint64_t g) { return bb_of(g) + 1; };
  auto it_of = [&](uint64_t g) { return bb_of(g) + 2; };

  // スーパーブロック
  std::vector<uint8_t> sb(1024, 0);
  p32(&sb[0], static_cast<uint32_t>(ipg * G));
  p32(&sb[4], static_cast<uint32_t>(N));
  p32(&sb[8], static_cast<uint32_t>(N * p.opt.reserved_percent / 100));
  p32(&sb[12], static_cast<uint32_t>(total_free));
  p32(&sb[16], static_cast<uint32_t>(ipg * G - first_ino));
  p32(&sb[20], fdb);
  p32(&sb[24], ilog2(B) - 10);
  p32(&sb[28], ilog2(B) - 10);
  p32(&sb[32], static_cast<uint32_t>(bpg));
  p32(&sb[36], static_cast<uint32_t>(bpg));
  p32(&sb[40], static_cast<uint32_t>(ipg));
  p32(&sb[44], 0);                 // mtime
  p32(&sb[48], now);               // wtime
  p16(&sb[52], 0);                 // mnt_count
  p16(&sb[54], 0xFFFF);            // max_mnt_count = -1
  p16(&sb[56], 0xEF53);
  p16(&sb[58], 1);                 // state: valid
  p16(&sb[60], 1);                 // errors: continue
  p32(&sb[64], now);               // lastcheck
  p32(&sb[76], 1);                 // rev_level
  p32(&sb[84], first_ino);
  p16(&sb[88], isz);
  p32(&sb[92], 0x20);              // compat: dir_index
  p32(&sb[96], 0x02);              // incompat: filetype
  p32(&sb[100], 0x01 | 0x02);   // ro_compat: sparse_super + large_file (extra_isize を立てると ext4 と判別される)
  std::memcpy(&sb[104], id.data(), 16);
  std::memcpy(&sb[120], p.opt.label.data(), p.opt.label.size());
  std::memcpy(&sb[236], hs.data(), 16);
  sb[252] = 1;                     // def_hash_version = half_md4
  p32(&sb[256], 0x000C);           // default_mount_opts: user_xattr + acl
  p32(&sb[264], now);              // mkfs_time
  if (big) { p16(&sb[348], 32); p16(&sb[350], 32); }
  p32(&sb[352], 1);                // flags: signed_directory_hash

  // GDT
  std::vector<uint8_t> gdt(static_cast<size_t>(gdb * B), 0);
  for (uint64_t g = 0; g < G; g++) {
    uint8_t* d = &gdt[static_cast<size_t>(g * 32)];
    p32(d, static_cast<uint32_t>(bb_of(g)));
    p32(d + 4, static_cast<uint32_t>(ib_of(g)));
    p32(d + 8, static_cast<uint32_t>(it_of(g)));
    p16(d + 12, free_blocks[g]);
    p16(d + 14, free_inodes[g]);
    p16(d + 16, g == 0 ? 2 : 0);
  }

  const std::string st_groups = "group-metadata", st_dirs = "root-directories", st_backup = "superblock-backups", st_primary = "primary-superblock";
  // グループ 1 以降の控え / ビットマップ / iノードテーブル
  for (uint64_t g = 0; g < G; g++) {
    if (g > 0 && has_backup(g)) {
      std::vector<uint8_t> hdr(static_cast<size_t>((1 + gdb) * B), 0);
      std::memcpy(hdr.data(), sb.data(), 1024);
      p16(&hdr[90], static_cast<uint32_t>(g));
      std::memcpy(hdr.data() + B, gdt.data(), gdt.size());
      b->data(st_backup, gstart[g] * B, std::move(hdr), "superblock-backup");
    }
    // ブロックビットマップ
    std::vector<uint8_t> bbm(B, 0), ibm(B, 0);
    auto setbit = [](std::vector<uint8_t>& v, uint64_t i) { v[static_cast<size_t>(i / 8)] |= static_cast<uint8_t>(1u << (i % 8)); };
    uint64_t used = (has_backup(g) ? 1 + gdb : 0) + 2 + itb + (g == 0 ? 1 + nlf : 0);
    for (uint64_t i = 0; i < used; i++) setbit(bbm, i);
    for (uint64_t i = gsize[g]; i < 8ull * B; i++) setbit(bbm, i);   // グループの外 (最後のグループ) と余白
    if (g == 0) for (uint32_t i = 0; i < first_ino; i++) setbit(ibm, i);
    for (uint64_t i = ipg; i < 8ull * B; i++) setbit(ibm, i);
    b->data(st_groups, bb_of(g) * B, std::move(bbm), "block-bitmap");
    b->data(st_groups, ib_of(g) * B, std::move(ibm), "inode-bitmap");
    if (g == 0) {
      uint64_t head = align_up(static_cast<uint64_t>(first_ino) * isz, B);   // 予約 iノード + lost+found までを含むブロック数分は別に書く
      b->zero(st_groups, it_of(g) * B + head, itb * B - head, "inode-table");
    } else {
      b->zero(st_groups, it_of(g) * B, itb * B, "inode-table");
    }
  }

  // ルートと lost+found (グループ 0)
  const uint64_t root_blk = it_of(0) + itb;
  const uint64_t lf_blk = root_blk + 1;
  auto dirblock = [&](uint32_t self, uint32_t parent) {
    std::vector<uint8_t> d(B, 0);
    p32(&d[0], self); p16(&d[4], 12); d[6] = 1; d[7] = 2; d[8] = '.';
    p32(&d[12], parent); p16(&d[16], B - 12); d[18] = 2; d[19] = 2; d[20] = '.'; d[21] = '.';
    return d;
  };
  auto emptyblock = [&]() {
    std::vector<uint8_t> d(B, 0);
    p16(&d[4], B);
    return d;
  };
  {
    std::vector<uint8_t> rd(B, 0);
    p32(&rd[0], 2); p16(&rd[4], 12); rd[6] = 1; rd[7] = 2; rd[8] = '.';
    p32(&rd[12], 2); p16(&rd[16], 12); rd[18] = 2; rd[19] = 2; rd[20] = '.'; rd[21] = '.';
    p32(&rd[24], 11); p16(&rd[28], B - 24); rd[30] = 10; rd[31] = 2; std::memcpy(&rd[32], "lost+found", 10);
    b->data(st_dirs, root_blk * B, std::move(rd), "root-directory");
  }
  for (uint32_t k = 0; k < nlf; k++) b->data(st_dirs, (lf_blk + k) * B, k == 0 ? dirblock(11, 2) : emptyblock(), "lost+found");
  // iノード 2 (ルート) と 11 (lost+found): iノードテーブルの該当位置だけを書く (残りはゼロのまま)
  auto inode = [&](uint16_t mode, uint16_t links, uint64_t size, uint64_t blocks512, const std::vector<uint64_t>& blks) {
    std::vector<uint8_t> in(isz, 0);
    p16(&in[0], mode);
    p32(&in[4], static_cast<uint32_t>(size));
    p32(&in[8], now); p32(&in[12], now); p32(&in[16], now);
    p16(&in[26], links);
    p32(&in[28], static_cast<uint32_t>(blocks512));
    for (size_t i = 0; i < blks.size(); i++) p32(&in[40 + 4 * i], static_cast<uint32_t>(blks[i]));
    if (big) { p16(&in[128], 32); p32(&in[144], now); }
    return in;
  };
  {
    std::vector<uint64_t> lfb;
    for (uint32_t k = 0; k < nlf; k++) lfb.push_back(lf_blk + k);
    auto root_in = inode(0x41ED, 3, B, B / 512, {root_blk});
    auto lf_in = inode(0x41C0, 2, static_cast<uint64_t>(nlf) * B, static_cast<uint64_t>(nlf) * B / 512, lfb);
    std::vector<uint8_t> head(static_cast<size_t>(align_up(static_cast<uint64_t>(first_ino) * isz, B)), 0);
    // iノード番号 n は テーブル内の (n-1) * isz
    std::memcpy(&head[static_cast<size_t>(1) * isz], root_in.data(), isz);
    std::memcpy(&head[static_cast<size_t>(10) * isz], lf_in.data(), isz);
    b->data(st_dirs, it_of(0) * B, std::move(head), "inode-table-head");
  }

  // 先頭 (グループ 0 のブート領域 + スーパーブロック + GDT) が確定のしるし
  {
    const uint64_t gdt_off = std::max<uint64_t>(B, 2048);
    std::vector<uint8_t> hdr(static_cast<size_t>(gdt_off + gdt.size()), 0);
    std::memcpy(&hdr[1024], sb.data(), 1024);
    std::memcpy(&hdr[static_cast<size_t>(gdt_off)], gdt.data(), gdt.size());
    // B == 1024 のときは 1024..2047 がスーパーブロックで GDT は 2048 から。それ以外は 1024..2047 の中にスーパーブロック。
    b->data(st_primary, 0, std::move(hdr), "primary-superblock-and-gdt");
  }
  // 順序: 控え → グループのメタデータ → ルート → 主スーパーブロック

  auto n = [](uint64_t v) { return std::to_string(v); };
  char uu[40];
  std::snprintf(uu, sizeof uu, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7],
                id[8], id[9], id[10], id[11], id[12], id[13], id[14], id[15]);
  b->layout = {{"blockSize", n(B)}, {"blockCount", n(N)}, {"groups", n(G)}, {"blocksPerGroup", n(bpg)}, {"inodesPerGroup", n(ipg)}, {"inodeCount", n(ipg * G)},
               {"inodeSize", n(isz)}, {"inodeTableBlocksPerGroup", n(itb)}, {"gdtBlocks", n(gdb)}, {"freeBlocks", n(total_free)}, {"uuid", uu}};
  if (N * B < p.length) b->warn("unused-tail", n(p.length - N * B) + " bytes at the end of the range do not fill a whole block (or a whole last group) and are not used");
  uint64_t zero_bytes = G * itb * B;
  if (zero_bytes > (1ull << 30)) b->warn("slow", "the inode tables take " + n(zero_bytes >> 20) + " MiB; clearing them can take a long time. Use a larger bytes-per-inode ratio to make it shorter");
  b->changes.push_back("create an ext2 file system: " + n(N) + " blocks of " + n(B) + " bytes in " + n(G) + " group(s), " + n(ipg * G) + " inodes");
}

}  // namespace mkfsimpl
}  // namespace usbmem
