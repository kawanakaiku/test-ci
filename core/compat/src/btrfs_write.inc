// SPDX-License-Identifier: GPL-2.0-or-later
// Btrfs 書き込みエンジン (btrfs_volume.cpp の struct BtrfsVolume::Impl の中に #include される。USBMEM_NTFS_WRITE のビルドだけ)。docs/btrfs-write.md
//
// 方針 (カーネルの fs/btrfs を載せずに、ディスク上の形式を直接 COW で更新する):
//  - ツリーブロックは必ず新しい場所 (空き領域) に書く。現世代で作ったブロックだけをその場で更新する (dirty 表)。確定済みのブロックは決して上書きしない。
//  - トランザクション内で解放したブロックは、コミットが終わるまで再利用しない (pinned: free には戻すが avail には戻さない)。
//  - コミット: (1) 遅延参照 (extent ツリー)・ブロックグループ項目・空き領域ツリー・ルート項目が安定するまで繰り返し更新
//    (2) 変更ブロックを全て書く (3) flush (4) スーパーブロック (バックアップルート込み) を書く (5) flush。クラッシュ時は旧スーパーブロックが有効で整合。
//  - 共有された (参照数 > 1 / 共有バックリファレンス) ツリーブロックは COW しない (EOPNOTSUPP で中止)。データ extent の参照は (root, inode, offset) の通常の参照だけ扱う。
//  - 書き込み可にする条件は prepare_rw() のホワイトリスト。満たさなければ読み取り専用 (ro_reason に理由)。

  // ============ 状態 ============
  static constexpr u64 kObjQuota = 8, kObjFst = 10, kObjBgTree = 11;
  static constexpr u64 kObjOrphan = static_cast<u64>(-5), kObjBalance = static_cast<u64>(-4);
  static constexpr u8 kInodeExtref = 13, kExtentItemK = 168, kMetadataItemK = 169, kTreeBlockRefK = 176, kExtentDataRefK = 178,
                      kBlockGroupItemK = 192, kFstInfoK = 198, kFstExtentK = 199, kFstBitmapK = 200, kDevExtentK = 204, kDevItemK = 216,
                      kOrphanItemK = 48, kTempItemK = 248, kDevReplaceK = 250;
  static constexpr u64 kBgData = 1, kBgSystem = 2, kBgMeta = 4;

  bool writable_ = false, aborted = false, txn_open = false;
  std::string ro_reason;
  u64 cur_gen = 0;                 // 走っているトランザクションの世代 (sb.generation + 1)
  u64 mutations = 0;               // 木・確保の変更回数 (操作の途中失敗の検出)
  std::unordered_map<u64, std::shared_ptr<Node>> dirty;   // 現世代で作った (未書き込みの) ブロック
  enum { R_ADD_TREE, R_DROP_TREE, R_ADD_DATA, R_INC_DATA, R_DEC_DATA };
  struct DRef { int kind = 0; u64 bytenr = 0, num = 0, root = 0, ino = 0, off = 0; int level = 0; };
  std::deque<DRef> dq;             // 遅延参照 (extent ツリーへの反映待ち)
  struct Bg {
    u64 start = 0, len = 0, flags = 0, used = 0, used_disk = 0;
    std::map<u64, u64> free, avail;        // free = 空き領域ツリーが示す空き (結合済み)。avail = 確保してよい分 (free から、同一トランザクションで解放した分とスーパーブロックの置き場を除く)
    bool dirty = false, item_disk = true, fst_disk = true, bitmaps = false;
    std::vector<std::pair<u64, u64>> runs_disk;   // ディスク上の空き領域ツリーが示す free の run
    struct Bm { u64 start = 0, len = 0; std::vector<u8> bytes; };
    std::vector<Bm> bm_disk;               // bitmap 形式のときのディスク上の内容
  };
  std::map<u64, Bg> bgs;
  std::vector<std::pair<u64, u64>> pending_free;   // このトランザクションで解放した範囲 (コミット後に avail へ)
  std::set<u64> dirty_roots;
  std::map<u64, int64_t> used_delta;               // ツリーごとの bytes_used の増減 (ROOT_ITEM)
  std::deque<u64> pending_chunks;                  // 作ったが chunk / dev ツリーに未反映の chunk
  std::vector<std::pair<u64, u64>> dev_ranges;     // 物理領域の使用済み範囲 (dev ツリー + 予約領域)
  bool dev_loaded = false;
  u64 dev_bytes_used = 0, dev_total = 0;
  u64 free_objectid = 0;
  u8 chunk_uuid[16] = {0}, dev_uuid[16] = {0};
  bool skinny = false, no_holes = false, mixed = false, bg_tree = false;
  size_t dirty_limit_nodes = 0;
  u64 commits_done = 0;

  // ============ 小物 ============
  static void put16(u8* p, u16 v) { p[0] = static_cast<u8>(v); p[1] = static_cast<u8>(v >> 8); }
  static void put32(u8* p, u32 v) { for (int i = 0; i < 4; i++) p[i] = static_cast<u8>(v >> (8 * i)); }
  static void put64(u8* p, u64 v) { for (int i = 0; i < 8; i++) p[i] = static_cast<u8>(v >> (8 * i)); }
  static void put_key(u8* p, const Key& k) { put64(p, k.obj); p[8] = k.type; put64(p + 9, k.off); }
  static u64 rup(u64 v, u64 a) { return (v + a - 1) / a * a; }

  u32 leaf_body() const { return nodesize - static_cast<u32>(kHdr); }
  u32 max_item() const { return leaf_body() / 2 - static_cast<u32>(kItemSz); }          // 1 項目の最大データ長 (分割で必ず収まる上限)
  u32 max_ptrs() const { return leaf_body() / static_cast<u32>(kPtrSz); }
  u32 max_ext_item() const { return (leaf_body() >> 4) - static_cast<u32>(kItemSz); }   // BTRFS_MAX_EXTENT_ITEM_SIZE
  u32 inline_max() const { return std::min<u32>({2048u, sectorsize - 1, max_item() - 21}); }

  static u32 item_off(const Node& n, u32 i) { return le32(&n.d[kHdr + static_cast<size_t>(i) * kItemSz + 17]); }
  static u32 item_sz(const Node& n, u32 i) { return le32(&n.d[kHdr + static_cast<size_t>(i) * kItemSz + 21]); }
  u32 leaf_data_start(const Node& n) const { return n.n ? item_off(n, n.n - 1) : leaf_body(); }
  u32 leaf_free(const Node& n) const { return leaf_data_start(n) - n.n * static_cast<u32>(kItemSz); }
  bool leaf_packed(const Node& n) const {
    u32 above = leaf_body();
    for (u32 i = 0; i < n.n; i++) {
      if (item_off(n, i) + item_sz(n, i) != above) return false;
      above = item_off(n, i);
    }
    return true;
  }
  static void set_nritems(Node& n, u32 v) { put32(&n.d[96], v); n.n = v; }
  u64 node_bytenr(const Node& n) const { return le64(&n.d[48]); }

  void leaf_insert(Node& n, u32 idx, const Key& k, const u8* data, u32 sz) {
    u32 cnt = n.n, ds = leaf_data_start(n);
    u32 above = idx ? item_off(n, idx - 1) : leaf_body();
    if (idx < cnt) {
      std::memmove(&n.d[kHdr + ds - sz], &n.d[kHdr + ds], above - ds);
      for (u32 i = idx; i < cnt; i++) put32(&n.d[kHdr + static_cast<size_t>(i) * kItemSz + 17], item_off(n, i) - sz);
      std::memmove(&n.d[kHdr + static_cast<size_t>(idx + 1) * kItemSz], &n.d[kHdr + static_cast<size_t>(idx) * kItemSz], static_cast<size_t>(cnt - idx) * kItemSz);
    }
    u8* h = &n.d[kHdr + static_cast<size_t>(idx) * kItemSz];
    put_key(h, k); put32(h + 17, above - sz); put32(h + 21, sz);
    if (sz) std::memcpy(&n.d[kHdr + above - sz], data, sz);
    set_nritems(n, cnt + 1);
  }
  void leaf_delete(Node& n, u32 idx) {
    u32 cnt = n.n, ds = leaf_data_start(n), off = item_off(n, idx), sz = item_sz(n, idx);
    std::memmove(&n.d[kHdr + ds + sz], &n.d[kHdr + ds], off - ds);
    std::memset(&n.d[kHdr + ds], 0, sz);
    for (u32 i = idx + 1; i < cnt; i++) put32(&n.d[kHdr + static_cast<size_t>(i) * kItemSz + 17], item_off(n, i) + sz);
    std::memmove(&n.d[kHdr + static_cast<size_t>(idx) * kItemSz], &n.d[kHdr + static_cast<size_t>(idx + 1) * kItemSz], static_cast<size_t>(cnt - idx - 1) * kItemSz);
    std::memset(&n.d[kHdr + static_cast<size_t>(cnt - 1) * kItemSz], 0, kItemSz);
    set_nritems(n, cnt - 1);
  }
  void node_insert_ptr(Node& n, u32 pos, const Key& k, u64 bytenr, u64 gen) {
    u32 cnt = n.n;
    if (pos < cnt) std::memmove(&n.d[kHdr + static_cast<size_t>(pos + 1) * kPtrSz], &n.d[kHdr + static_cast<size_t>(pos) * kPtrSz], static_cast<size_t>(cnt - pos) * kPtrSz);
    u8* h = &n.d[kHdr + static_cast<size_t>(pos) * kPtrSz];
    put_key(h, k); put64(h + 17, bytenr); put64(h + 25, gen);
    set_nritems(n, cnt + 1);
  }
  void node_del_ptr(Node& n, u32 pos) {
    u32 cnt = n.n;
    std::memmove(&n.d[kHdr + static_cast<size_t>(pos) * kPtrSz], &n.d[kHdr + static_cast<size_t>(pos + 1) * kPtrSz], static_cast<size_t>(cnt - pos - 1) * kPtrSz);
    std::memset(&n.d[kHdr + static_cast<size_t>(cnt - 1) * kPtrSz], 0, kPtrSz);
    set_nritems(n, cnt - 1);
  }

  std::shared_ptr<Node> new_node(u64 bytenr, int level, u64 owner) {
    auto n = std::make_shared<Node>();
    n->d.assign(nodesize, 0);
    std::memcpy(&n->d[32], meta_fsid.data(), 16);
    put64(&n->d[48], bytenr);
    put64(&n->d[56], 1ull | (1ull << 56));   // WRITTEN | backref revision 1 (MIXED_BACKREF)
    std::memcpy(&n->d[64], chunk_uuid, 16);
    put64(&n->d[80], cur_gen);
    put64(&n->d[88], owner);
    n->d[100] = static_cast<u8>(level);
    n->level = static_cast<u8>(level);
    n->n = 0;
    return n;
  }

  Subvol* T(u64 id) {
    u32 s;
    if (slot_for(id, &s)) return nullptr;
    return &subvols[s];
  }
  void set_root(u64 id, u64 bytenr, int level) {
    Subvol* sv = T(id);
    if (!sv) return;
    sv->bytenr = bytenr; sv->level = level;
    if (id == kRootTreeObj) { root_bytenr = bytenr; root_level = level; }
    else if (id == kChunkTreeObj) { chunk_root = bytenr; chunk_level = level; chunk_root_changed = true; }
    else if (id == kCsumTreeObj) { csum_root = *sv; }
    dirty_roots.insert(id);
  }

  // ---- 読み取り補助 (現在の木 = 未コミットの変更を含む)
  int get_item(u64 tree, const Key& k, std::vector<u8>* v) {
    Subvol* sv = T(tree);
    if (!sv) return -ENOENT;
    Cursor c;
    int rc = search(sv->bytenr, sv->level, k, true, c);
    if (rc < 0) return rc;
    if (rc > 0 || kcmp(c.key(), k) != 0) return -ENOENT;
    u32 sz;
    const u8* p = c.item(&sz);
    v->assign(p, p + sz);
    return 0;
  }
  int find_le(u64 tree, const Key& k, Key* fk, std::vector<u8>* v) {
    Subvol* sv = T(tree);
    if (!sv) return -ENOENT;
    Cursor c;
    int rc = search(sv->bytenr, sv->level, k, true, c);
    if (rc < 0) return rc;
    if (rc > 0) return -ENOENT;
    *fk = c.key();
    u32 sz;
    const u8* p = c.item(&sz);
    if (v) v->assign(p, p + sz);
    return 0;
  }
  int find_ge(u64 tree, const Key& k, Key* fk, std::vector<u8>* v) {
    Subvol* sv = T(tree);
    if (!sv) return -ENOENT;
    Cursor c;
    int rc = search(sv->bytenr, sv->level, k, false, c);
    if (rc < 0) return rc;
    if (rc > 0) return -ENOENT;
    *fk = c.key();
    u32 sz;
    const u8* p = c.item(&sz);
    if (v) v->assign(p, p + sz);
    return 0;
  }

  // ============ 空き領域 (メモリ) ============
  static void mapadd(std::map<u64, u64>& m, u64 s, u64 l) {
    u64 e = s + l;
    auto it = m.lower_bound(s);
    if (it != m.begin()) {
      auto p = std::prev(it);
      if (p->first + p->second >= s) { s = p->first; e = std::max(e, p->first + p->second); it = m.erase(p); }
    }
    while (it != m.end() && it->first <= e) { e = std::max(e, it->first + it->second); it = m.erase(it); }
    m[s] = e - s;
  }
  static void mapdel(std::map<u64, u64>& m, u64 s, u64 l) {
    u64 e = s + l;
    auto it = m.upper_bound(s);
    if (it != m.begin()) --it;
    while (it != m.end() && it->first < e) {
      u64 rs = it->first, re = rs + it->second;
      if (re <= s) { ++it; continue; }
      it = m.erase(it);
      if (rs < s) m[rs] = s - rs;
      if (re > e) m[e] = re - e;
    }
  }
  Bg* bg_of(u64 logical) {
    auto it = bgs.upper_bound(logical);
    if (it == bgs.begin()) return nullptr;
    --it;
    return logical - it->second.start < it->second.len ? &it->second : nullptr;
  }
  // 物理の予約領域 (スーパーブロック) に当たる論理範囲を avail から外す
  void exclude_super_ranges(Bg& g) {
    auto cit = chunks.find(g.start);
    if (cit == chunks.end()) return;
    for (const Stripe& s : cit->second.stripes) {
      if (s.devid != devid) continue;
      for (int i = 0; i < 3; i++) {
        u64 zs = kSbOffsets[i] & ~0xFFFFull, ze = zs + 0x10000;
        u64 a = std::max(zs, s.off), b = std::min(ze, s.off + g.len);
        if (a < b) mapdel(g.avail, g.start + (a - s.off), b - a);
      }
    }
  }

  // typebit: kBgData / kBgSystem / kBgMeta。exact なら len ちょうど以上の連続領域だけ。見つからなければ false
  bool find_space(u64 typebit, u64 len, u64 align, bool exact, u64* start, u64* got, Bg** gp) {
    u64 best_len = 0, best_start = 0;
    Bg* best_g = nullptr;
    for (auto& kv : bgs) {
      Bg& g = kv.second;
      if (!(g.flags & typebit)) continue;
      for (auto& r : g.avail) {
        u64 a = rup(r.first, align), rend = r.first + r.second;
        if (a >= rend) continue;
        u64 avl = rend - a;
        if (avl >= len) { *start = a; *got = len; *gp = &g; return true; }
        if (!exact && avl > best_len) { best_len = avl; best_start = a; best_g = &g; }
      }
    }
    if (!exact && best_g) {
      u64 l = best_len / sectorsize * sectorsize;
      if (l == 0) return false;
      *start = best_start; *got = l; *gp = best_g;
      return true;
    }
    return false;
  }
  void take_from(Bg& g, u64 s, u64 l) {
    mapdel(g.free, s, l); mapdel(g.avail, s, l);
    g.used += l; g.dirty = true; mutations++;
  }
  void give_back(u64 s, u64 l) {   // 解放: free には戻す (FST に反映される)。avail へはコミット後
    Bg* g = bg_of(s);
    if (!g) return;
    mapadd(g->free, s, l);
    g->used = g->used >= l ? g->used - l : 0;
    g->dirty = true;
    pending_free.push_back({s, l});
#ifdef USBMEM_MUTATE_REUSE_FREED
    mapadd(g->avail, s, l);   // 突然変異: 解放した領域を同じ世代のうちに再利用できるようにする
#endif
    mutations++;
  }
  bool meta_room(u64 need) {
    u64 sum = 0;
    for (auto& kv : bgs) {
      if (!(kv.second.flags & kBgMeta)) continue;
      for (auto& r : kv.second.avail) { sum += r.second; if (sum >= need) return true; }
    }
    return false;
  }

  // ============ ツリーブロックの確保・COW ============
  int alloc_tree_block(u64 owner, u64* out) {
    u64 want = owner == kChunkTreeObj ? kBgSystem : kBgMeta;
    u64 start, got;
    Bg* g;
    if (!find_space(want, nodesize, nodesize, true, &start, &got, &g)) {
      if (want == kBgMeta && alloc_chunk(false) == 0 && find_space(want, nodesize, nodesize, true, &start, &got, &g)) { /* 新しい chunk から */ }
      else return -ENOSPC;
    }
    take_from(*g, start, nodesize);
    used_delta[owner] += nodesize;
    *out = start;
    return 0;
  }
  void free_tree_block(u64 bytenr, int level, u64 owner) {
    dirty.erase(bytenr);
    ncache.erase(bytenr);
    DRef r; r.kind = R_DROP_TREE; r.bytenr = bytenr; r.level = level; r.root = owner;
    dq.push_back(r);
    used_delta[owner] -= nodesize;
    mutations++;
  }
  void queue_add_tree(u64 bytenr, int level, u64 owner) {
    DRef r; r.kind = R_ADD_TREE; r.bytenr = bytenr; r.level = level; r.root = owner;
    dq.push_back(r);
  }

  // 拡張項目 (extent ツリー)
  struct ERef { u8 type = 0; u64 root = 0, ino = 0, off = 0; u32 cnt = 0; };
  struct EItem { u64 refs = 0, gen = 0, flags = 0; std::vector<ERef> r; std::vector<u8> info; };
  static u64 data_ref_hash(u64 root, u64 ino, u64 off) {
    u8 b[8];
    u32 hi = 0xFFFFFFFFu, lo = 0xFFFFFFFFu;
    put64(b, root); hi = crc32c_raw(hi, b, 8);
    put64(b, ino); lo = crc32c_raw(lo, b, 8);
    put64(b, off); lo = crc32c_raw(lo, b, 8);
    return (static_cast<u64>(hi) << 31) ^ static_cast<u64>(lo);
  }
  static u64 eref_sortkey(const ERef& r) { return r.type == kExtentDataRefK ? data_ref_hash(r.root, r.ino, r.off) : r.root; }
  static int parse_eitem(const std::vector<u8>& v, bool nonskinny_tb, EItem* e) {
    if (v.size() < 24) return -EUCLEAN;
    e->refs = le64(&v[0]); e->gen = le64(&v[8]); e->flags = le64(&v[16]);
    size_t pos = 24;
    if (nonskinny_tb) {
      if (v.size() < 42) return -EUCLEAN;
      e->info.assign(v.begin() + 24, v.begin() + 42);
      pos = 42;
    }
    if (e->flags & 0x80) return -EOPNOTSUPP;   // FULL_BACKREF (共有)
    while (pos < v.size()) {
      u8 t = v[pos];
      ERef r; r.type = t;
      if (t == kTreeBlockRefK) {
        if (pos + 9 > v.size()) return -EUCLEAN;
        r.root = le64(&v[pos + 1]); pos += 9;
      } else if (t == kExtentDataRefK) {
        if (pos + 29 > v.size()) return -EUCLEAN;
        r.root = le64(&v[pos + 1]); r.ino = le64(&v[pos + 9]); r.off = le64(&v[pos + 17]); r.cnt = le32(&v[pos + 25]); pos += 29;
      } else {
        return -EOPNOTSUPP;   // 共有バックリファレンス (SHARED_BLOCK_REF / SHARED_DATA_REF) はスナップショット・reloc のもの
      }
      e->r.push_back(r);
    }
    return 0;
  }
  static void build_eitem(const EItem& e, std::vector<u8>* out) {
    out->assign(24 + e.info.size(), 0);
    put64(&(*out)[0], e.refs); put64(&(*out)[8], e.gen); put64(&(*out)[16], e.flags);
    if (!e.info.empty()) std::memcpy(&(*out)[24], e.info.data(), e.info.size());
    for (const ERef& r : e.r) {
      size_t o = out->size();
      if (r.type == kTreeBlockRefK) { out->resize(o + 9); (*out)[o] = r.type; put64(&(*out)[o + 1], r.root); }
      else { out->resize(o + 29); (*out)[o] = r.type; put64(&(*out)[o + 1], r.root); put64(&(*out)[o + 9], r.ino); put64(&(*out)[o + 17], r.off); put32(&(*out)[o + 25], r.cnt); }
    }
  }
  // ツリーブロックの拡張項目を探す (skinny → 通常の順)。k に実際のキー
  int find_tree_extent(u64 b, int level, Key* k, std::vector<u8>* v, bool* nonskinny) {
    int rc = get_item(kExtentTreeObj, Key{b, kMetadataItemK, static_cast<u64>(level)}, v);
    if (rc == 0) { *k = Key{b, kMetadataItemK, static_cast<u64>(level)}; *nonskinny = false; return 0; }
    if (rc != -ENOENT) return rc;
    rc = get_item(kExtentTreeObj, Key{b, kExtentItemK, nodesize}, v);
    if (rc == 0) { *k = Key{b, kExtentItemK, nodesize}; *nonskinny = true; return 0; }
    return rc == -ENOENT ? -EUCLEAN : rc;
  }
  // COW してよいブロックか: 参照が owner からの通常の参照 1 つだけ
  int check_unshared(u64 b, int level, u64 owner) {
    Key k; std::vector<u8> v; bool ns;
    int rc = find_tree_extent(b, level, &k, &v, &ns);
    if (rc) return rc;
    EItem e;
    rc = parse_eitem(v, ns, &e);
    if (rc) return rc;
    if (e.refs != 1 || e.r.size() != 1 || e.r[0].type != kTreeBlockRefK || e.r[0].root != owner || !(e.flags & 2)) return -EOPNOTSUPP;
    return 0;
  }

  int cow_node(u64 tree, const NodeP& old, std::shared_ptr<Node>* out) {
    u64 ob = node_bytenr(*old);
    auto it = dirty.find(ob);
    if (it != dirty.end()) { *out = it->second; return 0; }
    int rc = check_unshared(ob, old->level, tree);
    if (rc) return rc;
    u64 nb;
    rc = alloc_tree_block(tree, &nb);
    if (rc) return rc;
    auto nn = std::make_shared<Node>(*old);
    put64(&nn->d[48], nb); put64(&nn->d[80], cur_gen); put64(&nn->d[56], 1ull | (1ull << 56));
    put64(&nn->d[88], tree);
    dirty[nb] = nn;
    queue_add_tree(nb, old->level, tree);
    DRef r; r.kind = R_DROP_TREE; r.bytenr = ob; r.level = old->level; r.root = tree;
    dq.push_back(r);
    used_delta[tree] -= nodesize;
    ncache.erase(ob);
    mutations++;
    *out = nn;
    return 0;
  }

  struct WP { std::vector<std::pair<std::shared_ptr<Node>, int>> st; };

  int cow_root(u64 tree, std::shared_ptr<Node>* out) {
    Subvol* sv = T(tree);
    if (!sv) return -ENOENT;
    NodeP r;
    int rc = read_node(sv->bytenr, sv->level, &r);
    if (rc) return rc;
    std::shared_ptr<Node> m;
    rc = cow_node(tree, r, &m);
    if (rc) return rc;
    u64 nb = node_bytenr(*m);
    if (nb != sv->bytenr) set_root(tree, nb, sv->level);
    *out = m;
    return 0;
  }
  // 0 = 一致、1 = 無い (slot = 挿入位置)、負 = エラー。経路上の全ブロックを COW 済みにして返す
  int cow_search(u64 tree, const Key& k, WP& p) {
    p.st.clear();
    std::shared_ptr<Node> n;
    int rc = cow_root(tree, &n);
    if (rc) return rc;
    for (int depth = 0; depth < kMaxLevel + 1; depth++) {
      if (n->level == 0) {
        if (!leaf_packed(*n)) return -EUCLEAN;
        int lo = 0, hi = static_cast<int>(n->n);
        while (lo < hi) { int m = (lo + hi) / 2; if (kcmp(n->key(m), k) < 0) lo = m + 1; else hi = m; }
        p.st.push_back({n, lo});
        return (lo < static_cast<int>(n->n) && kcmp(n->key(lo), k) == 0) ? 0 : 1;
      }
      int lo = 0, hi = static_cast<int>(n->n);
      while (lo < hi) { int m = (lo + hi) / 2; if (kcmp(n->key(m), k) <= 0) lo = m + 1; else hi = m; }
      int idx = lo - 1;
      if (idx < 0) idx = 0;
      p.st.push_back({n, idx});
      NodeP ch;
      rc = read_node(n->child(idx), n->level - 1, &ch);
      if (rc) return rc;
      std::shared_ptr<Node> cm;
      rc = cow_node(tree, ch, &cm);
      if (rc) return rc;
      u64 cb = node_bytenr(*cm);
      if (cb != n->child(idx)) {
        put64(&n->d[kHdr + static_cast<size_t>(idx) * kPtrSz + 17], cb);
        put64(&n->d[kHdr + static_cast<size_t>(idx) * kPtrSz + 25], cur_gen);
      }
      n = cm;
    }
    return -EIO;
  }

  // p.st[idx] のノードの先頭キーが変わった: 親のキーを揃える (先頭スロットなら更に上へ)
  void fixup_low(WP& p, size_t idx, const Key& nk) {
    for (size_t l = idx; l > 0; l--) {
      auto& pa = p.st[l - 1];
      put_key(&pa.first->d[kHdr + static_cast<size_t>(pa.second) * kPtrSz], nk);
      if (pa.second != 0) break;
    }
  }

  // child_idx のノード (p.st[child_idx]) の右に新しいノード (key, bytenr) を親へ登録する
  int insert_ptr_after(u64 tree, WP& p, size_t child_idx, const Key& key, u64 nb, int child_level) {
    if (child_idx == 0) {   // ルートが分かれた: 新しいルート
      u64 rb;
      int rc = alloc_tree_block(tree, &rb);
      if (rc) return rc;
      auto root = new_node(rb, child_level + 1, tree);
      dirty[rb] = root;
      queue_add_tree(rb, child_level + 1, tree);
      const Node& oldroot = *p.st[0].first;
      node_insert_ptr(*root, 0, oldroot.key(0), node_bytenr(oldroot), le64(&oldroot.d[80]));
      node_insert_ptr(*root, 1, key, nb, cur_gen);
      set_root(tree, rb, child_level + 1);
      mutations++;
      return 0;
    }
    auto& pa = p.st[child_idx - 1];
    Node& par = *pa.first;
    u32 pos = static_cast<u32>(pa.second) + 1;
    if (par.n < max_ptrs()) { node_insert_ptr(par, pos, key, nb, cur_gen); return 0; }
    // 親も満杯: 半分に分ける
    struct P { Key k; u64 b, g; };
    std::vector<P> ps;
    for (u32 i = 0; i < par.n; i++) ps.push_back(P{par.key(static_cast<int>(i)), par.child(static_cast<int>(i)), le64(&par.d[kHdr + static_cast<size_t>(i) * kPtrSz + 25])});
    ps.insert(ps.begin() + pos, P{key, nb, cur_gen});
    size_t cut = ps.size() / 2;
    u64 rb;
    int rc = alloc_tree_block(tree, &rb);
    if (rc) return rc;
    auto right = new_node(rb, par.level, tree);
    dirty[rb] = right;
    queue_add_tree(rb, par.level, tree);
    std::memset(&par.d[kHdr], 0, nodesize - kHdr);
    set_nritems(par, 0);
    for (size_t i = 0; i < cut; i++) node_insert_ptr(par, static_cast<u32>(i), ps[i].k, ps[i].b, ps[i].g);
    for (size_t i = cut; i < ps.size(); i++) node_insert_ptr(*right, static_cast<u32>(i - cut), ps[i].k, ps[i].b, ps[i].g);
    mutations++;
    return insert_ptr_after(tree, p, child_idx - 1, ps[cut].k, rb, par.level);
  }

  int insert_into_leaf(u64 tree, WP& p, const Key& k, const u8* data, u32 sz) {
    std::shared_ptr<Node> leaf = p.st.back().first;
    u32 slot = static_cast<u32>(p.st.back().second);
    size_t li = p.st.size() - 1;
    if (leaf_free(*leaf) >= sz + kItemSz) {
      leaf_insert(*leaf, slot, k, data, sz);
      if (slot == 0) fixup_low(p, li, k);
      return 0;
    }
    struct LI { Key k; std::vector<u8> v; };
    std::vector<LI> items;
    for (u32 i = 0; i < leaf->n; i++) {
      u32 isz; const u8* ip = leaf->item(static_cast<int>(i), &isz);
      items.push_back(LI{leaf->key(static_cast<int>(i)), std::vector<u8>(ip, ip + isz)});
    }
    items.insert(items.begin() + slot, LI{k, std::vector<u8>(data, data + sz)});
    size_t tot = 0;
    for (auto& it : items) tot += it.v.size() + kItemSz;
    const size_t cap = leaf_body();
    size_t cut = 0;
    bool redo = false;   // 新しい項目が入る余地が作れないときは、まず既存の項目だけを slot で分けてから入れ直す
    auto left_sum = [&](size_t c) { size_t l = 0; for (size_t i = 0; i < c; i++) l += items[i].v.size() + kItemSz; return l; };
    auto fits = [&](size_t c) { size_t l = left_sum(c); return c >= 1 && c < items.size() && l <= cap && tot - l <= cap; };
    if (slot == leaf->n) {
      cut = items.size() - 1;   // 末尾への追加: 新しい項目だけの葉を右に作る
      if (!fits(cut)) return -EFBIG;
    } else {
      size_t acc = 0, bal = 1;
      for (size_t i = 0; i < items.size(); i++) { acc += items[i].v.size() + kItemSz; if (acc * 2 >= tot) { bal = i + 1; break; } }
      if (bal >= items.size()) bal = items.size() - 1;
      bool ok = false;
      for (size_t c : {bal, static_cast<size_t>(slot), static_cast<size_t>(slot) + 1, static_cast<size_t>(1)}) if (fits(c)) { cut = c; ok = true; break; }
      if (!ok) {
        if (slot == 0) return -EFBIG;
        items.erase(items.begin() + slot);
        tot -= static_cast<size_t>(sz) + kItemSz;
        cut = slot;
        if (!fits(cut)) return -EFBIG;
        redo = true;
      }
    }
    u64 rb;
    int rc = alloc_tree_block(tree, &rb);
    if (rc) return rc;
    auto right = new_node(rb, 0, tree);
    dirty[rb] = right;
    queue_add_tree(rb, 0, tree);
    if (cut < items.size() - 1 || slot != leaf->n) {
      // 左を作り直す
      std::memset(&leaf->d[kHdr], 0, nodesize - kHdr);
      set_nritems(*leaf, 0);
      for (size_t i = 0; i < cut; i++) leaf_insert(*leaf, static_cast<u32>(i), items[i].k, items[i].v.data(), static_cast<u32>(items[i].v.size()));
    }
    for (size_t i = cut; i < items.size(); i++) leaf_insert(*right, static_cast<u32>(i - cut), items[i].k, items[i].v.data(), static_cast<u32>(items[i].v.size()));
    mutations++;
    if (slot == 0 && cut > 0 && !redo) fixup_low(p, li, items[0].k);
    int rc2 = insert_ptr_after(tree, p, li, items[cut].k, rb, 0);
    if (rc2 || !redo) return rc2;
    return ins(tree, k, data, sz);
  }

  // 親から子を外す (p.st[idx] のノードの現在スロット)
  int del_ptr(u64 tree, WP& p, size_t idx) {
    std::shared_ptr<Node> nd = p.st[idx].first;
    u32 s = static_cast<u32>(p.st[idx].second);
    node_del_ptr(*nd, s);
    if (nd->n > 0) {
      if (s == 0) fixup_low(p, idx, nd->key(0));
      // ルートが 1 本だけのポインタなら 1 段下げる
      if (idx == 0) {
        while (nd->level > 0 && nd->n == 1) {
          u64 cb = nd->child(0);
          int lvl = nd->level - 1;
          u64 ob = node_bytenr(*nd);
          set_root(tree, cb, lvl);
          free_tree_block(ob, nd->level, tree);
          NodeP cn;
          if (read_node(cb, lvl, &cn)) return -EIO;
          if (auto it = dirty.find(cb); it != dirty.end()) nd = it->second;
          else { nd = std::make_shared<Node>(*cn); }   // 読み取りのみに使う (以降の変更はしない)
        }
      }
      return 0;
    }
    if (idx == 0) {   // ルートが空: 空の葉に置き換える
      u64 rb;
      int rc = alloc_tree_block(tree, &rb);
      if (rc) return rc;
      auto leaf = new_node(rb, 0, tree);
      dirty[rb] = leaf;
      queue_add_tree(rb, 0, tree);
      u64 ob = node_bytenr(*nd);
      int olvl = nd->level;
      set_root(tree, rb, 0);
      free_tree_block(ob, olvl, tree);
      return 0;
    }
    free_tree_block(node_bytenr(*nd), nd->level, tree);
    return del_ptr(tree, p, idx - 1);
  }
  int delete_at(u64 tree, WP& p) {
    std::shared_ptr<Node> leaf = p.st.back().first;
    u32 slot = static_cast<u32>(p.st.back().second);
    size_t li = p.st.size() - 1;
    leaf_delete(*leaf, slot);
    if (leaf->n > 0) {
      if (slot == 0) fixup_low(p, li, leaf->key(0));
      return 0;
    }
    if (li == 0) return 0;   // 空のルート葉
    free_tree_block(node_bytenr(*leaf), 0, tree);
    return del_ptr(tree, p, li - 1);
  }

  // ---- 公開プリミティブ
  int ins(u64 tree, const Key& k, const void* d, u32 sz) {
    if (sz + kItemSz > leaf_body()) return -EFBIG;
    mutations++;
    WP p;
    int rc = cow_search(tree, k, p);
    if (rc == 0) return -EEXIST;
    if (rc < 0) return rc;
    return insert_into_leaf(tree, p, k, static_cast<const u8*>(d), sz);
  }
  int del(u64 tree, const Key& k) {
    mutations++;
    WP p;
    int rc = cow_search(tree, k, p);
    if (rc > 0) return -ENOENT;
    if (rc < 0) return rc;
    return delete_at(tree, p);
  }
  int upd(u64 tree, const Key& k, const void* d, u32 sz) {
    mutations++;
    WP p;
    int rc = cow_search(tree, k, p);
    if (rc > 0) return -ENOENT;
    if (rc < 0) return rc;
    Node& leaf = *p.st.back().first;
    u32 slot = static_cast<u32>(p.st.back().second);
    if (item_sz(leaf, slot) == sz) {
      if (sz) std::memcpy(&leaf.d[kHdr + item_off(leaf, slot)], d, sz);
      return 0;
    }
    rc = delete_at(tree, p);
    if (rc) return rc;
    return ins(tree, k, d, sz);
  }
  // key 以上の最初の項目の key (範囲削除用)。見つからなければ -ENOENT
  int next_key(u64 tree, const Key& from, Key* out) { return find_ge(tree, from, out, nullptr); }

  // ============ extent ツリー (遅延参照の反映) ============
  int meta_ref_add(const DRef& r) {
    EItem e;
    e.refs = 1; e.gen = cur_gen; e.flags = 2;
    ERef rr; rr.type = kTreeBlockRefK; rr.root = r.root;
    e.r.push_back(rr);
    std::vector<u8> v;
    build_eitem(e, &v);
    return ins(kExtentTreeObj, Key{r.bytenr, kMetadataItemK, static_cast<u64>(r.level)}, v.data(), static_cast<u32>(v.size()));
  }
  int meta_ref_drop(const DRef& r) {
    Key k; std::vector<u8> v; bool ns;
    int rc = find_tree_extent(r.bytenr, r.level, &k, &v, &ns);
    if (rc) return rc;
    EItem e;
    rc = parse_eitem(v, ns, &e);
    if (rc) return rc;
    if (e.refs != 1 || e.r.size() != 1 || e.r[0].type != kTreeBlockRefK || e.r[0].root != r.root) return -EOPNOTSUPP;
    rc = del(kExtentTreeObj, k);
    if (rc) return rc;
    give_back(r.bytenr, nodesize);
    return 0;
  }

  int keyed_ref_find(u64 b, u64 root, u64 ino, u64 off, u64* hkey, u32* cnt) {
    u64 h = data_ref_hash(root, ino, off);
    for (int i = 0; i < 4096; i++, h++) {
      std::vector<u8> v;
      int rc = get_item(kExtentTreeObj, Key{b, kExtentDataRefK, h}, &v);
      if (rc == -ENOENT) return -ENOENT;
      if (rc) return rc;
      if (v.size() == 28 && le64(&v[0]) == root && le64(&v[8]) == ino && le64(&v[16]) == off) { *hkey = h; *cnt = le32(&v[24]); return 0; }
    }
    return -EUCLEAN;
  }
  int keyed_ref_put(u64 b, u64 root, u64 ino, u64 off, u32 cnt, bool create) {
    u8 v[28];
    put64(v, root); put64(v + 8, ino); put64(v + 16, off); put32(v + 24, cnt);
    if (!create) {
      u64 hk; u32 c;
      int rc = keyed_ref_find(b, root, ino, off, &hk, &c);
      if (rc) return rc;
      return upd(kExtentTreeObj, Key{b, kExtentDataRefK, hk}, v, 28);
    }
    u64 h = data_ref_hash(root, ino, off);
    for (int i = 0; i < 4096; i++, h++) {
      int rc = ins(kExtentTreeObj, Key{b, kExtentDataRefK, h}, v, 28);
      if (rc != -EEXIST) return rc;
    }
    return -EUCLEAN;
  }

  int data_ref_add_new(const DRef& r) {
    EItem e;
    e.refs = 1; e.gen = cur_gen; e.flags = 1;
    ERef rr; rr.type = kExtentDataRefK; rr.root = r.root; rr.ino = r.ino; rr.off = r.off; rr.cnt = 1;
    e.r.push_back(rr);
    std::vector<u8> v;
    build_eitem(e, &v);
    return ins(kExtentTreeObj, Key{r.bytenr, kExtentItemK, r.num}, v.data(), static_cast<u32>(v.size()));
  }
  int data_ref_inc(const DRef& r) {
    Key k{r.bytenr, kExtentItemK, r.num};
    std::vector<u8> v;
    int rc = get_item(kExtentTreeObj, k, &v);
    if (rc) return rc == -ENOENT ? -EUCLEAN : rc;
    EItem e;
    rc = parse_eitem(v, false, &e);
    if (rc) return rc;
    if (!(e.flags & 1)) return -EUCLEAN;
    int idx = -1;
    for (size_t i = 0; i < e.r.size(); i++) if (e.r[i].type == kExtentDataRefK && e.r[i].root == r.root && e.r[i].ino == r.ino && e.r[i].off == r.off) idx = static_cast<int>(i);
    if (idx >= 0) {
      e.r[static_cast<size_t>(idx)].cnt++;
    } else {
      u64 hk; u32 c;
      rc = keyed_ref_find(r.bytenr, r.root, r.ino, r.off, &hk, &c);
      if (rc == 0) {
        rc = keyed_ref_put(r.bytenr, r.root, r.ino, r.off, c + 1, false);
        if (rc) return rc;
      } else if (rc == -ENOENT) {
        ERef nr; nr.type = kExtentDataRefK; nr.root = r.root; nr.ino = r.ino; nr.off = r.off; nr.cnt = 1;
        if (v.size() + 29 <= max_ext_item()) {
          size_t pos = 0;
          while (pos < e.r.size() && (e.r[pos].type < nr.type || (e.r[pos].type == nr.type && eref_sortkey(e.r[pos]) < eref_sortkey(nr)))) pos++;
          e.r.insert(e.r.begin() + static_cast<long>(pos), nr);
        } else {
          rc = keyed_ref_put(r.bytenr, r.root, r.ino, r.off, 1, true);
          if (rc) return rc;
        }
      } else return rc;
    }
    e.refs++;
    build_eitem(e, &v);
    return upd(kExtentTreeObj, k, v.data(), static_cast<u32>(v.size()));
  }
  int data_ref_dec(const DRef& r) {
    Key k{r.bytenr, kExtentItemK, r.num};
    std::vector<u8> v;
    int rc = get_item(kExtentTreeObj, k, &v);
    if (rc) return rc == -ENOENT ? -EUCLEAN : rc;
    EItem e;
    rc = parse_eitem(v, false, &e);
    if (rc) return rc;
    if (!(e.flags & 1) || e.refs == 0) return -EUCLEAN;
    int idx = -1;
    for (size_t i = 0; i < e.r.size(); i++) if (e.r[i].type == kExtentDataRefK && e.r[i].root == r.root && e.r[i].ino == r.ino && e.r[i].off == r.off) idx = static_cast<int>(i);
    if (idx >= 0) {
      ERef& rr = e.r[static_cast<size_t>(idx)];
      if (rr.cnt == 0) return -EUCLEAN;
      if (--rr.cnt == 0) e.r.erase(e.r.begin() + idx);
    } else {
      u64 hk; u32 c;
      rc = keyed_ref_find(r.bytenr, r.root, r.ino, r.off, &hk, &c);
      if (rc == -ENOENT) return -EUCLEAN;
      if (rc) return rc;
      if (c <= 1) { rc = del(kExtentTreeObj, Key{r.bytenr, kExtentDataRefK, hk}); }
      else rc = keyed_ref_put(r.bytenr, r.root, r.ino, r.off, c - 1, false);
      if (rc) return rc;
    }
    e.refs--;
    if (e.refs == 0) {
      if (!e.r.empty()) return -EUCLEAN;
      rc = del(kExtentTreeObj, k);
      if (rc) return rc;
      rc = del_csums(r.bytenr, r.num);
      if (rc) return rc;
      give_back(r.bytenr, r.num);
      return 0;
    }
    build_eitem(e, &v);
    return upd(kExtentTreeObj, k, v.data(), static_cast<u32>(v.size()));
  }

  int run_delayed_refs() {
    int guard = 0;
    while (!dq.empty()) {
      if (++guard > 5000000) return -EIO;
      DRef r = dq.front();
      dq.pop_front();
      int rc;
      switch (r.kind) {
        case R_ADD_TREE: rc = meta_ref_add(r); break;
        case R_DROP_TREE: rc = meta_ref_drop(r); break;
        case R_ADD_DATA: rc = data_ref_add_new(r); break;
        case R_INC_DATA: rc = data_ref_inc(r); break;
        case R_DEC_DATA: rc = data_ref_dec(r); break;
        default: rc = -EIO;
      }
      if (rc) return rc;
    }
    return 0;
  }
  void queue_data(int kind, u64 bytenr, u64 num, u64 root, u64 ino, u64 off) {
    DRef r; r.kind = kind; r.bytenr = bytenr; r.num = num; r.root = root; r.ino = ino; r.off = off;
    dq.push_back(r);
    mutations++;
  }

  // ---- チェックサムツリー
  int add_csums(u64 start, const std::vector<u8>& cs) {
    if (!T(kCsumTreeObj)) return -EUCLEAN;
    size_t per = std::max<size_t>(1, max_item() / csum_len) * csum_len;
    for (size_t o = 0; o < cs.size(); o += per) {
      size_t n = std::min(per, cs.size() - o);
      int rc = ins(kCsumTreeObj, Key{kExtentCsumObj, kExtentCsum, start + o / csum_len * sectorsize}, &cs[o], static_cast<u32>(n));
      if (rc) return rc;
    }
    return 0;
  }
  int del_csums(u64 start, u64 len) {
    if (!T(kCsumTreeObj)) return 0;
    u64 end = start + len;
    for (int guard = 0; guard < 1 << 24; guard++) {
      Key k; std::vector<u8> v;
      int rc = find_le(kCsumTreeObj, Key{kExtentCsumObj, kExtentCsum, start}, &k, &v);
      bool have = false;
      if (rc == 0 && k.obj == kExtentCsumObj && k.type == kExtentCsum && k.off + v.size() / csum_len * sectorsize > start) have = true;
      if (!have) {
        rc = find_ge(kCsumTreeObj, Key{kExtentCsumObj, kExtentCsum, start}, &k, &v);
        if (rc == -ENOENT) return 0;
        if (rc) return rc;
        if (k.obj != kExtentCsumObj || k.type != kExtentCsum || k.off >= end) return 0;
      }
      u64 a = k.off, ae = a + v.size() / csum_len * sectorsize;
      rc = del(kCsumTreeObj, k);
      if (rc) return rc;
      if (a < start) {   // 前半を残す
        rc = ins(kCsumTreeObj, Key{kExtentCsumObj, kExtentCsum, a}, v.data(), static_cast<u32>((start - a) / sectorsize * csum_len));
        if (rc) return rc;
      }
      if (ae > end) {    // 後半を残す
        size_t o = static_cast<size_t>((end - a) / sectorsize) * csum_len;
        rc = ins(kCsumTreeObj, Key{kExtentCsumObj, kExtentCsum, end}, &v[o], static_cast<u32>(v.size() - o));
        if (rc) return rc;
        return 0;
      }
    }
    return -EIO;
  }

  // ============ ブロックグループ / 空き領域ツリーの反映 ============
  u64 bg_tree_id() const { return bg_tree ? kObjBgTree : kExtentTreeObj; }

  static std::vector<u8> fst_bitmap_bytes(const Bg& g, u64 rs, u64 rl, u32 ss) {
    std::vector<u8> b(static_cast<size_t>((rl / ss + 7) / 8), 0);
    for (auto it = g.free.lower_bound(rs); it != g.free.end() && it->first < rs + rl; ++it) {
      u64 a = std::max(it->first, rs), e = std::min(it->first + it->second, rs + rl);
      for (u64 x = a; x < e; x += ss) { size_t bit = static_cast<size_t>((x - rs) / ss); b[bit / 8] |= static_cast<u8>(1u << (bit % 8)); }
    }
    // 直前の run がこの範囲に食い込む場合
    auto it = g.free.upper_bound(rs);
    if (it != g.free.begin()) {
      --it;
      u64 a = std::max(it->first, rs), e = std::min(it->first + it->second, rs + rl);
      if (it->first < rs) for (u64 x = a; x < e; x += ss) { size_t bit = static_cast<size_t>((x - rs) / ss); b[bit / 8] |= static_cast<u8>(1u << (bit % 8)); }
    }
    return b;
  }

  int sync_one_bg(Bg& g) {
    g.dirty = false;
    std::vector<std::pair<u64, u64>> runs(g.free.begin(), g.free.end());
    u64 used_now = g.used;
    // ブロックグループ項目
    if (!g.item_disk) {
      u8 v[24];
      put64(v, used_now); put64(v + 8, kFirstFree); put64(v + 16, g.flags);
      int rc = ins(bg_tree_id(), Key{g.start, kBlockGroupItemK, g.len}, v, 24);
      if (rc) return rc;
      g.item_disk = true; g.used_disk = used_now;
    } else if (used_now != g.used_disk) {
      std::vector<u8> v;
      int rc = get_item(bg_tree_id(), Key{g.start, kBlockGroupItemK, g.len}, &v);
      if (rc) return rc == -ENOENT ? -EUCLEAN : rc;
      if (v.size() != 24) return -EUCLEAN;
      put64(&v[0], used_now);
      rc = upd(bg_tree_id(), Key{g.start, kBlockGroupItemK, g.len}, v.data(), 24);
      if (rc) return rc;
      g.used_disk = used_now;
    }
    // 空き領域ツリー
    u8 info[8];
    if (!g.fst_disk) {
      put32(info, static_cast<u32>(runs.size())); put32(info + 4, 0);
      int rc = ins(kObjFst, Key{g.start, kFstInfoK, g.len}, info, 8);
      if (rc) return rc;
      for (auto& r : runs) { rc = ins(kObjFst, Key{r.first, kFstExtentK, r.second}, nullptr, 0); if (rc) return rc; }
      g.runs_disk = runs; g.bitmaps = false; g.fst_disk = true;
      return 0;
    }
    if (!g.bitmaps) {
      std::vector<std::pair<u64, u64>> del_v, add_v;
      std::set_difference(g.runs_disk.begin(), g.runs_disk.end(), runs.begin(), runs.end(), std::back_inserter(del_v));
      std::set_difference(runs.begin(), runs.end(), g.runs_disk.begin(), g.runs_disk.end(), std::back_inserter(add_v));
      for (auto& r : del_v) { int rc = del(kObjFst, Key{r.first, kFstExtentK, r.second}); if (rc) return rc == -ENOENT ? -EUCLEAN : rc; }
      for (auto& r : add_v) { int rc = ins(kObjFst, Key{r.first, kFstExtentK, r.second}, nullptr, 0); if (rc) return rc; }
      if (!del_v.empty() || !add_v.empty() || runs.size() != g.runs_disk.size()) {
        put32(info, static_cast<u32>(runs.size())); put32(info + 4, 0);
        int rc = upd(kObjFst, Key{g.start, kFstInfoK, g.len}, info, 8);
        if (rc) return rc;
      }
      g.runs_disk = runs;
      return 0;
    }
    // bitmap 形式: 既存の bitmap 項目の内容だけを書き換える (項目の増減はしない)
    for (auto& bm : g.bm_disk) {
      std::vector<u8> nb = fst_bitmap_bytes(g, bm.start, bm.len, sectorsize);
      if (nb != bm.bytes) {
        int rc = upd(kObjFst, Key{bm.start, kFstBitmapK, bm.len}, nb.data(), static_cast<u32>(nb.size()));
        if (rc) return rc;
        bm.bytes = nb;
      }
    }
    if (runs.size() != g.runs_disk.size() || runs != g.runs_disk) {
      put32(info, static_cast<u32>(runs.size())); put32(info + 4, 1);
      int rc = upd(kObjFst, Key{g.start, kFstInfoK, g.len}, info, 8);
      if (rc) return rc;
      g.runs_disk = runs;
    }
    return 0;
  }
  bool any_bg_dirty() const { for (auto& kv : bgs) if (kv.second.dirty) return true; return false; }
  int sync_bgs() {
    for (int guard = 0; guard < 1000; guard++) {
      Bg* g = nullptr;
      for (auto& kv : bgs) if (kv.second.dirty) { g = &kv.second; break; }
      if (!g) return 0;
      int rc = sync_one_bg(*g);
      if (rc) return rc;
    }
    return -EIO;
  }

  // ============ chunk の割り当て ============
  int load_dev_ranges() {
    if (dev_loaded) return 0;
    dev_ranges.clear();
    dev_ranges.push_back({0, 1ull << 20});
    for (int i = 0; i < 3; i++) dev_ranges.push_back({kSbOffsets[i] & ~0xFFFFull, (kSbOffsets[i] & ~0xFFFFull) + 0x10000});
    Key k{devid, kDevExtentK, 0};
    for (int guard = 0; guard < 1 << 20; guard++) {
      std::vector<u8> v; Key fk;
      int rc = find_ge(kDevTreeObj, k, &fk, &v);
      if (rc == -ENOENT) break;
      if (rc) return rc;
      if (fk.obj != devid || fk.type != kDevExtentK) break;
      if (v.size() < 48) return -EUCLEAN;
      dev_ranges.push_back({fk.off, fk.off + le64(&v[24])});
      k = Key{fk.obj, fk.type, fk.off + 1};
    }
    std::sort(dev_ranges.begin(), dev_ranges.end());
    dev_loaded = true;
    return 0;
  }
  static constexpr u64 kDevTreeObj = 4;
  // 物理の空き (1MiB 境界) から len 以上の連続領域を探す。exclude: すでに選んだ範囲
  bool find_phys(u64 len, const std::vector<std::pair<u64, u64>>& extra, u64* out) {
    std::vector<std::pair<u64, u64>> r = dev_ranges;
    r.insert(r.end(), extra.begin(), extra.end());
    std::sort(r.begin(), r.end());
    u64 pos = 1ull << 20;
    for (auto& x : r) {
      u64 gs = rup(pos, 1ull << 20), ge = std::min(x.first, dev_total) & ~((1ull << 20) - 1);
      if (ge > gs && ge - gs >= len) { *out = gs; return true; }
      pos = std::max(pos, x.second);
    }
    u64 gs = rup(pos, 1ull << 20), ge = dev_total & ~((1ull << 20) - 1);
    if (ge > gs && ge - gs >= len) { *out = gs; return true; }
    return false;
  }
  // 新しい chunk を作る (データ / メタデータ)。メモリ上は即時、chunk / dev ツリーへの反映はコミット時 (pending_chunks)
  int alloc_chunk(bool data) {
    int rc = load_dev_ranges();
    if (rc) return rc;
    u64 type = mixed ? (kBgData | kBgMeta) : (data ? kBgData : kBgMeta);
    // 同じ種類の既存 BG のプロファイル (DUP か single) を踏襲する
    bool dup = false, found = false;
    for (auto& kv : bgs) if ((kv.second.flags & type) == type || (!mixed && (kv.second.flags & type))) { dup = (kv.second.flags & kBgDupBit) != 0; found = true; break; }
    (void)found;
    int ns = dup ? 2 : 1;
    u64 want = data ? std::min<u64>(1ull << 30, std::max<u64>(8ull << 20, dev_total / 10)) : std::min<u64>(256ull << 20, std::max<u64>(8ull << 20, dev_total / 16));
    if (mixed) want = std::min<u64>(256ull << 20, std::max<u64>(8ull << 20, dev_total / 10));
    want &= ~((1ull << 20) - 1);
    u64 p[2] = {0, 0};
    u64 size = want;
    for (;;) {
      std::vector<std::pair<u64, u64>> extra;
      bool ok = true;
      for (int i = 0; i < ns && ok; i++) {
        if (find_phys(size, extra, &p[i])) extra.push_back({p[i], p[i] + size}); else ok = false;
      }
      if (ok) break;
      if (size <= (4ull << 20)) return -ENOSPC;
      size = (size / 2) & ~((1ull << 20) - 1);
    }
    u64 lstart = 0;
    for (auto& kv : chunks) lstart = std::max(lstart, kv.first + kv.second.len);
    lstart = rup(lstart, 1ull << 20);
    if (lstart < kFirstChunkOff) lstart = kFirstChunkOff;
    Chunk c;
    c.start = lstart; c.len = size; c.type = type | (dup ? kBgDupBit : 0);
    for (int i = 0; i < ns; i++) { c.stripes.push_back(Stripe{devid, p[i]}); dev_ranges.push_back({p[i], p[i] + size}); }
    chunks[lstart] = c;
    if (std::getenv("USBMEM_BTRFS_TRACE")) std::fprintf(stderr, "btrfs-write: 新 chunk logical=%llu len=%llu type=%llx phys0=%llu dev_total=%llu\n", (unsigned long long)lstart, (unsigned long long)size, (unsigned long long)c.type, (unsigned long long)p[0], (unsigned long long)dev_total);
    Bg g;
    g.start = lstart; g.len = size; g.flags = c.type; g.used = 0; g.used_disk = 0;
    g.free[lstart] = size; g.avail[lstart] = size;
    g.item_disk = false; g.fst_disk = false; g.dirty = true;
    auto it = bgs.emplace(lstart, std::move(g)).first;
    exclude_super_ranges(it->second);
    pending_chunks.push_back(lstart);
    txn_chunks.push_back(lstart);
    dev_bytes_used += size * static_cast<u64>(ns);
    mutations++;
    return 0;
  }
  static constexpr u64 kBgDupBit = 0x20, kFirstChunkOff = 1ull << 20;

  int flush_pending_chunks() {
    while (!pending_chunks.empty()) {
      u64 cs = pending_chunks.front();
      pending_chunks.pop_front();
      Chunk c = chunks[cs];
      std::vector<u8> it(48 + 32 * c.stripes.size(), 0);
      put64(&it[0], c.len); put64(&it[8], kExtentTreeObj); put64(&it[16], 65536); put64(&it[24], c.type);
      put32(&it[32], 65536); put32(&it[36], 65536); put32(&it[40], sectorsize);
      put16(&it[44], static_cast<u16>(c.stripes.size())); put16(&it[46], 1);
      for (size_t i = 0; i < c.stripes.size(); i++) {
        put64(&it[48 + i * 32], c.stripes[i].devid); put64(&it[56 + i * 32], c.stripes[i].off);
        std::memcpy(&it[64 + i * 32], dev_uuid, 16);
      }
      int rc = ins(kChunkTreeObj, Key{kFirstChunkObj, kChunkItem, cs}, it.data(), static_cast<u32>(it.size()));
      if (rc) return rc;
      for (const Stripe& s : c.stripes) {
        u8 de[48];
        put64(de, kChunkTreeObj); put64(de + 8, kFirstChunkObj); put64(de + 16, cs); put64(de + 24, c.len);
        std::memcpy(de + 32, chunk_uuid, 16);
        rc = ins(kDevTreeObj, Key{s.devid, kDevExtentK, s.off}, de, 48);
        if (rc) return rc;
      }
      std::vector<u8> dv;
      rc = get_item(kChunkTreeObj, Key{1, kDevItemK, devid}, &dv);
      if (rc) return rc == -ENOENT ? -EUCLEAN : rc;
      if (dv.size() != 98) return -EUCLEAN;
      put64(&dv[16], dev_bytes_used);
      rc = upd(kChunkTreeObj, Key{1, kDevItemK, devid}, dv.data(), 98);
      if (rc) return rc;
    }
    return 0;
  }

  // ============ コミット ============
  int update_root_items() {
    std::vector<u64> ids;
    for (u64 id : dirty_roots) if (id != kRootTreeObj && id != kChunkTreeObj) ids.push_back(id);
    for (u64 id : ids) {
      dirty_roots.erase(id);
      Subvol* sv = T(id);
      if (!sv) return -EUCLEAN;
      Key k; std::vector<u8> v;
      int rc = find_le(kRootTreeObj, Key{id, kRootItem, ~0ull}, &k, &v);
      if (rc) return rc == -ENOENT ? -EUCLEAN : rc;
      if (k.obj != id || k.type != kRootItem || v.size() < 239) return -EUCLEAN;
      u64 old_gen = le64(&v[160]);
      put64(&v[176], sv->bytenr);
      v[238] = static_cast<u8>(sv->level);
      put64(&v[160], cur_gen);
      if (v.size() >= 247 && le64(&v[239]) == old_gen) put64(&v[239], cur_gen);
      int64_t d = used_delta[id];
      u64 used = le64(&v[192]);
      put64(&v[192], d >= 0 ? used + static_cast<u64>(d) : (used > static_cast<u64>(-d) ? used - static_cast<u64>(-d) : 0));
      used_delta[id] = 0;
      rc = upd(kRootTreeObj, k, v.data(), static_cast<u32>(v.size()));
      if (rc) return rc;
    }
    return 0;
  }

