// SPDX-License-Identifier: GPL-2.0-or-later
// XFS 書き込み: ノード形式のディレクトリ (データブロック + leafn ブロック + da ノード + free 索引ブロック)。xfs_write_full.inc の後で Impl の中に #include される。docs/xfs-write.md
//
// leaf1 形式 (データブロック + 1 つの葉) に収まらなくなったディレクトリ (約 505 エントリ / 4 KiB) は、ここでノード形式にする。
// 設計 (単純さ優先): データブロックの中のエントリを真実とし、変更のたびに「索引」(leafn 群・根の da ノード・free 索引) を全部作り直す。
//  - データブロック (論理ブロック 0..N-1): 既存と同じ。追加・削除はそのブロックの中だけ (空きに入れる / 空きにする)。空になったブロックは (0 番以外) 解放する。
//  - 葉 (LEAFN, magic 0x3dff): ハッシュ順に並べたエントリを (葉の容量の 7/8 ずつ) 詰める。葉が 1 つなら根 (論理ブロック LEAF_OFFSET) がその葉、
//    2 つ以上なら根が da ノード (magic 0x3ebe。level 1。各エントリ = (子の最大ハッシュ, 子の論理ブロック))、葉は LEAF_OFFSET + 1 から。
//  - free 索引 (XDF3, 論理ブロック FREE_OFFSET): 各データブロックの最大の空き (データブロックの bestfree[0]。ブロックが無ければ 0xFFFF)。
//  変更が無いチャンク (128 バイト) はログに載らないので、作り直しで同じ内容になった部分は書かれない。
// 上限: 葉が da ノード 1 枚に収まる数 (4 KiB で 504 枚 ≒ 20 万エントリ)、データブロックが free 索引 1 枚に収まる数 (4 KiB で 2016 ブロック)。超えると -ENOSPC。

  static constexpr uint32_t kLeafnMagic = 0x3dff, kDaNodeMagic = 0x3ebe, kFree3Magic = 0x58444633;
  uint64_t free_lblk() const { return (1ULL << 36) / bs; }   // XFS_DIR2_FREE_OFFSET (64 GiB)
  bool dir_is_node(const std::vector<Ext>& v) const { const uint64_t f = free_lblk(); for (const Ext& e : v) if (e.off >= f) return true; return false; }

  struct NodeScan {
    std::vector<std::pair<uint32_t, uint32_t>> lents;   // (ハッシュ, アドレス) 昇順
    std::vector<uint16_t> bests;                         // データブロックごとの最大の空き (ブロックが無ければ 0xFFFF)
  };
  // 全データブロックを読んで、葉エントリと bests を作る。name を渡せばその名前の位置 (ブロック / オフセット) も返す (無ければ -ENOENT)
  int node_scan(uint64_t dino, NodeScan* out, const std::string* name, uint64_t* hdb, uint32_t* heo) {
    std::shared_ptr<Inode> din;
    int rc = get_inode(dino, &din); if (rc) return rc;
    Inode* d = din.get();
    rc = load_extents(d); if (rc) return rc;
    if (d->size % bs || d->size == 0) return -EUCLEAN;
    const uint64_t ndata = d->size / bs;
    if (ndata >= leaf_lblk()) return -EUCLEAN;
    out->bests.assign(size_t(ndata), 0xFFFF);
    out->lents.clear();
    bool hit = false;
    for (uint64_t db = 0; db < ndata; db++) {
      if (!find_ext(*d->exts, db)) continue;
      WBuf* b; rc = dir_buf(d, db, BLFT_DIR_DATA, &b); if (rc) return rc;
      const uint8_t* p = b->d.data();
      if (be32(p) != kDir3Data) return -EUCLEAN;
      std::vector<DEnt> ents;
      rc = db_scan(p, bs, &ents, nullptr); if (rc) return rc;
      out->bests[size_t(db)] = uint16_t(be16(p + 50));
      for (const DEnt& e : ents) {
        out->lents.push_back({da_hash(reinterpret_cast<const uint8_t*>(e.name.data()), e.name.size()), uint32_t((db * bs + e.off) >> 3)});
        if (name && !hit && e.name == *name) { hit = true; if (hdb) *hdb = db; if (heo) *heo = e.off; }
      }
    }
    std::sort(out->lents.begin(), out->lents.end());
    if (name && !hit) return -ENOENT;
    return 0;
  }

  // 索引のブロック (論理ブロック lb) を書き換え用に開く (中身はゼロにする。型は blft に付け替える)
  int node_open_blk(Inode* d, uint64_t lb, uint8_t blft, uint8_t** p, uint64_t* off) {
    WBuf* b; int r = dir_buf(d, lb, blft, &b); if (r) return r;
    b->blft = blft;
    *p = bw(b); *off = b->off;
    std::memset(*p, 0, bs);
    return 0;
  }

  // 索引 (葉・根・free 索引) を s の内容から作り直す。必要なブロックを確保し、要らなくなったものは解放する
  int node_rebuild(uint64_t dino, const NodeScan& s) {
    const uint32_t cap = (bs - 64) / 8;                       // 葉・ノードの 1 ブロックあたりのエントリ数
    const size_t per = std::max<size_t>(1, cap - cap / 8);    // 葉に詰める数 (後の追加のために余裕を残す)
    const size_t n = s.lents.size();
    const size_t nleaf = std::max<size_t>(1, (n + per - 1) / per);
    if (nleaf > cap) return -ENOSPC;
    if (s.bests.size() > (bs - 64) / 2) return -ENOSPC;
    const uint64_t L = leaf_lblk(), F = free_lblk();
    const bool has_node = nleaf > 1;
    std::vector<uint64_t> want;   // 昇順
    if (!has_node) want.push_back(L); else for (size_t i = 0; i <= nleaf; i++) want.push_back(L + i);
    want.push_back(F);
    // 現在の 葉空間・free 空間のブロック
    std::shared_ptr<Inode> din;
    int rc = get_inode(dino, &din); if (rc) return rc;
    rc = load_extents(din.get()); if (rc) return rc;
    std::vector<uint64_t> cur;
    for (const Ext& e : *din->exts) if (e.off >= L) for (uint64_t k = 0; k < e.len; k++) cur.push_back(e.off + k);
    for (uint64_t lb : cur) if (!std::binary_search(want.begin(), want.end(), lb)) { rc = dir_drop_block(dino, lb); if (rc) return rc; }
    for (uint64_t lb : want) {
      if (std::binary_search(cur.begin(), cur.end(), lb)) continue;
      rc = get_inode(dino, &din); if (rc) return rc;
      WBuf* nb; rc = dir_add_block(din.get(), lb, lb == F ? BLFT_DIR_FREE : (has_node && lb == L ? BLFT_DA_NODE : BLFT_DIR_LEAFN), &nb); if (rc) return rc;
    }
    rc = get_inode(dino, &din); if (rc) return rc;
    Inode* d = din.get();
    rc = load_extents(d); if (rc) return rc;
    auto blkinfo = [&](uint8_t* p, uint32_t magic, uint64_t off, uint32_t forw, uint32_t back) {
      pb32(p, forw); pb32(p + 4, back); pb16(p + 8, magic);
      pb64(p + 16, off / 512);
      std::memcpy(p + 32, uuid, 16);
      pb64(p + 48, dino);
    };
    // 葉
    std::vector<uint32_t> maxhash;
    for (size_t j = 0; j < nleaf; j++) {
      const uint64_t lb = has_node ? L + 1 + j : L;
      uint8_t* p; uint64_t off;
      rc = node_open_blk(d, lb, BLFT_DIR_LEAFN, &p, &off); if (rc) return rc;
      const size_t lo = j * per, hi = std::min(n, lo + per);
      blkinfo(p, kLeafnMagic, off, j + 1 < nleaf ? uint32_t(lb + 1) : 0, j > 0 ? uint32_t(lb - 1) : 0);
      pb16(p + 56, uint32_t(hi - lo)); pb16(p + 58, 0);
      for (size_t i = lo; i < hi; i++) { pb32(p + 64 + (i - lo) * 8, s.lents[i].first); pb32(p + 68 + (i - lo) * 8, s.lents[i].second); }
      maxhash.push_back(hi > lo ? s.lents[hi - 1].first : 0xFFFFFFFFu);
    }
    // 根 (da ノード)
    if (has_node) {
      uint8_t* p; uint64_t off;
      rc = node_open_blk(d, L, BLFT_DA_NODE, &p, &off); if (rc) return rc;
      blkinfo(p, kDaNodeMagic, off, 0, 0);
      pb16(p + 56, uint32_t(nleaf)); pb16(p + 58, 1);
      for (size_t j = 0; j < nleaf; j++) { pb32(p + 64 + j * 8, maxhash[j]); pb32(p + 68 + j * 8, uint32_t(L + 1 + j)); }
    }
    // free 索引
    {
      uint8_t* p; uint64_t off;
      rc = node_open_blk(d, F, BLFT_DIR_FREE, &p, &off); if (rc) return rc;
      pb32(p, kFree3Magic);
      pb64(p + 8, off / 512);
      std::memcpy(p + 24, uuid, 16);
      pb64(p + 40, dino);
      uint32_t used = 0;
      for (size_t i = 0; i < s.bests.size(); i++) { pb16(p + 64 + i * 2, s.bests[i]); if (s.bests[i] != 0xFFFF) used++; }
      pb32(p + 48, 0); pb32(p + 52, uint32_t(s.bests.size())); pb32(p + 56, used);
    }
    return 0;
  }

  // ノード形式のディレクトリ (または leaf1 から移行するディレクトリ) にエントリを足す
  int node_add(uint64_t dino, const std::string& name, uint64_t ino, uint8_t ft) {
    NodeScan s;
    int rc = node_scan(dino, &s, nullptr, nullptr, nullptr); if (rc) return rc;
    const uint32_t sz = ent_sz(name.size());
    int64_t target = -1;
    for (size_t i = 0; i < s.bests.size(); i++) if (s.bests[i] != 0xFFFF && s.bests[i] >= sz) { target = int64_t(i); break; }
    std::shared_ptr<Inode> din;
    rc = get_inode(dino, &din); if (rc) return rc;
    WBuf* db = nullptr;
    if (target < 0) {
      const uint64_t ndata = s.bests.size();
      if (ndata + 1 > (bs - 64) / 2 || (ndata + 1) * bs >= kDirLeafOffsetBytes) return -ENOSPC;
      rc = dir_add_block(din.get(), ndata, BLFT_DIR_DATA, &db); if (rc) return rc;
      uint8_t* p = db->d.data();
      dir_hdr_init(p, kDir3Data, db->off, dino);
      db_put_free(p, kDataHdr, bs - kDataHdr);
      pb16(p + 48, kDataHdr); pb16(p + 50, bs - kDataHdr);
      s.bests.push_back(uint16_t(bs - kDataHdr));
      target = int64_t(ndata);
      rc = dir_set_size(dino, (ndata + 1) * bs); if (rc) return rc;
      rc = get_inode(dino, &din); if (rc) return rc;
    } else {
      rc = dir_buf(din.get(), uint64_t(target), BLFT_DIR_DATA, &db); if (rc) return rc;
      if (be32(db->d.data()) != kDir3Data) return -EUCLEAN;
    }
    uint8_t* p = bw(db);
    std::vector<FreeRg> fr; rc = db_scan(p, bs, nullptr, &fr); if (rc) return rc;
    uint32_t off = 0;
    for (auto& f : fr) if (f.len >= sz) { off = f.off; db_put_free(p, f.off, f.len); if (f.len > sz) db_put_free(p, f.off + sz, f.len - sz); break; }
    if (!off) return -EUCLEAN;
    db_put_entry(p, off, ino, name, ft);
    rc = db_normalize(p, bs, bs); if (rc) return rc;
    s.bests[size_t(target)] = uint16_t(be16(p + 50));
    std::pair<uint32_t, uint32_t> ne{da_hash(reinterpret_cast<const uint8_t*>(name.data()), name.size()), uint32_t((uint64_t(target) * bs + off) >> 3)};
    s.lents.insert(std::lower_bound(s.lents.begin(), s.lents.end(), ne), ne);
    return node_rebuild(dino, s);
  }

  // ノード形式のディレクトリから name を取り除く。データブロックが空になれば (0 番以外) 解放し、末尾の穴を詰めてサイズを縮める
  int node_remove(uint64_t dino, const std::string& name) {
    NodeScan s; uint64_t hdb = 0; uint32_t heo = 0;
    int rc = node_scan(dino, &s, &name, &hdb, &heo); if (rc) return rc;
    std::shared_ptr<Inode> din;
    rc = get_inode(dino, &din); if (rc) return rc;
    WBuf* b; rc = dir_buf(din.get(), hdb, BLFT_DIR_DATA, &b); if (rc) return rc;
    uint8_t* p = bw(b);
    std::pair<uint32_t, uint32_t> ge{da_hash(reinterpret_cast<const uint8_t*>(name.data()), name.size()), uint32_t((hdb * bs + heo) >> 3)};
    auto it = std::lower_bound(s.lents.begin(), s.lents.end(), ge);
    if (it == s.lents.end() || *it != ge) return -EUCLEAN;
    s.lents.erase(it);
    db_put_free(p, heo, ent_sz(name.size()));
    rc = db_normalize(p, bs, bs); if (rc) return rc;
    s.bests[size_t(hdb)] = uint16_t(be16(p + 50));
    if (hdb != 0 && s.bests[size_t(hdb)] == bs - kDataHdr) {
      rc = dir_drop_block(dino, hdb); if (rc) return rc;
      s.bests[size_t(hdb)] = 0xFFFF;
      if (hdb + 1 == s.bests.size()) {
        while (s.bests.size() > 1 && s.bests.back() == 0xFFFF) s.bests.pop_back();
        rc = dir_set_size(dino, uint64_t(s.bests.size()) * bs); if (rc) return rc;
      }
    }
    return node_rebuild(dino, s);
  }
