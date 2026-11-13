// SPDX-License-Identifier: Apache-2.0
// F2FS の書き込み実装 (f2fs_volume.cpp から #include される。USBMEM_NTFS_WRITE のビルドだけ)。設計・安全規則・制約は docs/f2fs-write.md。
//
// 安全規則 (これを弱めて通すことはしない):
//  (1) 新しいデータ / ノード / NAT / SIT / SSA は、最後にコミットしたチェックポイントから見て「使われていない」ブロックにだけ書く。
//      NAT と SIT は 2 面のうち使っていない側へ書き、CP のビットマップを反転する (旧い側は触らない)。
//  (2) 解放したブロック / セグメントは次のチェックポイントがコミットされるまで再利用しない (cvb = コミット済みの有効数が 0 のセグメントだけから割り当てる)。
//  (3) チェックポイント パックは必ず「現在と反対側」へ、先頭 → ペイロード → サマリ → (フラッシュ) → 末尾 の順に書き、最後にフラッシュする。
//      先頭と末尾のバージョンと CRC がそろわないパックは無効なので、電源断のあとも旧いパックが有効のまま残る。
//  (4) 変更はすべてメモリ上に溜めて 1 回の完全なチェックポイントでコミットする (rename / unlink を含めて原子的)。roll-forward 用の fsync ログ / orphan は書かない。

// ---------------------------------------------------------------- 低レベル
uint64_t F2fsVolume::Impl::seg_time() const {
  uint64_t now = static_cast<uint64_t>(time(nullptr));
  return cp_elapsed + (now > mount_time ? now - mount_time : 0);
}
uint8_t* F2fsVolume::Impl::sit_bm_w() {
  uint8_t* c = ckpt.data();
  if (cp_flags & kCpLargeNat) return c + 192 + 4 + le32(c + 160);
  if (sb_cp_payload > 0) return c + kBlk;
  return c + 192;
}
void F2fsVolume::Impl::drop_cache(uint32_t a) {
  auto it = cache.find(a);
  if (it != cache.end()) { lru.erase(it->second.second); cache.erase(it); }
}
int F2fsVolume::Impl::write_blocks(uint32_t a, const void* d, uint32_t n) {
  if (a < seg0 || static_cast<uint64_t>(a) + n > max_blkaddr || static_cast<uint64_t>(a) + n > dev_blocks) return -EIO;
  Status st = dev->write_at(static_cast<uint64_t>(a) * kBlk, d, static_cast<size_t>(n) * kBlk);
  for (uint32_t i = 0; i < n; i++) drop_cache(a + i);
  return st.ok() ? 0 : -EIO;
}
int F2fsVolume::Impl::invalidate(uint32_t a) {
  if (!in_main(a)) return -kEUCLEAN;
  uint32_t s = (a - main_blkaddr) / kSegBlks, off = (a - main_blkaddr) % kSegBlks;
  Seg& g = segs[s];
  if (!bm_msb_get(g.map, off) || g.vb == 0) return -kEUCLEAN;   // すでに無効なブロックを解放しようとした = メタデータの不整合
  bm_msb_clr(g.map, off);
  g.vb--;
  g.mtime = seg_time();
  sit_dirty.insert(s / kSitPerBlock);
  return 0;
}
bool F2fsVolume::Impl::is_cur(uint32_t s) const {
  for (int i = 0; i < 6; i++) if (cur[i].segno == s) return true;
  return false;
}
bool F2fsVolume::Impl::seg_is_free(uint32_t s) const {
  if (segs[s].vb != 0) return false;
#ifndef USBMEM_MUTATE_F2FS_REUSE_FREED
  if (cvb[s] != 0) return false;                    // 今のエポックで空になっただけ: コミットまで再利用しない
#endif
  if (is_cur(s) || committed_cur.count(s)) return false;
  return true;
}
int F2fsVolume::Impl::alloc_seg(uint32_t* out) {
  for (uint32_t k = 0; k < main_segs; k++) {
    uint32_t s = (seg_hint + k) % main_segs;
    if (seg_is_free(s)) { *out = s; seg_hint = s + 1; if (free_segs) free_segs--; return 0; }
  }
  return -ENOSPC;
}
void F2fsVolume::Impl::init_ssa(uint32_t segno, int type) {
  auto& v = ssa[segno];
  v.assign(kBlk, 0);
  v[kBlk - 5] = type >= kLogHotNode ? 1 : 0;   // footer.entry_type (SUM_TYPE_NODE = 1)
  ssa_dirty.insert(segno);
}
int F2fsVolume::Impl::alloc_blk(int type, uint32_t owner_nid, uint8_t ver, uint16_t ofs, uint32_t* addr) {
  Cur& c = cur[type];
  if (c.blkoff >= kSegBlks) {
    uint32_t ns;
    int rc = alloc_seg(&ns);
    if (rc) return rc;
    c.segno = ns; c.blkoff = 0;
    Seg& g = segs[ns];
    g = Seg();
    g.type = static_cast<uint8_t>(type);
    g.mtime = seg_time();
    init_ssa(ns, type);
    sit_dirty.insert(ns / kSitPerBlock);
  }
  uint32_t s = c.segno, off = c.blkoff;
  auto it = ssa.find(s);
  if (it == ssa.end()) return -kEUCLEAN;
  uint8_t* e = it->second.data() + 7 * off;
  put32(e, owner_nid); e[4] = type >= kLogHotNode ? 0 : ver; put16(e + 5, type >= kLogHotNode ? 0 : ofs);
  ssa_dirty.insert(s);
  Seg& g = segs[s];
  if (bm_msb_get(g.map, off)) return -kEUCLEAN;
  bm_msb_set(g.map, off);
  g.vb++;
  g.type = static_cast<uint8_t>(type);
  g.mtime = seg_time();
  sit_dirty.insert(s / kSitPerBlock);
  c.blkoff++;
  *addr = main_blkaddr + s * kSegBlks + off;
  return 0;
}
int F2fsVolume::Impl::nat_get(uint32_t nid, NatEnt* e) { return nat_lookup(nid, e); }
int F2fsVolume::Impl::alloc_nid(uint32_t* out) {
  uint32_t span = max_nid - kReservedNodes;
  if (nid_hint < kReservedNodes || nid_hint >= max_nid) nid_hint = kReservedNodes;
  for (uint32_t k = 0; k < span; k++) {
    uint32_t nid = kReservedNodes + (nid_hint - kReservedNodes + k) % span;
    if (freed_nids.count(nid)) continue;
    NatEnt e;
    int rc = nat_lookup(nid, &e);
    if (rc) return rc == -ENOENT ? -kEUCLEAN : rc;
    if (e.blkaddr == kNullAddr) { *out = nid; nid_hint = nid + 1; return 0; }
  }
  return -ENOSPC;
}
bool F2fsVolume::Impl::space_ok(uint64_t blocks) const {
  uint64_t need = pend_blocks + nodes.size() + blocks + blocks / 500 + 16;
  uint64_t cap = static_cast<uint64_t>(free_segs) * kSegBlks;
  for (int i = 0; i < 6; i++) cap += kSegBlks - std::min<uint32_t>(cur[i].blkoff, kSegBlks);
  if (cap < need + static_cast<uint64_t>(kReserveSegs) * kSegBlks) return false;
  if (valid_blocks + blocks > user_blocks) return false;
  return true;
}