#ifdef USBMEM_MUTATE_DATA_AFTER
  std::vector<std::pair<u64, std::vector<u8>>> deferred_data;
#endif
  int write_logical(u64 logical, const void* buf, size_t n) {
#ifdef USBMEM_MUTATE_DATA_AFTER
    return write_logical_now(logical, buf, n);
  }
  int write_logical_now(u64 logical, const void* buf, size_t n) {
#endif
    std::vector<u64> phys;
    int rc = map_logical(logical, n, &phys);
    if (rc) return rc;
    for (u64 p : phys) {
      Status st = dev->write_at(p, buf, n);
      if (!st.ok()) { if (std::getenv("USBMEM_BTRFS_TRACE")) std::fprintf(stderr, "btrfs-write: write_at(phys=%llu n=%zu dev=%llu) failed\n", (unsigned long long)p, n, (unsigned long long)dev->size()); return -EIO; }
    }
    return 0;
  }

  void set_backup_root(u8* sbp, int idx) {
    u8* b = sbp + 2859 + idx * 168;
    std::memset(b, 0, 168);
    auto root_of = [&](u64 id, u64* by, u8* lv, u64* gn) {
      Subvol* sv = T(id);
      *by = sv ? sv->bytenr : 0; *lv = sv ? static_cast<u8>(sv->level) : 0; *gn = cur_gen;
    };
    u64 by, gn; u8 lv;
    put64(b + 0, root_bytenr); put64(b + 8, cur_gen); b[152] = static_cast<u8>(root_level);
    put64(b + 16, chunk_root); put64(b + 24, le64(&sbp[164])); b[153] = static_cast<u8>(chunk_level);
    root_of(kExtentTreeObj, &by, &lv, &gn); put64(b + 32, by); put64(b + 40, gn); b[154] = lv;
    root_of(kFsTreeObj, &by, &lv, &gn); put64(b + 48, by); put64(b + 56, gn); b[155] = lv;
    root_of(kDevTreeObj, &by, &lv, &gn); put64(b + 64, by); put64(b + 72, gn); b[156] = lv;
    root_of(kCsumTreeObj, &by, &lv, &gn); put64(b + 80, by); put64(b + 88, gn); b[157] = lv;
    put64(b + 96, le64(&sbp[0x70])); put64(b + 104, le64(&sbp[0x78])); put64(b + 112, le64(&sbp[0x88]));
  }

  // 環境変数 USBMEM_BTRFS_TRACE があるとき、書き込みエンジンの失敗箇所を標準エラーに出す (調査用)
  static void trace_fail(const char* where) { if (std::getenv("USBMEM_BTRFS_TRACE")) std::fprintf(stderr, "btrfs-write: %s で失敗\n", where); }

  int commit_txn() {
    if (!txn_open) return 0;
    if (aborted) return -EIO;
    if (dirty.empty() && dq.empty() && pending_chunks.empty() && pending_free.empty() && !any_bg_dirty() && dirty_roots.empty()) { txn_open = false; return 0; }
    int rc = 0;
    int guard = 0;
    for (;;) {
      if (++guard > 200) { rc = -EIO; trace_fail("commit: 収束しない"); break; }
      rc = flush_pending_chunks(); if (rc) { trace_fail("flush_pending_chunks"); break; }
      rc = run_delayed_refs(); if (rc) { trace_fail("run_delayed_refs"); break; }
      rc = sync_bgs(); if (rc) { trace_fail("sync_bgs"); break; }
      rc = update_root_items(); if (rc) { trace_fail("update_root_items"); break; }
      bool more = !dq.empty() || any_bg_dirty() || !pending_chunks.empty();
      for (u64 id : dirty_roots) if (id != kRootTreeObj && id != kChunkTreeObj) more = true;
      if (!more) break;
    }
    if (rc) { abort_txn(rc); return rc; }
    // 全 BG の used の合計
    u64 total_used = 0;
    for (auto& kv : bgs) total_used += kv.second.used;
    // 1. ツリーブロックを書く
    std::vector<u64> keys;
    keys.reserve(dirty.size());
    for (auto& kv : dirty) keys.push_back(kv.first);
    std::sort(keys.begin(), keys.end());
    for (u64 b : keys) {
      Node& n = *dirty[b];
      put64(&n.d[80], cur_gen);
      u8 cs[32];
      calc_csum(csum_type, &n.d[32], nodesize - 32, cs);
      std::memset(&n.d[0], 0, 32);
      std::memcpy(&n.d[0], cs, csum_len);
      rc = write_logical(b, n.d.data(), nodesize);
      if (rc) { trace_fail("write_logical(tree)"); if (std::getenv("USBMEM_BTRFS_TRACE")) std::fprintf(stderr, "  logical=%llu rc=%d\n", (unsigned long long)b, rc); abort_txn(rc); return rc; }
    }
#ifndef USBMEM_MUTATE_NO_FLUSH
    Status st = dev->flush();   // ツリーブロックとデータをデバイスへ確定させてから、スーパーブロックを書く
#else
    Status st = Status::Ok();   // 突然変異: ここの flush を省く (キャッシュのあるデバイスで順序が崩れる)
#endif
    if (!st.ok()) { abort_txn(-EIO); return -EIO; }
    // 2. スーパーブロック
    std::array<u8, kSbSize> nsb = sb;
    put64(&nsb[0x48], cur_gen);
    put64(&nsb[0x50], root_bytenr);
    put64(&nsb[0x58], chunk_root);
    put64(&nsb[0x60], 0);
    put64(&nsb[0x78], total_used);
    if (chunk_root_changed) put64(&nsb[164], cur_gen);
    nsb[198] = static_cast<u8>(root_level);
    nsb[199] = static_cast<u8>(chunk_level);
    put64(&nsb[217], dev_bytes_used);
    set_backup_root(nsb.data(), static_cast<int>(cur_gen % 4));
    // 本体 → ミラーの順。ミラーは範囲内のものだけ
#ifndef USBMEM_MUTATE_NO_SB_WRITE
    for (int i = 0; i < 3; i++) {
      if (!range_ok(dev->size(), kSbOffsets[i], kSbSize)) continue;
      put64(&nsb[0x30], kSbOffsets[i]);
      u8 cs[32];
      calc_csum(csum_type, &nsb[32], kSbSize - 32, cs);
      std::memset(&nsb[0], 0, 32);
      std::memcpy(&nsb[0], cs, csum_len);
      st = dev->write_at(kSbOffsets[i], nsb.data(), kSbSize);
      if (!st.ok()) { abort_txn(-EIO); return -EIO; }
    }
#endif
    st = dev->flush();
    if (!st.ok()) { abort_txn(-EIO); return -EIO; }
#ifdef USBMEM_MUTATE_DATA_AFTER
    // 突然変異: データをスーパーブロックの確定後に書く (電源断でスーパーブロックだけが先に反映されると、中身の無い extent を指す)
    for (auto& d : deferred_data) { rc = write_logical_now(d.first, d.second.data(), d.second.size()); if (rc) { abort_txn(rc); return rc; } }
    deferred_data.clear();
    st = dev->flush();
    if (!st.ok()) { abort_txn(-EIO); return -EIO; }
#endif
    // 3. 確定: メモリの状態を更新
    put64(&nsb[0x30], kSbOffsets[0]);
    {
      u8 cs[32];
      calc_csum(csum_type, &nsb[32], kSbSize - 32, cs);
      std::memset(&nsb[0], 0, 32);
      std::memcpy(&nsb[0], cs, csum_len);
    }
    sb = nsb;
    generation = cur_gen;
    for (auto& kv : dirty) { if (ncache.size() >= ncache_max) ncache.clear(); ncache[kv.first] = kv.second; }
    dirty.clear();
    for (auto& r : pending_free) {
      Bg* g = bg_of(r.first);
      if (g) { mapadd(g->avail, r.first, r.second); mapdel_unavail(*g, r.first, r.second); }
    }
    pending_free.clear();
    used_delta.clear();
    dirty_roots.clear();
    txn_chunks.clear();
    chunk_root_changed = false;
    txn_open = false;
    txn_data_bytes = 0; txn_ops = 0;
    commits_done++;
    return 0;
  }
  bool chunk_root_changed = false;
  std::vector<u64> txn_chunks;          // このトランザクションで作った chunk (中断時に取り除く)
  u64 txn_data_bytes = 0, txn_ops = 0;
  // 解放済みでも avail に戻してはいけない範囲 (スーパーブロックの置き場) を再び外す
  void mapdel_unavail(Bg& g, u64 s, u64 l) {
    (void)s; (void)l;
    exclude_super_ranges(g);
  }

  void abort_txn(int why) {
    if (std::getenv("USBMEM_BTRFS_TRACE")) std::fprintf(stderr, "btrfs-write: abort_txn why=%d\n", why);
    aborted = true;
    writable_ = false;
    ro_reason = "書き込み中にエラー (errno " + std::to_string(-why) + ") が起きたため、読み取り専用に切り替えた";
    // メモリ上の未コミットの変更を捨て、確定済みのルートに戻す
    dirty.clear(); dq.clear(); pending_free.clear(); pending_chunks.clear(); dirty_roots.clear(); used_delta.clear();
    for (u64 c : txn_chunks) chunks.erase(c);
    txn_chunks.clear();
    bgs.clear();
    ncache.clear();
    csum_loaded = false; csum_present = false;
    root_bytenr = le64(&sb[0x50]); root_level = sb[198];
    chunk_root = le64(&sb[0x58]); chunk_level = sb[199];
    subvols.clear(); subvol_slot.clear();
    u32 slot;
    slot_for(mount_subvol, &slot);
    txn_open = false;
  }

  int begin_txn() {
    if (!writable_ || aborted) return -EROFS;
    if (!txn_open) { cur_gen = generation + 1; txn_open = true; }
    return 0;
  }

  // メタデータの空きを確保する (操作の前に呼ぶ)。足りなければコミットで解放分を戻し、それでも足りなければ新しい chunk
  int reserve_meta(bool freeing = false) {
    u64 need = static_cast<u64>(freeing ? kReserveFreeing : kReserveNodes) * nodesize;
    if (meta_room(need)) return 0;
    if (!pending_free.empty()) {
      int rc = commit_txn();
      if (rc) return rc;
      rc = begin_txn();
      if (rc) return rc;
      if (meta_room(need)) return 0;
    }
    if (alloc_chunk(false) == 0 && meta_room(need)) return 0;
    return -ENOSPC;
  }
  bool data_room(u64 need) {
    u64 sum = 0;
    for (auto& kv : bgs) {
      if (!(kv.second.flags & kBgData)) continue;
      for (auto& r : kv.second.avail) { sum += r.second / sectorsize * sectorsize; if (sum >= need) return true; }
    }
    return false;
  }
  static constexpr int kReserveNodes = 96, kReserveFreeing = 32;   // 通常の操作は 96 ノード分、削除などは 32 ノード分の余裕が無いと始めない (差の分が削除専用の予備)

  // 大きくなったら自動コミット
  int maybe_autocommit() {
    if (txn_open && (dirty.size() * static_cast<size_t>(nodesize) > (48u << 20) || txn_data_bytes > (96u << 20) || txn_ops > 2000)) return commit_txn();
    return 0;
  }
