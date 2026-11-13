// SPDX-License-Identifier: GPL-2.0-or-later
// Btrfs 書き込み: 書き込みマウントの検査・読み込み (rw_load) と、ファイル / ディレクトリ操作。btrfs_volume.cpp の Impl の中に btrfs_write.inc の後で #include される。
// 対象は最上位サブボリューム (ID 5) 1 つだけ。サブボリューム / スナップショット / クォータ / RAID / 圧縮済み extent の新規作成などは持たない (読み取りだけ)。docs/btrfs-write.md

  // ============ 書き込みマウントの検査と読み込み ============
  static constexpr u8 kFtReg = 1, kFtDir = 2, kFtSymlink = 7;
  static constexpr u64 kObjRelocData = static_cast<u64>(-9);
  static constexpr u64 kObjUuid = 9;

  static u8 ft_of_mode(u32 mode) {
    switch (mode & 0170000) {
      case 0100000: return 1; case 0040000: return 2; case 0020000: return 3; case 0060000: return 4;
      case 0010000: return 5; case 0140000: return 6; case 0120000: return 7; default: return 0;
    }
  }

  // 書き込み可にできない理由を返す (空 = 可)。致命的な読み取りエラーも「理由」にして読み取り専用へ落とす
  std::string rw_load() {
    auto fail = [](const std::string& m) { return m; };
    if (dev->is_read_only()) return fail("デバイスが読み取り専用");
    if (log_ignored || le64(&sb[0x60]) != 0) return fail("未再生の tree-log がある");
    u64 flags = le64(&sb[0x38]);
    if (flags & ~1ull) return fail("スーパーブロックの flags に未対応のビットがある");
    u64 compat = le64(&sb[0xac]), compat_ro = le64(&sb[0xb4]);
    if (compat) return fail("未知の compat 機能がある");
    if (compat_ro & ~0xbull) return fail("未対応の compat_ro 機能がある");
    if ((compat_ro & 3) != 3) return fail("空き領域ツリー (free space tree) が無い / 有効でない (v1 の空き領域キャッシュは未対応)");
    bg_tree = (compat_ro & 8) != 0;
    if (incompat & ((1ull << 2) | (1ull << 7) | (1ull << 16))) return fail("mixed block group / RAID56 / simple quota は未対応");
    skinny = (incompat & (1ull << 8)) != 0;
    no_holes = (incompat & (1ull << 9)) != 0;
    if (!skinny) return fail("skinny metadata が無いボリュームは未対応");
    if (!no_holes) return fail("no-holes が無いボリュームは未対応");
    mixed = false;
    // スーパーブロックのミラー
    {
      int n_in = 0;
      for (int i = 0; i < 3; i++) {
        if (!sb_inrange[i]) continue;
        n_in++;
        if (!sb_valid_seen[i]) return fail("スーパーブロックのミラー " + std::to_string(i) + " が壊れている / 無い");
        if (sb_gen_seen[i] != generation) return fail("スーパーブロックのミラーの世代が一致しない (電源断の途中かもしれない。Linux で一度マウントして整合させてから)");
      }
      if (n_in == 0) return fail("スーパーブロックが見つからない");
    }
    dev_total = le64(&sb[209]);
    dev_bytes_used = le64(&sb[217]);
    if (dev_total == 0 || dev->size() < dev_total) return fail("デバイスがファイルシステムの大きさより小さい");
    std::memcpy(dev_uuid, &sb[267], 16);
    // chunk のプロファイル
    for (auto& kv : chunks) {
      const Chunk& c = kv.second;
      if (c.type & ~(kBgData | kBgSystem | kBgMeta | kBgDupBit)) return fail("single / DUP 以外のプロファイルの chunk がある");
      int kinds = ((c.type & kBgData) ? 1 : 0) + ((c.type & kBgSystem) ? 1 : 0) + ((c.type & kBgMeta) ? 1 : 0);
      if (kinds != 1) return fail("データとメタデータが同居した chunk がある");
      size_t want = (c.type & kBgDupBit) ? 2 : 1;
      if (c.stripes.size() != want) return fail("chunk のストライプ数が想定と合わない");
      for (const Stripe& s : c.stripes) if (s.devid != devid) return fail("別デバイスのストライプがある");
      if ((c.start % sectorsize) || (c.len % sectorsize)) return fail("chunk がセクタ境界に揃っていない");
    }
    // chunk ツリーの UUID
    {
      NodeP cn;
      if (read_node(chunk_root, chunk_level, &cn)) return fail("chunk ツリーが読めない");
      std::memcpy(chunk_uuid, &cn->d[64], 16);
    }
    // ルートツリーの項目
    {
      Cursor c;
      int rc = search(root_bytenr, root_level, Key{0, 0, 0}, false, c);
      if (rc < 0) return fail("ルートツリーが読めない");
      for (int guard = 0; rc == 0 && guard < (1 << 20); guard++) {
        Key k = c.key();
        if (k.type == kRootItem) {
          bool ok = k.obj == kExtentTreeObj || k.obj == kDevTreeObj || k.obj == kFsTreeObj || k.obj == kCsumTreeObj || k.obj == kObjFst ||
                    k.obj == kObjBgTree || k.obj == kObjUuid || k.obj == kObjRelocData;
          if (!ok) return fail("サブボリューム / スナップショット / クォータなど未対応のルートがある (ID " + std::to_string(k.obj) + ")");
        } else if (k.obj != kRootTreeDirObj && !(k.type == kInodeRef && k.obj == kFsTreeObj && k.off == kRootTreeDirObj)) {
          return fail("ルートツリーに未対応の項目 (型 " + std::to_string(k.type) + ") がある");
        }
        rc = next(c);
        if (rc < 0) return fail("ルートツリーが読めない");
      }
    }
    if (mount_subvol != kFsTreeObj) return fail("既定のサブボリュームがトップレベルではない");
    // 木のルートを読み込んで固定 (以降は subvols[] のコピーを更新する)
    for (u64 id : {kRootTreeObj, kExtentTreeObj, kChunkTreeObj, kDevTreeObj, kFsTreeObj, kCsumTreeObj, kObjFst}) if (!T(id)) return fail("必要なツリー (ID " + std::to_string(id) + ") が無い");
    if (bg_tree && !T(kObjBgTree)) return fail("ブロックグループツリーが無い");
    if (!bg_tree && T(kObjBgTree)) return fail("ブロックグループツリーの宣言と実体が合わない");
    if (T(kFsTreeObj)->flags & 1) return fail("ルートサブボリュームが読み取り専用");
    csum_root = *T(kCsumTreeObj); csum_loaded = true; csum_present = true;
    // ブロックグループ
    {
      u64 pos = 0;
      u64 tid = bg_tree_id();
      for (int guard = 0; guard < (1 << 20); guard++) {
        Key k; std::vector<u8> v;
        int rc = find_ge(tid, Key{pos, kBlockGroupItemK, 0}, &k, &v);
        if (rc == -ENOENT) break;
        if (rc) return fail("ブロックグループが読めない");
        if (k.type != kBlockGroupItemK) {
          if (bg_tree) return fail("ブロックグループツリーに別の項目がある");
          rc = find_ge(tid, Key{k.obj, kBlockGroupItemK, 0}, &k, &v);
          if (rc || k.type != kBlockGroupItemK) return fail("ブロックグループが見つからない");
        }
        if (v.size() != 24) return fail("ブロックグループ項目の大きさが不正");
        auto cit = chunks.find(k.obj);
        if (cit == chunks.end() || cit->second.len != k.off) return fail("ブロックグループに対応する chunk が無い");
        u64 fl = le64(&v[16]);
        if (fl != cit->second.type) return fail("ブロックグループと chunk の種類が一致しない");
        Bg g;
        g.start = k.obj; g.len = k.off; g.flags = fl; g.used = g.used_disk = le64(&v[0]);
        if (g.used > g.len) return fail("ブロックグループの使用量が大きさを超えている");
        bgs.emplace(g.start, std::move(g));
        pos = k.obj + k.off;
        if (pos < k.obj) return fail("ブロックグループの範囲が不正");
      }
      if (bgs.size() != chunks.size()) return fail("ブロックグループの数と chunk の数が合わない");
    }
    // 空き領域ツリー
    {
      std::map<u64, bool> seen_info;
      std::map<u64, u32> info_count;
      Key q{0, 0, 0};
      for (int guard = 0; guard < (1 << 24); guard++) {
        Key k; std::vector<u8> v;
        int rc = find_ge(kObjFst, q, &k, &v);
        if (rc == -ENOENT) break;
        if (rc) return fail("空き領域ツリーが読めない");
        q = Key{k.obj, k.type, k.off + 1};
        if (k.off + 1 == 0) q = Key{k.obj, static_cast<u8>(k.type + 1), 0};
        if (k.type == kFstInfoK) {
          auto it = bgs.find(k.obj);
          if (it == bgs.end() || it->second.len != k.off || v.size() != 8) return fail("空き領域ツリーの INFO がブロックグループと合わない");
          info_count[k.obj] = le32(&v[0]);
          it->second.bitmaps = (le32(&v[4]) & 1) != 0;
          seen_info[k.obj] = true;
        } else if (k.type == kFstExtentK) {
          Bg* g = bg_of(k.obj);
          if (!g || k.obj + k.off > g->start + g->len || k.off == 0 || !v.empty() || (k.obj % sectorsize) || (k.off % sectorsize)) return fail("空き領域ツリーの extent が範囲外");
          if (g->bitmaps) return fail("bitmap 形式の領域に extent 形式の項目がある");
          auto nx = g->free.lower_bound(k.obj);
          if (nx != g->free.end() && nx->first < k.obj + k.off) return fail("空き領域が重なっている");
          if (nx != g->free.begin()) { auto pv = std::prev(nx); if (pv->first + pv->second > k.obj) return fail("空き領域が重なっている"); }
          g->runs_disk.push_back({k.obj, k.off});
          mapadd(g->free, k.obj, k.off);
        } else if (k.type == kFstBitmapK) {
          Bg* g = bg_of(k.obj);
          if (!g || !g->bitmaps || k.obj + k.off > g->start + g->len || k.off == 0 || (k.obj % sectorsize) || (k.off % sectorsize)) return fail("空き領域ツリーの bitmap が範囲外");
          if (v.size() != (k.off / sectorsize + 7) / 8) return fail("bitmap の大きさが不正");
          Bg::Bm bm; bm.start = k.obj; bm.len = k.off; bm.bytes = v;
          for (u64 i = 0; i < k.off / sectorsize; i++) if (v[i / 8] & (1u << (i % 8))) mapadd(g->free, k.obj + i * sectorsize, sectorsize);
          g->bm_disk.push_back(std::move(bm));
        } else {
          return fail("空き領域ツリーに未知の項目がある");
        }
      }
      for (auto& kv : bgs) {
        Bg& g = kv.second;
        if (!seen_info[g.start]) return fail("空き領域ツリーにブロックグループの INFO が無い");
        if (g.bitmaps) {
          u64 cover = 0;
          for (auto& bm : g.bm_disk) cover += bm.len;
          if (cover != g.len) return fail("bitmap がブロックグループ全体を覆っていない");
          g.runs_disk.assign(g.free.begin(), g.free.end());
        } else {
          std::sort(g.runs_disk.begin(), g.runs_disk.end());
        }
        if (info_count[g.start] != g.runs_disk.size()) return fail("空き領域ツリーの extent 数が INFO と合わない");
        u64 fsum = 0;
        for (auto& r : g.free) fsum += r.second;
        if (fsum > g.len) return fail("空き領域がブロックグループより大きい");
        g.fst_disk = true; g.item_disk = true; g.dirty = false;
        g.avail = g.free;
        exclude_super_ranges(g);
      }
    }
    // inode 番号
    {
      Key k;
      int rc = find_le(kFsTreeObj, Key{(1ull << 48) - 2, 255, ~0ull}, &k, nullptr);
      if (rc) return fail("ファイルシステムツリーが空");
      if (k.obj < kFirstFree) return fail("ルートディレクトリが無い");
      if (k.obj >= (1ull << 48) - 16) return fail("inode 番号が上限に近い");
      free_objectid = k.obj + 1;
    }
    {
      Inode ri;
      if (get_inode(*T(kFsTreeObj), kFirstFree, &ri)) return fail("ルートディレクトリが読めない");
    }
    return "";
  }

  // ============ 小さな補助 ============
  static u64 now_sec() { return static_cast<u64>(std::time(nullptr)); }
  static void put_ts(u8* p, u64 sec) { put64(p, sec); put32(p + 8, 0); }

  int inode_get(u64 obj, std::vector<u8>* v) {
    int rc = get_item(kFsTreeObj, Key{obj, kInodeItem, 0}, v);
    if (rc == 0 && v->size() != 160) return -EUCLEAN;
    return rc;
  }
  int inode_put(u64 obj, const std::vector<u8>& v) { return upd(kFsTreeObj, Key{obj, kInodeItem, 0}, v.data(), 160); }
  static u64 i_size(const std::vector<u8>& v) { return le64(&v[16]); }
  static u64 i_nbytes(const std::vector<u8>& v) { return le64(&v[24]); }
  static u32 i_nlink(const std::vector<u8>& v) { return le32(&v[40]); }
  static u32 i_mode(const std::vector<u8>& v) { return le32(&v[52]); }
  static u64 i_flags(const std::vector<u8>& v) { return le64(&v[64]); }
  void i_touch(std::vector<u8>& v, bool mtime) {
    u64 t = now_sec();
    put64(&v[8], cur_gen);
    put64(&v[72], le64(&v[72]) + 1);
    put_ts(&v[124], t);
    if (mtime) put_ts(&v[136], t);
  }

  static bool name_ok(const std::string& n) {
    if (n.empty() || n.size() > 255) return false;
    for (char c : n) if (c == '/' || c == '\0') return false;
    return n != "." && n != "..";
  }

  // 1 項目に複数ある dir_item / inode_ref の分解
  struct Span { size_t off, len; };
  static int split_diritems(const std::vector<u8>& v, std::vector<Span>* out) {
    size_t cur = 0;
    while (cur + 30 <= v.size()) {
      u32 dl = le16(&v[cur + 25]), nl = le16(&v[cur + 27]);
      if (nl == 0 || cur + 30 + nl + dl > v.size()) return -EUCLEAN;
      out->push_back(Span{cur, 30 + nl + dl});
      cur += 30 + nl + dl;
    }
    return cur == v.size() ? 0 : -EUCLEAN;
  }
  std::vector<u8> mk_diritem(u64 child, u8 ft, const std::string& name) {
    std::vector<u8> e(30 + name.size(), 0);
    put_key(&e[0], Key{child, kInodeItem, 0});
    put64(&e[17], cur_gen);
    put16(&e[25], 0);
    put16(&e[27], static_cast<u16>(name.size()));
    e[29] = ft;
    std::memcpy(&e[30], name.data(), name.size());
    return e;
  }
  // DIR_ITEM を探す。0 = 見つかった
  int dir_find(u64 dir, const std::string& name, u64* child, u8* ft, u8* loc_type) {
    std::vector<u8> v;
    u32 h = name_hash(reinterpret_cast<const u8*>(name.data()), name.size());
    int rc = get_item(kFsTreeObj, Key{dir, kDirItem, h}, &v);
    if (rc) return rc;
    std::vector<Span> sp;
    rc = split_diritems(v, &sp);
    if (rc) return rc;
    for (auto& s : sp) {
      u32 nl = le16(&v[s.off + 27]);
      if (nl == name.size() && std::memcmp(&v[s.off + 30], name.data(), nl) == 0) {
        if (child) *child = le64(&v[s.off]);
        if (loc_type) *loc_type = v[s.off + 8];
        if (ft) *ft = v[s.off + 29];
        return 0;
      }
    }
    return -ENOENT;
  }
  int dir_item_add(u64 dir, const std::string& name, u64 child, u8 ft) {
    u32 h = name_hash(reinterpret_cast<const u8*>(name.data()), name.size());
    std::vector<u8> e = mk_diritem(child, ft, name);
    Key k{dir, kDirItem, h};
    std::vector<u8> v;
    int rc = get_item(kFsTreeObj, k, &v);
    if (rc == -ENOENT) return ins(kFsTreeObj, k, e.data(), static_cast<u32>(e.size()));
    if (rc) return rc;
    if (v.size() + e.size() > max_item()) return -EOVERFLOW;
    v.insert(v.end(), e.begin(), e.end());
    return upd(kFsTreeObj, k, v.data(), static_cast<u32>(v.size()));
  }
  int dir_item_remove(u64 dir, const std::string& name) {
    u32 h = name_hash(reinterpret_cast<const u8*>(name.data()), name.size());
    Key k{dir, kDirItem, h};
    std::vector<u8> v;
    int rc = get_item(kFsTreeObj, k, &v);
    if (rc) return rc == -ENOENT ? -EUCLEAN : rc;
    std::vector<Span> sp;
    rc = split_diritems(v, &sp);
    if (rc) return rc;
    std::vector<u8> nv;
    bool found = false;
    for (auto& s : sp) {
      u32 nl = le16(&v[s.off + 27]);
      if (!found && nl == name.size() && std::memcmp(&v[s.off + 30], name.data(), nl) == 0) { found = true; continue; }
      nv.insert(nv.end(), v.begin() + static_cast<long>(s.off), v.begin() + static_cast<long>(s.off + s.len));
    }
    if (!found) return -EUCLEAN;
    if (nv.empty()) return del(kFsTreeObj, k);
    return upd(kFsTreeObj, k, nv.data(), static_cast<u32>(nv.size()));
  }

  // INODE_REF: {index u64, name_len u16, name}
  int inref_add(u64 child, u64 parent, u64 index, const std::string& name) {
    Key k{child, kInodeRef, parent};
    std::vector<u8> e(10 + name.size());
    put64(&e[0], index); put16(&e[8], static_cast<u16>(name.size()));
    std::memcpy(&e[10], name.data(), name.size());
    std::vector<u8> v;
    int rc = get_item(kFsTreeObj, k, &v);
    if (rc == -ENOENT) return ins(kFsTreeObj, k, e.data(), static_cast<u32>(e.size()));
    if (rc) return rc;
    if (v.size() + e.size() > max_item()) return -EMLINK;
    v.insert(v.end(), e.begin(), e.end());
    return upd(kFsTreeObj, k, v.data(), static_cast<u32>(v.size()));
  }
  int inref_remove(u64 child, u64 parent, const std::string& name, u64* index) {
    Key k{child, kInodeRef, parent};
    std::vector<u8> v;
    int rc = get_item(kFsTreeObj, k, &v);
    if (rc == -ENOENT) {   // 拡張 ref (INODE_EXTREF) に入っているものは扱わない
      return -EOPNOTSUPP;
    }
    if (rc) return rc;
    std::vector<u8> nv;
    size_t cur = 0;
    bool found = false;
    while (cur + 10 <= v.size()) {
      u32 nl = le16(&v[cur + 8]);
      if (cur + 10 + nl > v.size()) return -EUCLEAN;
      if (!found && nl == name.size() && std::memcmp(&v[cur + 10], name.data(), nl) == 0) { found = true; *index = le64(&v[cur]); }
      else nv.insert(nv.end(), v.begin() + static_cast<long>(cur), v.begin() + static_cast<long>(cur + 10 + nl));
      cur += 10 + nl;
    }
    if (cur != v.size()) return -EUCLEAN;
    if (!found) return -EOPNOTSUPP;
    if (nv.empty()) return del(kFsTreeObj, k);
    return upd(kFsTreeObj, k, nv.data(), static_cast<u32>(nv.size()));
  }
  bool has_extref(u64 obj) {
    Key k;
    int rc = find_ge(kFsTreeObj, Key{obj, kInodeExtref, 0}, &k, nullptr);
    return rc == 0 && k.obj == obj && k.type == kInodeExtref;
  }

  int next_index(u64 dir, u64* idx) {
    Key k;
    int rc = find_le(kFsTreeObj, Key{dir, kDirIndex, ~0ull}, &k, nullptr);
    if (rc == 0 && k.obj == dir && k.type == kDirIndex) { *idx = k.off + 1; return 0; }
    if (rc && rc != -ENOENT) return rc;
    *idx = 2;
    return 0;
  }
  bool dir_empty(u64 dir, int* err) {
    Key k;
    int rc = find_ge(kFsTreeObj, Key{dir, kDirIndex, 0}, &k, nullptr);
    *err = (rc && rc != -ENOENT) ? rc : 0;
    return !(rc == 0 && k.obj == dir && k.type == kDirIndex);
  }

  int dir_adjust(u64 dir, int64_t delta) {
    std::vector<u8> v;
    int rc = inode_get(dir, &v);
    if (rc) return rc == -ENOENT ? -EUCLEAN : rc;
    u64 sz = i_size(v);
    if (delta < 0 && sz < static_cast<u64>(-delta)) return -EUCLEAN;
    put64(&v[16], delta >= 0 ? sz + static_cast<u64>(delta) : sz - static_cast<u64>(-delta));
    i_touch(v, true);
    return inode_put(dir, v);
  }

  // 親ディレクトリにエントリを足す (DIR_ITEM + DIR_INDEX + INODE_REF + 親の size)
  int add_entry(u64 dir, const std::string& name, u64 child, u8 ft) {
    u64 idx;
    int rc = next_index(dir, &idx);
    if (rc) return rc;
    rc = dir_item_add(dir, name, child, ft);
    if (rc) return rc;
    std::vector<u8> e = mk_diritem(child, ft, name);
    rc = ins(kFsTreeObj, Key{dir, kDirIndex, idx}, e.data(), static_cast<u32>(e.size()));
    if (rc) return rc;
    rc = inref_add(child, dir, idx, name);
    if (rc) return rc;
    return dir_adjust(dir, 2 * static_cast<int64_t>(name.size()));
  }
  int remove_entry(u64 dir, const std::string& name, u64 child) {
    u64 idx;
    int rc = inref_remove(child, dir, name, &idx);
    if (rc) return rc;
    rc = dir_item_remove(dir, name);
    if (rc) return rc;
    rc = del(kFsTreeObj, Key{dir, kDirIndex, idx});
    if (rc) return rc == -ENOENT ? -EUCLEAN : rc;
    return dir_adjust(dir, -2 * static_cast<int64_t>(name.size()));
  }

  int mk_inode(u64 obj, u32 mode, u32 nlink, u64 size, u64 nbytes, u64 flags) {
    std::vector<u8> v(160, 0);
    put64(&v[0], cur_gen); put64(&v[8], cur_gen);
    put64(&v[16], size); put64(&v[24], nbytes);
    put32(&v[40], nlink); put32(&v[52], mode); put64(&v[64], flags);
    u64 t = now_sec();
    put_ts(&v[112], t); put_ts(&v[124], t); put_ts(&v[136], t); put_ts(&v[148], t);
    return ins(kFsTreeObj, Key{obj, kInodeItem, 0}, v.data(), 160);
  }

  // ---- extent 項目
  struct FileExt { Key k{0, 0, 0}; std::vector<u8> v; u64 start = 0, num = 0, disk = 0, dlen = 0, eoff = 0, ram = 0; u8 type = 0, comp = 0; };
  static int parse_fe(const Key& k, const std::vector<u8>& v, FileExt* e) {
    if (v.size() < 21) return -EUCLEAN;
    e->k = k; e->v = v; e->start = k.off; e->ram = le64(&v[8]); e->comp = v[16]; e->type = v[20];
    if (v[17] != 0 || le16(&v[18]) != 0) return -EOPNOTSUPP;
    if (e->type == 0) { e->num = e->ram; return 0; }
    if (e->type > 2 || v.size() < 53) return -EUCLEAN;
    e->disk = le64(&v[21]); e->dlen = le64(&v[29]); e->eoff = le64(&v[37]); e->num = le64(&v[45]);
    if (e->num == 0 || e->start + e->num < e->start) return -EUCLEAN;
    return 0;
  }
  int fe_le(u64 obj, u64 pos, FileExt* e) {
    Key fk; std::vector<u8> v;
    int rc = find_le(kFsTreeObj, Key{obj, kExtentData, pos}, &fk, &v);
    if (rc) return rc;
    if (fk.obj != obj || fk.type != kExtentData) return -ENOENT;
    return parse_fe(fk, v, e);
  }
  int fe_first(u64 obj, FileExt* e) {
    Key fk; std::vector<u8> v;
    int rc = find_ge(kFsTreeObj, Key{obj, kExtentData, 0}, &fk, &v);
    if (rc) return rc;
    if (fk.obj != obj || fk.type != kExtentData) return -ENOENT;
    return parse_fe(fk, v, e);
  }
  std::vector<u8> mk_inline(const u8* d, size_t n) {
    std::vector<u8> v(21 + n, 0);
    put64(&v[0], cur_gen); put64(&v[8], n);
    if (n) std::memcpy(&v[21], d, n);
    return v;
  }
  std::vector<u8> mk_regular(u64 disk, u64 len) {
    std::vector<u8> v(53, 0);
    put64(&v[0], cur_gen); put64(&v[8], len); v[20] = 1;
    put64(&v[21], disk); put64(&v[29], len); put64(&v[37], 0); put64(&v[45], len);
    return v;
  }

  // [ps, pe) のファイル内容を extent から外す (分割・縮小・削除)。nb: nbytes の増減
  int punch(u64 obj, u64 ps, u64 pe, int64_t* nb) {
    for (int guard = 0; guard < (1 << 26); guard++) {
      FileExt e;
      int rc = fe_le(obj, pe - 1, &e);
      if (rc == -ENOENT) return 0;
      if (rc) return rc;
      u64 s = e.start, en = s + e.num;
      if (en <= ps) return 0;
      if (e.type == 0) {
        if (ps > s) return -EOPNOTSUPP;   // インラインの一部だけ消すのは呼び出し側で済ませる
        rc = del(kFsTreeObj, e.k);
        if (rc) return rc;
        *nb -= static_cast<int64_t>(e.num);
        continue;
      }
      u64 a = std::max(ps, s), b = std::min(pe, en);
      bool counted = e.disk != 0;
      u64 refoff = s - e.eoff;
      if (ps <= s && pe >= en) {
        rc = del(kFsTreeObj, e.k);
        if (rc) return rc;
        if (counted) queue_data(R_DEC_DATA, e.disk, e.dlen, kFsTreeObj, obj, refoff);
      } else if (ps <= s) {   // 先頭を削る
        rc = del(kFsTreeObj, e.k);
        if (rc) return rc;
        std::vector<u8> nv = e.v;
        put64(&nv[37], e.eoff + (pe - s)); put64(&nv[45], en - pe);
        rc = ins(kFsTreeObj, Key{obj, kExtentData, pe}, nv.data(), static_cast<u32>(nv.size()));
        if (rc) return rc;
      } else if (pe >= en) {   // 末尾を削る
        std::vector<u8> nv = e.v;
        put64(&nv[45], ps - s);
        rc = upd(kFsTreeObj, e.k, nv.data(), static_cast<u32>(nv.size()));
        if (rc) return rc;
      } else {   // 真ん中を削る: 2 つに分ける
        std::vector<u8> lv = e.v, rv = e.v;
        put64(&lv[45], ps - s);
        put64(&rv[37], e.eoff + (pe - s)); put64(&rv[45], en - pe);
        rc = upd(kFsTreeObj, e.k, lv.data(), static_cast<u32>(lv.size()));
        if (rc) return rc;
        rc = ins(kFsTreeObj, Key{obj, kExtentData, pe}, rv.data(), static_cast<u32>(rv.size()));
        if (rc) return rc;
        if (counted) queue_data(R_INC_DATA, e.disk, e.dlen, kFsTreeObj, obj, refoff);
      }
      if (counted) *nb -= static_cast<int64_t>(b - a);
    }
    return -EIO;
  }

  // extent ツリーに残っていない領域であることの確認 (空き領域ツリーが壊れていても既存データを二重に割り当てないための保険)
  int verify_unused(u64 start, u64 len) {
    Key q{start + len - 1, 255, ~0ull};
    for (int guard = 0; guard < 16; guard++) {
      Key k; std::vector<u8> v;
      int rc = find_le(kExtentTreeObj, q, &k, &v);
      if (rc == -ENOENT) return 0;
      if (rc) return rc;
      if (k.type == kMetadataItemK || k.type == kExtentItemK) {
        u64 elen = k.type == kMetadataItemK ? nodesize : k.off;
        return k.obj + elen > start ? -EUCLEAN : 0;
      }
      if (k.type > kMetadataItemK) q = Key{k.obj, kMetadataItemK, ~0ull};
      else { if (k.obj == 0) return 0; q = Key{k.obj - 1, 255, ~0ull}; }
    }
    return -EUCLEAN;
  }

  int ensure_data_space(u64 need) {
    for (int guard = 0; guard < 64; guard++) {
      u64 sum = 0;
      for (auto& kv : bgs) {
        if (!(kv.second.flags & kBgData)) continue;
        for (auto& r : kv.second.avail) sum += r.second / sectorsize * sectorsize;
        if (sum >= need) return 0;
      }
      if (alloc_chunk(true)) return -ENOSPC;
    }
    return -ENOSPC;
  }

  // a (セクタ境界) から L バイト (セクタの倍数) を新しい extent として書く。既存の内容は外す
  int write_regular(u64 obj, u64 a, const u8* buf, u64 L, bool csum, int64_t* nb) {
    int rc = ensure_data_space(L);
    if (rc) return rc;
    u64 done = 0;
    while (done < L) {
      u64 start, got;
      Bg* g;
      u64 want = std::min<u64>(L - done, 128ull << 20);
      if (!find_space(kBgData, want, sectorsize, false, &start, &got, &g)) return -ENOSPC;
      rc = verify_unused(start, got);
      if (rc) return rc;
      take_from(*g, start, got);
#ifdef USBMEM_MUTATE_DATA_AFTER
      deferred_data.push_back({start, std::vector<u8>(buf + done, buf + done + got)});
#else
      rc = write_logical(start, buf + done, static_cast<size_t>(got));
      if (rc) return rc;
#endif
      if (csum) {
        std::vector<u8> cs(static_cast<size_t>(got / sectorsize) * csum_len);
        for (u64 i = 0; i < got / sectorsize; i++) calc_csum(csum_type, buf + done + i * sectorsize, sectorsize, &cs[static_cast<size_t>(i) * csum_len]);
        rc = add_csums(start, cs);
        if (rc) return rc;
      }
      rc = punch(obj, a + done, a + done + got, nb);
      if (rc) return rc;
      std::vector<u8> iv = mk_regular(start, got);
      rc = ins(kFsTreeObj, Key{obj, kExtentData, a + done}, iv.data(), static_cast<u32>(iv.size()));
      if (rc) return rc;
      queue_data(R_ADD_DATA, start, got, kFsTreeObj, obj, a + done);
      *nb += static_cast<int64_t>(got);
      txn_data_bytes += got;
      done += got;
    }
    return 0;
  }

  int apply_nbytes(std::vector<u8>& iv, int64_t nb) {
    u64 cur = i_nbytes(iv);
    if (nb < 0 && cur < static_cast<u64>(-nb)) return -EUCLEAN;
    put64(&iv[24], nb >= 0 ? cur + static_cast<u64>(nb) : cur - static_cast<u64>(-nb));
    return 0;
  }

  int read_range(u64 obj, u64 off, u8* buf, size_t len) {
    Inode in;
    int rc = get_inode(*T(kFsTreeObj), obj, &in);
    if (rc) return rc;
    int64_t n = read_file(*T(kFsTreeObj), obj, in, off, buf, len);
    return n < 0 ? static_cast<int>(n) : 0;
  }

  // 圧縮されたインライン extent を展開して、非圧縮のインライン extent として扱えるようにする (項目自体はまだ書き換えない。呼び出し側が del して入れ直す)
  int decomp_inline(u64 obj, FileExt* f) {
    std::vector<u8> d(static_cast<size_t>(f->ram), 0);
    if (!d.empty()) {
      int rc = read_range(obj, 0, d.data(), d.size());
      if (rc) return rc;
    }
    f->v = mk_inline(d.data(), d.size());
    f->comp = 0;
    return 0;
  }

  // インライン extent を通常の extent に変換する (data は内容の先頭から)
  int inline_to_regular(u64 obj, FileExt f, bool csum, int64_t* nb) {
    if (f.comp != 0) { int r0 = decomp_inline(obj, &f); if (r0) return r0; }
    std::vector<u8> sec(static_cast<size_t>(rup(std::max<u64>(f.ram, 1), sectorsize)), 0);
    size_t n = std::min<size_t>(f.v.size() - 21, static_cast<size_t>(f.ram));
    std::memcpy(sec.data(), &f.v[21], n);
    int rc = del(kFsTreeObj, f.k);
    if (rc) return rc;
    *nb -= static_cast<int64_t>(f.num);
    return write_regular(obj, 0, sec.data(), sec.size(), csum, nb);
  }

  int write_chunk(u64 obj, u64 off, const u8* p, u64 n) {
    std::vector<u8> iv;
    int rc = inode_get(obj, &iv);
    if (rc) return rc;
    u32 fmt = i_mode(iv) & 0170000;
    if (fmt == 0040000) return -EISDIR;
    if (fmt != 0100000) return -EINVAL;
    u64 flags = i_flags(iv);
    if (flags & 0x10) return -EPERM;
    u64 size0 = i_size(iv), end = off + n;
    bool csum = !(flags & 1);
    u64 nsize = std::max(size0, end);
    int64_t nb = 0;
    FileExt f;
    rc = fe_first(obj, &f);
    bool has_ext = rc == 0;
    if (rc && rc != -ENOENT) return rc;
    bool done = false;
    if (has_ext && f.type == 0) {
      if (f.comp != 0) { rc = decomp_inline(obj, &f); if (rc) return rc; }
      if (f.start != 0) return -EUCLEAN;
      if (nsize <= inline_max()) {
        std::vector<u8> d(static_cast<size_t>(nsize), 0);
        std::memcpy(d.data(), &f.v[21], std::min<size_t>(f.v.size() - 21, d.size()));
        std::memcpy(d.data() + off, p, static_cast<size_t>(n));
        rc = del(kFsTreeObj, f.k);
        if (rc) return rc;
        std::vector<u8> nv = mk_inline(d.data(), d.size());
        rc = ins(kFsTreeObj, Key{obj, kExtentData, 0}, nv.data(), static_cast<u32>(nv.size()));
        if (rc) return rc;
        nb += static_cast<int64_t>(nsize) - static_cast<int64_t>(f.num);
        done = true;
      } else {
        rc = inline_to_regular(obj, f, csum, &nb);
        if (rc) return rc;
      }
    } else if (!has_ext && off == 0 && n <= inline_max() && size0 <= n) {
      std::vector<u8> nv = mk_inline(p, static_cast<size_t>(n));
      rc = ins(kFsTreeObj, Key{obj, kExtentData, 0}, nv.data(), static_cast<u32>(nv.size()));
      if (rc) return rc;
      nb += static_cast<int64_t>(n);
      done = true;
    }
    if (!done) {
      u64 a = off & ~sec_mask(), b = rup(end, sectorsize);
      std::vector<u8> buf(static_cast<size_t>(b - a), 0);
      if (off > a && a < size0) { rc = read_range(obj, a, buf.data(), static_cast<size_t>(std::min(off, size0) - a)); if (rc) return rc; }
      if (end < b && end < size0) { rc = read_range(obj, end, buf.data() + (end - a), static_cast<size_t>(std::min(b, size0) - end)); if (rc) return rc; }
      std::memcpy(buf.data() + (off - a), p, static_cast<size_t>(n));
      rc = write_regular(obj, a, buf.data(), buf.size(), csum, &nb);
      if (rc) return rc;
    }
    rc = inode_get(obj, &iv);
    if (rc) return rc;
    put64(&iv[16], nsize);
    rc = apply_nbytes(iv, nb);
    if (rc) return rc;
    i_touch(iv, true);
    return inode_put(obj, iv);
  }

  int truncate_file(u64 obj, u64 size) {
    std::vector<u8> iv;
    int rc = inode_get(obj, &iv);
    if (rc) return rc;
    u32 fmt = i_mode(iv) & 0170000;
    if (fmt == 0040000) return -EISDIR;
    if (fmt != 0100000) return -EINVAL;
    u64 flags = i_flags(iv);
    if (flags & 0x10) return -EPERM;
    u64 size0 = i_size(iv);
    if (size == size0) return 0;
    bool csum = !(flags & 1);
    int64_t nb = 0;
    FileExt f;
    rc = fe_first(obj, &f);
    bool has_ext = rc == 0;
    if (rc && rc != -ENOENT) return rc;
    if (has_ext && f.type == 0) {
      if (f.comp != 0) { rc = decomp_inline(obj, &f); if (rc) return rc; }
      if (size == 0) {
        rc = del(kFsTreeObj, f.k);
        if (rc) return rc;
        nb -= static_cast<int64_t>(f.num);
      } else if (size <= inline_max()) {
        std::vector<u8> d(static_cast<size_t>(size), 0);
        std::memcpy(d.data(), &f.v[21], std::min<size_t>(std::min<size_t>(f.v.size() - 21, d.size()), static_cast<size_t>(f.ram)));
        rc = del(kFsTreeObj, f.k);
        if (rc) return rc;
        std::vector<u8> nv = mk_inline(d.data(), d.size());
        rc = ins(kFsTreeObj, Key{obj, kExtentData, 0}, nv.data(), static_cast<u32>(nv.size()));
        if (rc) return rc;
        nb += static_cast<int64_t>(size) - static_cast<int64_t>(f.num);
      } else {
        rc = inline_to_regular(obj, f, csum, &nb);
        if (rc) return rc;
        if (size < size0) {   // 変換後の通常 extent を縮める
          u64 keep = rup(size, sectorsize);
          rc = punch(obj, keep, 1ull << 62, &nb);
          if (rc) return rc;
        }
      }
    } else if (size < size0) {
      u64 keep = rup(size, sectorsize);
      rc = punch(obj, keep, 1ull << 62, &nb);
      if (rc) return rc;
      if (size % sectorsize) {   // 最後のセクタの EOF 以降を 0 にする (COW で 1 セクタ書き直す)
        u64 sa = size & ~sec_mask();
        FileExt c;
        rc = fe_le(obj, sa, &c);
        if (rc && rc != -ENOENT) return rc;
        if (rc == 0 && c.start + c.num > sa) {
          std::vector<u8> buf(sectorsize, 0);
          rc = read_range(obj, sa, buf.data(), static_cast<size_t>(size - sa));
          if (rc) return rc;
          rc = write_regular(obj, sa, buf.data(), sectorsize, csum, &nb);
          if (rc) return rc;
        }
      }
    }
    rc = inode_get(obj, &iv);
    if (rc) return rc;
    put64(&iv[16], size);
    rc = apply_nbytes(iv, nb);
    if (rc) return rc;
    i_touch(iv, true);
    return inode_put(obj, iv);
  }

  // inode とその項目 (extent・xattr など) を全て消す。データ extent の参照を落とす
  int inode_delete(u64 obj) {
    for (int guard = 0; guard < (1 << 26); guard++) {
      Key k; std::vector<u8> v;
      int rc = find_ge(kFsTreeObj, Key{obj, 0, 0}, &k, &v);
      if (rc == -ENOENT) return 0;
      if (rc) return rc;
      if (k.obj != obj) return 0;
      if (k.type == kExtentData) {
        FileExt e;
        rc = parse_fe(k, v, &e);
        if (rc) return rc;
        if (e.type != 0 && e.disk != 0) queue_data(R_DEC_DATA, e.disk, e.dlen, kFsTreeObj, obj, e.start - e.eoff);
      }
      rc = del(kFsTreeObj, k);
      if (rc) return rc;
    }
    return -EIO;
  }

  // ---- 公開操作の本体 (mu は取得済み)
  int check_dir(u64 dir, std::vector<u8>* iv) {
    if (dir >> 48) return -EOPNOTSUPP;
    int rc = inode_get(dir, iv);
    if (rc) return rc == -ENOENT ? -ENOENT : rc;
    if ((i_mode(*iv) & 0170000) != 0040000) return -ENOTDIR;
    return 0;
  }
  u64 inherit_flags(const std::vector<u8>& dir_iv) { return i_flags(dir_iv) & 3; }

  int new_node_in_dir(u64 dir, const std::string& name, u32 mode, u64 size, u64 nbytes, u32 nlink, u8 ft, u64* out) {
    if (!name_ok(name)) return name.size() > 255 ? -ENAMETOOLONG : -EINVAL;
    std::vector<u8> div;
    int rc = check_dir(dir, &div);
    if (rc) return rc;
    u64 c; u8 f, lt;
    rc = dir_find(dir, name, &c, &f, &lt);
    if (rc == 0) return -EEXIST;
    if (rc != -ENOENT) return rc;
    if (free_objectid >= (1ull << 48) - 16) return -ENOSPC;
    u64 obj = free_objectid++;
    rc = mk_inode(obj, mode, nlink, size, nbytes, (mode & 0170000) == 0120000 ? 0 : inherit_flags(div));
    if (rc) return rc;
    rc = add_entry(dir, name, obj, ft);
    if (rc) return rc;
    *out = obj;
    return 0;
  }
  int op_create(u64 dir, const std::string& name, u32 mode, u64* ino) {
    u64 obj;
    int rc = new_node_in_dir(dir, name, (mode & 07777) | 0100000, 0, 0, 1, kFtReg, &obj);
    if (rc) return rc;
    *ino = obj;
    return 0;
  }
  int op_mkdir(u64 dir, const std::string& name, u32 mode, u64* ino) {
    u64 obj;
    int rc = new_node_in_dir(dir, name, (mode & 07777) | 0040000, 0, 0, 1, kFtDir, &obj);
    if (rc) return rc;
    *ino = obj;
    return 0;
  }
  int op_symlink(u64 dir, const std::string& name, const std::string& target, u64* ino) {
    if (target.empty() || target.size() > 4095 || target.size() > max_item() - 21) return -ENAMETOOLONG;
    u64 obj;
    int rc = new_node_in_dir(dir, name, 0120777, target.size(), target.size(), 1, kFtSymlink, &obj);
    if (rc) return rc;
    std::vector<u8> iv = mk_inline(reinterpret_cast<const u8*>(target.data()), target.size());
    rc = ins(kFsTreeObj, Key{obj, kExtentData, 0}, iv.data(), static_cast<u32>(iv.size()));
    if (rc) return rc;
    *ino = obj;
    return 0;
  }
  int drop_link(u64 child, bool want_dir) {
    std::vector<u8> iv;
    int rc = inode_get(child, &iv);
    if (rc) return rc == -ENOENT ? -EUCLEAN : rc;
    (void)want_dir;
    u32 nl = i_nlink(iv);
    if (nl <= 1) return inode_delete(child);
    put32(&iv[40], nl - 1);
    i_touch(iv, false);
    return inode_put(child, iv);
  }
  int op_unlink(u64 dir, const std::string& name) {
    if (!name_ok(name)) return name.size() > 255 ? -ENAMETOOLONG : -EINVAL;
    std::vector<u8> div;
    int rc = check_dir(dir, &div);
    if (rc) return rc;
    u64 child; u8 ft, lt;
    rc = dir_find(dir, name, &child, &ft, &lt);
    if (rc) return rc;
    if (lt != kInodeItem) return -EOPNOTSUPP;
    std::vector<u8> civ;
    rc = inode_get(child, &civ);
    if (rc) return rc == -ENOENT ? -EUCLEAN : rc;
    if ((i_mode(civ) & 0170000) == 0040000) return -EISDIR;
    if (has_extref(child)) return -EOPNOTSUPP;
    rc = remove_entry(dir, name, child);
    if (rc) return rc;
    return drop_link(child, false);
  }
  int op_rmdir(u64 dir, const std::string& name) {
    if (!name_ok(name)) return name.size() > 255 ? -ENAMETOOLONG : -EINVAL;
    std::vector<u8> div;
    int rc = check_dir(dir, &div);
    if (rc) return rc;
    u64 child; u8 ft, lt;
    rc = dir_find(dir, name, &child, &ft, &lt);
    if (rc) return rc;
    if (lt != kInodeItem) return -EOPNOTSUPP;
    if (child == kFirstFree) return -EBUSY;
    std::vector<u8> civ;
    rc = inode_get(child, &civ);
    if (rc) return rc == -ENOENT ? -EUCLEAN : rc;
    if ((i_mode(civ) & 0170000) != 0040000) return -ENOTDIR;
    int err;
    if (!dir_empty(child, &err)) return err ? err : -ENOTEMPTY;
    rc = remove_entry(dir, name, child);
    if (rc) return rc;
    return inode_delete(child);
  }
  int parent_of(u64 obj, u64* parent) {
    Key k;
    int rc = find_ge(kFsTreeObj, Key{obj, kInodeRef, 0}, &k, nullptr);
    if (rc) return rc == -ENOENT ? -EUCLEAN : rc;
    if (k.obj != obj || k.type != kInodeRef) return -EUCLEAN;
    *parent = k.off;
    return 0;
  }
  int op_rename(u64 od, const std::string& on, u64 nd, const std::string& nn) {
    if (!name_ok(on) || !name_ok(nn)) return (on.size() > 255 || nn.size() > 255) ? -ENAMETOOLONG : -EINVAL;
    std::vector<u8> a, b;
    int rc = check_dir(od, &a);
    if (rc) return rc;
    rc = check_dir(nd, &b);
    if (rc) return rc;
    u64 src; u8 sft, slt;
    rc = dir_find(od, on, &src, &sft, &slt);
    if (rc) return rc;
    if (slt != kInodeItem) return -EOPNOTSUPP;
    if (od == nd && on == nn) return 0;
    std::vector<u8> siv;
    rc = inode_get(src, &siv);
    if (rc) return rc == -ENOENT ? -EUCLEAN : rc;
    bool sdir = (i_mode(siv) & 0170000) == 0040000;
    if (has_extref(src)) return -EOPNOTSUPP;
    if (sdir) {   // 自分自身・子孫への移動は不可
      u64 cur = nd;
      for (int g = 0; g < 4096; g++) {
        if (cur == src) return -EINVAL;
        if (cur == kFirstFree) break;
        rc = parent_of(cur, &cur);
        if (rc) return rc;
      }
    }
    u64 dst; u8 dft, dlt;
    rc = dir_find(nd, nn, &dst, &dft, &dlt);
    if (rc && rc != -ENOENT) return rc;
    if (rc == 0) {
      if (dlt != kInodeItem) return -EOPNOTSUPP;
      if (dst == src) return 0;   // 同じ inode への別名どうし: 何もしない
      std::vector<u8> div;
      rc = inode_get(dst, &div);
      if (rc) return rc == -ENOENT ? -EUCLEAN : rc;
      bool ddir = (i_mode(div) & 0170000) == 0040000;
      if (sdir && !ddir) return -ENOTDIR;
      if (!sdir && ddir) return -EISDIR;
      if (ddir) { int err; if (!dir_empty(dst, &err)) return err ? err : -ENOTEMPTY; }
      if (has_extref(dst)) return -EOPNOTSUPP;
      rc = remove_entry(nd, nn, dst);
      if (rc) return rc;
      rc = ddir ? inode_delete(dst) : drop_link(dst, false);
      if (rc) return rc;
    }
    rc = remove_entry(od, on, src);
    if (rc) return rc;
    rc = add_entry(nd, nn, src, sft);
    if (rc) return rc;
    rc = inode_get(src, &siv);
    if (rc) return rc;
    i_touch(siv, false);
    return inode_put(src, siv);
  }
  int op_link(u64 dir, const std::string& name, u64 target) {
    if (!name_ok(name)) return name.size() > 255 ? -ENAMETOOLONG : -EINVAL;
    if (target >> 48) return -EOPNOTSUPP;
    std::vector<u8> div, tiv;
    int rc = check_dir(dir, &div);
    if (rc) return rc;
    rc = inode_get(target, &tiv);
    if (rc) return rc;
    u32 mode = i_mode(tiv);
    if ((mode & 0170000) == 0040000) return -EPERM;
    if (i_nlink(tiv) >= 65000) return -EMLINK;
    if (has_extref(target)) return -EOPNOTSUPP;
    u64 c; u8 f, lt;
    rc = dir_find(dir, name, &c, &f, &lt);
    if (rc == 0) return -EEXIST;
    if (rc != -ENOENT) return rc;
    u8 ft = ft_of_mode(mode);
    if (!ft) return -EINVAL;
    rc = add_entry(dir, name, target, ft);
    if (rc) return rc;
    rc = inode_get(target, &tiv);
    if (rc) return rc;
    put32(&tiv[40], i_nlink(tiv) + 1);
    i_touch(tiv, false);
    return inode_put(target, tiv);
  }

  // ---- 操作の共通枠 (mu 取得済み)。事前検査で失敗したものは何も変えずに返し、変更を始めた後の失敗は書き込みを諦めて読み取り専用に落とす
  // freeing: 領域を空ける操作 (削除・0 への縮小)。メタデータの COW に必要な余裕を、他の操作より少なく見積もる (満杯でも削除で回復できるように)
  template <class F> int run_op(F f, u64 data_need = 0, bool freeing = false) {
    if (aborted) return -EIO;
    if (!writable_) return -EROFS;
    int rc = begin_txn();
    if (rc) return rc;
    if (data_need && !data_room(data_need) && !pending_free.empty()) {   // 解放済みの領域はコミット後に使えるようになる
      rc = commit_txn();
      if (rc) return rc;
      rc = begin_txn();
      if (rc) return rc;
    }
    rc = reserve_meta(freeing);
    if (rc) return rc;
    if (data_need) { rc = ensure_data_space(data_need); if (rc) return rc; }
    u64 m0 = mutations;
    rc = f();
    if (rc && mutations != m0) { abort_txn(rc); return rc; }
    if (rc) return rc;
    txn_ops++;
    rc = maybe_autocommit();
    return rc;
  }

  int do_sync() {
    if (aborted) return -EIO;
    if (!writable_) return 0;
    return commit_txn();
  }

  int64_t write_all(uint64_t ino, uint64_t off, const void* buf, size_t len) {
    if (ino >> 48) return writable_ ? -EOPNOTSUPP : -EROFS;
    if (len == 0) return writable_ ? 0 : (aborted ? -EIO : -EROFS);
    if (off + len < off || off + len > (1ull << 62)) return writable_ ? -EFBIG : -EROFS;
    const u8* p = static_cast<const u8*>(buf);
    size_t done = 0;
    while (done < len) {
      size_t n = std::min<size_t>(len - done, 1u << 20);
      int rc = run_op([&] { return write_chunk(ino, off + done, p + done, n); }, n + 2 * static_cast<u64>(sectorsize));
      if (rc) return done ? static_cast<int64_t>(done) : rc;
      done += n;
    }
    return static_cast<int64_t>(done);
  }