// ---------------------------------------------------------------- ノード
void F2fsVolume::Impl::set_footer(uint8_t* p, uint32_t nid, uint32_t ino, uint32_t flag) {
  uint8_t* f = p + kBlk - 24;
  put32(f, nid); put32(f + 4, ino); put32(f + 8, flag);
}
void F2fsVolume::Impl::finalize_inode(uint8_t* p, uint32_t nid) {
  if (!(feature & 0x20u)) return;                       // inode_checksum 機能
  if (!(p[3] & kInlExtra)) return;
  uint32_t extra = le16(p + 360);
  if (extra < 12) return;                               // i_inode_checksum がインラインの拡張領域に収まらない
  put32(p + 368, 0);
  uint32_t seed = crc32_le(0xFFFFFFFFu, sb_uuid, 16);
  uint8_t t[4];
  put32(t, nid); uint32_t c = crc32_le(seed, t, 4);
  put32(t, le32(p + 68)); c = crc32_le(c, t, 4);       // i_generation
  c = crc32_le(c, p, 368);
  put32(t, 0); c = crc32_le(c, t, 4);
  c = crc32_le(c, p + 372, kBlk - 372);
  put32(p + 368, c);
}
int F2fsVolume::Impl::mut_node(uint32_t nid, uint32_t owner, NodeVec* out) {
  auto it = nodes.find(nid);
  if (it != nodes.end()) { *out = it->second; return 0; }
  Blk b;
  int rc = get_node(nid, owner, &b);
  if (rc) return rc;
  auto v = std::make_shared<std::vector<uint8_t>>(*b);
  nodes[nid] = v;
  dirty = true;
  *out = v;
  return 0;
}
int F2fsVolume::Impl::new_node(uint32_t ino, uint32_t ofs, bool cold, uint32_t* out) {
  uint32_t nid;
  int rc = alloc_nid(&nid);
  if (rc) return rc;
  NatEnt e;
  rc = nat_lookup(nid, &e);
  if (rc) return rc;
  auto v = std::make_shared<std::vector<uint8_t>>(kBlk, 0);
  set_footer(v->data(), nid, ino, (ofs << 3) | (cold ? 1u : 0u));
  nodes[nid] = v;
  NatEnt ne; ne.ino = ino; ne.blkaddr = kNewAddr; ne.version = e.version;
  nat_dirty[nid] = ne;
  valid_blocks++; valid_nodes++;
  dirty = true;
  *out = nid;
  return 0;
}
void F2fsVolume::Impl::free_node(uint32_t nid) {
  NatEnt e;
  nat_lookup(nid, &e);
  if (e.blkaddr != kNullAddr && e.blkaddr != kNewAddr && valid_data_blk(e.blkaddr)) invalidate(e.blkaddr);
  nodes.erase(nid);
  NatEnt ne; ne.ino = 0; ne.blkaddr = kNullAddr;
  ne.version = (e.blkaddr != kNewAddr) ? static_cast<uint8_t>(e.version + 1) : e.version;   // 実体のあったノードを消したらバージョンを進める
  nat_dirty[nid] = ne;
  freed_nids.insert(nid);
  if (valid_blocks) valid_blocks--;
  if (valid_nodes) valid_nodes--;
  dirty = true;
}
int F2fsVolume::Impl::iblocks(uint32_t ino, long delta) {
  NodeVec v;
  int rc = mut_node(ino, 0, &v);
  if (rc) return rc;
  int64_t b = static_cast<int64_t>(le64(v->data() + 24)) + delta;
  if (b < 1) b = 1;
  put64(v->data() + 24, static_cast<uint64_t>(b));
  return 0;
}
int F2fsVolume::Impl::touch(uint32_t nid, bool mtime) {
  NodeVec v;
  int rc = mut_node(nid, 0, &v);
  if (rc) return rc;
  uint64_t t = static_cast<uint64_t>(time(nullptr));
  put64(v->data() + 40, t);
  if (mtime) put64(v->data() + 48, t);
  return 0;
}

