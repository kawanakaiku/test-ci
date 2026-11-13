// SPDX-License-Identifier: Apache-2.0
// F2FS 書き込み: チェックポイント (コミット) と、書き込み可マウントの準備 (f2fs_write.inc の続き)。

// ---------------------------------------------------------------- コミット
namespace {
bool is_dnode_ofs(uint32_t ofs) {   // カーネルの IS_DNODE: 直接ノード / inode / xattr ノードは true、間接ノードは false
  const uint32_t N = kNidsPerBlock;
  if (ofs == 0x1FFFFFFFu) return true;
  if (ofs == 3 || ofs == 4 + N || ofs == 5 + 2 * N) return false;
  if (ofs >= 6 + 2 * N) { ofs -= 6 + 2 * N; if (ofs % (N + 1) == 0) return false; }
  return true;
}
}  // namespace

int F2fsVolume::Impl::flush_nodes() {
  for (auto& kv : nodes) {
    uint32_t nid = kv.first;
    std::vector<uint8_t>& v = *kv.second;
    uint8_t* f = v.data() + kBlk - 24;
    uint32_t ino = le32(f + 4), flag = le32(f + 8), ofs = flag >> 3;
    NatEnt e;
    int rc = nat_lookup(nid, &e);
    if (rc) return rc == -ENOENT ? -kEUCLEAN : rc;
    int type = !is_dnode_ofs(ofs) ? kLogColdNode : ((flag & 1) ? kLogWarmNode : kLogHotNode);
    uint32_t addr;
    rc = alloc_blk(type, nid, 0, 0, &addr);
    if (rc) return rc;
    put64(f + 12, cp_ver);        // 書いた時点の (コミット済みの) チェックポイント世代。fsync の印 (ビット 1) は立てないので roll-forward の対象にならない
    put32(f + 20, addr + 1);
    if (nid == ino) finalize_inode(v.data(), nid);
    rc = write_blocks(addr, v.data(), 1);
    if (rc) return rc;
    if (e.blkaddr != kNewAddr && e.blkaddr != kNullAddr) {
      if (!valid_data_blk(e.blkaddr)) return -kEUCLEAN;
      rc = invalidate(e.blkaddr);
      if (rc) return rc;
    }
    NatEnt ne; ne.ino = ino; ne.blkaddr = addr; ne.version = e.version;
    nat_dirty[nid] = ne;
  }
  nodes.clear();
  return 0;
}
int F2fsVolume::Impl::roll_full_cursegs() {
  for (int t = 0; t < 6; t++) {
    Cur& c = cur[t];
    if (c.blkoff < kSegBlks) continue;
    uint32_t ns;
    int rc = alloc_seg(&ns);
    if (rc) return rc;
    c.segno = ns; c.blkoff = 0;
    Seg& g = segs[ns];
    g = Seg();
    g.type = static_cast<uint8_t>(t);
    g.mtime = seg_time();
    init_ssa(ns, t);
    sit_dirty.insert(ns / kSitPerBlock);
  }
  return 0;
}
int F2fsVolume::Impl::write_ssa_blocks() {
  for (uint32_t s : ssa_dirty) {
    auto it = ssa.find(s);
    if (it == ssa.end() || s >= main_segs) return -kEUCLEAN;
    int rc = write_blocks(ssa_blkaddr + s, it->second.data(), 1);
    if (rc) return rc;
  }
  return 0;
}
int F2fsVolume::Impl::flush_nat() {
  for (auto& kv : nat_journal) nat_dirty.emplace(kv.first, kv.second);   // ジャーナルの内容も NAT ブロックへ書く (新しいチェックポイントのジャーナルは空)
  uint8_t* bm = nat_bm_w();
  auto it = nat_dirty.begin();
  while (it != nat_dirty.end()) {
    uint32_t bidx = it->first / kNatPerBlock;
    if (bidx >= nat_blocks || (bidx >> 3) >= nat_bitmap_bytes) return -kEUCLEAN;
    uint32_t base = nat_blkaddr + (bidx / kSegBlks) * kSegBlks * 2 + (bidx % kSegBlks);
    bool bit = bm_msb_get(bm, bidx);
    uint32_t cur_addr = base + (bit ? kSegBlks : 0), new_addr = base + (bit ? 0 : kSegBlks);
#ifdef USBMEM_MUTATE_F2FS_NAT_INPLACE
    new_addr = cur_addr;
#endif
    Blk b;
    int rc = get(cur_addr, &b);
    if (rc) return rc;
    std::vector<uint8_t> nb(*b);
    for (; it != nat_dirty.end() && it->first / kNatPerBlock == bidx; ++it) {
      const NatEnt& e = it->second;
      if (e.blkaddr == kNewAddr) return -kEUCLEAN;   // 未配置のノードが残っている
      uint8_t* p = nb.data() + 9 * (it->first % kNatPerBlock);
      p[0] = e.version; put32(p + 1, e.ino); put32(p + 5, e.blkaddr);
    }
    rc = write_blocks(new_addr, nb.data(), 1);
    if (rc) return rc;
#ifndef USBMEM_MUTATE_F2FS_NAT_INPLACE
    if (bit) bm_msb_clr(bm, bidx); else bm_msb_set(bm, bidx);
#endif
  }
  return 0;
}
int F2fsVolume::Impl::flush_sit() {
  uint8_t* bm = sit_bm_w();
  uint32_t per_copy = (sb_seg_sit / 2) * kSegBlks;
  uint32_t sbytes = le32(ckpt.data() + 156);
  for (uint32_t idx : sit_dirty) {
    if (idx >= per_copy || (idx >> 3) >= sbytes) return -kEUCLEAN;
    std::vector<uint8_t> nb(kBlk, 0);
    for (uint32_t i = 0; i < kSitPerBlock; i++) {
      uint32_t s = idx * kSitPerBlock + i;
      if (s >= main_segs) break;
      const Seg& g = segs[s];
      uint8_t* e = nb.data() + kSitEntSize * i;
      put16(e, static_cast<uint32_t>(g.vb) | (static_cast<uint32_t>(g.type) << 10));
      memcpy(e + 2, g.map, 64);
      put64(e + 66, g.mtime);
    }
    bool bit = bm_msb_get(bm, idx);
    uint32_t cur_addr = sit_blkaddr + idx + (bit ? per_copy : 0), new_addr = sit_blkaddr + idx + (bit ? 0 : per_copy);
#ifdef USBMEM_MUTATE_F2FS_SIT_INPLACE
    new_addr = cur_addr;
#endif
    int rc = write_blocks(new_addr, nb.data(), 1);
    if (rc) return rc;
#ifndef USBMEM_MUTATE_F2FS_SIT_INPLACE
    if (bit) bm_msb_clr(bm, idx); else bm_msb_set(bm, idx);
#endif
  }
  return 0;
}
int F2fsVolume::Impl::write_cp_pack() {
  if (!dpages.empty() || !nodes.empty() || pend_blocks != 0) return -kEUCLEAN;
  uint32_t dst = (cp_start == cp_blkaddr) ? cp_blkaddr + kSegBlks : cp_blkaddr;   // 常に今のパックの反対側へ
#ifdef USBMEM_MUTATE_F2FS_CP_SAME_PACK
  dst = cp_start;
#endif
  const uint32_t payload = sb_cp_payload, total = 8 + payload;
  std::vector<uint8_t> nck = ckpt;
  uint8_t* c = nck.data();
  uint64_t ver = cp_ver + 1;
#ifdef USBMEM_MUTATE_F2FS_CP_VER_SAME
  ver = cp_ver;
#endif
  put64(c, ver);
  put64(c + 16, valid_blocks);
  uint32_t fr = 0;
  for (uint32_t s = 0; s < main_segs; s++) if (segs[s].vb == 0 && !is_cur(s)) fr++;
  put32(c + 32, fr);
  for (int t = 0; t < 3; t++) {
    put32(c + 36 + 4 * t, cur[3 + t].segno); put16(c + 68 + 2 * t, cur[3 + t].blkoff);
    put32(c + 84 + 4 * t, cur[t].segno);     put16(c + 116 + 2 * t, cur[t].blkoff);
  }
  for (int i = 0; i < 6; i++) c[176 + i] = 0;   // LFS
  uint32_t flags = (cp_flags | kCpUmount) & ~(kCpCompactSum | 0x80u /*nat_bits*/ | 0x100u /*trimmed*/);
  put32(c + 132, flags);
  put32(c + 136, total);
  put32(c + 140, 1 + payload);
  put32(c + 144, valid_nodes); put32(c + 148, valid_inodes); put32(c + 152, nid_hint);
  uint32_t off = le32(c + 164);
  if (off < 192 || off > kBlk - 4) return -kEUCLEAN;
  uint32_t crc = crc32_le(kMagic, c, off);
  if (off < kBlk - 4) crc = crc32_le(crc, c + off + 4, kBlk - off - 4);
  put32(c + off, crc);
  std::vector<uint8_t> pack(static_cast<size_t>(total) * kBlk, 0);
  memcpy(pack.data(), nck.data(), static_cast<size_t>(1 + payload) * kBlk);
  for (int t = 0; t < 6; t++) {
    auto it = ssa.find(cur[t].segno);
    if (it == ssa.end()) return -kEUCLEAN;
    memcpy(pack.data() + static_cast<size_t>(1 + payload + t) * kBlk, it->second.data(), kBlk);
  }
  memcpy(pack.data() + static_cast<size_t>(total - 1) * kBlk, nck.data(), kBlk);   // 末尾 = 先頭の複製
  int rc;
#ifdef USBMEM_MUTATE_F2FS_FOOTER_FIRST
  rc = write_blocks(dst + total - 1, pack.data() + static_cast<size_t>(total - 1) * kBlk, 1);
  if (rc) return rc;
#endif
  rc = write_blocks(dst, pack.data(), total - 1);
  if (rc) return rc;
#ifndef USBMEM_MUTATE_F2FS_NO_FLUSH
  if (!dev->flush().ok()) return -EIO;      // 本体 (NAT / SIT / SSA / ノード / データを含む) がすべて永続化されてから末尾を書く
#endif
#ifndef USBMEM_MUTATE_F2FS_FOOTER_FIRST
  rc = write_blocks(dst + total - 1, pack.data() + static_cast<size_t>(total - 1) * kBlk, 1);
  if (rc) return rc;
#endif
  if (!dev->flush().ok()) return -EIO;
  // コミット完了
  size_t noff = static_cast<size_t>(nat_bitmap - ckpt.data());
  ckpt = std::move(nck);
  nat_bitmap = ckpt.data() + noff;
  cp_ver = ver; cp_flags = flags; cp_start = dst;
  return 0;
}
int F2fsVolume::Impl::checkpoint() {
  if (!rw) return -EROFS;
  if (broken) return -EIO;
  if (!dirty) return 0;
  auto bad = [&](int e) { broken = true; return e; };
  int rc = writeback_pages();
  if (rc) return bad(rc);
  rc = flush_nodes();
  if (rc) return bad(rc);
  rc = roll_full_cursegs();
  if (rc) return bad(rc);
  rc = write_ssa_blocks();
  if (rc) return bad(rc);
  rc = flush_nat();
  if (rc) return bad(rc);
  rc = flush_sit();
  if (rc) return bad(rc);
  rc = write_cp_pack();
  if (rc) return bad(rc);
  // コミット後の状態
  for (uint32_t idx : sit_dirty)
    for (uint32_t i = 0; i < kSitPerBlock; i++) { uint32_t s = idx * kSitPerBlock + i; if (s < main_segs) cvb[s] = segs[s].vb; }
  committed_cur.clear();
  for (int t = 0; t < 6; t++) committed_cur.insert(cur[t].segno);
  free_segs = 0;
  for (uint32_t s = 0; s < main_segs; s++) if (seg_is_free(s)) free_segs++;
  sit_dirty.clear(); ssa_dirty.clear(); nat_dirty.clear(); nat_journal.clear(); freed_nids.clear();
  for (auto it = ssa.begin(); it != ssa.end();) { if (is_cur(it->first)) ++it; else it = ssa.erase(it); }
  dirty = false;
  return 0;
}

