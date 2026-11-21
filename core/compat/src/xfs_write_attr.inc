// SPDX-License-Identifier: GPL-2.0-or-later
// XFS 書き込み: 属性フォーク (xattr) と親ポインタ (parent pointer)。xfs_write_ops.inc の後で Impl の中に #include される。docs/xfs-write.md
//
// 親ポインタ (XFS_SB_FEAT_INCOMPAT_PARENT) は、子の inode の属性フォークに持つ「(親の中での名前) → (親の inode 番号, 親の世代)」の属性 (名前空間 XFS_ATTR_PARENT)。
// ファイル / ディレクトリ / シンボリックリンクの作成・hard link・unlink・rename のたびに親ポインタを足す / 外す。
//
// 属性フォークは 2 形式だけを書く:
//  - short form (inode の中。名前 / 値は 255 バイトまで。エントリは足した順)
//  - leaf 形式 (extent 形式の属性フォークの 1 ブロックだけ。V5 の attr3 leaf。エントリはハッシュ順)。1 ブロックに収まらなくなる (node 形式が要る) と EMLINK (hard link が多すぎる)
// それ以外 (node 形式・bmbt 形式の属性フォーク、リモート値、INCOMPLETE のエントリ) を持つ inode への変更は EOPNOTSUPP。
// 属性フォークの大きさ (di_forkoff) は、データフォークに必要な分を残して決める (カーネルの xfs_attr_shortform_bytesfit と同じ考え方)。
// 書き込みは「全エントリを読んで、変更して、全部書き直す」(1 inode あたり高々 inode の大きさ / 1 ブロック)。

  static constexpr uint8_t kBlftAttrLeaf = 16;
  static constexpr uint8_t kAfLocal = 1, kAfRoot = 2, kAfSecure = 4, kAfParent = 8, kAfIncomplete = 0x80;
  static constexpr uint32_t kAttrLeafHdr = 80, kAttrLeafMagic = 0x3bee;
  bool has_parent = false;
  struct AEnt { uint8_t flags = 0; std::string name; std::vector<uint8_t> val; };

  static Own own_attr(uint64_t ino, uint64_t lblk) { return Own{ino, (lblk & kRmOffMask) | kRmAttr}; }
  uint32_t lit_size() const { return inodesize - 176; }
  // カーネルの xfs_default_attroffset >> 3 (属性フォークを新しく作るときの di_forkoff の既定)
  uint32_t default_forkoff() const { return inodesize == 256 ? ((lit_size() - 36) >> 3) : (196u >> 3); }
  // 親ポインタを持たせるときにシンボリックリンクをインラインに置ける最大の長さ (属性フォークに既定の領域を残す)
  uint32_t local_data_limit() const { return has_parent ? default_forkoff() * 8 : lit_size(); }

  static uint32_t attr_hash(const AEnt& e) {
    uint32_t h = da_hash(reinterpret_cast<const uint8_t*>(e.name.data()), e.name.size());
    if ((e.flags & kAfParent) && e.val.size() >= 8) { uint64_t pino = be64(e.val.data()); h ^= uint32_t(pino >> 32) ^ uint32_t(pino); }
    return h;
  }
  static uint32_t attr_ent_size(const AEnt& e) { return uint32_t((3 + e.name.size() + e.val.size() + 3) & ~size_t(3)); }

  // 属性フォークの全エントリを読む。*leaf_fsb = leaf 形式のブロック (そうでなければ ~0)
  int attr_load(uint64_t ino, std::vector<AEnt>* out, uint64_t* leaf_fsb = nullptr) {
    out->clear();
    if (leaf_fsb) *leaf_fsb = ~0ULL;
    uint8_t* ip; int rc = ino_raw(ino, &ip, false); if (rc) return rc;
    const uint32_t fo = ip[82];
    if (fo == 0) return 0;
    const uint32_t lit = lit_size();
    if (fo * 8 >= lit) return -EUCLEAN;
    const uint8_t* area = ip + 176 + fo * 8;
    const uint32_t asz = lit - fo * 8;
    const uint8_t afmt = ip[83];
    if (afmt == kFmtLocal) {
      const uint32_t tot = be16(area), cnt = area[2];
      if (tot < 4 || tot > asz) return -EUCLEAN;
      uint32_t o = 4;
      for (uint32_t i = 0; i < cnt; i++) {
        if (o + 3 > tot) return -EUCLEAN;
        const uint32_t nl = area[o], vl = area[o + 1];
        if (o + 3 + nl + vl > tot) return -EUCLEAN;
        AEnt e; e.flags = area[o + 2];
        if (e.flags & kAfIncomplete) return -EOPNOTSUPP;
        e.name.assign(reinterpret_cast<const char*>(area + o + 3), nl);
        e.val.assign(area + o + 3 + nl, area + o + 3 + nl + vl);
        out->push_back(std::move(e));
        o += 3 + nl + vl;
      }
      return 0;
    }
    if (afmt != kFmtExtents) return -EOPNOTSUPP;
    const uint64_t anext = nrext64 ? be32(ip + 76) : be16(ip + 80);
    if (anext == 0) return 0;
    if (anext != 1 || asz < 16) return -EOPNOTSUPP;
    const uint64_t l0 = be64(area), l1 = be64(area + 8);
    const uint64_t off = (l0 >> 9) & ((1ULL << 54) - 1), fsb = ((l0 & 0x1ff) << 43) | (l1 >> 21), len = l1 & 0x1fffff;
    if (off != 0 || len != 1 || (l0 >> 63)) return -EOPNOTSUPP;
    uint64_t blk; if (!fsb_to_blk(fsb, &blk)) return -EUCLEAN;
    WBuf* b; rc = bget(blk * bs, bs, kBlftAttrLeaf, &b); if (rc) return rc;
    const uint8_t* p = b->d.data();
    if (be16(p + 8) != kAttrLeafMagic || be32(p) != 0 || be32(p + 4) != 0) return -EOPNOTSUPP;   // 単独の leaf ではない (node 形式)
    if (be64(p + 48) != ino) return -EUCLEAN;
    const uint32_t cnt = be16(p + 56);
    if (kAttrLeafHdr + 8ull * cnt > bs) return -EUCLEAN;
    for (uint32_t i = 0; i < cnt; i++) {
      const uint8_t* en = p + kAttrLeafHdr + 8 * i;
      const uint32_t idx = be16(en + 4); const uint8_t fl = en[6];
      if ((fl & kAfIncomplete) || !(fl & kAfLocal)) return -EOPNOTSUPP;   // リモート値は未対応
      if (idx < kAttrLeafHdr || idx + 3 > bs) return -EUCLEAN;
      const uint32_t vl = be16(p + idx), nl = p[idx + 2];
      if (idx + 3 + nl + vl > bs) return -EUCLEAN;
      AEnt e; e.flags = fl & ~kAfLocal;
      e.name.assign(reinterpret_cast<const char*>(p + idx + 3), nl);
      e.val.assign(p + idx + 3 + nl, p + idx + 3 + nl + vl);
      out->push_back(std::move(e));
    }
    if (leaf_fsb) *leaf_fsb = fsb;
    return 0;
  }

  // leaf ブロックの構築 (全エントリをハッシュ順に並べ、名前 / 値は末尾から詰める)。1 ブロックに収まらなければ false
  bool attr_leaf_build(uint64_t ino, uint64_t blkoff, const std::vector<AEnt>& ents, std::vector<uint8_t>* out) {
    std::vector<size_t> ord(ents.size());
    for (size_t i = 0; i < ord.size(); i++) ord[i] = i;
    std::vector<uint32_t> hv(ents.size());
    for (size_t i = 0; i < ents.size(); i++) hv[i] = attr_hash(ents[i]);
    std::stable_sort(ord.begin(), ord.end(), [&](size_t a, size_t b) { return hv[a] < hv[b]; });
    uint64_t used = 0;
    for (auto& e : ents) used += attr_ent_size(e);
    if (kAttrLeafHdr + 8ull * ents.size() + used > bs) return false;
    out->assign(bs, 0);
    uint8_t* p = out->data();
    pb16(p + 8, kAttrLeafMagic);
    pb64(p + 16, blkoff / 512);
    std::memcpy(p + 32, uuid, 16);
    pb64(p + 48, ino);
    uint32_t pos = bs;
    for (size_t k = 0; k < ord.size(); k++) {
      const AEnt& e = ents[ord[k]];
      const uint32_t sz = attr_ent_size(e);
      pos -= sz;
      pb16(p + pos, uint32_t(e.val.size())); p[pos + 2] = uint8_t(e.name.size());
      std::memcpy(p + pos + 3, e.name.data(), e.name.size());
      if (!e.val.empty()) std::memcpy(p + pos + 3 + e.name.size(), e.val.data(), e.val.size());
      uint8_t* en = p + kAttrLeafHdr + 8 * k;
      pb32(en, hv[ord[k]]); pb16(en + 4, pos); en[6] = uint8_t(e.flags | kAfLocal);
    }
    const uint32_t tab_end = kAttrLeafHdr + 8 * uint32_t(ents.size());
    pb16(p + 56, uint32_t(ents.size())); pb16(p + 58, uint32_t(used)); pb16(p + 60, pos);
    pb16(p + 64, tab_end); pb16(p + 66, pos - tab_end);   // freemap[0]
    return true;
  }

  // データフォークが使っているバイト数 (属性フォークの領域を決めるため)。btree 形式は現在の領域のまま動かせない
  int data_fork_bytes(const uint8_t* ip, uint32_t* out) {
    const uint32_t fo = ip[82];
    switch (ip[5]) {
      case 0: *out = 8; return 0;   // デバイス
      case kFmtLocal: *out = uint32_t(std::min<uint64_t>(be64(ip + 56), lit_size())); return 0;
      case kFmtExtents: { uint64_t n = (be64(ip + 120) & kFlags2Nrext64) ? be64(ip + 24) : be32(ip + 76); *out = uint32_t(std::min<uint64_t>(n * 16, lit_size())); return 0; }
      case kFmtBtree: *out = fo ? fo * 8 : lit_size(); return 0;
      default: return -EUCLEAN;
    }
  }

  // 属性フォークを ents の内容にする。収まる形式 (short form → leaf) を選び、必要なら di_forkoff を調整する
  int attr_store(uint64_t ino, const std::vector<AEnt>& ents) {
    uint8_t* ip; int rc = ino_raw(ino, &ip); if (rc) return rc;
    const uint32_t lit = lit_size();
    uint32_t fo = ip[82];
    const uint8_t afmt = ip[83];
    bool had_leaf = false; uint64_t leaf_fsb = 0;
    if (fo) {
      if (fo * 8 >= lit) return -EUCLEAN;
      if (afmt == kFmtExtents) {
        const uint64_t anext = nrext64 ? be32(ip + 76) : be16(ip + 80);
        if (anext == 1) {
          const uint8_t* area = ip + 176 + fo * 8;
          const uint64_t l0 = be64(area), l1 = be64(area + 8);
          if (((l0 >> 9) & ((1ULL << 54) - 1)) != 0 || (l1 & 0x1fffff) != 1 || (l0 >> 63)) return -EOPNOTSUPP;
          leaf_fsb = ((l0 & 0x1ff) << 43) | (l1 >> 21); had_leaf = true;
        } else if (anext != 0) return -EOPNOTSUPP;
      } else if (afmt != kFmtLocal) return -EOPNOTSUPP;
    }
    auto clear_counts = [&](uint8_t* q) { if (nrext64) pb32(q + 76, 0); else pb16(q + 80, 0); };
    if (ents.empty()) {   // 属性が 1 つも無い: 属性フォークを無くす
      if (had_leaf) {
        const Own ow = own_attr(ino, 0);
        rc = fs_free(leaf_fsb, 1, &ow); if (rc) return rc;
        rc = ino_raw(ino, &ip); if (rc) return rc;
        pb64(ip + 64, be64(ip + 64) - 1);
      }
      if (fo) std::memset(ip + 176 + fo * 8, 0, lit - fo * 8);
      ip[82] = 0; ip[83] = kFmtExtents; clear_counts(ip);
      return 0;
    }
    size_t sfneed = 4; bool sf_ok = true;
    for (auto& e : ents) { sfneed += 3 + e.name.size() + e.val.size(); if (e.name.size() > 255 || e.val.size() > 255) sf_ok = false; }
    if (sfneed > 0xFFFF) sf_ok = false;
    uint32_t dbytes; rc = data_fork_bytes(ip, &dbytes); if (rc) return rc;
    const uint8_t dfmt = ip[5];
    const uint32_t maxfo = (lit - 36) >> 3;
    uint32_t minfo = dfmt == kFmtBtree ? fo : uint32_t((rup(std::max<uint32_t>(dbytes, 52), 8)) >> 3);
    if (dfmt == kFmtBtree && fo == 0) return -EOPNOTSUPP;
    uint32_t nfo = fo;
    bool use_sf = sf_ok;
    if (use_sf) {
      if (!fo) {
        const uint32_t def = default_forkoff();
        if (def >= minfo && lit - def * 8 >= sfneed) nfo = def;
        else if (lit >= sfneed && ((lit - sfneed) >> 3) >= minfo) nfo = std::min<uint32_t>((lit - uint32_t(sfneed)) >> 3, maxfo);
        else use_sf = false;
      } else if (lit - fo * 8 < sfneed) {
        if (dfmt != kFmtBtree && lit >= sfneed && ((lit - sfneed) >> 3) >= minfo) nfo = (lit - uint32_t(sfneed)) >> 3;
        else use_sf = false;
      }
    }
    if (!use_sf && !fo) { nfo = std::max(default_forkoff(), minfo); if (nfo > maxfo) return -ENOSPC; }
    if (use_sf) {
      std::vector<uint8_t> sf(sfneed, 0);
      pb16(sf.data(), uint32_t(sfneed)); sf[2] = uint8_t(ents.size());
      size_t o = 4;
      for (auto& e : ents) {
        sf[o] = uint8_t(e.name.size()); sf[o + 1] = uint8_t(e.val.size()); sf[o + 2] = e.flags;
        std::memcpy(&sf[o + 3], e.name.data(), e.name.size());
        if (!e.val.empty()) std::memcpy(&sf[o + 3 + e.name.size()], e.val.data(), e.val.size());
        o += 3 + e.name.size() + e.val.size();
      }
      if (had_leaf) {
        const Own ow = own_attr(ino, 0);
        rc = fs_free(leaf_fsb, 1, &ow); if (rc) return rc;
        rc = ino_raw(ino, &ip); if (rc) return rc;
        pb64(ip + 64, be64(ip + 64) - 1);
      }
      if (fo) std::memset(ip + 176 + fo * 8, 0, lit - fo * 8);   // 古い属性フォーク (領域が動くときの残り) を消す
      std::memset(ip + 176 + nfo * 8, 0, lit - nfo * 8);
      ip[82] = uint8_t(nfo); ip[83] = kFmtLocal; clear_counts(ip);
      std::memcpy(ip + 176 + nfo * 8, sf.data(), sfneed);
      return 0;
    }
    // leaf 形式 (1 ブロック)
    uint64_t fsb = leaf_fsb;
    if (!had_leaf) {
      uint32_t got;
      const Own ow = own_attr(ino, 0);
      rc = fs_alloc(uint32_t(ino >> (agblklog + inopblog)), ~0ULL, 1, &fsb, &got, true, &ow); if (rc) return rc;
    }
    uint64_t blk; if (!fsb_to_blk(fsb, &blk)) return -EUCLEAN;
    std::vector<uint8_t> tmp;
    if (!attr_leaf_build(ino, blk * bs, ents, &tmp)) return -EMLINK;   // 親ポインタが 1 ブロックに収まらない = hard link が多すぎる (node 形式の属性は未実装)
    if (had_leaf) { WBuf* b; rc = bget(blk * bs, bs, kBlftAttrLeaf, &b); if (rc) return rc; std::memcpy(bw(b), tmp.data(), bs); }
    else {
      WBuf* b = bnew(blk * bs, bs, kBlftAttrLeaf); std::memcpy(b->d.data(), tmp.data(), bs);
      rc = ino_raw(ino, &ip); if (rc) return rc;
      std::memset(ip + 176 + nfo * 8, 0, lit - nfo * 8);
      ip[82] = uint8_t(nfo); ip[83] = kFmtExtents;
      if (nrext64) pb32(ip + 76, 1); else pb16(ip + 80, 1);
      ext_encode(Ext{0, fsb, 1, false}, ip + 176 + nfo * 8);
      pb64(ip + 64, be64(ip + 64) + 1);
    }
    return 0;
  }

  // ============ 親ポインタ ============
  int pp_value(uint64_t dir, std::vector<uint8_t>* v) {
    uint8_t* dip; int rc = ino_raw(dir, &dip, false); if (rc) return rc;
    v->assign(12, 0);
    pb64(v->data(), dir); pb32(v->data() + 8, be32(dip + 92));
    return 0;
  }
  // ino に「dir の中の name」への親ポインタを足す
  int pp_add(uint64_t ino, uint64_t dir, const std::string& name) {
    if (!has_parent) return 0;
    std::vector<AEnt> ents; int rc = attr_load(ino, &ents); if (rc) return rc;
    AEnt e; e.flags = kAfParent; e.name = name;
    rc = pp_value(dir, &e.val); if (rc) return rc;
    for (auto& x : ents) if ((x.flags & kAfParent) && x.name == name && x.val.size() >= 8 && be64(x.val.data()) == dir) return -EUCLEAN;   // 二重
    ents.push_back(std::move(e));
    return attr_store(ino, ents);
  }
  int pp_remove(uint64_t ino, uint64_t dir, const std::string& name) {
    if (!has_parent) return 0;
    std::vector<AEnt> ents; int rc = attr_load(ino, &ents); if (rc) return rc;
    size_t i = 0;
    while (i < ents.size() && !((ents[i].flags & kAfParent) && ents[i].name == name && ents[i].val.size() >= 8 && be64(ents[i].val.data()) == dir)) i++;
    if (i == ents.size()) return -EUCLEAN;
    ents.erase(ents.begin() + static_cast<std::ptrdiff_t>(i));
    return attr_store(ino, ents);
  }