// ファイルの fblk 番目のブロックを指すスロット (inode の i_addr か、直接ノード内) を、書き込み用に (経路上のノードをダーティ化して) 返す。
// create なら無いノードを作る。穴で create でないなら *slot = nullptr。
int F2fsVolume::Impl::slot_ptr(uint32_t ino, uint64_t fblk, bool create, uint8_t** slot, uint32_t* dn_nid, uint32_t* ofs_in_node) {
  *slot = nullptr;
  Inode in;
  int rc = load_inode(ino, &in);
  if (rc) return rc;
  if (in.compressed) return -kEOPNOTSUPP;
  const uint64_t dpi = in.addrs_in_inode, D = kAddrsPerBlock, N = kNidsPerBlock;
  int level = 0, slot0 = 0;
  uint64_t idx[4] = {0, 0, 0, 0}, noff[4] = {0, 0, 0, 0};
  if (fblk < dpi) { idx[0] = fblk; }
  else {
    uint64_t b = fblk - dpi;
    if (b < D) { slot0 = 0; level = 1; noff[1] = 1; idx[1] = b; }
    else if ((b -= D) < D) { slot0 = 1; level = 1; noff[1] = 2; idx[1] = b; }
    else if ((b -= D) < D * N) { slot0 = 2; level = 2; noff[1] = 3; idx[1] = b / D; noff[2] = 4 + idx[1]; idx[2] = b % D; }
    else if ((b -= D * N) < D * N) { slot0 = 3; level = 2; noff[1] = 4 + N; idx[1] = b / D; noff[2] = 5 + N + idx[1]; idx[2] = b % D; }
    else if ((b -= D * N) < D * N * N) {
      slot0 = 4; level = 3; noff[1] = 5 + 2 * N; idx[1] = b / (D * N); noff[2] = 6 + 2 * N + idx[1] * (N + 1);
      idx[2] = (b / D) % N; noff[3] = 7 + 2 * N + idx[1] * (N + 1) + idx[2]; idx[3] = b % D;
    } else return -EFBIG;
  }
  NodeVec iv;
  rc = mut_node(ino, 0, &iv);
  if (rc) return rc;
  memset(iv->data() + 348, 0, 12);   // i_ext (最大エクステントのキャッシュ) は、ブロックの配置を変えると古くなる。fsck.f2fs は不一致を壊れと見なすので消す
  bool is_dir_ino = in.is_dir();
  if (level == 0) {
    *slot = iv->data() + 360 + 4 * in.extra_words + 4 * idx[0];
    *dn_nid = ino; *ofs_in_node = static_cast<uint32_t>(idx[0]);
    return 0;
  }
  NodeVec node = iv;
  uint32_t node_nid = ino;
  uint8_t* nidslot = iv->data() + kBlk - 24 - 20 + 4 * slot0;
  for (int lv = 1; lv <= level; lv++) {
    uint32_t child = le32(nidslot);
    if (child == 0) {
      if (!create) return 0;
      rc = new_node(ino, static_cast<uint32_t>(noff[lv]), !is_dir_ino, &child);
      if (rc) return rc;
      put32(nidslot, child);
      rc = iblocks(ino, 1);
      if (rc) return rc;
    }
    rc = mut_node(child, ino, &node);
    if (rc) return rc == -ENOENT ? -kEUCLEAN : rc;
    node_nid = child;
    if (lv < level) nidslot = node->data() + 4 * idx[lv];
    else { *slot = node->data() + 4 * idx[lv]; *dn_nid = node_nid; *ofs_in_node = static_cast<uint32_t>(idx[lv]); }
  }
  return 0;
}
int F2fsVolume::Impl::reserve_block(uint32_t ino, uint64_t fblk) {
  uint8_t* slot; uint32_t dn, ofs;
  int rc = slot_ptr(ino, fblk, true, &slot, &dn, &ofs);
  if (rc) return rc;
  if (le32(slot) != kNullAddr) return 0;
  put32(slot, kNewAddr);
  rc = iblocks(ino, 1);
  if (rc) return rc;
  valid_blocks++; pend_blocks++;
  dirty = true;
  return 0;
}
// 書き込み用のページ。無ければ (穴なら予約して) 作る。full_overwrite なら既存の内容を読まない
int F2fsVolume::Impl::dpage_mut(uint32_t ino, uint64_t fblk, bool full_overwrite, std::vector<uint8_t>** out) {
  uint64_t key = (static_cast<uint64_t>(ino) << 32) | fblk;
  auto it = dpages.find(key);
  if (it != dpages.end()) { *out = &it->second; return 0; }
  Inode in;
  int rc = load_inode(ino, &in);
  if (rc) return rc;
  uint32_t addr;
  rc = bmap(in, fblk, &addr);
  if (rc) return rc;
  std::vector<uint8_t> page(kBlk, 0);
  if (addr == kNullAddr) {
    rc = reserve_block(ino, fblk);
    if (rc) return rc;
  } else if (addr != kNewAddr && !full_overwrite) {
    if (!valid_data_blk(addr)) return -kEUCLEAN;
    rc = read_raw(addr, page.data());
    if (rc) return rc;
  }
  dirty = true;
  auto res = dpages.emplace(key, std::move(page));
  *out = &res.first->second;
  return 0;
}
void F2fsVolume::Impl::drop_dpages(uint32_t ino, uint64_t from_blk) {
  auto it = dpages.lower_bound((static_cast<uint64_t>(ino) << 32) | from_blk);
  while (it != dpages.end() && (it->first >> 32) == ino) it = dpages.erase(it);
}
void F2fsVolume::Impl::free_data_addr(uint32_t a) {
  if (a == kNullAddr) return;
  if (a == kNewAddr) { if (pend_blocks) pend_blocks--; }
  else invalidate(a);
  if (valid_blocks) valid_blocks--;
  acc_iblocks++;
}
int F2fsVolume::Impl::writeback_pages() {
  if (dpages.empty()) return 0;
  uint32_t run_addr = 0, run_ino = 0;
  std::vector<uint8_t> run;
  int rc = 0;
  auto flush_run = [&]() -> int {
    if (run.empty()) return 0;
    int r = write_blocks(run_addr, run.data(), static_cast<uint32_t>(run.size() / kBlk));
    run.clear();
    return r;
  };
  Inode in; uint32_t in_ino = 0;
  for (auto it = dpages.begin(); it != dpages.end(); ++it) {
    uint32_t ino = static_cast<uint32_t>(it->first >> 32);
    uint64_t fblk = it->first & 0xFFFFFFFFull;
    if (in_ino != ino) { rc = load_inode(ino, &in); if (rc) return rc; in_ino = ino; }
    uint8_t* slot; uint32_t dn, ofs;
    rc = slot_ptr(ino, fblk, false, &slot, &dn, &ofs);
    if (rc) return rc;
    if (!slot || le32(slot) == kNullAddr) return -kEUCLEAN;   // ページがあるのに予約されていない
    uint32_t old = le32(slot);
    NatEnt ne;
    rc = nat_lookup(dn, &ne);
    if (rc) return rc;
    uint32_t addr;
    rc = alloc_blk(in.is_dir() ? kLogHotData : kLogWarmData, dn, ne.version, static_cast<uint16_t>(ofs), &addr);
    if (rc) return rc;
    put32(slot, addr);
    if (old == kNewAddr) { if (pend_blocks) pend_blocks--; }
    else { rc = invalidate(old); if (rc) return rc; }
    if (!run.empty() && (addr != run_addr + run.size() / kBlk || run.size() >= 256 * kBlk)) { rc = flush_run(); if (rc) return rc; }
    if (run.empty()) { run_addr = addr; run_ino = ino; }
    run.insert(run.end(), it->second.begin(), it->second.end());
  }
  (void)run_ino;
  rc = flush_run();
  if (rc) return rc;
  dpages.clear();
  return 0;
}

