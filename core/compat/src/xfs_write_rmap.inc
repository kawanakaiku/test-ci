// SPDX-License-Identifier: GPL-2.0-or-later
// XFS 書き込み: 逆引き B+木 (rmapbt)。xfs_write_alloc.inc の後で Impl の中に #include される。docs/xfs-write.md
//
// rmapbt は AG ごとに「ブロック範囲 → 持ち主 (inode / AG 内部)」を持つ B+木。ブロックの確保・解放のたびに更新する (Linux の xfs_rmap_map / xfs_rmap_unmap と同じ規則で、
// 隣と併合 / 分割する。xfs_repair が自分で作り直した結果と 1 レコードずつ比べるので、併合の規則はカーネルと同じでなければならない)。
//  - レコード (葉, 24 バイト): startblock(4) blockcount(4) owner(8) offset(8)。owner は inode 番号か特別な持ち主 (OWN_FS / OWN_AG / OWN_INOBT / OWN_INODES など = 負の値)。
//    offset の上位 3 ビットはフラグ (属性フォーク / bmbt ブロック / 書き込み前 extent)、下位 54 ビットが論理ブロック。
//  - ノード: 各エントリが (低いキー, 高いキー) の 2 つ (各 20 バイト) と子へのポインタ。高いキーは部分木の最大の「レコードの最後のブロック」(オーバーラップする区間木)。
//  - 木のブロックは AGFL から取り / に返す (bnobt / cntbt と同じ)。AGF の rmap_blocks と btreeblks に数える。
// ブロックの移動: 空き領域 ↔ AGFL は OWN_AG の rmap を足す / 外す (fix_freelist)。AGFL ↔ 木のブロックは何も変えない。

  static constexpr int BT_RMAP = 4;
  static constexpr uint32_t kRmapCrcMagic = 0x524d4233;   // "RMB3"
  static constexpr uint64_t kOwnFs = ~2ULL, kOwnLog = ~3ULL, kOwnAg = ~4ULL, kOwnInobt = ~5ULL, kOwnInodes = ~6ULL, kOwnRefc = ~7ULL, kOwnCow = ~8ULL;   // -3 .. -9
  static constexpr uint64_t kRmAttr = 1ULL << 63, kRmBmbt = 1ULL << 62, kRmUnw = 1ULL << 61, kRmFlags = 7ULL << 61, kRmOffMask = (1ULL << 54) - 1;
  static constexpr uint64_t kRmLenMax = 0xFFFFFFFFULL;
  bool has_rmapbt = false;

  // 持ち主 (fs_alloc / fs_free に渡す): owner = inode か OWN_*、off = フラグ | 論理ブロック
  struct Own { uint64_t owner, off; };
  static Own own_file(uint64_t ino, uint64_t lblk, bool unwritten = false) { return Own{ino, (lblk & kRmOffMask) | (unwritten ? kRmUnw : 0)}; }
  static Own own_bmbt(uint64_t ino) { return Own{ino, kRmBmbt}; }

  struct Rm { uint32_t start = 0, len = 0; uint64_t owner = 0, off = 0; };
  using RmE = std::array<uint8_t, 44>;
  static Rm rm_parse(const uint8_t* r) { Rm x; x.start = be32(r); x.len = be32(r + 4); x.owner = be64(r + 8); x.off = be64(r + 16); return x; }
  static void rm_put(const Rm& x, uint8_t* r) { pb32(r, x.start); pb32(r + 4, x.len); pb64(r + 8, x.owner); pb64(r + 16, x.off); }
  static bool rm_noninode(uint64_t o) { return (o >> 63) != 0; }
  static void rm_lowkey(const Rm& x, uint8_t* k) { pb32(k, x.start); pb64(k + 4, x.owner); pb64(k + 12, x.off); }
  static void rm_highkey(const Rm& x, uint8_t* k) {
    const uint32_t adj = x.len - 1;
    pb32(k, x.start + adj); pb64(k + 4, x.owner);
    if (rm_noninode(x.owner) || (x.off & kRmBmbt)) { pb64(k + 12, x.off); return; }
    pb64(k + 12, ((x.off & kRmOffMask) + adj) & kRmOffMask);
  }
  static int rm_kcmp(const uint8_t* a, const uint8_t* b) {
    uint32_t x = be32(a), y = be32(b);
    if (x != y) return x < y ? -1 : 1;
    uint64_t p = be64(a + 4), q = be64(b + 4);
    if (p != q) return p < q ? -1 : 1;
    p = be64(a + 12) & kRmOffMask; q = be64(b + 12) & kRmOffMask;
    return p < q ? -1 : (p > q ? 1 : 0);
  }
  uint32_t rm_nmax() const { return (bs - 56) / 44; }
  uint32_t rm_lmax() const { return (bs - 56) / 24; }
  uint32_t rm_max(bool leaf) const { return leaf ? rm_lmax() : rm_nmax(); }

  // ---- AGF の rmap 関連 ----
  int rm_root(uint32_t ag, uint32_t* root, uint32_t* height) {
    WBuf* b; int rc = agf_buf(ag, &b); if (rc) return rc;
    *root = be32(b->d.data() + 24); *height = be32(b->d.data() + 36);
    return 0;
  }
  int rm_set_root(uint32_t ag, uint32_t root, uint32_t height) {
    WBuf* b; int rc = agf_buf(ag, &b); if (rc) return rc;
    uint8_t* p = bw(b); pb32(p + 24, root); pb32(p + 36, height);
    return 0;
  }
  int rm_blocks_adj(uint32_t ag, int d) {
    WBuf* b; int rc = agf_buf(ag, &b); if (rc) return rc;
    uint8_t* p = bw(b); pb32(p + 80, uint32_t(int64_t(be32(p + 80)) + d));
    return 0;
  }
  // 木のブロックは AGFL から (bnobt / cntbt と同じ)。rmap_blocks と btreeblks に数える。AGFL ↔ 木は rmap を変えない
  int rm_alloc_block(uint32_t ag, uint32_t* bno) {
    int rc = agfl_get(ag, bno); if (rc) return rc;
    rc = agf_adj(ag, 0, 0, 1); if (rc) return rc;
    return rm_blocks_adj(ag, 1);
  }
  int rm_free_block(uint32_t ag, uint32_t bno) {
    bdrop_fresh(agb_off(ag, bno));
    int rc = agfl_put(ag, bno); if (rc) return rc;
    rc = agf_adj(ag, 0, 0, -1); if (rc) return rc;
    return rm_blocks_adj(ag, -1);
  }

  // ---- ブロックの読み書き (エントリ配列 RmE: 葉は先頭 24 バイト = レコード、ノードは 0..39 = (低, 高) のキー、40..43 = 子のポインタ) ----
  void rm_load(const WBuf* b, std::vector<RmE>* v) const {
    const uint8_t* p = b->d.data();
    const bool leaf = be16(p + 4) == 0;
    const uint32_t n = std::min<uint32_t>(be16(p + 6), rm_max(leaf));
    v->assign(n, RmE{});
    for (uint32_t i = 0; i < n; i++) {
      if (leaf) std::memcpy((*v)[i].data(), p + 56 + size_t(i) * 24, 24);
      else { std::memcpy((*v)[i].data(), p + 56 + size_t(i) * 40, 40); std::memcpy((*v)[i].data() + 40, p + 56 + size_t(rm_nmax()) * 40 + size_t(i) * 4, 4); }
    }
  }
  void rm_store(WBuf* b, const std::vector<RmE>& v) {
    uint8_t* p = bw(b);
    const bool leaf = be16(p + 4) == 0;
    pb16(p + 6, uint32_t(v.size()));
    for (size_t i = 0; i < v.size(); i++) {
      if (leaf) std::memcpy(p + 56 + i * 24, v[i].data(), 24);
      else { std::memcpy(p + 56 + i * 40, v[i].data(), 40); std::memcpy(p + 56 + size_t(rm_nmax()) * 40 + i * 4, v[i].data() + 40, 4); }
    }
  }
  static void rm_ent_low(bool leaf, const RmE& e, uint8_t* out) { if (leaf) rm_lowkey(rm_parse(e.data()), out); else std::memcpy(out, e.data(), 20); }
  static void rm_ent_high(bool leaf, const RmE& e, uint8_t* out) { if (leaf) rm_highkey(rm_parse(e.data()), out); else std::memcpy(out, e.data() + 20, 20); }
  // エントリ列の (最小の低いキー, 最大の高いキー)
  static void rm_ents_keys(bool leaf, const std::vector<RmE>& v, uint8_t* low, uint8_t* high) {
    rm_ent_low(leaf, v[0], low); rm_ent_high(leaf, v[0], high);
    for (size_t i = 1; i < v.size(); i++) {
      uint8_t h[20]; rm_ent_high(leaf, v[i], h);
      if (rm_kcmp(h, high) > 0) std::memcpy(high, h, 20);
    }
  }
  int rm_blk_keys(uint32_t ag, uint32_t bno, uint8_t* low, uint8_t* high) {
    WBuf* b; int rc = bt_blk(ag, bno, &b); if (rc) return rc;
    if (be32(b->d.data()) != kRmapCrcMagic) return -EUCLEAN;
    std::vector<RmE> v; rm_load(b, &v);
    if (v.empty()) return -EUCLEAN;
    rm_ents_keys(be16(b->d.data() + 4) == 0, v, low, high);
    return 0;
  }

  // ---- 検索 ----
  struct RmP { WBuf* b; uint32_t bno; uint32_t idx; };
  // key を含む / 直前の位置まで根から降りる。葉の idx = key 以下のレコードの個数 (idx - 1 が key 以下で最大のレコード)。ノードの idx = 低いキーが key 以下の最後のエントリ
  int rm_find(uint32_t ag, const uint8_t* key, std::vector<RmP>* path) {
    path->clear();
    uint32_t root, height;
    int rc = rm_root(ag, &root, &height); if (rc) return rc;
    if (height < 1 || height > 12 || root == 0 || root == kNullAgbno) return -EUCLEAN;
    uint32_t bno = root;
    for (uint32_t lv = height; lv >= 1; lv--) {
      WBuf* b; rc = bt_blk(ag, bno, &b); if (rc) return rc;
      const uint8_t* p = b->d.data();
      if (be32(p) != kRmapCrcMagic || be16(p + 4) != lv - 1) return -EUCLEAN;
      const uint32_t n = be16(p + 6);
      RmP pe{b, bno, 0};
      if (lv == 1) {
        if (n > rm_lmax()) return -EUCLEAN;
        uint32_t lo = 0, hi = n;
        while (lo < hi) {
          uint32_t mid = (lo + hi) / 2; uint8_t k[20];
          rm_lowkey(rm_parse(p + 56 + size_t(mid) * 24), k);
          if (rm_kcmp(k, key) <= 0) lo = mid + 1; else hi = mid;
        }
        pe.idx = lo;
        path->push_back(pe);
        break;
      }
      if (n == 0 || n > rm_nmax()) return -EUCLEAN;
      uint32_t lo = 0, hi = n;
      while (lo < hi) { uint32_t mid = (lo + hi) / 2; if (rm_kcmp(p + 56 + size_t(mid) * 40, key) <= 0) lo = mid + 1; else hi = mid; }
      pe.idx = lo ? lo - 1 : 0;
      path->push_back(pe);
      bno = be32(p + 56 + size_t(rm_nmax()) * 40 + size_t(pe.idx) * 4);
    }
    return 0;
  }
  // key 以下で最大のレコード (lt) と、その次のレコード (gt) を返す。無ければ has_* = false
  int rm_neighbors(uint32_t ag, const uint8_t* key, Rm* lt, bool* hl, Rm* gt, bool* hg) {
    std::vector<RmP> path;
    int rc = rm_find(ag, key, &path); if (rc) return rc;
    RmP& lf = path.back();
    const uint8_t* p = lf.b->d.data();
    const uint32_t n = be16(p + 6);
    *hl = lf.idx > 0; *hg = false;
    if (*hl) *lt = rm_parse(p + 56 + size_t(lf.idx - 1) * 24);
    if (lf.idx < n) { *gt = rm_parse(p + 56 + size_t(lf.idx) * 24); *hg = true; return 0; }
    uint32_t rs = be32(p + 12);
    for (int guard = 0; rs != kNullAgbno && guard < 100000; guard++) {   // 右隣の葉 (空の葉は飛ばす)
      WBuf* r; rc = bt_blk(ag, rs, &r); if (rc) return rc;
      if (be16(r->d.data() + 6) > 0) { *gt = rm_parse(r->d.data() + 56); *hg = true; return 0; }
      rs = be32(r->d.data() + 12);
    }
    return 0;
  }

  // ---- キーの更新 (path[pos] のブロックから根へ。親のエントリのキーを子から作り直す。変わらなければそこで止める) ----
  int rm_fix_up(uint32_t ag, std::vector<RmP>& path, int pos) {
    for (int p = pos; p > 0; p--) {
      uint8_t low[20], high[20];
      std::vector<RmE> v; rm_load(path[p].b, &v);
      if (v.empty()) return -EUCLEAN;
      rm_ents_keys(be16(path[p].b->d.data() + 4) == 0, v, low, high);
      WBuf* par = path[p - 1].b;
      uint8_t* kp = par->d.data() + 56 + size_t(path[p - 1].idx) * 40;
      if (std::memcmp(kp, low, 20) == 0 && std::memcmp(kp + 20, high, 20) == 0) break;
      kp = bw(par) + 56 + size_t(path[p - 1].idx) * 40;
      std::memcpy(kp, low, 20); std::memcpy(kp + 20, high, 20);
    }
    (void)ag;
    return 0;
  }

  // ---- 挿入側: path[pos] のブロックの全エントリを ents にする。溢れたら分割して親へ登録する ----
  int rm_commit_block(uint32_t ag, std::vector<RmP>& path, int pos, std::vector<RmE>& ents) {
    WBuf* b = path[pos].b;
    const bool leaf = be16(b->d.data() + 4) == 0;
    if (ents.size() <= rm_max(leaf)) { rm_store(b, ents); return rm_fix_up(ag, path, pos); }
    const size_t n = ents.size(), nl = (n + 1) / 2;
    std::vector<RmE> le(ents.begin(), ents.begin() + static_cast<std::ptrdiff_t>(nl)), re(ents.begin() + static_cast<std::ptrdiff_t>(nl), ents.end());
    uint32_t rbno;
    int rc = rm_alloc_block(ag, &rbno); if (rc) return rc;
    const uint32_t level = be16(b->d.data() + 4), oldright = be32(b->d.data() + 12);
    WBuf* r = bt_newblk(ag, BT_RMAP, rbno, level, path[pos].bno, oldright);
    rm_store(b, le);
    pb32(bw(b) + 12, rbno);
    rm_store(r, re);
    if (oldright != kNullAgbno) { WBuf* o; rc = bt_blk(ag, oldright, &o); if (rc) return rc; pb32(bw(o) + 8, rbno); }
    uint8_t ll[20], lh[20], rl[20], rh[20];
    rm_ents_keys(leaf, le, ll, lh); rm_ents_keys(leaf, re, rl, rh);
    if (pos == 0) {
      uint32_t nroot, root, height;
      rc = rm_alloc_block(ag, &nroot); if (rc) return rc;
      rc = rm_root(ag, &root, &height); if (rc) return rc;
      WBuf* nb = bt_newblk(ag, BT_RMAP, nroot, level + 1, kNullAgbno, kNullAgbno);
      std::vector<RmE> ne(2);
      std::memcpy(ne[0].data(), ll, 20); std::memcpy(ne[0].data() + 20, lh, 20); pb32(ne[0].data() + 40, path[0].bno);
      std::memcpy(ne[1].data(), rl, 20); std::memcpy(ne[1].data() + 20, rh, 20); pb32(ne[1].data() + 40, rbno);
      rm_store(nb, ne);
      return rm_set_root(ag, nroot, height + 1);
    }
    std::vector<RmE> pent; rm_load(path[pos - 1].b, &pent);
    const uint32_t pi = path[pos - 1].idx;
    if (pi >= pent.size()) return -EUCLEAN;
    std::memcpy(pent[pi].data(), ll, 20); std::memcpy(pent[pi].data() + 20, lh, 20);
    RmE ne{}; std::memcpy(ne.data(), rl, 20); std::memcpy(ne.data() + 20, rh, 20); pb32(ne.data() + 40, rbno);
    pent.insert(pent.begin() + static_cast<std::ptrdiff_t>(pi) + 1, ne);
    return rm_commit_block(ag, path, pos - 1, pent);
  }
  int rm_insert(uint32_t ag, const Rm& rec) {
    uint8_t key[20]; rm_lowkey(rec, key);
    std::vector<RmP> path;
    int rc = rm_find(ag, key, &path); if (rc) return rc;
    std::vector<RmE> ents; rm_load(path.back().b, &ents);
    const uint32_t idx = path.back().idx;
    if (idx > ents.size()) return -EUCLEAN;
    if (idx > 0) { uint8_t k2[20]; rm_lowkey(rm_parse(ents[idx - 1].data()), k2); if (rm_kcmp(k2, key) == 0) return -EUCLEAN; }   // 重複
    RmE e{}; rm_put(rec, e.data());
    ents.insert(ents.begin() + static_cast<std::ptrdiff_t>(idx), e);
    return rm_commit_block(ag, path, int(path.size()) - 1, ents);
  }
  // 同じ位置のレコード old を new に置き換える (new のキーは隣のレコードとの順序を変えない)
  int rm_replace(uint32_t ag, const Rm& old, const Rm& nw) {
    uint8_t key[20]; rm_lowkey(old, key);
    std::vector<RmP> path;
    int rc = rm_find(ag, key, &path); if (rc) return rc;
    RmP& lf = path.back();
    if (lf.idx == 0) return -EUCLEAN;
    uint8_t k2[20]; rm_lowkey(rm_parse(lf.b->d.data() + 56 + size_t(lf.idx - 1) * 24), k2);
    if (rm_kcmp(k2, key) != 0) return -EUCLEAN;
    rm_put(nw, bw(lf.b) + 56 + size_t(lf.idx - 1) * 24);
    return rm_fix_up(ag, path, int(path.size()) - 1);
  }

  // ---- 削除側 ----
  // path[pos] のブロックからエントリ idx を取り除く。refresh = 取り除いたあと、キーを子から作り直すエントリの位置 (ノードのみ)
  int rm_remove_at(uint32_t ag, std::vector<RmP>& path, int pos, uint32_t idx, std::vector<uint32_t> refresh) {
    WBuf* b = path[pos].b;
    const bool leaf = be16(b->d.data() + 4) == 0;
    std::vector<RmE> ents; rm_load(b, &ents);
    if (idx >= ents.size()) return -EUCLEAN;
    ents.erase(ents.begin() + static_cast<std::ptrdiff_t>(idx));
    for (uint32_t r : refresh) {
      if (leaf || r >= ents.size()) return -EUCLEAN;
      uint8_t lo[20], hi[20];
      int rc = rm_blk_keys(ag, be32(ents[r].data() + 40), lo, hi); if (rc) return rc;
      std::memcpy(ents[r].data(), lo, 20); std::memcpy(ents[r].data() + 20, hi, 20);
    }
    rm_store(b, ents);
    if (pos == 0) return 0;
    const uint32_t mn = rm_max(leaf) / 2;
    if (ents.size() >= mn && !ents.empty()) return rm_fix_up(ag, path, pos);
    // 最小のレコード数を割った: 同じ親の隣のブロックと併合 / 振り分け
    std::vector<RmE> pent; rm_load(path[pos - 1].b, &pent);
    const uint32_t pn = uint32_t(pent.size()), ci = path[pos - 1].idx;
    if (pn < 2 || ci >= pn) { if (ents.empty()) return -EUCLEAN; return rm_fix_up(ag, path, pos); }
    const uint32_t li = ci + 1 < pn ? ci : ci - 1;
    const uint32_t lbno = be32(pent[li].data() + 40), rbno = be32(pent[li + 1].data() + 40);
    WBuf *L, *R;
    int rc = bt_blk(ag, lbno, &L); if (rc) return rc;
    rc = bt_blk(ag, rbno, &R); if (rc) return rc;
    std::vector<RmE> le, re; rm_load(L, &le); rm_load(R, &re);
    const size_t tot = le.size() + re.size(), mx = rm_max(leaf);
    if (tot <= mx) {
      le.insert(le.end(), re.begin(), re.end());
      if (le.empty()) return -EUCLEAN;
      rm_store(L, le);
      const uint32_t rsib = be32(R->d.data() + 12);
      pb32(bw(L) + 12, rsib);
      if (rsib != kNullAgbno) { WBuf* x; rc = bt_blk(ag, rsib, &x); if (rc) return rc; pb32(bw(x) + 8, lbno); }
      rc = rm_free_block(ag, rbno); if (rc) return rc;
      return rm_remove_at(ag, path, pos - 1, li + 1, {li});
    }
    const size_t tl = tot / 2;
    std::vector<RmE> all = le; all.insert(all.end(), re.begin(), re.end());
    std::vector<RmE> nl(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(tl)), nr(all.begin() + static_cast<std::ptrdiff_t>(tl), all.end());
    rm_store(L, nl); rm_store(R, nr);
    uint8_t lo[20], hi[20];
    rm_ents_keys(leaf, nl, lo, hi); std::memcpy(pent[li].data(), lo, 20); std::memcpy(pent[li].data() + 20, hi, 20);
    rm_ents_keys(leaf, nr, lo, hi); std::memcpy(pent[li + 1].data(), lo, 20); std::memcpy(pent[li + 1].data() + 20, hi, 20);
    rm_store(path[pos - 1].b, pent);
    return rm_fix_up(ag, path, pos - 1);
  }
  // 根が子 1 つだけなら高さを減らす
  int rm_collapse(uint32_t ag) {
    for (int guard = 0; guard < 16; guard++) {
      uint32_t root, height;
      int rc = rm_root(ag, &root, &height); if (rc) return rc;
      if (height <= 1) return 0;
      WBuf* b; rc = bt_blk(ag, root, &b); if (rc) return rc;
      if (be16(b->d.data() + 6) != 1) return 0;
      uint32_t child = be32(b->d.data() + 56 + size_t(rm_nmax()) * 40);
      rc = rm_set_root(ag, child, height - 1); if (rc) return rc;
      rc = rm_free_block(ag, root); if (rc) return rc;
    }
    return 0;
  }
  int rm_delete(uint32_t ag, const Rm& rec) {
    uint8_t key[20]; rm_lowkey(rec, key);
    std::vector<RmP> path;
    int rc = rm_find(ag, key, &path); if (rc) return rc;
    RmP& lf = path.back();
    if (lf.idx == 0) return -EUCLEAN;
    uint8_t k2[20]; rm_lowkey(rm_parse(lf.b->d.data() + 56 + size_t(lf.idx - 1) * 24), k2);
    if (rm_kcmp(k2, key) != 0) return -EUCLEAN;
    rc = rm_remove_at(ag, path, int(path.size()) - 1, lf.idx - 1, {}); if (rc) return rc;
    return rm_collapse(ag);
  }

  // ============ 公開: map / unmap (xfs_rmap_map / xfs_rmap_unmap 相当) ============
  static bool rm_mergeable(const Rm& r, uint64_t owner, uint64_t offraw) { return r.owner == owner && ((r.off ^ offraw) & kRmFlags) == 0; }
  // [bno, bno+len) を (owner, offraw) の持ち主として足す。隣と併合する
  int rm_map(uint32_t ag, uint32_t bno, uint32_t len, uint64_t owner, uint64_t offraw) {
    if (!has_rmapbt) return 0;
    if (len == 0) return -EINVAL;
    const bool inode_off = !rm_noninode(owner) && !(offraw & kRmBmbt);
    uint8_t key[20]; Rm probe; probe.start = bno; probe.len = len; probe.owner = owner; probe.off = offraw; rm_lowkey(probe, key);
    Rm lt, gt; bool hl, hg;
    int rc = rm_neighbors(ag, key, &lt, &hl, &gt, &hg); if (rc) return rc;
    if (hl) { uint8_t k2[20]; rm_lowkey(lt, k2); if (rm_kcmp(k2, key) == 0) return -EUCLEAN; }   // 同じキー: 二重登録
    if (hl && uint64_t(lt.start) + lt.len > bno) return -EUCLEAN;   // 重なる (新しく確保したブロックには起きない)
    if (hg && uint64_t(gt.start) < uint64_t(bno) + len) return -EUCLEAN;
    bool ml = hl && rm_mergeable(lt, owner, offraw) && uint64_t(lt.start) + lt.len == bno &&
              (!inode_off || (lt.off & kRmOffMask) + lt.len == (offraw & kRmOffMask));
    bool mg = hg && rm_mergeable(gt, owner, offraw) && uint64_t(gt.start) == uint64_t(bno) + len &&
              (!inode_off || (offraw & kRmOffMask) + len == (gt.off & kRmOffMask));
    if (ml && uint64_t(lt.len) + len > kRmLenMax) ml = false;
    if (mg && uint64_t(len) + gt.len > kRmLenMax) mg = false;
    if (ml && mg && uint64_t(lt.len) + len + gt.len > kRmLenMax) mg = false;
    if (ml && mg) {
      Rm n = lt; n.len = lt.len + len + gt.len;
      rc = rm_replace(ag, lt, n); if (rc) return rc;
      return rm_delete(ag, gt);
    }
    if (ml) { Rm n = lt; n.len = lt.len + len; return rm_replace(ag, lt, n); }
    if (mg) { Rm n = gt; n.start = bno; n.len = len + gt.len; if (inode_off) n.off = (gt.off & kRmFlags) | (offraw & kRmOffMask); return rm_replace(ag, gt, n); }
    return rm_insert(ag, probe);
  }
  // [bno, bno+len) の (owner, offraw) の登録を外す。1 つのレコードに含まれていること (端 / 中 / 全体)
  int rm_unmap(uint32_t ag, uint32_t bno, uint32_t len, uint64_t owner, uint64_t offraw) {
    if (!has_rmapbt) return 0;
    if (len == 0) return -EINVAL;
    const bool inode_off = !rm_noninode(owner) && !(offraw & kRmBmbt);
    uint8_t key[20]; Rm probe; probe.start = bno; probe.len = len; probe.owner = owner; probe.off = offraw; rm_lowkey(probe, key);
    Rm lt, gt; bool hl, hg;
    int rc = rm_neighbors(ag, key, &lt, &hl, &gt, &hg); if (rc) return rc;
    if (!hl || lt.owner != owner || ((lt.off ^ offraw) & kRmFlags)) return -EUCLEAN;
    if (lt.start > bno || uint64_t(lt.start) + lt.len < uint64_t(bno) + len) return -EUCLEAN;
    const uint64_t d = bno - lt.start;
    if (inode_off && (lt.off & kRmOffMask) + d != (offraw & kRmOffMask)) return -EUCLEAN;
    if (lt.start == bno && lt.len == len) return rm_delete(ag, lt);
    if (lt.start == bno) {   // 先頭を削る
      Rm n = lt; n.start = bno + len; n.len = lt.len - len;
      if (inode_off) n.off = (lt.off & kRmFlags) | (((lt.off & kRmOffMask) + len) & kRmOffMask);
      return rm_replace(ag, lt, n);
    }
    if (uint64_t(lt.start) + lt.len == uint64_t(bno) + len) { Rm n = lt; n.len = uint32_t(d); return rm_replace(ag, lt, n); }   // 末尾を削る
    // 中を削る: 前半を縮め、後半を新しいレコードにする
    Rm a = lt; a.len = uint32_t(d);
    Rm c = lt; c.start = bno + len; c.len = uint32_t(uint64_t(lt.start) + lt.len - bno - len);
    if (inode_off) c.off = (lt.off & kRmFlags) | (((lt.off & kRmOffMask) + d + len) & kRmOffMask);
    rc = rm_replace(ag, lt, a); if (rc) return rc;
    return rm_insert(ag, c);
  }
  int rm_map_fsb(uint64_t fsb, uint32_t len, const Own& o) { return rm_map(uint32_t(fsb >> agblklog), uint32_t(fsb & ((1ULL << agblklog) - 1)), len, o.owner, o.off); }
  int rm_unmap_fsb(uint64_t fsb, uint32_t len, const Own& o) { return rm_unmap(uint32_t(fsb >> agblklog), uint32_t(fsb & ((1ULL << agblklog) - 1)), len, o.owner, o.off); }
