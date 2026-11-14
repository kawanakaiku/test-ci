// SPDX-License-Identifier: GPL-2.0-or-later
// XFS 書き込み (Full の範囲): unlink / rmdir / truncate / rename / link。xfs_write_ops.inc の後で Impl の中に #include される。docs/xfs-write.md
//
// どの操作も run_op の中で呼ばれ、1 操作 = バッファ上の変更の集まり。失敗すれば op_abort が開始時点へ戻す (ログには何も書かない)。コミットは従来どおり
// ログ (カーネルが再生できる形式) 経由。inode の解放はカーネルの「unlinked list に載せてから解放」の 2 段階を踏まず、1 トランザクションで
// (名前の削除 + nlink 0 → ブロックの解放 + inode を空きに戻す) まで行う。電源断のとき中間状態 (名前が無いのに inode が残る) は存在しない。
// 解放したブロックは busy に載る (xfs_write_alloc.inc の fs_free_raw): 同じコミットの窓の中でデータとして再利用しない (write_file が kRetryBusy)。

  static constexpr uint16_t kDiflagImmutable = 1u << 3, kDiflagAppend = 1u << 4;

  static uint8_t ft_of_mode(uint32_t mode) {
    switch (mode & S_FMT) {
      case S_REG: return 1; case S_DIR: return 2; case S_CHR: return 3; case S_BLK: return 4; case S_FIFO: return 5; case S_SOCK: return 6; case S_LNK: return 7;
    }
    return 0;
  }
  void dm_del(uint64_t dir, const std::string& name) { for (auto& dm : dirmaps) if (dm->ino == dir) dm->m.erase(name); }
  void dm_set(uint64_t dir, const std::string& name, uint64_t ino) { for (auto& dm : dirmaps) if (dm->ino == dir) dm->m[name] = ino; }

  int dir_is_empty(Inode* d, bool* empty) {
    bool any = false;
    int rc = walk_dir(d, false, [&](const Ent&) { any = true; return true; });
    if (rc) return rc;
    *empty = !any;
    return 0;
  }

  // inode の nlink を増減し、ctime を更新する (dir_time = true なら mtime も。ディレクトリの内容が変わったとき)
  int ino_adjust(uint64_t ino, int dlink, bool dir_time) {
    uint8_t* ip; int rc = ino_raw(ino, &ip); if (rc) return rc;
    if (dlink) {
      int64_t nl = int64_t(be32(ip + 16)) + dlink;
      if (nl < 0) nl = 0;
      if (nl > 0xFFFFFFFFLL) return -EMLINK;
      pb32(ip + 16, uint32_t(nl));
    }
    int64_t sec; uint32_t nsec; now_ts(&sec, &nsec);
    ino_times(ip, dir_time, true, sec, nsec);
    return 0;
  }

  // ============ inode の破棄 ============
  // nlink が 0 になった inode のブロック (データフォークの extent と bmbt) をすべて解放し、inode を空きに戻す。
  // 破棄できない inode (リアルタイム、reflink で共有されうるブロック、extent / btree 形式の属性フォーク) は -EOPNOTSUPP (何も変えずに失敗)。
  int ino_destroy(uint64_t ino) {
    std::shared_ptr<Inode> in;
    int rc = get_inode(ino, &in); if (rc) return rc;
    Inode* d = in.get();
    if (d->flags & 1) return -EOPNOTSUPP;
    {
      uint8_t* ip; rc = ino_raw(ino, &ip, false); if (rc) return rc;
      if (ip[82]) {   // 属性フォークがある: inode の中に収まる (local) ものだけ。ブロックを持つものは未対応
        if (ip[83] == kFmtLocal) { /* インライン */ }
        else if (ip[83] == kFmtExtents) {
          uint64_t anext = (be64(ip + 120) & kFlags2Nrext64) ? be32(ip + 76) : be16(ip + 80);
          if (anext != 0) return -EOPNOTSUPP;
        } else return -EOPNOTSUPP;
      }
    }
    std::vector<std::pair<uint64_t, uint32_t>> frees;   // (fsb, 長さ)
    uint64_t total = 0;
    if (d->format == kFmtExtents || d->format == kFmtBtree) {
      rc = load_extents(d); if (rc) return rc;
      for (const Ext& e : *d->exts) { frees.push_back({e.fsb, uint32_t(e.len)}); total += e.len; }
      if (d->format == kFmtBtree) {
        std::vector<uint64_t> bl;
        rc = bmbt_blocks(ino, d->dfork, &bl); if (rc) return rc;
        for (uint64_t f : bl) { frees.push_back({f, 1}); total++; }
      }
      if (total != d->nblocks) return -EUCLEAN;   // 数が合わない: 壊れている。解放しない
      if (total && (d->flags2 & kFlags2Reflink)) return -EOPNOTSUPP;   // 共有ブロックの参照カウントを持たない
    }
    for (auto& f : frees) { rc = fs_free(f.first, f.second); if (rc) return rc; }
    // inode を「空き」の形にする (mkfs / 新しい inode チャンクと同じ: magic / version 3 / 世代 / next_unlinked = NULL / 自身の番号 / uuid)
    {
      uint8_t* ip; rc = ino_raw(ino, &ip); if (rc) return rc;
      uint32_t gen = be32(ip + 92);
      std::memset(ip, 0, inodesize);
      pb16(ip, kInodeMagic); ip[4] = 3;
      pb32(ip + 92, gen);
      pb32(ip + 96, 0xFFFFFFFFu);
      pb64(ip + 152, ino);
      std::memcpy(ip + 160, uuid, 16);
    }
    rc = ino_free(ino); if (rc) return rc;
    icache.erase(ino);
    for (auto it = dirmaps.begin(); it != dirmaps.end();) { if ((*it)->ino == ino) it = dirmaps.erase(it); else ++it; }
    return 0;
  }

  // ============ ディレクトリからの削除 ============
  // 論理ブロック lb (ディレクトリのデータブロック) を extent の一覧から外して空き領域に返す
  int dir_drop_block(uint64_t dino, uint64_t lb) {
    std::shared_ptr<Inode> din;
    int rc = get_inode(dino, &din); if (rc) return rc;
    Inode* d = din.get();
    rc = load_extents(d); if (rc) return rc;
    std::vector<Ext> v = *d->exts;
    size_t i = 0;
    while (i < v.size() && !(v[i].off <= lb && lb < v[i].off + v[i].len)) i++;
    if (i == v.size()) return -EUCLEAN;
    const Ext e = v[i];
    const uint64_t fsb = e.fsb + (lb - e.off);
    if (e.len == 1) v.erase(v.begin() + i);
    else if (lb == e.off) { v[i].off++; v[i].fsb++; v[i].len--; }
    else if (lb == e.off + e.len - 1) v[i].len--;
    else { v[i].len = lb - e.off; v.insert(v.begin() + i + 1, Ext{lb + 1, fsb + 1, e.off + e.len - lb - 1, false}); }
    rc = store_fork(dino, v, -1); if (rc) return rc;
    return fs_free(fsb, 1);
  }

  // leaf1 形式のディレクトリから name を取り除く。データブロックが空になれば (0 番以外) 解放する。node 形式は -EOPNOTSUPP
  int dir_remove_leaf(uint64_t dino, const std::string& name) {
    std::shared_ptr<Inode> din;
    int rc = get_inode(dino, &din); if (rc) return rc;
    Inode* d = din.get();
    rc = load_extents(d); if (rc) return rc;
    const uint64_t leaf_fb = leaf_lblk();
    if (d->size % bs || d->size == 0) return -EUCLEAN;
    uint64_t ndata = d->size / bs;
    WBuf* lf; rc = dir_buf(d, leaf_fb, BLFT_DIR_LEAF1, &lf); if (rc) return rc;
    std::vector<std::pair<uint32_t, uint32_t>> lents;
    std::vector<uint16_t> bests;
    {
      const uint8_t* p = lf->d.data();
      if (be16(p + 8) != 0x3df1) return -EOPNOTSUPP;   // node 形式 (未対応)
      uint32_t count = be16(p + 56), bc = be32(p + bs - 4);
      if (64 + 8 * uint64_t(count) + 2 * uint64_t(bc) + 4 > bs || bc != ndata) return -EUCLEAN;
      for (uint32_t i = 0; i < count; i++) { uint32_t a = be32(p + 68 + i * 8); if (a != 0) lents.push_back({be32(p + 64 + i * 8), a}); }
      for (uint32_t i = 0; i < bc; i++) bests.push_back(be16(p + bs - 4 - 2 * bc + 2 * i));
    }
    const uint32_t h = da_hash(reinterpret_cast<const uint8_t*>(name.data()), name.size());
    int64_t hit = -1; uint64_t hdb = 0; uint32_t heo = 0;
    for (size_t i = 0; i < lents.size() && hit < 0; i++) {
      if (lents[i].first != h) continue;
      uint64_t off = uint64_t(lents[i].second) * 8, db = off / bs; uint32_t eo = uint32_t(off % bs);
      if (db >= ndata || bests[db] == 0xFFFF || eo < kDataHdr || eo + 16 > bs) continue;
      WBuf* b; rc = dir_buf(d, db, BLFT_DIR_DATA, &b); if (rc) return rc;
      const uint8_t* q = b->d.data();
      if (be32(q) != kDir3Data || be16(q + eo) == 0xFFFF) continue;
      size_t nl = q[eo + 8];
      if (nl == name.size() && eo + ent_sz(nl) <= bs && std::memcmp(q + eo + 9, name.data(), nl) == 0) { hit = int64_t(i); hdb = db; heo = eo; }
    }
    if (hit < 0) return -ENOENT;
    {
      WBuf* b; rc = dir_buf(d, hdb, BLFT_DIR_DATA, &b); if (rc) return rc;
      uint8_t* wp = bw(b);
      db_put_free(wp, heo, ent_sz(name.size()));
      rc = db_normalize(wp, bs, bs); if (rc) return rc;
      bests[hdb] = be16(wp + 50);
    }
    lents.erase(lents.begin() + hit);
    if (hdb != 0 && bests[hdb] == bs - kDataHdr) {   // データブロックが空になった: 解放する
      rc = dir_drop_block(dino, hdb); if (rc) return rc;
      bests[hdb] = 0xFFFF;
      if (hdb == ndata - 1) {   // 末尾なら、サイズを縮める (手前の穴も詰める)
        while (!bests.empty() && bests.back() == 0xFFFF) bests.pop_back();
        rc = dir_set_size(dino, uint64_t(bests.size()) * bs); if (rc) return rc;
      }
    }
    rc = get_inode(dino, &din); if (rc) return rc;
    d = din.get();
    rc = load_extents(d); if (rc) return rc;
    WBuf* lf2; rc = dir_buf(d, leaf_fb, BLFT_DIR_LEAF1, &lf2); if (rc) return rc;
    return leaf1_build(dino, bw(lf2), lf2->off, lents, bests) ? 0 : -EUCLEAN;
  }

  // 親ディレクトリ dino から name の名前を取り除く (指していた inode は触らない)
  int dir_remove(uint64_t dino, const std::string& name) {
    std::shared_ptr<Inode> din;
    int rc = get_inode(dino, &din); if (rc) return rc;
    Inode* d = din.get();
    if ((d->mode & S_FMT) != S_DIR) return -ENOTDIR;
    if (d->format == kFmtLocal) {
      SfDir sf; rc = sf_decode(d, &sf); if (rc) return rc;
      size_t i = 0;
      while (i < sf.ents.size() && sf.ents[i].name != name) i++;
      if (i == sf.ents.size()) return -ENOENT;
      sf.ents.erase(sf.ents.begin() + i);
      return dir_sf_store(dino, sf);
    }
    rc = load_extents(d); if (rc) return rc;
    const uint64_t leaf_fb = leaf_lblk();
    bool has_leaf = false;
    for (auto& e : *d->exts) if (e.off >= leaf_fb) { has_leaf = true; break; }
    if (dir_is_node(*d->exts)) return node_remove(dino, name);   // ノード形式 (xfs_write_node.inc)
    if (d->size == bs && !has_leaf) {   // block 形式: 全エントリを詰め直す
      WBuf* b; rc = dir_buf(d, 0, BLFT_DIR_BLOCK, &b); if (rc) return rc;
      const uint8_t* p = b->d.data();
      if (be32(p) != kDir3Block) return -EUCLEAN;
      uint32_t count = be32(p + bs - 8);
      if (uint64_t(count) * 8 + 8 > bs - kDataHdr) return -EUCLEAN;
      uint32_t leafstart = bs - 8 - 8 * count;
      std::vector<DEnt> ents;
      rc = db_scan(p, leafstart, &ents, nullptr); if (rc) return rc;
      size_t i = 0;
      while (i < ents.size() && ents[i].name != name) i++;
      if (i == ents.size()) return -ENOENT;
      ents.erase(ents.begin() + i);
      std::vector<uint8_t> tmp(bs);
      if (!block_build(dino, tmp.data(), b->off, ents)) return -EUCLEAN;
      std::memcpy(bw(b), tmp.data(), bs);
      return 0;
    }
    return dir_remove_leaf(dino, name);
  }

  // dino の ".." を newparent にする
  int dir_set_parent(uint64_t dino, uint64_t newparent) {
    std::shared_ptr<Inode> din;
    int rc = get_inode(dino, &din); if (rc) return rc;
    Inode* d = din.get();
    if ((d->mode & S_FMT) != S_DIR) return -ENOTDIR;
    if (d->format == kFmtLocal) {
      SfDir sf; rc = sf_decode(d, &sf); if (rc) return rc;
      sf.parent = newparent;
      return dir_sf_store(dino, sf);
    }
    rc = load_extents(d); if (rc) return rc;
    const uint64_t leaf_fb = leaf_lblk();
    bool has_leaf = false;
    for (auto& e : *d->exts) if (e.off >= leaf_fb) { has_leaf = true; break; }
    const bool block = d->size == bs && !has_leaf;
    WBuf* b; rc = dir_buf(d, 0, block ? BLFT_DIR_BLOCK : BLFT_DIR_DATA, &b); if (rc) return rc;
    const uint8_t* p = b->d.data();
    uint32_t scan_end = bs;
    if (block) {
      if (be32(p) != kDir3Block) return -EUCLEAN;
      uint32_t count = be32(p + bs - 8);
      if (uint64_t(count) * 8 + 8 > bs - kDataHdr) return -EUCLEAN;
      scan_end = bs - 8 - 8 * count;
    } else if (be32(p) != kDir3Data) return -EUCLEAN;
    std::vector<DEnt> ents;
    rc = db_scan(p, scan_end, &ents, nullptr); if (rc) return rc;
    for (auto& e : ents) if (e.name == "..") { pb64(bw(b) + e.off, newparent); return 0; }
    return -EUCLEAN;
  }

  // ============ unlink / rmdir ============
  int remove_node(uint64_t dir, const std::string& name, bool is_rmdir) {
    if (name == "." || name == "..") return is_rmdir ? -EINVAL : -EISDIR;
    if (!name_ok(name)) return name.size() > 255 ? -ENAMETOOLONG : -EINVAL;
    std::shared_ptr<Inode> parent;
    int rc = get_inode(dir, &parent); if (rc) return rc;
    if ((parent->mode & S_FMT) != S_DIR) return -ENOTDIR;
    uint64_t ino;
    rc = dir_find(parent.get(), name, &ino); if (rc) return rc;
    std::shared_ptr<Inode> t;
    rc = get_inode(ino, &t); if (rc) return rc;
    const bool is_dir = (t->mode & S_FMT) == S_DIR;
    if (is_rmdir && !is_dir) return -ENOTDIR;
    if (!is_rmdir && is_dir) return -EISDIR;
    if ((t->flags & (kDiflagImmutable | kDiflagAppend)) || (parent->flags & (kDiflagImmutable | kDiflagAppend))) return -EPERM;
    if (is_dir) {
      bool empty; rc = dir_is_empty(t.get(), &empty); if (rc) return rc;
      if (!empty) return -ENOTEMPTY;
    }
    rc = dir_remove(dir, name); if (rc) return rc;
    dm_del(dir, name);
    rc = ino_adjust(dir, is_dir ? -1 : 0, true); if (rc) return rc;
    if (is_dir || t->nlink <= 1) { rc = ino_destroy(ino); if (rc) return rc; }
    else { rc = ino_adjust(ino, -1, false); if (rc) return rc; }
    icache.erase(dir);
    return 0;
  }

  // ============ link ============
  int link_node(uint64_t dir, const std::string& name, uint64_t target) {
    if (!name_ok(name)) return name.size() > 255 ? -ENAMETOOLONG : -EINVAL;
    std::shared_ptr<Inode> parent, t;
    int rc = get_inode(dir, &parent); if (rc) return rc;
    if ((parent->mode & S_FMT) != S_DIR) return -ENOTDIR;
    if (parent->nlink == 0) return -ENOENT;
    rc = get_inode(target, &t); if (rc) return rc;
    if ((t->mode & S_FMT) == S_DIR) return -EPERM;
    if (t->nlink == 0) return -ENOENT;
    if (t->nlink >= 0xFFFFFFF0u) return -EMLINK;
    if ((t->flags & (kDiflagImmutable | kDiflagAppend)) || (parent->flags & (kDiflagImmutable | kDiflagAppend))) return -EPERM;
    uint64_t ex;
    rc = dir_find(parent.get(), name, &ex);
    if (rc == 0) return -EEXIST;
    if (rc != -ENOENT) return rc;
    rc = dir_add(dir, name, target, ft_of_mode(t->mode)); if (rc) return rc;
    rc = ino_adjust(target, +1, false); if (rc) return rc;
    rc = ino_adjust(dir, 0, true); if (rc) return rc;
    dm_set(dir, name, target);
    icache.erase(dir);
    return 0;
  }

  // ============ truncate ============
  int truncate_file(uint64_t ino, uint64_t newsize) {
    std::shared_ptr<Inode> in;
    int rc = get_inode(ino, &in); if (rc) return rc;
    if ((in->mode & S_FMT) == S_DIR) return -EISDIR;
    if ((in->mode & S_FMT) != S_REG) return -EINVAL;
    if (in->flags & 1) return -EOPNOTSUPP;   // リアルタイム
    if (in->flags & (kDiflagImmutable | kDiflagAppend)) return -EPERM;
    if (newsize > (1ULL << 62) / 2) return -EFBIG;
    const uint64_t old = in->size;
    const bool reflink = (in->flags2 & kFlags2Reflink) != 0;
    rc = load_extents(in.get()); if (rc) return rc;
    if (newsize < old) {
      const uint64_t keep = (newsize + bs - 1) / bs;   // 残す論理ブロック数 [0, keep)
      std::vector<Ext> v;
      std::vector<std::pair<uint64_t, uint32_t>> frees;
      uint64_t nfreed = 0;
      for (const Ext& e : *in->exts) {
        if (e.off + e.len <= keep) { v.push_back(e); continue; }
        if (e.off >= keep) { frees.push_back({e.fsb, uint32_t(e.len)}); nfreed += e.len; continue; }
        Ext k = e; k.len = keep - e.off; v.push_back(k);
        frees.push_back({e.fsb + k.len, uint32_t(e.len - k.len)}); nfreed += e.len - k.len;
      }
      if (nfreed) {
        if (reflink) return -EOPNOTSUPP;   // 共有ブロックかもしれない
        rc = store_fork(ino, v, -int64_t(nfreed)); if (rc) return rc;
        for (auto& f : frees) { rc = fs_free(f.first, f.second); if (rc) return rc; }
      }
    } else if (newsize > old && (old % bs) != 0) {
      // 拡張: 旧 EOF から (そのブロックの終わりか新しい EOF まで) をゼロにする (縮めたときは EOF 以降の残りを消さない設計: 拡張のときに消す)
      uint64_t lb = old / bs;
      const Ext* e = find_ext(*in->exts, lb);
      if (e && !e->unwritten) {
        if (reflink) return -EOPNOTSUPP;
        if (tail_zero_unsafe(ino, old)) return kRetryBusy;   // 直前の縮小がまだコミットされていない: 先にコミットする (write_file と同じ)
        uint64_t zend = std::min<uint64_t>(newsize, (lb + 1) * bs);
        uint64_t b; if (!fsb_to_blk(e->fsb + (lb - e->off), &b)) return -EUCLEAN;
        std::vector<uint8_t> z(size_t(zend - old), 0);
        rc = dev_write_data(b * bs + (old % bs), z.data(), z.size()); if (rc) return rc;
      }
    }
    uint8_t* ip; rc = ino_raw(ino, &ip); if (rc) return rc;
    pb64(ip + 56, newsize);
    int64_t sec; uint32_t nsec; now_ts(&sec, &nsec);
    ino_times(ip, true, true, sec, nsec);
    return 0;
  }

  // ============ rename ============
  int rename_node(uint64_t od, const std::string& on, uint64_t nd, const std::string& nn) {
    if (!name_ok(on)) return (on == "." || on == "..") ? -EINVAL : (on.size() > 255 ? -ENAMETOOLONG : -EINVAL);
    if (!name_ok(nn)) return (nn == "." || nn == "..") ? -EINVAL : (nn.size() > 255 ? -ENAMETOOLONG : -EINVAL);
    std::shared_ptr<Inode> opar, npar;
    int rc = get_inode(od, &opar); if (rc) return rc;
    rc = get_inode(nd, &npar); if (rc) return rc;
    if ((opar->mode & S_FMT) != S_DIR || (npar->mode & S_FMT) != S_DIR) return -ENOTDIR;
    uint64_t sino;
    rc = dir_find(opar.get(), on, &sino); if (rc) return rc;
    std::shared_ptr<Inode> src;
    rc = get_inode(sino, &src); if (rc) return rc;
    const bool src_dir = (src->mode & S_FMT) == S_DIR;
    uint64_t dino = 0; bool dst_exists = false;
    rc = dir_find(npar.get(), nn, &dino);
    if (rc == 0) dst_exists = true; else if (rc != -ENOENT) return rc;
    if (dst_exists && dino == sino) return 0;   // 同じ inode (同じ名前を含む): 何もしない
    if (npar->nlink == 0) return -ENOENT;
    const uint16_t kProt = kDiflagImmutable | kDiflagAppend;
    if ((src->flags & kProt) || (opar->flags & kProt) || (npar->flags & kProt)) return -EPERM;
    if (src_dir) {   // 自分自身や自分の下への移動は不可
      uint64_t cur = nd;
      for (int guard = 0; guard < 100000; guard++) {
        if (cur == sino) return -EINVAL;
        std::shared_ptr<Inode> c; rc = get_inode(cur, &c); if (rc) return rc;
        uint64_t up; rc = dir_find(c.get(), "..", &up); if (rc) return rc;
        if (up == cur) break;   // ルート
        cur = up;
        if (guard == 99999) return -EUCLEAN;
      }
    }
    std::shared_ptr<Inode> dst;
    bool dst_dir = false;
    if (dst_exists) {
      rc = get_inode(dino, &dst); if (rc) return rc;
      dst_dir = (dst->mode & S_FMT) == S_DIR;
      if (src_dir && !dst_dir) return -ENOTDIR;
      if (!src_dir && dst_dir) return -EISDIR;
      if (dst->flags & kProt) return -EPERM;
      if (dst_dir) { bool empty; rc = dir_is_empty(dst.get(), &empty); if (rc) return rc; if (!empty) return -ENOTEMPTY; }
    }
    // 実行: (1) 上書きされる名前を消す (2) 新しい名前を足す (3) 古い名前を消す (4) ディレクトリなら ".." と親の nlink
    if (dst_exists) {
      rc = dir_remove(nd, nn); if (rc) return rc;
      dm_del(nd, nn);
      if (dst_dir) { rc = ino_adjust(nd, -1, true); if (rc) return rc; }
      if (dst_dir || dst->nlink <= 1) { rc = ino_destroy(dino); if (rc) return rc; }
      else { rc = ino_adjust(dino, -1, false); if (rc) return rc; }
    }
    rc = dir_add(nd, nn, sino, ft_of_mode(src->mode)); if (rc) return rc;
    rc = dir_remove(od, on); if (rc) return rc;
    if (src_dir) {
      rc = ino_adjust(od, -1, false); if (rc) return rc;
      rc = ino_adjust(nd, +1, false); if (rc) return rc;
      if (od != nd) { rc = dir_set_parent(sino, nd); if (rc) return rc; }
    }
    rc = ino_adjust(sino, 0, false); if (rc) return rc;
    rc = ino_adjust(od, 0, true); if (rc) return rc;
    if (nd != od) { rc = ino_adjust(nd, 0, true); if (rc) return rc; }
    dirmaps.clear(); icache.erase(od); icache.erase(nd); icache.erase(sino);
    return 0;
  }