// ---------------------------------------------------------------- 解放 (truncate / inode 削除)
int F2fsVolume::Impl::trunc_node(uint32_t nid, int level, uint32_t ino, uint64_t base, uint64_t from) {
  NodeVec nv;
  int rc = mut_node(nid, ino, &nv);
  if (rc) return rc == -ENOENT ? -kEUCLEAN : rc;
  const uint64_t D = kAddrsPerBlock, N = kNidsPerBlock;
  uint64_t cspan = level == 1 ? 1 : (level == 2 ? D : D * N);
  for (uint32_t i = 0; i < kAddrsPerBlock; i++) {
    uint64_t cbase = base + static_cast<uint64_t>(i) * cspan;
    if (cbase + cspan <= from) continue;
    uint32_t v = le32(nv->data() + 4 * i);
    if (v == 0) continue;
    if (level == 1) {
      free_data_addr(v);
      put32(nv->data() + 4 * i, 0);
    } else {
      rc = trunc_node(v, level - 1, ino, cbase, from);
      if (rc) return rc;
      // 子が全部解放されたら親のスロットも 0 に (子が残っていれば残す)
      if (cbase >= from) put32(nv->data() + 4 * i, 0);
    }
  }
  if (base >= from) { free_node(nid); acc_iblocks++; }
  return 0;
}
int F2fsVolume::Impl::truncate_blocks(uint32_t ino, uint64_t from_blk) {
  Inode in;
  int rc = load_inode(ino, &in);
  if (rc) return rc;
  if (in.compressed) return -kEOPNOTSUPP;
  NodeVec iv;
  rc = mut_node(ino, 0, &iv);
  if (rc) return rc;
  memset(iv->data() + 348, 0, 12);
  acc_iblocks = 0;
  const uint64_t dpi = in.addrs_in_inode, D = kAddrsPerBlock, N = kNidsPerBlock;
  for (uint64_t i = from_blk; i < dpi; i++) {
    uint8_t* s = iv->data() + 360 + 4 * in.extra_words + 4 * i;
    uint32_t a = le32(s);
    if (a) { free_data_addr(a); put32(s, 0); }
  }
  struct Sl { int slot, level; uint64_t base; } sl[5] = {
    {0, 1, dpi}, {1, 1, dpi + D}, {2, 2, dpi + 2 * D}, {3, 2, dpi + 2 * D + D * N}, {4, 3, dpi + 2 * D + 2 * D * N}};
  for (auto& x : sl) {
    uint64_t span = x.level == 1 ? D : (x.level == 2 ? D * N : D * N * N);
    if (x.base + span <= from_blk) continue;
    uint8_t* ns = iv->data() + kBlk - 24 - 20 + 4 * x.slot;
    uint32_t child = le32(ns);
    if (!child) continue;
    rc = trunc_node(child, x.level, ino, x.base, from_blk);
    if (rc) return rc;
    if (x.base >= from_blk) put32(ns, 0);
  }
  long d = acc_iblocks;
  acc_iblocks = 0;
  dirty = true;
  return iblocks(ino, -d);
}
int F2fsVolume::Impl::free_inode(uint32_t ino) {
  Inode in;
  int rc = load_inode(ino, &in);
  if (rc) return rc;
  if (!in.inline_data() && !in.inline_dentry()) {
    drop_dpages(ino, 0);
    rc = truncate_blocks(ino, 0);
    if (rc) return rc;
  }
  uint32_t xnid = le32(in.blk->data() + 76);
  if (xnid) { free_node(xnid); }
  free_node(ino);
  if (valid_inodes) valid_inodes--;
  return 0;
}