// ---------------------------------------------------------------- 書き込み可マウントの準備
void F2fsVolume::Impl::reset_write_state() {
  rw = false; broken = false; dirty = false;
  segs.clear(); cvb.clear(); committed_cur.clear(); sit_dirty.clear(); ssa.clear(); ssa_dirty.clear();
  nodes.clear(); nat_dirty.clear(); freed_nids.clear(); dpages.clear();
  pend_blocks = 0; free_segs = 0;
}
int F2fsVolume::Impl::load_sit() {
  segs.assign(main_segs, Seg());
  cvb.assign(main_segs, 0);
  uint32_t nb = (main_segs + kSitPerBlock - 1) / kSitPerBlock, per_copy = (sb_seg_sit / 2) * kSegBlks;
  uint32_t sbytes = le32(ckpt.data() + 156);
  if (nb > per_copy || static_cast<uint64_t>(sbytes) * 8 < nb) return -kEUCLEAN;
  const uint8_t* bm = sit_bm_w();
  if (bm + sbytes > ckpt.data() + ckpt.size()) return -kEUCLEAN;
  std::vector<uint8_t> buf;
  for (uint32_t idx = 0; idx < nb;) {
    bool bit = bm_msb_get(bm, idx);
    uint32_t addr = sit_blkaddr + idx + (bit ? per_copy : 0);
    uint32_t run = 1;
    while (idx + run < nb && run < 128 && bm_msb_get(bm, idx + run) == bit) run++;
    if (static_cast<uint64_t>(addr) + run > dev_blocks) return -EIO;
    buf.resize(static_cast<size_t>(run) * kBlk);
    Status st = dev->read_at(static_cast<uint64_t>(addr) * kBlk, buf.data(), buf.size());
    if (!st.ok()) return -EIO;
    for (uint32_t k = 0; k < run; k++) {
      for (uint32_t i = 0; i < kSitPerBlock; i++) {
        uint32_t s = (idx + k) * kSitPerBlock + i;
        if (s >= main_segs) break;
        const uint8_t* e = buf.data() + static_cast<size_t>(k) * kBlk + kSitEntSize * i;
        uint32_t raw = le16(e);
        Seg& g = segs[s];
        g.vb = static_cast<uint16_t>(raw & 0x3FF);
        g.type = static_cast<uint8_t>(raw >> 10);
        if (g.type > 5 || g.vb > kSegBlks) return -kEUCLEAN;
        memcpy(g.map, e + 2, 64);
        g.mtime = le64(e + 66);
        uint32_t pc = 0;
        for (int q = 0; q < 64; q++) pc += static_cast<uint32_t>(__builtin_popcount(g.map[q]));
        if (pc != g.vb) return -kEUCLEAN;
      }
    }
    idx += run;
  }
  return 0;
}
int F2fsVolume::Impl::load_cursegs() {
  const uint8_t* c = ckpt.data();
  const bool umount = (cp_flags & kCpUmount) != 0, compact = (cp_flags & kCpCompactSum) != 0;
  const uint32_t total = le32(c + 136), start_sum = le32(c + 140);
  for (int t = 0; t < 3; t++) {
    cur[t].segno = le32(c + 84 + 4 * t); cur[t].blkoff = le16(c + 116 + 2 * t);
    cur[3 + t].segno = le32(c + 36 + 4 * t); cur[3 + t].blkoff = le16(c + 68 + 2 * t);
  }
  for (int i = 0; i < 6; i++) {
    if (c[176 + i] > 1) return -kEUCLEAN;
    if (cur[i].segno >= main_segs || cur[i].blkoff > kSegBlks) return -kEUCLEAN;
    if (c[176 + i] == 1) {
      // SSR (sload.f2fs などが付ける)。blkoff 以降がすべて空いていれば LFS と同じ追記として扱える (CP は LFS として書き直す)。穴を埋める割り当ては GC 無しでは扱わない
      for (uint32_t j = cur[i].blkoff; j < kSegBlks; j++)
        if (bm_msb_get(segs[cur[i].segno].map, j)) return -kEOPNOTSUPP;
    }
    for (int j = 0; j < i; j++) if (cur[j].segno == cur[i].segno) return -kEUCLEAN;
  }
  std::vector<uint8_t> sums[6];
  for (int i = 0; i < 6; i++) { sums[i].assign(kBlk, 0); sums[i][kBlk - 5] = i >= 3 ? 1 : 0; }
  const uint8_t* sitj = nullptr;
  Blk jb;
  if (compact) {
    uint32_t blk = start_sum;
    Blk b;
    if (blk == 0 || blk >= total) return -kEUCLEAN;
    int rc = get(cp_start + blk, &b);
    if (rc) return rc;
    sitj = b->data() + (kBlk - 5 - 7 * 512);
    jb = b;
    uint32_t off = 2 * (kBlk - 5 - 7 * 512);
    for (int t = 0; t < 3; t++) {
      for (uint32_t j = 0; j < cur[t].blkoff; j++) {
        if (off + 7 > kBlk - 5) {
          blk++;
          if (blk >= total) return -kEUCLEAN;
          rc = get(cp_start + blk, &b);
          if (rc) return rc;
          off = 0;
        }
        memcpy(sums[t].data() + 7 * j, b->data() + off, 7);
        off += 7;
      }
    }
  } else {
    uint32_t base = umount ? 6 : 3;
    if (total < base + 1) return -kEUCLEAN;
    for (int t = 0; t < 3; t++) {
      Blk b;
      int rc = get(cp_start + total - (base + 1) + t, &b);
      if (rc) return rc;
      memcpy(sums[t].data(), b->data(), 7 * 512);
      if (t == 2) { jb = b; sitj = b->data() + 7 * 512; }
    }
  }
  if (umount) {
    if (total < 4) return -kEUCLEAN;
    for (int t = 0; t < 3; t++) {
      Blk b;
      int rc = get(cp_start + total - 4 + t, &b);
      if (rc) return rc;
      memcpy(sums[3 + t].data(), b->data(), 7 * 512);
      for (uint32_t j = 0; j < 512; j++) { uint8_t* e = sums[3 + t].data() + 7 * j; e[4] = 0; put16(e + 5, 0); }
    }
  } else {
    for (int t = 0; t < 3; t++) {   // 非 UMOUNT: ノードのサマリはブロックの footer から復元する
      const Cur& cc = cur[3 + t];
      for (uint32_t j = 0; j < cc.blkoff; j++) {
        Blk b;
        int rc = get(main_blkaddr + cc.segno * kSegBlks + j, &b);
        if (rc) return rc;
        put32(sums[3 + t].data() + 7 * j, le32(b->data() + kBlk - 24));
      }
    }
  }
  // SIT ジャーナル (最新の SIT エントリ): segs に上書きし、そのブロックはダーティにして新しい CP で SIT に反映する
  if (sitj) {
    uint32_t n = le16(sitj);
    if (n > (kBlk - 5 - 7 * 512 - 2) / (4 + kSitEntSize)) return -kEUCLEAN;
    for (uint32_t i = 0; i < n; i++) {
      const uint8_t* e = sitj + 2 + (4 + kSitEntSize) * i;
      uint32_t s = le32(e);
      if (s >= main_segs) return -kEUCLEAN;
      const uint8_t* se = e + 4;
      uint32_t raw = le16(se);
      Seg& g = segs[s];
      g.vb = static_cast<uint16_t>(raw & 0x3FF);
      g.type = static_cast<uint8_t>(raw >> 10);
      if (g.type > 5 || g.vb > kSegBlks) return -kEUCLEAN;
      memcpy(g.map, se + 2, 64);
      g.mtime = le64(se + 66);
      uint32_t pc = 0;
      for (int q = 0; q < 64; q++) pc += static_cast<uint32_t>(__builtin_popcount(g.map[q]));
      if (pc != g.vb) return -kEUCLEAN;
      sit_dirty.insert(s / kSitPerBlock);
    }
  }
  (void)jb;
  for (int i = 0; i < 6; i++) { ssa[cur[i].segno] = std::move(sums[i]); }
  return 0;
}
int F2fsVolume::Impl::load_write_state() {
  reset_write_state();
  const uint8_t* c = ckpt.data();
  const uint32_t allowed = kFeatExtraAttr | 0x20u /*inode_checksum*/ | kFeatFlexXattr | 0x100u /*inode_crtime*/ | 0x200u /*lost_found*/ | kFeatSbChksum;
  if (feature & ~allowed) return -kEOPNOTSUPP;
  const uint32_t ok_flags = kCpUmount | kCpCompactSum | 0x20u /*fastboot*/ | kCpCrcRecovery | 0x80u /*nat_bits*/ | 0x100u /*trimmed*/ | kCpNocrcRecovery;
  if (cp_flags & ~ok_flags) return -kEOPNOTSUPP;     // orphan / error / fsck 要求 / large nat bitmap / checkpoint 停止 / resize など
  if (sb_segs_per_sec != 1 || sb_seg_main == 0 || sb_seg_main > 524288) return -kEOPNOTSUPP;
  if (static_cast<uint64_t>(main_blkaddr) + static_cast<uint64_t>(sb_seg_main) * kSegBlks > max_blkaddr) return -kEUCLEAN;
  main_segs = sb_seg_main;
  user_blocks = le64(c + 8); valid_blocks = le64(c + 16);
  valid_nodes = le32(c + 144); valid_inodes = le32(c + 148);
  nid_hint = le32(c + 152);
  cp_elapsed = le64(c + 168);
  mount_time = static_cast<uint64_t>(time(nullptr));
  int rc = load_sit();
  if (rc) return rc;
  rc = load_cursegs();
  if (rc) return rc;
  uint64_t sum = 0;
  for (uint32_t s = 0; s < main_segs; s++) sum += segs[s].vb;
  if (sum != valid_blocks) return -kEUCLEAN;         // NEW_ADDR など SIT に載らないブロックがある / 不整合: 書き込みは拒否
  Inode r;
  rc = load_inode(root_ino, &r);
  if (rc) return rc;
  new_extra = 0;
  if (feature & kFeatExtraAttr) {
    if (!(r.inl & kInlExtra)) return -kEOPNOTSUPP;
    new_extra = le16(r.blk->data() + 360);
    if (new_extra < 4 || (new_extra & 3) || new_extra > 36) return -kEOPNOTSUPP;
  }
  for (uint32_t s = 0; s < main_segs; s++) cvb[s] = segs[s].vb;
  committed_cur.clear();
  for (int t = 0; t < 6; t++) committed_cur.insert(cur[t].segno);
  free_segs = 0;
  for (uint32_t s = 0; s < main_segs; s++) if (seg_is_free(s)) free_segs++;
  seg_hint = 0;
  rw = true;
  return 0;
}
