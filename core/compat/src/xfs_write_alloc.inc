// SPDX-License-Identifier: GPL-2.0-or-later
// XFS 書き込み: AG の空き領域 (bnobt / cntbt + AGFL)、inode B+木 (inobt / finobt)、inode の確保。xfs_write.inc の後で Impl の中に #include される。docs/xfs-write.md
//
// B+木は 4 種類 (BT_BNO / BT_CNT / BT_INO / BT_FINO) を 1 つの汎用の挿入 / 削除で扱う (短いポインタ形式、V5 の 56 バイトのヘッダ)。
//  - 挿入: 満杯なら半分に分割して親に登録 (根が割れたら高さが 1 増える)。キーが先頭に入るときは親のキーを更新する
//  - 削除: ブロックが空になったら解放して親から外す (根が子 1 つだけになったら高さを 1 減らす)。空きブロックを使い回すための併合はしない (カーネルも読める形)
//  - ブロックの確保 / 解放: 空き領域の木 (bnobt / cntbt) は AGFL から取り / に戻す。inode 木は空き領域から 1 ブロック確保 / に返す
// 空き領域の操作の前には fix_freelist() で AGFL に (2 本の木の高さ + 1) ブロックを用意する (木の分割が AGFL を使い切らないように)。

  // ============ AG ヘッダ ============
  static constexpr int BT_BNO = 0, BT_CNT = 1, BT_INO = 2, BT_FINO = 3;
  static constexpr uint32_t kNullAgbno = 0xFFFFFFFFu;
  int agf_buf(uint32_t ag, WBuf** b) { return bget(agb_off(ag, 0) + sectsize, sectsize, BLFT_AGF, b); }
  int agi_buf(uint32_t ag, WBuf** b) { return bget(agb_off(ag, 0) + 2 * sectsize, sectsize, BLFT_AGI, b); }
  int agfl_buf(uint32_t ag, WBuf** b) { return bget(agb_off(ag, 0) + 3 * sectsize, sectsize, BLFT_AGFL, b); }

  // AGF の freeblks / flcount / btreeblks を増減する (sb の fdblocks の差分も合わせる)
  int agf_adj(uint32_t ag, int64_t dfree, int64_t dfl, int64_t dbt) {
    WBuf* b; int rc = agf_buf(ag, &b); if (rc) return rc;
    uint8_t* p = bw(b);
    pb32(p + 52, uint32_t(int64_t(be32(p + 52)) + dfree));
    pb32(p + 48, uint32_t(int64_t(be32(p + 48)) + dfl));
    pb32(p + 60, uint32_t(int64_t(be32(p + 60)) + dbt));
    d_fdblocks += dfree + dfl + dbt;
    return 0;
  }

  uint32_t bt_magic(int k) const { static const uint32_t m[4] = {0x41423342, 0x41423343, 0x49414233, 0x46494233}; return m[k]; }
  uint32_t bt_keylen(int k) const { return k < 2 ? 8 : 4; }
  uint32_t bt_reclen(int k) const { return k < 2 ? 8 : 16; }
  uint32_t bt_nmax(int k) const { return (bs - 56) / (bt_keylen(k) + 4); }
  uint32_t bt_lmax(int k) const { return (bs - 56) / bt_reclen(k); }
  static int bt_cmpk(int k, const uint8_t* a, const uint8_t* b) {
    if (k == BT_CNT) { uint32_t x = be32(a + 4), y = be32(b + 4); if (x != y) return x < y ? -1 : 1; }
    uint32_t x = be32(a), y = be32(b);
    return x < y ? -1 : (x > y ? 1 : 0);
  }
  uint8_t* bt_keyp(int k, WBuf* b, uint32_t i) { return b->d.data() + 56 + size_t(i) * bt_keylen(k); }
  uint8_t* bt_ptrp(int k, WBuf* b, uint32_t i) { return b->d.data() + 56 + size_t(bt_nmax(k)) * bt_keylen(k) + size_t(i) * 4; }
  uint8_t* bt_recp(int k, WBuf* b, uint32_t i) { return b->d.data() + 56 + size_t(i) * bt_reclen(k); }
  static uint32_t bt_nrecs(const WBuf* b) { return be16(b->d.data() + 6); }
  static uint32_t bt_level(const WBuf* b) { return be16(b->d.data() + 4); }

  // 木の根と高さ (高さ = ブロックの level + 1) を AGF / AGI から読み書きする
  int bt_root(uint32_t ag, int k, uint32_t* root, uint32_t* height) {
    WBuf* b; int rc = (k < 2) ? agf_buf(ag, &b) : agi_buf(ag, &b); if (rc) return rc;
    const uint8_t* p = b->d.data();
    switch (k) {
      case BT_BNO: *root = be32(p + 16); *height = be32(p + 28); break;
      case BT_CNT: *root = be32(p + 20); *height = be32(p + 32); break;
      case BT_INO: *root = be32(p + 20); *height = be32(p + 24); break;
      default: *root = be32(p + 328); *height = be32(p + 332); break;
    }
    return 0;
  }
  int bt_set_root(uint32_t ag, int k, uint32_t root, uint32_t height) {
    WBuf* b; int rc = (k < 2) ? agf_buf(ag, &b) : agi_buf(ag, &b); if (rc) return rc;
    uint8_t* p = bw(b);
    switch (k) {
      case BT_BNO: pb32(p + 16, root); pb32(p + 28, height); break;
      case BT_CNT: pb32(p + 20, root); pb32(p + 32, height); break;
      case BT_INO: pb32(p + 20, root); pb32(p + 24, height); break;
      default: pb32(p + 328, root); pb32(p + 332, height); break;
    }
    return 0;
  }
  int bt_blk(uint32_t ag, uint32_t bno, WBuf** b) {
    if (bno >= ag_len(ag)) return -EUCLEAN;
    int rc = bget(agb_off(ag, bno), bs, BLFT_BTREE, b);
    if (rc) return rc;
    const uint8_t* p = (*b)->d.data();
    uint32_t m = be32(p);
    if (!is_short_btree_magic(m)) return -EUCLEAN;
    if (be64(p + 16) != (*b)->off / 512) return -EUCLEAN;
    return 0;
  }
  WBuf* bt_newblk(uint32_t ag, int k, uint32_t bno, uint32_t level, uint32_t left, uint32_t right) {
    WBuf* b = bnew(agb_off(ag, bno), bs, BLFT_BTREE);
    uint8_t* p = b->d.data();
    pb32(p, bt_magic(k)); pb16(p + 4, level); pb16(p + 6, 0);
    pb32(p + 8, left); pb32(p + 12, right);
    pb64(p + 16, b->off / 512);
    std::memcpy(p + 32, uuid, 16);
    pb32(p + 48, ag);
    return b;
  }

  // ---- AGFL ----
  int agfl_get(uint32_t ag, uint32_t* bno) {
    WBuf *f, *l;
    int rc = agf_buf(ag, &f); if (rc) return rc;
    rc = agfl_buf(ag, &l); if (rc) return rc;
    uint8_t* fp = bw(f);
    uint32_t cnt = be32(fp + 48), first = be32(fp + 40);
    if (cnt == 0) return -ENOSPC;
    *bno = be32(l->d.data() + 36 + 4 * first);
    pb32(fp + 40, (first + 1) % agfl_slots);
    pb32(fp + 48, cnt - 1);
    d_fdblocks -= 1;   // flcount の減りは、呼び出し側が btreeblks / freeblks に移すときに差し引きゼロになるよう agf_adj で補う
    return 0;
  }
  int agfl_put(uint32_t ag, uint32_t bno) {
    WBuf *f, *l;
    int rc = agf_buf(ag, &f); if (rc) return rc;
    rc = agfl_buf(ag, &l); if (rc) return rc;
    uint8_t* fp = bw(f);
    uint32_t cnt = be32(fp + 48), last = be32(fp + 44);
    if (cnt >= agfl_slots) return -ENOSPC;
    uint32_t nl = (last + 1) % agfl_slots;
    pb32(bw(l) + 36 + 4 * nl, bno);
    pb32(fp + 44, nl);
    pb32(fp + 48, cnt + 1);
    d_fdblocks += 1;
    return 0;
  }

  // ============ 汎用 B+木 ============
  struct Pe { WBuf* b; uint32_t bno; uint32_t idx; };

  int bt_alloc_block(uint32_t ag, int k, uint32_t* bno) {
    if (k < 2) {
      int rc = agfl_get(ag, bno); if (rc) return rc == -ENOSPC ? -EUCLEAN : rc;
      return agf_adj(ag, 0, 0, 1) ? -EIO : 0;   // AGFL → 木のブロック: flcount は agfl_get が減らした。btreeblks +1。fdblocks は (-1 +1) で 0
    }
    uint32_t start, got;
    int rc = ag_alloc_raw(ag, 1, 1, kNullAgbno, &start, &got); if (rc) return rc;
    *bno = start;
    WBuf* b; rc = agi_buf(ag, &b); if (rc) return rc;
    if (has_inobtcount) { uint8_t* p = bw(b); pb32(p + (k == BT_INO ? 336 : 340), be32(p + (k == BT_INO ? 336 : 340)) + 1); }
    return 0;
  }
  int bt_free_block(uint32_t ag, int k, uint32_t bno) {
    // バッファは捨てる (中身はもう参照されない)
    auto it = wb.find(agb_off(ag, bno));
    if (it != wb.end() && it->second.fresh) { it->second.dirty = false; }
    if (k < 2) {
      int rc = agfl_put(ag, bno); if (rc) return rc;
      return agf_adj(ag, 0, 0, -1);
    }
    WBuf* b; int rc = agi_buf(ag, &b); if (rc) return rc;
    if (has_inobtcount) { uint8_t* p = bw(b); pb32(p + (k == BT_INO ? 336 : 340), be32(p + (k == BT_INO ? 336 : 340)) - 1); }
    return fs_free_raw(ag, bno, 1);
  }

  // key を含む / 直前の位置まで根から降りる。葉の idx = key より大きい最初のレコードの位置
  int bt_find(uint32_t ag, int k, const uint8_t* key, std::vector<Pe>* path) {
    path->clear();
    uint32_t root, height;
    int rc = bt_root(ag, k, &root, &height); if (rc) return rc;
    if (height < 1 || height > 12 || root == 0 || root == kNullAgbno) return -EUCLEAN;
    uint32_t bno = root;
    for (uint32_t lv = height; lv >= 1; lv--) {
      WBuf* b; rc = bt_blk(ag, bno, &b); if (rc) return rc;
      if (bt_level(b) != lv - 1 || be32(b->d.data()) != bt_magic(k)) return -EUCLEAN;
      uint32_t n = bt_nrecs(b);
      Pe pe{b, bno, 0};
      if (lv == 1) {
        if (n > bt_lmax(k)) return -EUCLEAN;
        uint32_t i = 0;
        { uint32_t lo = 0, hi = n; while (lo < hi) { uint32_t mid = (lo + hi) / 2; if (bt_cmpk(k, bt_recp(k, b, mid), key) <= 0) lo = mid + 1; else hi = mid; } i = lo; }
        pe.idx = i;
        path->push_back(pe);
        break;
      }
      if (n == 0 || n > bt_nmax(k)) return -EUCLEAN;
      uint32_t lo = 0, hi = n;   // key[i] <= key を満たす最後の i
      while (lo < hi) { uint32_t mid = (lo + hi) / 2; if (bt_cmpk(k, bt_keyp(k, b, mid), key) <= 0) lo = mid + 1; else hi = mid; }
      pe.idx = lo ? lo - 1 : 0;
      path->push_back(pe);
      bno = be32(bt_ptrp(k, b, pe.idx));
    }
    return 0;
  }

  // mode 0: key 以下で最大のレコード / mode 1: key 以上で最小のレコード。無ければ -ENOENT
  int bt_get(uint32_t ag, int k, const uint8_t* key, int mode, uint8_t* out) {
    std::vector<Pe> path;
    int rc = bt_find(ag, k, key, &path); if (rc) return rc;
    Pe& lf = path.back();
    uint32_t n = bt_nrecs(lf.b);
    if (mode == 0) {
      if (lf.idx == 0) return -ENOENT;
      std::memcpy(out, bt_recp(k, lf.b, lf.idx - 1), bt_reclen(k));
      return 0;
    }
    if (lf.idx > 0 && bt_cmpk(k, bt_recp(k, lf.b, lf.idx - 1), key) == 0) { std::memcpy(out, bt_recp(k, lf.b, lf.idx - 1), bt_reclen(k)); return 0; }
    if (lf.idx < n) { std::memcpy(out, bt_recp(k, lf.b, lf.idx), bt_reclen(k)); return 0; }
    uint32_t rs = be32(lf.b->d.data() + 12);
    for (int guard = 0; rs != kNullAgbno && guard < 100000; guard++) {   // 右隣の葉 (空の葉は飛ばす)
      WBuf* r; rc = bt_blk(ag, rs, &r); if (rc) return rc;
      if (bt_nrecs(r) > 0) { std::memcpy(out, bt_recp(k, r, 0), bt_reclen(k)); return 0; }
      rs = be32(r->d.data() + 12);
    }
    return -ENOENT;
  }
  int bt_edge(uint32_t ag, int k, bool last, uint8_t* out) {
    uint32_t root, height;
    int rc = bt_root(ag, k, &root, &height); if (rc) return rc;
    uint32_t bno = root;
    for (uint32_t lv = height; lv >= 1; lv--) {
      WBuf* b; rc = bt_blk(ag, bno, &b); if (rc) return rc;
      if (bt_level(b) != lv - 1) return -EUCLEAN;
      uint32_t n = bt_nrecs(b);
      if (lv == 1) {
        if (n == 0) return -ENOENT;
        std::memcpy(out, bt_recp(k, b, last ? n - 1 : 0), bt_reclen(k));
        return 0;
      }
      if (n == 0 || n > bt_nmax(k)) return -EUCLEAN;
      bno = be32(bt_ptrp(k, b, last ? n - 1 : 0));
    }
    return -EUCLEAN;
  }

  // 親のキーを (先頭が変わったので) 更新する。path[pos] の先頭エントリのキー = key
  void bt_fix_keys(int k, std::vector<Pe>& path, int pos, const uint8_t* key) {
    for (int i = pos - 1; i >= 0; i--) {
      uint8_t* kp = bw(path[i].b);
      (void)kp;
      std::memcpy(bt_keyp(k, path[i].b, path[i].idx), key, bt_keylen(k));
      if (path[i].idx != 0) break;
    }
  }

  // path[pos] のブロックに (keyrec, ptr) を idx の位置へ入れる。node なら keyrec = キー、ptr = 子。leaf なら keyrec = レコード全体
  int bt_blk_insert(uint32_t ag, int k, std::vector<Pe>& path, int pos, uint32_t idx, const uint8_t* keyrec, uint32_t ptr) {
    Pe& pe = path[pos];
    WBuf* b = pe.b;
    const bool leaf = bt_level(b) == 0;
    const uint32_t n = bt_nrecs(b), mx = leaf ? bt_lmax(k) : bt_nmax(k);
    const uint32_t kl = bt_keylen(k), rl = bt_reclen(k);
    if (n < mx) {
      uint8_t* p = bw(b);
      if (leaf) {
        std::memmove(bt_recp(k, b, idx + 1), bt_recp(k, b, idx), size_t(n - idx) * rl);
        std::memcpy(bt_recp(k, b, idx), keyrec, rl);
      } else {
        std::memmove(bt_keyp(k, b, idx + 1), bt_keyp(k, b, idx), size_t(n - idx) * kl);
        std::memmove(bt_ptrp(k, b, idx + 1), bt_ptrp(k, b, idx), size_t(n - idx) * 4);
        std::memcpy(bt_keyp(k, b, idx), keyrec, kl);
        pb32(bt_ptrp(k, b, idx), ptr);
      }
      pb16(p + 6, n + 1);
      if (idx == 0 && pos > 0) bt_fix_keys(k, path, pos, keyrec);
      return 0;
    }
    // 分割: n + 1 個を (左, 右) に分ける
    std::vector<uint8_t> ents, ptrs;
    const uint32_t es = leaf ? rl : kl;
    ents.resize(size_t(n + 1) * es);
    if (leaf) {
      std::memcpy(ents.data(), bt_recp(k, b, 0), size_t(idx) * rl);
      std::memcpy(ents.data() + size_t(idx) * rl, keyrec, rl);
      std::memcpy(ents.data() + size_t(idx + 1) * rl, bt_recp(k, b, idx), size_t(n - idx) * rl);
    } else {
      ptrs.resize(size_t(n + 1) * 4);
      std::memcpy(ents.data(), bt_keyp(k, b, 0), size_t(idx) * kl);
      std::memcpy(ents.data() + size_t(idx) * kl, keyrec, kl);
      std::memcpy(ents.data() + size_t(idx + 1) * kl, bt_keyp(k, b, idx), size_t(n - idx) * kl);
      std::memcpy(ptrs.data(), bt_ptrp(k, b, 0), size_t(idx) * 4);
      pb32(ptrs.data() + size_t(idx) * 4, ptr);
      std::memcpy(ptrs.data() + size_t(idx + 1) * 4, bt_ptrp(k, b, idx), size_t(n - idx) * 4);
    }
    const uint32_t nl = (n + 1 + 1) / 2, nr = n + 1 - nl;
    uint32_t rbno;
    int rc = bt_alloc_block(ag, k, &rbno); if (rc) return rc;
    uint32_t oldright = be32(b->d.data() + 12);
    WBuf* r = bt_newblk(ag, k, rbno, bt_level(b), pe.bno, oldright);
    uint8_t* lp = bw(b);
    pb16(lp + 6, nl); pb32(lp + 12, rbno);
    pb16(r->d.data() + 6, nr);
    if (leaf) {
      std::memcpy(bt_recp(k, b, 0), ents.data(), size_t(nl) * rl);
      std::memcpy(bt_recp(k, r, 0), ents.data() + size_t(nl) * rl, size_t(nr) * rl);
    } else {
      std::memcpy(bt_keyp(k, b, 0), ents.data(), size_t(nl) * kl);
      std::memcpy(bt_ptrp(k, b, 0), ptrs.data(), size_t(nl) * 4);
      std::memcpy(bt_keyp(k, r, 0), ents.data() + size_t(nl) * kl, size_t(nr) * kl);
      std::memcpy(bt_ptrp(k, r, 0), ptrs.data() + size_t(nl) * 4, size_t(nr) * 4);
    }
    if (oldright != kNullAgbno) {
      WBuf* o; rc = bt_blk(ag, oldright, &o); if (rc) return rc;
      pb32(bw(o) + 8, rbno);
    }
    if (idx == 0 && pos > 0) bt_fix_keys(k, path, pos, ents.data());
    uint8_t rkey[16];
    std::memcpy(rkey, ents.data() + size_t(nl) * es, kl);   // 右の先頭エントリのキー (レコードの先頭 kl バイトがキー)
    if (pos > 0) {
      return bt_blk_insert(ag, k, path, pos - 1, path[pos - 1].idx + 1, rkey, rbno);
    }
    // 根が割れた: 新しい根
    uint32_t nroot;
    rc = bt_alloc_block(ag, k, &nroot); if (rc) return rc;
    uint32_t root, height;
    rc = bt_root(ag, k, &root, &height); if (rc) return rc;
    WBuf* nb = bt_newblk(ag, k, nroot, bt_level(b) + 1, kNullAgbno, kNullAgbno);
    pb16(nb->d.data() + 6, 2);
    std::memcpy(bt_keyp(k, nb, 0), ents.data(), kl);
    std::memcpy(bt_keyp(k, nb, 1), rkey, kl);
    pb32(bt_ptrp(k, nb, 0), pe.bno);
    pb32(bt_ptrp(k, nb, 1), rbno);
    return bt_set_root(ag, k, nroot, height + 1);
  }

  int bt_insert(uint32_t ag, int k, const uint8_t* rec) {
    std::vector<Pe> path;
    int rc = bt_find(ag, k, rec, &path); if (rc) return rc;
    Pe& lf = path.back();
    if (lf.idx > 0 && bt_cmpk(k, bt_recp(k, lf.b, lf.idx - 1), rec) == 0) return -EUCLEAN;   // 重複
    return bt_blk_insert(ag, k, path, int(path.size()) - 1, lf.idx, rec, 0);
  }

  // path[pos] のブロックからエントリ idx を取り除く。空になったら解放して親から外す
  int bt_blk_remove(uint32_t ag, int k, std::vector<Pe>& path, int pos, uint32_t idx) {
    Pe& pe = path[pos];
    WBuf* b = pe.b;
    const bool leaf = bt_level(b) == 0;
    const uint32_t n = bt_nrecs(b);
    const uint32_t kl = bt_keylen(k), rl = bt_reclen(k);
    if (idx >= n) return -EUCLEAN;
    if (n == 1 && pos > 0) {
      // 空になる非根ブロック: 兄弟をつなぎ替えて解放し、親から外す
      uint32_t l = be32(b->d.data() + 8), r = be32(b->d.data() + 12);
      if (l != kNullAgbno) { WBuf* x; int rc = bt_blk(ag, l, &x); if (rc) return rc; pb32(bw(x) + 12, r); }
      if (r != kNullAgbno) { WBuf* x; int rc = bt_blk(ag, r, &x); if (rc) return rc; pb32(bw(x) + 8, l); }
      int rc = bt_free_block(ag, k, pe.bno); if (rc) return rc;
      return bt_blk_remove(ag, k, path, pos - 1, path[pos - 1].idx);
    }
    uint8_t* p = bw(b);
    if (leaf) std::memmove(bt_recp(k, b, idx), bt_recp(k, b, idx + 1), size_t(n - idx - 1) * rl);
    else {
      std::memmove(bt_keyp(k, b, idx), bt_keyp(k, b, idx + 1), size_t(n - idx - 1) * kl);
      std::memmove(bt_ptrp(k, b, idx), bt_ptrp(k, b, idx + 1), size_t(n - idx - 1) * 4);
    }
    pb16(p + 6, n - 1);
    if (idx == 0 && n > 1 && pos > 0) bt_fix_keys(k, path, pos, leaf ? bt_recp(k, b, 0) : bt_keyp(k, b, 0));
    return 0;
  }
  // 根が子 1 つだけなら高さを減らす
  int bt_collapse(uint32_t ag, int k) {
    for (int guard = 0; guard < 16; guard++) {
      uint32_t root, height;
      int rc = bt_root(ag, k, &root, &height); if (rc) return rc;
      if (height <= 1) return 0;
      WBuf* b; rc = bt_blk(ag, root, &b); if (rc) return rc;
      if (bt_nrecs(b) != 1) return 0;
      uint32_t child = be32(bt_ptrp(k, b, 0));
      rc = bt_set_root(ag, k, child, height - 1); if (rc) return rc;
      rc = bt_free_block(ag, k, root); if (rc) return rc;
    }
    return 0;
  }
  int bt_delete(uint32_t ag, int k, const uint8_t* rec) {
    std::vector<Pe> path;
    int rc = bt_find(ag, k, rec, &path); if (rc) return rc;
    Pe& lf = path.back();
    if (lf.idx == 0 || bt_cmpk(k, bt_recp(k, lf.b, lf.idx - 1), rec) != 0) return -EUCLEAN;
    rc = bt_blk_remove(ag, k, path, int(path.size()) - 1, lf.idx - 1); if (rc) return rc;
    return bt_collapse(ag, k);
  }
  // 同じキーのレコードを置き換える (キーは変えない)
  int bt_replace(uint32_t ag, int k, const uint8_t* rec) {
    std::vector<Pe> path;
    int rc = bt_find(ag, k, rec, &path); if (rc) return rc;
    Pe& lf = path.back();
    if (lf.idx == 0 || bt_cmpk(k, bt_recp(k, lf.b, lf.idx - 1), rec) != 0) return -EUCLEAN;
    bw(lf.b);
    std::memcpy(bt_recp(k, lf.b, lf.idx - 1), rec, bt_reclen(k));
    return 0;
  }

  // ============ 空き領域 ============
  int fs_rec_add(uint32_t ag, uint32_t s, uint32_t l) {
    uint8_t r[8]; pb32(r, s); pb32(r + 4, l);
    int rc = bt_insert(ag, BT_BNO, r); if (rc) return rc;
    return bt_insert(ag, BT_CNT, r);
  }
  int fs_rec_del(uint32_t ag, uint32_t s, uint32_t l) {
    uint8_t r[8]; pb32(r, s); pb32(r + 4, l);
    int rc = bt_delete(ag, BT_BNO, r); if (rc) return rc;
    return bt_delete(ag, BT_CNT, r);
  }
  int agf_update_longest(uint32_t ag) {
    uint8_t r[8];
    int rc = bt_edge(ag, BT_CNT, true, r);
    uint32_t longest = 0;
    if (rc == 0) longest = be32(r + 4); else if (rc != -ENOENT) return rc;
    WBuf* b; rc = agf_buf(ag, &b); if (rc) return rc;
    if (be32(b->d.data() + 56) != longest) pb32(bw(b) + 56, longest);
    return 0;
  }
  // [cs, cs+cl) を空き領域から取り除く (全体が 1 つの空きレコードに含まれていること)
  int fs_carve(uint32_t ag, uint32_t cs, uint32_t cl) {
    uint8_t key[8], rec[8];
    pb32(key, cs); pb32(key + 4, 0);
    int rc = bt_get(ag, BT_BNO, key, 0, rec); if (rc) return rc == -ENOENT ? -EUCLEAN : rc;
    uint32_t rs = be32(rec), rl = be32(rec + 4);
    if (cs < rs || uint64_t(cs) + cl > uint64_t(rs) + rl) return -EUCLEAN;
    rc = fs_rec_del(ag, rs, rl); if (rc) return rc;
    if (cs > rs) { rc = fs_rec_add(ag, rs, cs - rs); if (rc) return rc; }
    if (cs + cl < rs + rl) { rc = fs_rec_add(ag, cs + cl, rs + rl - cs - cl); if (rc) return rc; }
    rc = agf_adj(ag, -int64_t(cl), 0, 0); if (rc) return rc;
    return agf_update_longest(ag);
  }
  // 空き領域に [bno, bno+len) を返す (隣と併合)
  int fs_free_raw(uint32_t ag, uint32_t bno, uint32_t len) {
    busy.push_back({ag, bno, len});   // コミットまでは (データを直接書く) 確保に使わない
    uint8_t key[8], rec[8];
    uint32_t s = bno, l = len;
    pb32(key, bno); pb32(key + 4, 0);
    int rc = bt_get(ag, BT_BNO, key, 0, rec);
    if (rc == 0) {
      uint32_t ls = be32(rec), ll = be32(rec + 4);
      if (uint64_t(ls) + ll > bno) return -EUCLEAN;   // 二重解放
      if (uint64_t(ls) + ll == bno) { rc = fs_rec_del(ag, ls, ll); if (rc) return rc; s = ls; l += ll; }
    } else if (rc != -ENOENT) return rc;
    rc = bt_get(ag, BT_BNO, key, 1, rec);
    if (rc == 0) {
      uint32_t rs = be32(rec), rl = be32(rec + 4);
      if (rs < uint64_t(bno) + len) return -EUCLEAN;
      if (rs == uint64_t(bno) + len) { rc = fs_rec_del(ag, rs, rl); if (rc) return rc; l += rl; }
    } else if (rc != -ENOENT) return rc;
    rc = fs_rec_add(ag, s, l); if (rc) return rc;
    rc = agf_adj(ag, len, 0, 0); if (rc) return rc;
    return agf_update_longest(ag);
  }
  int fix_freelist(uint32_t ag) {
    for (int guard = 0; guard < 400; guard++) {
      WBuf* f; int rc = agf_buf(ag, &f); if (rc) return rc;
      const uint8_t* p = f->d.data();
      uint32_t flcount = be32(p + 48);
      uint32_t want = be32(p + 28) + 1 + be32(p + 32) + 1 + 2;
      if (want > agfl_slots - 4) want = agfl_slots - 4;
      if (flcount > want + 8) {   // 余分を空き領域に戻す
        uint32_t bno;
        rc = agfl_get(ag, &bno); if (rc) return rc;
        rc = fs_free_raw(ag, bno, 1); if (rc) return rc;
        continue;
      }
      if (flcount >= want) return 0;
      uint8_t r[8];
      uint8_t key[8]; pb32(key, 0); pb32(key + 4, 0);
      rc = bt_get(ag, BT_CNT, key, 1, r);
      if (rc == -ENOENT) return -ENOSPC;
      if (rc) return rc;
      uint32_t s = be32(r);
      rc = fs_carve(ag, s, 1); if (rc) return rc;
      rc = agfl_put(ag, s); if (rc) return rc;
    }
    return -EUCLEAN;
  }
  // AG の空きブロック数
  int ag_freeblks(uint32_t ag, uint32_t* fb) {
    WBuf* f; int rc = agf_buf(ag, &f); if (rc) return rc;
    *fb = be32(f->d.data() + 52);
    return 0;
  }
  // 内部 (予約なし) の確保: [minlen, maxlen] ブロック。hint の位置から連続して取れればそれを優先
  int ag_alloc_raw(uint32_t ag, uint32_t minlen, uint32_t maxlen, uint32_t hint, uint32_t* start, uint32_t* got) {
    int rc = fix_freelist(ag); if (rc) return rc;
    uint8_t key[8], rec[8];
    if (hint != kNullAgbno) {
      pb32(key, hint); pb32(key + 4, 0);
      rc = bt_get(ag, BT_BNO, key, 0, rec);
      if (rc == 0) {
        uint32_t rs = be32(rec), rl = be32(rec + 4);
        if (hint >= rs && uint64_t(hint) < uint64_t(rs) + rl) {
          uint32_t avail = rs + rl - hint, take = std::min(avail, maxlen);
          if (take >= minlen) { rc = fs_carve(ag, hint, take); if (rc) return rc; *start = hint; *got = take; return fix_freelist(ag); }
        }
      } else if (rc != -ENOENT) return rc;
    }
    pb32(key, 0); pb32(key + 4, maxlen);   // cnt の key は (blockcount, startblock) の順で比較: 先頭 4 バイトが startblock、次が blockcount
    pb32(key, 0);
    rc = bt_get(ag, BT_CNT, key, 1, rec);
    if (rc == -ENOENT) {
      rc = bt_edge(ag, BT_CNT, true, rec);
      if (rc == -ENOENT) return -ENOSPC;
    }
    if (rc) return rc;
    uint32_t rs = be32(rec), rl = be32(rec + 4);
    uint32_t take = std::min(rl, maxlen);
    if (take < minlen) return -ENOSPC;
    rc = fs_carve(ag, rs, take); if (rc) return rc;
    *start = rs; *got = take;
    return fix_freelist(ag);
  }
  // データ / メタデータのブロック確保。reserve = その AG に残しておく空きブロック数 (カーネルの予約 + 余裕)
  int ag_alloc(uint32_t ag, uint32_t minlen, uint32_t maxlen, uint32_t hint, uint32_t reserve, uint32_t* start, uint32_t* got) {
    uint32_t fb;
    int rc = ag_freeblks(ag, &fb); if (rc) return rc;
    uint32_t held = reserve + 16;
    if (fb <= held) return -ENOSPC;
    if (maxlen > fb - held) maxlen = fb - held;
    if (maxlen < minlen) return -ENOSPC;
    return ag_alloc_raw(ag, minlen, maxlen, hint, start, got);
  }
  uint32_t ag_reserve(uint32_t ag) const { return std::max<uint32_t>(8, ag_len(ag) / 64); }
  // ファイルシステム全体から (ag_pref の AG から順に) 確保して FSB を返す
  int fs_alloc(uint32_t ag_pref, uint64_t hint_fsb, uint32_t maxlen, uint64_t* fsb, uint32_t* got, bool meta = false) {
    int last = -ENOSPC;
    for (uint32_t i = 0; i < agcount; i++) {
      uint32_t ag = (ag_pref + i) % agcount;
      uint32_t hint = kNullAgbno;
      if (hint_fsb != ~0ULL && (hint_fsb >> agblklog) == ag) hint = uint32_t(hint_fsb & ((1ULL << agblklog) - 1));
      uint32_t s, g;
      int rc = ag_alloc(ag, 1, maxlen, hint, meta ? 4 : ag_reserve(ag), &s, &g);
      if (rc == 0) { *fsb = fsb_of(ag, s); *got = g; return 0; }
      if (rc != -ENOSPC) return rc;
      last = rc;
    }
    return last;
  }
  int fs_free(uint64_t fsb, uint32_t len) {
    uint32_t ag = uint32_t(fsb >> agblklog), bno = uint32_t(fsb & ((1ULL << agblklog) - 1));
    if (ag >= agcount || uint64_t(bno) + len > ag_len(ag)) return -EUCLEAN;
    int rc = fix_freelist(ag); if (rc) return rc;
    bdrop_range(agb_off(ag, bno), uint64_t(len) * bs);
    rc = fs_free_raw(ag, bno, len); if (rc) return rc;
    return fix_freelist(ag);
  }

  // ============ inode の確保 ============
  uint64_t ino_of(uint32_t ag, uint32_t agino) const { return (uint64_t(ag) << (agblklog + inopblog)) | agino; }
  // inode 木のレコード (16 バイト) の部品
  struct IRec { uint32_t startino; uint16_t holemask; uint8_t count; uint32_t freecount; uint64_t free; };
  IRec irec_parse(const uint8_t* r) const {
    IRec x;
    x.startino = be32(r);
    if (has_spino) { x.holemask = be16(r + 4); x.count = r[6]; x.freecount = r[7]; } else { x.holemask = 0; x.count = 64; x.freecount = be32(r + 4); }
    x.free = be64(r + 8);
    return x;
  }
  void irec_put(const IRec& x, uint8_t* r) const {
    pb32(r, x.startino);
    if (has_spino) { pb16(r + 4, x.holemask); r[6] = x.count; r[7] = uint8_t(x.freecount); } else pb32(r + 4, x.freecount);
    pb64(r + 8, x.free);
  }

  // 空き inode のある inobt レコードを探す (finobt があればそれ、なければ inobt を先頭から)
  int ino_find_free(uint32_t ag, IRec* out, bool* found) {
    *found = false;
    WBuf* a; int rc = agi_buf(ag, &a); if (rc) return rc;
    if (be32(a->d.data() + 28) == 0) return 0;
    uint8_t r[16];
    if (has_finobt) {
      rc = bt_edge(ag, BT_FINO, false, r);
      if (rc == -ENOENT) return 0;
      if (rc) return rc;
      *out = irec_parse(r); *found = true;
      return 0;
    }
    uint8_t key[4]; pb32(key, 0);
    uint32_t cur = 0;
    for (int guard = 0; guard < 1000000; guard++) {
      pb32(key, cur);
      rc = bt_get(ag, BT_INO, key, 1, r);
      if (rc == -ENOENT) return 0;
      if (rc) return rc;
      IRec x = irec_parse(r);
      if (x.freecount > 0) { *out = x; *found = true; return 0; }
      cur = x.startino + 1;
    }
    return -EUCLEAN;
  }

  // 新しい inode チャンク (64 inode) を確保する
  int ino_new_chunk(uint32_t ag) {
    const uint32_t chunk_blks = std::max<uint32_t>(1, 64 / inopblock);
    uint32_t align = std::max<uint32_t>({inoalignmt, chunk_blks, std::max<uint32_t>(1, 8192 / bs)});
    // 上限 (imaxpct)
    if (imax_pct) {
      uint64_t icount_blocks = dblocks * imax_pct / 100;
      uint64_t maxicount = (icount_blocks << bslog) / inodesize;
      if (sb_icount_now() + 64 > maxicount) return -ENOSPC;
    }
    int rc = fix_freelist(ag); if (rc) return rc;
    uint32_t fb; rc = ag_freeblks(ag, &fb); if (rc) return rc;
    if (fb < chunk_blks + ag_reserve(ag) / 2 + 16) return -ENOSPC;
    // bnobt を先頭から走査して、整列した chunk_blks が取れる空きを探す
    uint8_t rec[8], key[8];
    uint32_t cur = 0, cs = 0; bool found = false;
    for (int guard = 0; guard < 4000000 && !found; guard++) {
      pb32(key, cur); pb32(key + 4, 0);
      rc = bt_get(ag, BT_BNO, key, 1, rec);
      if (rc == -ENOENT) break;
      if (rc) return rc;
      uint32_t rs = be32(rec), rl = be32(rec + 4);
      uint64_t a = rup(rs, align);
      if (a + chunk_blks <= uint64_t(rs) + rl && ((a * inopblock) >> 32) == 0) { cs = uint32_t(a); found = true; break; }
      cur = rs + 1;
    }
    if (!found) return -ENOSPC;
    rc = fs_carve(ag, cs, chunk_blks); if (rc) return rc;
    // inode バッファを初期化 (空き inode: magic / version / ino / uuid / next_unlinked)
    const uint32_t startino = cs * inopblock;
    uint32_t gen = uint32_t(time(nullptr)) * 2654435761u;
    for (uint32_t blk = 0; blk < chunk_blks; blk++) {
      WBuf* b = bnew(agb_off(ag, cs + blk), bs, BLFT_DINO);
      for (uint32_t s = 0; s < inopblock; s++) {
        uint8_t* ip = b->d.data() + size_t(s) * inodesize;
        pb16(ip, kInodeMagic); ip[4] = 3;
        pb32(ip + 92, gen++);
        pb32(ip + 96, 0xFFFFFFFFu);
        pb64(ip + 152, ino_of(ag, startino + blk * inopblock + s));
        std::memcpy(ip + 160, uuid, 16);
      }
    }
    uint8_t r[16];
    IRec x; x.startino = startino; x.holemask = 0; x.count = 64; x.freecount = 64; x.free = ~0ULL;
    irec_put(x, r);
    rc = bt_insert(ag, BT_INO, r); if (rc) return rc;
    if (has_finobt) { rc = bt_insert(ag, BT_FINO, r); if (rc) return rc; }
    WBuf* a; rc = agi_buf(ag, &a); if (rc) return rc;
    uint8_t* p = bw(a);
    pb32(p + 16, be32(p + 16) + 64); pb32(p + 28, be32(p + 28) + 64); pb32(p + 32, startino);
    d_icount += 64; d_ifree += 64;
    return 0;
  }
  uint64_t sb_icount_now() {
    WBuf* sbb = nullptr;
    if (bget(0, sectsize, BLFT_SB, &sbb)) return ~0ULL;
    return be64(sbb->d.data() + 128) + d_icount;
  }

  // inode を 1 つ確保する (ag_pref の AG を優先)。*ino に番号を返す。inode の初期化は呼び出し側
  int ino_alloc(uint32_t ag_pref, uint64_t* ino) {
    int last = -ENOSPC;
    for (uint32_t i = 0; i < agcount; i++) {
      uint32_t ag = (ag_pref + i) % agcount;
      IRec x; bool found = false;
      int rc = ino_find_free(ag, &x, &found);
      if (rc) return rc;
      if (!found) {
        rc = ino_new_chunk(ag);
        if (rc == -ENOSPC) { last = rc; continue; }
        if (rc) return rc;
        rc = ino_find_free(ag, &x, &found); if (rc) return rc;
        if (!found) return -EUCLEAN;
      }
      // 最も小さい空き inode (穴のある部分は除く)
      int bit = -1;
      for (int j = 0; j < 64; j++) {
        if (!((x.free >> j) & 1)) continue;
        if ((x.holemask >> (j / 4)) & 1) continue;
        bit = j; break;
      }
      if (bit < 0) return -EUCLEAN;
      x.free &= ~(1ULL << bit); x.freecount--;
      uint8_t r[16]; irec_put(x, r);
      rc = bt_replace(ag, BT_INO, r); if (rc) return rc;
      if (has_finobt) {
        if (x.freecount == 0) rc = bt_delete(ag, BT_FINO, r); else rc = bt_replace(ag, BT_FINO, r);
        if (rc) return rc;
      }
      WBuf* a; rc = agi_buf(ag, &a); if (rc) return rc;
      uint8_t* p = bw(a);
      pb32(p + 28, be32(p + 28) - 1);
      d_ifree -= 1;
      *ino = ino_of(ag, x.startino + bit);
      return 0;
    }
    return last;
  }
  // inode を空きに戻す (inode のバッファ側は呼び出し側が mode = 0 などにしておく)
  int ino_free(uint64_t ino) {
    uint32_t ag = uint32_t(ino >> (agblklog + inopblog));
    uint32_t agino = uint32_t(ino & ((1ULL << (agblklog + inopblog)) - 1));
    if (ag >= agcount) return -EUCLEAN;
    uint8_t key[4], r[16];
    pb32(key, agino);
    int rc = bt_get(ag, BT_INO, key, 0, r); if (rc) return rc == -ENOENT ? -EUCLEAN : rc;
    IRec x = irec_parse(r);
    if (agino < x.startino || agino >= x.startino + 64) return -EUCLEAN;
    int bit = int(agino - x.startino);
    if ((x.free >> bit) & 1) return -EUCLEAN;
    bool was_full = x.freecount == 0;
    x.free |= 1ULL << bit; x.freecount++;
    irec_put(x, r);
    rc = bt_replace(ag, BT_INO, r); if (rc) return rc;
    if (has_finobt) {
      if (was_full) rc = bt_insert(ag, BT_FINO, r); else rc = bt_replace(ag, BT_FINO, r);
      if (rc) return rc;
    }
    WBuf* a; rc = agi_buf(ag, &a); if (rc) return rc;
    uint8_t* p = bw(a);
    pb32(p + 28, be32(p + 28) + 1);
    d_ifree += 1;
    return 0;
  }