// ---------------------------------------------------------------- inode の作成
int F2fsVolume::Impl::make_inode(uint16_t mode, uint32_t pino, const std::string& name, uint32_t uid, uint32_t gid, uint32_t* out) {
  uint32_t nid;
  int rc = alloc_nid(&nid);
  if (rc) return rc;
  NatEnt e;
  rc = nat_lookup(nid, &e);
  if (rc) return rc;
  bool isdir = (mode & 0170000) == 0040000;
  auto v = std::make_shared<std::vector<uint8_t>>(kBlk, 0);
  uint8_t* p = v->data();
  uint64_t t = static_cast<uint64_t>(time(nullptr));
  put16(p, mode);
  p[3] = (feature & kFeatExtraAttr) ? kInlExtra : 0;
  put32(p + 4, uid); put32(p + 8, gid);
  put32(p + 12, isdir ? 2 : 1);
  put64(p + 16, 0); put64(p + 24, 1);
  put64(p + 32, t); put64(p + 40, t); put64(p + 48, t);
  gen_state = gen_state * 1664525u + 1013904223u + nid + static_cast<uint32_t>(t);
  put32(p + 68, gen_state);
  put32(p + 72, 1);                      // 作った時点の i_current_depth (ディレクトリ以外では i_gc_failures)。ディレクトリ以外は 0 にする
  if (!isdir) put32(p + 72, 0);
  put32(p + 84, pino);
  put32(p + 88, static_cast<uint32_t>(name.size()));
  memcpy(p + 92, name.data(), name.size());
  if (feature & kFeatExtraAttr) {
    put16(p + 360, new_extra);
    put16(p + 362, 0);                   // i_inline_xattr_size
    if (new_extra >= 24) { put64(p + 372, t); put32(p + 380, 0); }   // i_crtime / i_crtime_nsec
  }
  set_footer(p, nid, nid, isdir ? 0u : 1u);   // 暖かいノード用に cold ビット (ディレクトリ以外)
  nodes[nid] = v;
  NatEnt ne; ne.ino = nid; ne.blkaddr = kNewAddr; ne.version = e.version;
  nat_dirty[nid] = ne;
  valid_blocks++; valid_nodes++; valid_inodes++;
  dirty = true;
  *out = nid;
  return 0;
}

// ---------------------------------------------------------------- ディレクトリ
int F2fsVolume::Impl::check_name(const std::string& name) {
  if (name.empty() || name.size() > 255) return -ENAMETOOLONG;
  if (name == "." || name == "..") return -EINVAL;
  if (name.find('/') != std::string::npos || name.find('\0') != std::string::npos) return -EINVAL;
  return 0;
}
int F2fsVolume::Impl::check_mutable_dir(uint32_t dnid, Inode* d) {
  int rc = load_inode(dnid, d);
  if (rc) return rc;
  if (!d->is_dir()) return -ENOTDIR;
  if (d->flags & (kFlEncryptFl | kFlCasefoldFl | 0x10u | 0x20u)) return -EPERM;
  return 0;
}
int F2fsVolume::Impl::inline_dbuf(const Inode& in, uint8_t* base, DBuf* d) {
  uint32_t maxi = in.max_inline;
  uint32_t nr = maxi * 8 / ((11 + 8) * 8 + 1);
  uint32_t bms = (nr + 7) / 8;
  if (nr == 0 || bms + 19u * nr > maxi) return -kEUCLEAN;
  uint32_t reserved = maxi - 19 * nr - bms;
  uint8_t* r = base + 360 + 4 * in.extra_words + 4;
  *d = {r, r + bms + reserved, r + bms + reserved + 11 * nr, nr};
  return 0;
}
int F2fsVolume::Impl::dir_blk_read(const Inode& d, uint64_t b, Blk* out, bool* hole) {
  *hole = false;
  if (const std::vector<uint8_t>* dp = dirty_page(d.nid, b)) { *out = std::make_shared<const std::vector<uint8_t>>(*dp); return 0; }
  uint32_t addr;
  int rc = bmap(d, b, &addr);
  if (rc) return rc;
  if (addr == kNullAddr || addr == kNewAddr) { *hole = true; return 0; }
  if (!valid_data_blk(addr)) return -kEUCLEAN;
  return get(addr, out);
}
int F2fsVolume::Impl::dir_find(uint32_t dnid, const std::string& name, uint32_t* ino, uint8_t* ft, bool* found) {
  *found = false;
  Inode d;
  int rc = load_inode(dnid, &d);
  if (rc) return rc;
  if (!d.is_dir()) return -ENOTDIR;
  uint32_t hash = dentry_hash(reinterpret_cast<const uint8_t*>(name.data()), name.size());
  if (d.inline_dentry()) {
    DBuf db;
    rc = inline_dbuf(d, const_cast<uint8_t*>(d.blk->data()), &db);
    if (rc) return rc;
    int r = db.find(hash, name, ino, ft);
    if (r < -1) return r;
    *found = r >= 0;
    return 0;
  }
  uint32_t depth = le32(d.blk->data() + 72), dir_level = d.blk->data()[347];
  if (depth > 63) depth = 63;
  for (uint32_t level = 0; level < depth; level++) {
    uint32_t nbucket = dir_buckets(level, dir_level), nblock = bucket_blocks(level);
    uint64_t bidx = dir_block_index(level, dir_level, hash % nbucket);
    for (uint32_t k = 0; k < nblock; k++) {
      Blk b; bool hole;
      rc = dir_blk_read(d, bidx + k, &b, &hole);
      if (rc) return rc;
      if (hole) continue;
      DBuf db = dbuf_blk(const_cast<uint8_t*>(b->data()));
      int r = db.find(hash, name, ino, ft);
      if (r < -1) return r;
      if (r >= 0) { *found = true; return 0; }
    }
  }
  return 0;
}
void F2fsVolume::Impl::dir_cache_add(uint32_t dnid, const std::string& name, uint32_t ino, uint8_t ft) {
  auto it = dirs.find(dnid);
  if (it == dirs.end()) return;
  it->second->idx.emplace(name, it->second->ents.size());
  it->second->ents.push_back({name, ino, dtype_of(ft)});
}
int F2fsVolume::Impl::dir_convert_inline(uint32_t dnid) {
  Inode d;
  int rc = load_inode(dnid, &d);
  if (rc) return rc;
  NodeVec iv;
  rc = mut_node(dnid, 0, &iv);
  if (rc) return rc;
  DBuf src;
  rc = inline_dbuf(d, iv->data(), &src);
  if (rc) return rc;
  std::vector<uint8_t> tmp(kBlk, 0);
  DBuf dst = dbuf_blk(tmp.data());
  if (src.nr > dst.nr) return -kEUCLEAN;
  memcpy(dst.bm, src.bm, (src.nr + 7) / 8);
  memcpy(dst.de, src.de, 11 * src.nr);
  memcpy(dst.fn, src.fn, 8 * src.nr);
  // インライン領域 (i_addr 全体) をゼロにして、インラインの印を外す
  uint32_t cur_words = kDefAddrsPerInode - d.extra_words;
  uint32_t new_xattr = (feature & kFeatFlexXattr) ? d.xattr_words : ((iv->data()[3] & kInlXattr) ? 50u : 0u);   // インライン dentry の印を外した後の xattr 領域
  if (new_xattr > cur_words) return -kEUCLEAN;
  memset(iv->data() + 360 + 4 * d.extra_words, 0, 4 * (cur_words - new_xattr));
  iv->data()[3] = static_cast<uint8_t>(iv->data()[3] & ~(kInlDentry | kInlDataExist));
  std::vector<uint8_t>* pg;
  rc = dpage_mut(dnid, 0, true, &pg);
  if (rc) return rc;
  *pg = tmp;
  if (le64(iv->data() + 16) < kBlk) put64(iv->data() + 16, kBlk);
  put32(iv->data() + 72, 1);
  dirs.erase(dnid);
  return 0;
}
int F2fsVolume::Impl::dir_add(uint32_t dnid, const std::string& name, uint32_t ino, uint8_t ft) {
  Inode d;
  int rc = load_inode(dnid, &d);
  if (rc) return rc;
  uint32_t hash = dentry_hash(reinterpret_cast<const uint8_t*>(name.data()), name.size());
  uint32_t slots = static_cast<uint32_t>((name.size() + 7) / 8);
  if (d.inline_dentry()) {
    NodeVec iv;
    rc = mut_node(dnid, 0, &iv);
    if (rc) return rc;
    DBuf db;
    rc = inline_dbuf(d, iv->data(), &db);
    if (rc) return rc;
    int slot = db.room(slots);
    if (slot >= 0) {
      db.put(static_cast<uint32_t>(slot), hash, name, ino, ft);
      dir_cache_add(dnid, name, ino, ft);
      return touch(dnid, true);
    }
    rc = dir_convert_inline(dnid);
    if (rc) return rc;
    rc = load_inode(dnid, &d);
    if (rc) return rc;
  }
  uint32_t depth = le32(d.blk->data() + 72), dir_level = d.blk->data()[347];
  if (depth == 0 || depth > 63) return -kEUCLEAN;
  for (uint32_t level = 0; level < 63; level++) {
    if (level == depth) depth++;
    uint32_t nbucket = dir_buckets(level, dir_level), nblock = bucket_blocks(level);
    uint64_t bidx = dir_block_index(level, dir_level, hash % nbucket);
    for (uint32_t k = 0; k < nblock; k++) {
      uint64_t blk = bidx + k;
      Blk b; bool hole;
      rc = dir_blk_read(d, blk, &b, &hole);
      if (rc) return rc;
      int slot = 0;
      if (!hole) slot = dbuf_blk(const_cast<uint8_t*>(b->data())).room(slots);
      if (slot < 0) continue;
      std::vector<uint8_t>* pg;
      rc = dpage_mut(dnid, blk, hole, &pg);
      if (rc) return rc;
      dbuf_blk(pg->data()).put(static_cast<uint32_t>(slot), hash, name, ino, ft);
      NodeVec iv;
      rc = mut_node(dnid, 0, &iv);
      if (rc) return rc;
      if (le64(iv->data() + 16) < (blk + 1) * kBlk) put64(iv->data() + 16, (blk + 1) * kBlk);
      if (le32(iv->data() + 72) < depth) put32(iv->data() + 72, depth);
      dir_cache_add(dnid, name, ino, ft);
      return touch(dnid, true);
    }
  }
  return -ENOSPC;
}
int F2fsVolume::Impl::dir_del(uint32_t dnid, const std::string& name) {
  Inode d;
  int rc = load_inode(dnid, &d);
  if (rc) return rc;
  uint32_t hash = dentry_hash(reinterpret_cast<const uint8_t*>(name.data()), name.size());
  dirs.erase(dnid);
  if (d.inline_dentry()) {
    NodeVec iv;
    rc = mut_node(dnid, 0, &iv);
    if (rc) return rc;
    DBuf db;
    rc = inline_dbuf(d, iv->data(), &db);
    if (rc) return rc;
    int slot = db.find(hash, name, nullptr, nullptr);
    if (slot < 0) return slot == -1 ? -ENOENT : slot;
    db.del(static_cast<uint32_t>(slot));
    return touch(dnid, true);
  }
  uint32_t depth = le32(d.blk->data() + 72), dir_level = d.blk->data()[347];
  if (depth > 63) depth = 63;
  for (uint32_t level = 0; level < depth; level++) {
    uint32_t nbucket = dir_buckets(level, dir_level), nblock = bucket_blocks(level);
    uint64_t bidx = dir_block_index(level, dir_level, hash % nbucket);
    for (uint32_t k = 0; k < nblock; k++) {
      uint64_t blk = bidx + k;
      Blk b; bool hole;
      rc = dir_blk_read(d, blk, &b, &hole);
      if (rc) return rc;
      if (hole) continue;
      int slot = dbuf_blk(const_cast<uint8_t*>(b->data())).find(hash, name, nullptr, nullptr);
      if (slot < -1) return slot;
      if (slot < 0) continue;
      std::vector<uint8_t>* pg;
      rc = dpage_mut(dnid, blk, false, &pg);
      if (rc) return rc;
      DBuf db = dbuf_blk(pg->data());
      db.del(static_cast<uint32_t>(slot));
      if (blk != 0 && db.empty()) {   // 空になったブロックは解放 (ブロック 0 には "." ".." が残る)
        dpages.erase((static_cast<uint64_t>(dnid) << 32) | blk);
        uint8_t* s; uint32_t dn, ofs;
        rc = slot_ptr(dnid, blk, false, &s, &dn, &ofs);
        if (rc) return rc;
        if (s && le32(s)) {
          acc_iblocks = 0;
          free_data_addr(le32(s));
          put32(s, 0);
          long dd = acc_iblocks; acc_iblocks = 0;
          rc = iblocks(dnid, -dd);
          if (rc) return rc;
        }
      }
      return touch(dnid, true);
    }
  }
  return -ENOENT;
}
int F2fsVolume::Impl::dir_set_ino(uint32_t dnid, const std::string& name, uint32_t ino) {
  Inode d;
  int rc = load_inode(dnid, &d);
  if (rc) return rc;
  uint32_t hash = dentry_hash(reinterpret_cast<const uint8_t*>(name.data()), name.size());
  dirs.erase(dnid);
  if (d.inline_dentry()) {
    NodeVec iv;
    rc = mut_node(dnid, 0, &iv);
    if (rc) return rc;
    DBuf db;
    rc = inline_dbuf(d, iv->data(), &db);
    if (rc) return rc;
    int slot = db.find(hash, name, nullptr, nullptr);
    if (slot < 0) return slot == -1 ? -ENOENT : slot;
    put32(db.de + 11 * slot + 4, ino);
    return 0;
  }
  // ".." はブロック 0 のスロット 1 (ハッシュ 0)。それ以外は通常の検索
  if (name == "..") {
    std::vector<uint8_t>* pg;
    rc = dpage_mut(dnid, 0, false, &pg);
    if (rc) return rc;
    DBuf db = dbuf_blk(pg->data());
    if (!db.used(1) || le16(db.de + 11 + 8) != 2) return -kEUCLEAN;
    put32(db.de + 11 + 4, ino);
    return 0;
  }
  return -EINVAL;
}
int F2fsVolume::Impl::dir_is_empty(uint32_t dnid, bool* empty) {
  Inode d;
  int rc = load_inode(dnid, &d);
  if (rc) return rc;
  std::vector<FsDirEntry> ents;
  rc = list_dir(d, &ents);
  if (rc) return rc;
  *empty = ents.empty();
  return 0;
}
