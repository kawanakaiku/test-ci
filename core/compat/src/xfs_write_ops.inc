// SPDX-License-Identifier: GPL-2.0-or-later
// XFS 書き込み: inode の更新・データフォーク (extent / bmbt)・ディレクトリ (sf / block / leaf)・ファイル操作。xfs_write_alloc.inc の後で Impl の中に #include される。docs/xfs-write.md
// 対応: create / mkdir / symlink / write (追記・上書き・穴あけ) の CreateOnly の範囲。削除・rename・truncate・link は未対応 (EOPNOTSUPP)。

  // ============ 時刻・inode の生データ ============
  static constexpr uint64_t kFlags2Reflink = 1u << 1, kFlags2Bigtime = 1u << 3, kFlags2Nrext64 = 1u << 4;
  static constexpr int kRetryBusy = -EDEADLK;     // 解放して間もないブロックにデータを書くことになった: 先にコミットしてからやり直す
  struct BusyExt { uint32_t ag, bno, len; };
  std::vector<BusyExt> busy;
  size_t busy_mark = 0;

  void now_ts(int64_t* sec, uint32_t* nsec) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    *sec = ts.tv_sec; *nsec = uint32_t(ts.tv_nsec);
  }
  void put_time(uint8_t* p, uint64_t flags2, int64_t sec, uint32_t nsec) {
    if (flags2 & kFlags2Bigtime) pb64(p, uint64_t(sec + 2147483648LL) * 1000000000ULL + nsec);
    else { pb32(p, uint32_t(int32_t(sec))); pb32(p + 4, nsec); }
  }

  // inode の生データ (ブロックのバッファと、その中の位置) を得る
  int ino_loc(uint64_t ino, uint64_t* blkoff, uint32_t* slot) {
    unsigned shift = agblklog + inopblog;
    if (shift >= 64 || (ino >> shift) >= agcount) return -ENOENT;
    uint64_t agbno = (ino >> inopblog) & ((1ULL << agblklog) - 1);
    uint32_t sl = uint32_t(ino & ((1ULL << inopblog) - 1));
    if (agbno >= agblocks || sl >= inopblock) return -ENOENT;
    uint64_t blk = (ino >> shift) * agblocks + agbno;
    if (blk >= dblocks) return -ENOENT;
    *blkoff = blk * bs; *slot = sl;
    return 0;
  }
  // 書き込み用に inode を開く。成功したら *ip を inode 先頭へ (bw 済み)。キャッシュは捨てる
  int ino_raw(uint64_t ino, uint8_t** ip, bool for_write = true) {
    uint64_t bo; uint32_t sl;
    int rc = ino_loc(ino, &bo, &sl); if (rc) return rc;
    WBuf* b; rc = bget(bo, bs, BLFT_DINO, &b); if (rc) return rc;
    uint8_t* base = for_write ? bw(b) : b->d.data();
    uint8_t* p = base + size_t(sl) * inodesize;
    if (be16(p) != kInodeMagic || p[4] != 3) return -EUCLEAN;
    if (be64(p + 152) != ino) return -EUCLEAN;
    *ip = p;
    if (for_write) icache.erase(ino);
    return 0;
  }
  void ino_times(uint8_t* ip, bool mtime, bool ctime, int64_t sec, uint32_t nsec) {
    uint64_t f2 = be64(ip + 120);
    if (mtime) put_time(ip + 40, f2, sec, nsec);
    if (ctime) put_time(ip + 48, f2, sec, nsec);
    pb64(ip + 104, be64(ip + 104) + 1);   // di_changecount
  }

  // ============ extent の一覧 ============
  static void ext_encode(const Ext& e, uint8_t* r) {
    uint64_t l0 = (uint64_t(e.unwritten) << 63) | (e.off << 9) | (e.fsb >> 43);
    uint64_t l1 = ((e.fsb & ((1ULL << 43) - 1)) << 21) | e.len;
    pb64(r, l0); pb64(r + 8, l1);
  }
  static constexpr uint64_t kMaxExtLen = (1u << 21) - 1;
  // 昇順を保って挿入し、論理・物理が連続する隣と併合する
  static void ext_add(std::vector<Ext>& v, Ext e) {
    size_t i = 0;
    while (i < v.size() && v[i].off < e.off) i++;
    v.insert(v.begin() + i, e);
    // 後ろと
    if (i + 1 < v.size()) {
      Ext& a = v[i]; Ext& b = v[i + 1];
      if (a.off + a.len == b.off && a.fsb + a.len == b.fsb && a.unwritten == b.unwritten && a.len + b.len <= kMaxExtLen) { a.len += b.len; v.erase(v.begin() + i + 1); }
    }
    if (i > 0) {
      Ext& a = v[i - 1]; Ext& b = v[i];
      if (a.off + a.len == b.off && a.fsb + a.len == b.fsb && a.unwritten == b.unwritten && a.len + b.len <= kMaxExtLen) { a.len += b.len; v.erase(v.begin() + i); }
    }
  }

  // bmbt の (全レベルの) ブロックの FSB を根から幅優先で集める
  int bmbt_blocks(uint64_t ino, const std::vector<uint8_t>& df, std::vector<uint64_t>* out) {
    out->clear();
    if (df.size() < 4) return -EUCLEAN;
    unsigned level = be16(df.data());
    unsigned numrecs = be16(df.data() + 2);
    size_t maxrecs = (df.size() - 4) / 16;
    if (level == 0 || level > kMaxBtreeLevel || numrecs == 0 || numrecs > maxrecs) return -EUCLEAN;
    std::vector<uint64_t> cur;
    for (unsigned i = 0; i < numrecs; i++) cur.push_back(be64(df.data() + 4 + maxrecs * 8 + size_t(i) * 8));
    const size_t hdr = kBtreeLHdrV5;
    for (unsigned lv = level; lv >= 1; lv--) {
      std::vector<uint64_t> next;
      for (uint64_t fsb : cur) {
        uint64_t b;
        if (!fsb_to_blk(fsb, &b)) return -EUCLEAN;
        out->push_back(fsb);
        if (out->size() > (1u << 22)) return -EUCLEAN;
        if (lv > 1) {
          std::vector<uint8_t> blk(bs);
          int rc = read_meta(b * bs, blk.data(), bs); if (rc) return rc;
          if (be32(blk.data()) != kBmapCrcMagic || be16(blk.data() + 4) != lv - 1) return -EUCLEAN;
          unsigned nr = be16(blk.data() + 6);
          size_t maxr = (bs - hdr) / 16;
          if (nr > maxr) return -EUCLEAN;
          for (unsigned i = 0; i < nr; i++) next.push_back(be64(blk.data() + hdr + maxr * 8 + size_t(i) * 8));
        }
      }
      cur.swap(next);
    }
    return 0;
  }

  // データフォークを extent の一覧 v から書き直す (extents 形式に収まれば extents、なければ bmbt を作り直す)。
  // 既存の bmbt のブロックは再利用し、余ったら解放、足りなければ確保する。nblocks / nextents / format も更新。data_delta = データブロック数の増減
  int store_fork(uint64_t ino, std::vector<Ext>& v, int64_t data_delta) {
    uint8_t* ip;
    int rc = ino_raw(ino, &ip); if (rc) return rc;
    const uint32_t core = 176;
    const uint32_t litino = inodesize - core;
    const uint32_t dsize = ip[82] ? uint32_t(ip[82]) * 8 : litino;
    if (dsize > litino) return -EUCLEAN;
    const uint8_t fmt = ip[5];
    uint64_t nblocks = be64(ip + 64);
    std::vector<uint64_t> old_blocks;
    if (fmt == kFmtBtree) {
      std::vector<uint8_t> df(ip + core, ip + core + dsize);
      rc = bmbt_blocks(ino, df, &old_blocks); if (rc) return rc;
    } else if (fmt != kFmtExtents) return -EUCLEAN;
    const uint64_t n = v.size();
    uint32_t ag_pref = uint32_t(ino >> (agblklog + inopblog));
    std::vector<uint64_t> pool = old_blocks;   // 再利用できるブロック
    int64_t meta_delta = -int64_t(old_blocks.size());
    uint8_t* fork = ip + core;
    std::memset(fork, 0, dsize);
    if (n * 16 <= dsize) {
      for (uint64_t i = 0; i < n; i++) ext_encode(v[i], fork + i * 16);
      ip[5] = kFmtExtents;
      for (uint64_t fsb : pool) { rc = fs_free(fsb, 1); if (rc) return rc; }
    } else {
      const size_t leaf_max = (bs - 72) / 16, node_max = (bs - 72) / 16, root_max = (dsize - 4) / 16;
      if (root_max < 2) return -ENOSPC;
      std::vector<size_t> cnt;
      cnt.push_back((n + leaf_max - 1) / leaf_max);
      while (cnt.back() > root_max) cnt.push_back((cnt.back() + node_max - 1) / node_max);
      size_t total = 0; for (size_t c : cnt) total += c;
      if (cnt.size() > 8) return -EFBIG;
      // ブロックを割り当てる (幅優先: 上位レベルから)
      std::vector<std::vector<uint64_t>> lvblocks(cnt.size());
      size_t pi = 0;
      for (size_t li = cnt.size(); li-- > 0;) {
        for (size_t j = 0; j < cnt[li]; j++) {
          uint64_t fsb;
          if (pi < pool.size()) fsb = pool[pi++];
          else {
            uint32_t got;
            rc = fs_alloc(ag_pref, ~0ULL, 1, &fsb, &got, true); if (rc) return rc;
          }
          lvblocks[li].push_back(fsb);
        }
      }
      for (size_t i = pi; i < pool.size(); i++) { rc = fs_free(pool[i], 1); if (rc) return rc; }
      meta_delta += int64_t(total);
      // 葉 → 上位へ作る
      std::vector<uint64_t> keys;   // 1 つ下のレベルの各ブロックの先頭キー (startoff)
      for (size_t li = 0; li < cnt.size(); li++) {
        std::vector<uint64_t> nkeys;
        const size_t per = li == 0 ? leaf_max : node_max;
        const size_t items = li == 0 ? n : cnt[li - 1];
        for (size_t j = 0; j < cnt[li]; j++) {
          uint64_t fsb = lvblocks[li][j];
          uint64_t bblk; if (!fsb_to_blk(fsb, &bblk)) return -EUCLEAN;
          WBuf* b = bnew(bblk * bs, bs, BLFT_BTREE);
          uint8_t* p = b->d.data();
          size_t lo = j * per, hi = std::min(items, lo + per);
          pb32(p, kBmapCrcMagic); pb16(p + 4, uint32_t(li)); pb16(p + 6, uint32_t(hi - lo));
          pb64(p + 8, j > 0 ? lvblocks[li][j - 1] : ~0ULL);
          pb64(p + 16, j + 1 < cnt[li] ? lvblocks[li][j + 1] : ~0ULL);
          pb64(p + 24, b->off / 512);
          std::memcpy(p + 40, uuid, 16);
          pb64(p + 56, ino);
          if (li == 0) {
            for (size_t i = lo; i < hi; i++) ext_encode(v[i], p + 72 + (i - lo) * 16);
            nkeys.push_back(v[lo].off);
          } else {
            size_t maxr = (bs - 72) / 16;
            for (size_t i = lo; i < hi; i++) { pb64(p + 72 + (i - lo) * 8, keys[i]); pb64(p + 72 + maxr * 8 + (i - lo) * 8, lvblocks[li - 1][i]); }
            nkeys.push_back(keys[lo]);
          }
        }
        keys.swap(nkeys);
      }
      // 根 (inode の中)
      pb16(fork, uint32_t(cnt.size())); pb16(fork + 2, uint32_t(cnt.back()));
      for (size_t i = 0; i < cnt.back(); i++) { pb64(fork + 4 + i * 8, keys[i]); pb64(fork + 4 + root_max * 8 + i * 8, lvblocks.back()[i]); }
      ip[5] = kFmtBtree;
    }
    int64_t nb = int64_t(nblocks) + data_delta + meta_delta;
    if (nb < 0) return -EUCLEAN;
    pb64(ip + 64, uint64_t(nb));
    if (be64(ip + 120) & kFlags2Nrext64) pb64(ip + 24, n); else pb32(ip + 76, uint32_t(n));
    return 0;
  }

  // ============ ディレクトリ ============
  static uint32_t ent_sz(size_t nlen) { return uint32_t((8 + 1 + nlen + 1 + 2 + 7) & ~size_t(7)); }
  static uint32_t da_hash(const uint8_t* name, size_t n) {
    uint32_t h = 0;
    auto rol = [](uint32_t x, int k) { return (x << k) | (x >> (32 - k)); };
    while (n >= 4) {
      h = (uint32_t(name[0]) << 21) ^ (uint32_t(name[1]) << 14) ^ (uint32_t(name[2]) << 7) ^ uint32_t(name[3]) ^ rol(h, 28);
      name += 4; n -= 4;
    }
    switch (n) {
      case 3: return (uint32_t(name[0]) << 14) ^ (uint32_t(name[1]) << 7) ^ uint32_t(name[2]) ^ rol(h, 21);
      case 2: return (uint32_t(name[0]) << 7) ^ uint32_t(name[1]) ^ rol(h, 14);
      case 1: return uint32_t(name[0]) ^ rol(h, 7);
      default: return h;
    }
  }
  struct DEnt { std::string name; uint64_t ino; uint8_t ft; uint32_t off; };
  struct FreeRg { uint32_t off, len; };
  static constexpr uint32_t kDataHdr = 64;

  void dir_hdr_init(uint8_t* p, uint32_t magic, uint64_t blkoff, uint64_t owner) {
    pb32(p, magic);
    pb64(p + 8, blkoff / 512);
    std::memcpy(p + 24, uuid, 16);
    pb64(p + 40, owner);
  }
  void db_put_entry(uint8_t* p, uint32_t off, uint64_t ino, const std::string& name, uint8_t ft) {
    uint32_t sz = ent_sz(name.size());
    std::memset(p + off, 0, sz);
    pb64(p + off, ino);
    p[off + 8] = uint8_t(name.size());
    std::memcpy(p + off + 9, name.data(), name.size());
    p[off + 9 + name.size()] = ft;
    pb16(p + off + sz - 2, off);
  }
  void db_put_free(uint8_t* p, uint32_t off, uint32_t len) {
    std::memset(p + off, 0, len);
    pb16(p + off, 0xFFFF); pb16(p + off + 2, len); pb16(p + off + len - 2, off);
  }
  // [kDataHdr, scan_end) を走査して生きたエントリと空き領域を集める
  int db_scan(const uint8_t* p, uint32_t scan_end, std::vector<DEnt>* ents, std::vector<FreeRg>* fr) {
    uint32_t o = kDataHdr;
    while (o < scan_end) {
      if (scan_end - o < 8) return -EUCLEAN;
      if (be16(p + o) == 0xFFFF) {
        uint32_t len = be16(p + o + 2);
        if (len < 8 || (len & 7) || len > scan_end - o) return -EUCLEAN;
        if (fr) fr->push_back({o, len});
        o += len;
        continue;
      }
      size_t nlen = p[o + 8];
      uint32_t sz = ent_sz(nlen);
      if (nlen == 0 || sz > scan_end - o) return -EUCLEAN;
      if (ents) ents->push_back({std::string(reinterpret_cast<const char*>(p + o + 9), nlen), be64(p + o), p[o + 9 + nlen], o});
      o += sz;
    }
    return 0;
  }
  // 空き領域を併合して書き直し (scan_end → new_end まで広げる)、bestfree を更新する
  int db_normalize(uint8_t* p, uint32_t scan_end, uint32_t new_end) {
    std::vector<DEnt> ents; std::vector<FreeRg> fr;
    int rc = db_scan(p, scan_end, &ents, &fr); if (rc) return rc;
    // 生きたエントリの間を空きにする
    std::vector<FreeRg> out;
    uint32_t cur = kDataHdr;
    for (auto& e : ents) {
      if (e.off > cur) out.push_back({cur, e.off - cur});
      cur = e.off + ent_sz(e.name.size());
    }
    if (new_end > cur) out.push_back({cur, new_end - cur});
    for (auto& f : out) db_put_free(p, f.off, f.len);
    db_set_bestfree(p, out);
    return 0;
  }
  void db_set_bestfree(uint8_t* p, const std::vector<FreeRg>& fr) {
    FreeRg best[3] = {{0, 0}, {0, 0}, {0, 0}};
    for (auto& f : fr) {
      if (f.len > best[0].len) { best[2] = best[1]; best[1] = best[0]; best[0] = f; }
      else if (f.len > best[1].len) { best[2] = best[1]; best[1] = f; }
      else if (f.len > best[2].len) best[2] = f;
    }
    for (int i = 0; i < 3; i++) { pb16(p + 48 + i * 4, best[i].off); pb16(p + 50 + i * 4, best[i].len); }
  }

  // ディレクトリブロック (論理ブロック lb) のバッファ
  int dir_buf(Inode* d, uint64_t lb, uint8_t blft, WBuf** b) {
    int rc = load_extents(d); if (rc) return rc;
    const Ext* e = find_ext(*d->exts, lb);
    if (!e || e->unwritten) return -EUCLEAN;
    uint64_t blk;
    if (!fsb_to_blk(e->fsb + (lb - e->off), &blk)) return -EUCLEAN;
    return bget(blk * bs, bs, blft, b);
  }

  // ---- sf ----
  struct SfDir { uint64_t parent = 0; std::vector<DEnt> ents; };
  int sf_decode(Inode* d, SfDir* out) {
    const std::vector<uint8_t>& df = d->dfork;
    size_t size = size_t(d->size);
    if (size > df.size() || size < 6) return -EUCLEAN;
    unsigned count = df[0], i8 = df[1];
    size_t isz = i8 ? 8 : 4, o = 2;
    out->parent = isz == 8 ? be64(&df[o]) : be32(&df[o]);
    o += isz;
    for (unsigned i = 0; i < count; i++) {
      if (o + 3 > size) return -EUCLEAN;
      size_t nlen = df[o];
      if (nlen == 0 || o + 3 + nlen + 1 + isz > size) return -EUCLEAN;
      DEnt e;
      e.off = be16(&df[o + 1]);
      e.name.assign(reinterpret_cast<const char*>(&df[o + 3]), nlen);
      e.ft = df[o + 3 + nlen];
      e.ino = isz == 8 ? be64(&df[o + 4 + nlen]) : be32(&df[o + 4 + nlen]);
      out->ents.push_back(e);
      o += 3 + nlen + 1 + isz;
    }
    return 0;
  }
  std::vector<uint8_t> sf_encode(const SfDir& s) {
    bool i8 = s.parent > 0xFFFFFFFFULL;
    for (auto& e : s.ents) if (e.ino > 0xFFFFFFFFULL) i8 = true;
    size_t isz = i8 ? 8 : 4;
    std::vector<uint8_t> v(2 + isz, 0);
    v[0] = uint8_t(s.ents.size());
    v[1] = i8 ? uint8_t(s.ents.size()) : 0;
    if (i8) pb64(&v[2], s.parent); else pb32(&v[2], uint32_t(s.parent));
    for (auto& e : s.ents) {
      size_t o = v.size();
      v.resize(o + 3 + e.name.size() + 1 + isz);
      v[o] = uint8_t(e.name.size());
      pb16(&v[o + 1], e.off);
      std::memcpy(&v[o + 3], e.name.data(), e.name.size());
      v[o + 3 + e.name.size()] = e.ft;
      if (i8) pb64(&v[o + 4 + e.name.size()], e.ino); else pb32(&v[o + 4 + e.name.size()], uint32_t(e.ino));
    }
    return v;
  }

  // block 形式のブロックを (再) 構築: 全エントリを詰めて並べ、末尾に葉 + tail。収まらなければ false
  bool block_build(uint64_t dino, uint8_t* p, uint64_t blkoff, const std::vector<DEnt>& ents) {
    uint64_t need = kDataHdr + 8 + 8 * uint64_t(ents.size());
    for (auto& e : ents) need += ent_sz(e.name.size());
    if (need > bs) return false;
    std::memset(p, 0, bs);
    dir_hdr_init(p, kDir3Block, blkoff, dino);
    uint32_t o = kDataHdr;
    struct L { uint32_t hash, addr; };
    std::vector<L> leaf;
    for (auto& e : ents) {
      db_put_entry(p, o, e.ino, e.name, e.ft);
      leaf.push_back({da_hash(reinterpret_cast<const uint8_t*>(e.name.data()), e.name.size()), o >> 3});
      o += ent_sz(e.name.size());
    }
    uint32_t leafstart = bs - 8 - 8 * uint32_t(leaf.size());
    std::vector<FreeRg> fr;
    if (leafstart > o) { db_put_free(p, o, leafstart - o); fr.push_back({o, leafstart - o}); }
    db_set_bestfree(p, fr);
    std::stable_sort(leaf.begin(), leaf.end(), [](const L& a, const L& b) { return a.hash < b.hash; });
    for (size_t i = 0; i < leaf.size(); i++) { pb32(p + leafstart + i * 8, leaf[i].hash); pb32(p + leafstart + i * 8 + 4, leaf[i].addr); }
    pb32(p + bs - 8, uint32_t(leaf.size())); pb32(p + bs - 4, 0);
    return true;
  }

  // leaf1 ブロックの構築: ents = (hash, address)、bests = 各データブロックの最大の空き (なければ 0xFFFF)
  bool leaf1_build(uint64_t dino, uint8_t* p, uint64_t blkoff, std::vector<std::pair<uint32_t, uint32_t>>& ents, const std::vector<uint16_t>& bests) {
    if (64 + 8 * ents.size() + 2 * bests.size() + 4 > bs) return false;
    std::memset(p, 0, bs);
    pb16(p + 8, 0x3df1);
    pb64(p + 16, blkoff / 512);
    std::memcpy(p + 32, uuid, 16);
    pb64(p + 48, dino);
    pb16(p + 56, uint32_t(ents.size())); pb16(p + 58, 0);
    for (size_t i = 0; i < ents.size(); i++) { pb32(p + 64 + i * 8, ents[i].first); pb32(p + 68 + i * 8, ents[i].second); }
    pb32(p + bs - 4, uint32_t(bests.size()));
    for (size_t i = 0; i < bests.size(); i++) pb16(p + bs - 4 - 2 * bests.size() + 2 * i, bests[i]);
    return true;
  }

  uint64_t leaf_lblk() const { return kDirLeafOffsetBytes / bs; }
  // ディレクトリの inode (書き込み用) へ extents / size を反映する共通処理: ディレクトリに新しいブロックを論理位置 lb に足す
  int dir_add_block(Inode* d0, uint64_t lb, uint8_t blft, WBuf** out, uint64_t* fsb_out = nullptr) {
    std::shared_ptr<Inode> hold;
    int rc = get_inode(d0->ino, &hold); if (rc) return rc;
    Inode* d = hold.get();
    rc = load_extents(d); if (rc) return rc;
    uint64_t fsb; uint32_t got;
    uint32_t ag = uint32_t(d->ino >> (agblklog + inopblog));
    uint64_t hint = ~0ULL;
    if (!d->exts->empty()) hint = d->exts->back().fsb + d->exts->back().len;
    rc = fs_alloc(ag, hint, 1, &fsb, &got, true); if (rc) return rc;
    uint64_t blk; if (!fsb_to_blk(fsb, &blk)) return -EUCLEAN;
    std::vector<Ext> v = *d->exts;
    ext_add(v, Ext{lb, fsb, 1, false});
    rc = store_fork(d->ino, v, 1); if (rc) return rc;
    *out = bnew(blk * bs, bs, blft);
    if (fsb_out) *fsb_out = fsb;
    return 0;
  }

  int dir_set_size(uint64_t dino, uint64_t size) {
    uint8_t* ip; int rc = ino_raw(dino, &ip); if (rc) return rc;
    pb64(ip + 56, size);
    return 0;
  }

  // 親ディレクトリ d に (name → ino, ft) を追加する
  int dir_add(uint64_t dino, const std::string& name, uint64_t ino, uint8_t ft) {
    std::shared_ptr<Inode> din;
    int rc = get_inode(dino, &din); if (rc) return rc;
    Inode* d = din.get();
    if ((d->mode & S_FMT) != S_DIR) return -ENOTDIR;
    const uint32_t sz = ent_sz(name.size());
    uint8_t* ip;
    if (d->format == kFmtLocal) {
      SfDir sf; rc = sf_decode(d, &sf); if (rc) return rc;
      uint32_t next = 0x60;
      for (auto& e : sf.ents) next = std::max(next, e.off + ent_sz(e.name.size()));
      sf.ents.push_back({name, ino, ft, next});
      std::vector<uint8_t> enc = sf_encode(sf);
      rc = ino_raw(dino, &ip); if (rc) return rc;
      uint32_t dsize = ip[82] ? uint32_t(ip[82]) * 8 : inodesize - 176;
      if (enc.size() <= dsize) {
        std::memset(ip + 176, 0, dsize);
        std::memcpy(ip + 176, enc.data(), enc.size());
        pb64(ip + 56, enc.size());
        return 0;
      }
      // block 形式へ変換 (. と .. と全エントリを詰めて 1 ブロックに)
      std::vector<DEnt> all;
      all.push_back({".", dino, 2, 0}); all.push_back({"..", sf.parent, 2, 0});
      for (auto& e : sf.ents) all.push_back(e);
      uint64_t fsb; uint32_t got;
      rc = fs_alloc(uint32_t(dino >> (agblklog + inopblog)), ~0ULL, 1, &fsb, &got, true); if (rc) return rc;
      uint64_t blk; if (!fsb_to_blk(fsb, &blk)) return -EUCLEAN;
      WBuf* b = bnew(blk * bs, bs, BLFT_DIR_BLOCK);
      if (!block_build(dino, b->d.data(), b->off, all)) return -EUCLEAN;
      rc = ino_raw(dino, &ip); if (rc) return rc;
      std::memset(ip + 176, 0, dsize);
      ip[5] = kFmtExtents;
      pb64(ip + 56, bs);
      pb64(ip + 64, be64(ip + 64) + 1);
      if (be64(ip + 120) & kFlags2Nrext64) pb64(ip + 24, 1); else pb32(ip + 76, 1);
      ext_encode(Ext{0, fsb, 1, false}, ip + 176);
      return 0;
    }
    rc = load_extents(d); if (rc) return rc;
    const uint64_t leaf_fb = leaf_lblk();
    const std::vector<Ext>& ev = *d->exts;
    bool has_leaf = false;
    for (auto& e : ev) if (e.off >= leaf_fb) { has_leaf = true; break; }
    if (d->size == bs && !has_leaf) {
      // block 形式
      WBuf* b; rc = dir_buf(d, 0, BLFT_DIR_BLOCK, &b); if (rc) return rc;
      const uint8_t* p = b->d.data();
      if (be32(p) != kDir3Block) return -EUCLEAN;
      uint32_t count = be32(p + bs - 8);
      if (uint64_t(count) * 8 + 8 > bs - kDataHdr) return -EUCLEAN;
      uint32_t leafstart = bs - 8 - 8 * count;
      std::vector<DEnt> ents;
      rc = db_scan(p, leafstart, &ents, nullptr); if (rc) return rc;
      std::vector<DEnt> all = ents;
      all.push_back({name, ino, ft, 0});
      std::vector<uint8_t> tmp(bs);
      if (block_build(dino, tmp.data(), b->off, all)) {
        std::memcpy(bw(b), tmp.data(), bs);
        return 0;
      }
      // leaf 形式へ変換: ブロック 0 をデータブロックにし、葉ブロックを新設
      std::vector<std::pair<uint32_t, uint32_t>> lents;
      for (uint32_t i = 0; i < count; i++) lents.push_back({be32(p + leafstart + i * 8), be32(p + leafstart + i * 8 + 4)});
      uint8_t* wp = bw(b);
      b->blft = BLFT_DIR_DATA;
      pb32(wp, kDir3Data);
      rc = db_normalize(wp, leafstart, bs); if (rc) return rc;
      uint16_t best0 = be16(wp + 50);
      WBuf* lf; rc = dir_add_block(d, leaf_fb, BLFT_DIR_LEAF1, &lf); if (rc) return rc;
      if (!leaf1_build(dino, lf->d.data(), lf->off, lents, {best0})) return -EUCLEAN;
      return dir_add_leaf(dino, name, ino, ft);
    }
    return dir_add_leaf(dino, name, ino, ft);
  }

  // leaf 形式のディレクトリ (データブロック 0..N-1 + leaf1 ブロック) にエントリを足す
  int dir_add_leaf(uint64_t dino, const std::string& name, uint64_t ino, uint8_t ft) {
    std::shared_ptr<Inode> din;
    int rc = get_inode(dino, &din); if (rc) return rc;
    Inode* d = din.get();
    rc = load_extents(d); if (rc) return rc;
    const uint64_t leaf_fb = leaf_lblk();
    if (d->size % bs || d->size == 0) return -EUCLEAN;
    uint64_t ndata = d->size / bs;
    // 葉ブロックを読む
    WBuf* lf; rc = dir_buf(d, leaf_fb, BLFT_DIR_LEAF1, &lf); if (rc) return rc;
    {
      const uint8_t* p = lf->d.data();
      if (be16(p + 8) != 0x3df1) return -EOPNOTSUPP;   // node 形式 (未対応)
    }
    std::vector<std::pair<uint32_t, uint32_t>> lents;
    std::vector<uint16_t> bests;
    {
      const uint8_t* p = lf->d.data();
      uint32_t count = be16(p + 56), stale = be16(p + 58), bc = be32(p + bs - 4);
      if (stale != 0) return -EOPNOTSUPP;
      if (64 + 8 * uint64_t(count) + 2 * uint64_t(bc) + 4 > bs || bc != ndata) return -EUCLEAN;
      for (uint32_t i = 0; i < count; i++) lents.push_back({be32(p + 64 + i * 8), be32(p + 68 + i * 8)});
      for (uint32_t i = 0; i < bc; i++) bests.push_back(be16(p + bs - 4 - 2 * bc + 2 * i));
    }
    const uint32_t sz = ent_sz(name.size());
    // 空きのあるデータブロックを探す
    int64_t target = -1;
    for (uint64_t i = 0; i < ndata; i++) if (bests[i] != 0xFFFF && bests[i] >= sz) { target = int64_t(i); break; }
    WBuf* db = nullptr;
    if (target < 0) {
      if (64 + 8 * (lents.size() + 1) + 2 * (bests.size() + 1) + 4 > bs) return -ENOSPC;   // node 形式が必要 (未対応)
      rc = dir_add_block(d, ndata, BLFT_DIR_DATA, &db); if (rc) return rc;
      uint8_t* p = db->d.data();
      dir_hdr_init(p, kDir3Data, db->off, dino);
      db_put_free(p, kDataHdr, bs - kDataHdr);
      pb16(p + 48, kDataHdr); pb16(p + 50, bs - kDataHdr);
      target = int64_t(ndata);
      bests.push_back(uint16_t(bs - kDataHdr));
      ndata++;
      rc = dir_set_size(dino, ndata * bs); if (rc) return rc;
      rc = get_inode(dino, &din); if (rc) return rc;   // extent が変わったので取り直す
      d = din.get();
    } else {
      rc = dir_buf(d, uint64_t(target), BLFT_DIR_DATA, &db); if (rc) return rc;
      if (be32(db->d.data()) != kDir3Data) return -EUCLEAN;
    }
    uint8_t* p = bw(db);
    std::vector<FreeRg> fr; rc = db_scan(p, bs, nullptr, &fr); if (rc) return rc;
    uint32_t off = 0;
    for (auto& f : fr) if (f.len >= sz) { off = f.off; db_put_free(p, f.off, f.len); if (f.len > sz) db_put_free(p, f.off + sz, f.len - sz); break; }
    if (!off) return -EUCLEAN;
    db_put_entry(p, off, ino, name, ft);
    rc = db_normalize(p, bs, bs); if (rc) return rc;
    bests[size_t(target)] = be16(p + 50);
    // 葉ブロック
    uint32_t h = da_hash(reinterpret_cast<const uint8_t*>(name.data()), name.size());
    uint32_t addr = uint32_t((uint64_t(target) * bs + off) >> 3);
    size_t pos = 0;
    while (pos < lents.size() && lents[pos].first <= h) pos++;
    lents.insert(lents.begin() + pos, {h, addr});
    WBuf* lf2; rc = dir_buf(d, leaf_fb, BLFT_DIR_LEAF1, &lf2); if (rc) return rc;
    uint8_t* lp = bw(lf2);
    if (!leaf1_build(dino, lp, lf2->off, lents, bests)) return -ENOSPC;
    return 0;
  }

  // ============ inode の作成 ============
  int ino_init(uint64_t ino, uint32_t mode, uint32_t uid, uint32_t gid, uint32_t nlink, uint8_t format) {
    uint8_t* ip; int rc = ino_raw(ino, &ip); if (rc) return rc;
    uint32_t gen = be32(ip + 92) + 1;
    std::memset(ip, 0, inodesize);
    int64_t sec; uint32_t nsec; now_ts(&sec, &nsec);
    uint64_t f2 = (bigtime ? kFlags2Bigtime : 0) | (nrext64 ? kFlags2Nrext64 : 0);
    pb16(ip, kInodeMagic); pb16(ip + 2, mode); ip[4] = 3; ip[5] = format;
    pb32(ip + 8, uid); pb32(ip + 12, gid); pb32(ip + 16, nlink);
    put_time(ip + 32, f2, sec, nsec); put_time(ip + 40, f2, sec, nsec); put_time(ip + 48, f2, sec, nsec);
    ip[83] = kFmtExtents;   // 属性フォークなし
    pb32(ip + 92, gen); pb32(ip + 96, 0xFFFFFFFFu);
    pb64(ip + 104, 1);
    pb64(ip + 120, f2);
    put_time(ip + 144, f2, sec, nsec);
    pb64(ip + 152, ino);
    std::memcpy(ip + 160, uuid, 16);
    return 0;
  }

  static bool name_ok(const std::string& n) {
    if (n.empty() || n.size() > 255 || n == "." || n == "..") return false;
    for (char c : n) if (c == '/' || c == '\0') return false;
    return true;
  }
  int dir_find(Inode* d, const std::string& name, uint64_t* ino) {
    if (d->format != kFmtLocal) {
      int mrc = 0;
      std::shared_ptr<DirMap> dm = get_dirmap(d, &mrc);
      if (mrc) return mrc;
      if (dm) { auto it = dm->m.find(name); if (it == dm->m.end()) return -ENOENT; *ino = it->second; return 0; }
    }
    uint64_t found = 0; bool hit = false;
    int rc = walk_dir(d, true, [&](const Ent& e) {
      if (e.nlen == name.size() && std::memcmp(e.name, name.data(), e.nlen) == 0) { found = e.ino; hit = true; return true; }
      return false;
    });
    if (rc) return rc;
    if (!hit) return -ENOENT;
    *ino = found;
    return 0;
  }

  // 親 dir に新しい inode を作って名前を付ける (共通)。kind: 1 = ファイル / 2 = ディレクトリ / 7 = シンボリックリンク
  int new_node(uint64_t dir, const std::string& name, uint32_t mode, int kind, const std::string* target, uint64_t* out) {
    if (!name_ok(name)) return name.size() > 255 ? -ENAMETOOLONG : -EINVAL;
    std::shared_ptr<Inode> parent;
    int rc = get_inode(dir, &parent); if (rc) return rc;
    if ((parent->mode & S_FMT) != S_DIR) return -ENOTDIR;
    if (parent->nlink == 0) return -ENOENT;
    uint64_t ex;
    rc = dir_find(parent.get(), name, &ex);
    if (rc == 0) return -EEXIST;
    if (rc != -ENOENT) return rc;
    uint32_t uid, gid;
    { uint8_t* pip; rc = ino_raw(dir, &pip, false); if (rc) return rc; uid = be32(pip + 8); gid = be32(pip + 12); }
    uint32_t ptype = kind == 1 ? S_REG : kind == 2 ? S_DIR : S_LNK;
    uint32_t perm = kind == 7 ? 0777 : (mode & 07777);
    uint64_t ino;
    rc = ino_alloc(uint32_t(dir >> (agblklog + inopblog)), &ino); if (rc) return rc;
    uint8_t ft = uint8_t(kind);
    if (kind == 1) { rc = ino_init(ino, ptype | perm, uid, gid, 1, kFmtExtents); if (rc) return rc; }
    else if (kind == 2) {
      rc = ino_init(ino, ptype | perm, uid, gid, 2, kFmtLocal); if (rc) return rc;
      uint8_t* ip; rc = ino_raw(ino, &ip); if (rc) return rc;
      SfDir sf; sf.parent = dir;
      std::vector<uint8_t> enc = sf_encode(sf);
      std::memcpy(ip + 176, enc.data(), enc.size());
      pb64(ip + 56, enc.size());
    } else {
      rc = ino_symlink_body(ino, ptype | perm, uid, gid, *target); if (rc) return rc;
    }
    rc = dir_add(dir, name, ino, ft); if (rc) return rc;
    // 親の更新: mtime / ctime、サブディレクトリなら nlink + 1
    {
      uint8_t* pip; rc = ino_raw(dir, &pip); if (rc) return rc;
      int64_t sec; uint32_t nsec; now_ts(&sec, &nsec);
      ino_times(pip, true, true, sec, nsec);
      if (kind == 2) pb32(pip + 16, be32(pip + 16) + 1);
    }
    for (auto& dm : dirmaps) if (dm->ino == dir) dm->m[name] = ino;
    icache.erase(dir);
    *out = ino;
    return 0;
  }

  // シンボリックリンクの本体: 短ければ local、長ければ remote (XSLM ブロック)
  int ino_symlink_body(uint64_t ino, uint32_t mode, uint32_t uid, uint32_t gid, const std::string& target) {
    if (target.empty() || target.size() > kMaxSymlink) return -ENAMETOOLONG;
    for (char c : target) if (c == '\0') return -EINVAL;
    const uint32_t litino = inodesize - 176;
    int rc;
    if (target.size() <= litino) {
      rc = ino_init(ino, mode, uid, gid, 1, kFmtLocal); if (rc) return rc;
      uint8_t* ip; rc = ino_raw(ino, &ip); if (rc) return rc;
      std::memcpy(ip + 176, target.data(), target.size());
      pb64(ip + 56, target.size());
      return 0;
    }
    rc = ino_init(ino, mode, uid, gid, 1, kFmtExtents); if (rc) return rc;
    const size_t per = bs - kSymlinkHdr;
    uint32_t nb = uint32_t((target.size() + per - 1) / per);
    std::vector<Ext> v;
    size_t done = 0; uint32_t lb = 0;
    uint32_t ag = uint32_t(ino >> (agblklog + inopblog));
    while (lb < nb) {
      uint64_t fsb; uint32_t got;
      rc = fs_alloc(ag, ~0ULL, nb - lb, &fsb, &got, true); if (rc) return rc;
      for (uint32_t k = 0; k < got; k++, lb++) {
        uint64_t blk; if (!fsb_to_blk(fsb + k, &blk)) return -EUCLEAN;
        WBuf* b = bnew(blk * bs, bs, BLFT_SYMLINK);
        uint8_t* p = b->d.data();
        size_t take = std::min(per, target.size() - done);
        pb32(p, kSymlinkMagic); pb32(p + 4, uint32_t(done)); pb32(p + 8, uint32_t(take));
        std::memcpy(p + 16, uuid, 16);
        pb64(p + 32, ino); pb64(p + 40, b->off / 512);
        std::memcpy(p + kSymlinkHdr, target.data() + done, take);
        done += take;
      }
      ext_add(v, Ext{lb - got, fsb, got, false});
    }
    rc = store_fork(ino, v, nb); if (rc) return rc;
    uint8_t* ip; rc = ino_raw(ino, &ip); if (rc) return rc;
    pb64(ip + 56, target.size());
    return 0;
  }

  // ============ ファイルの書き込み ============
  int write_file(uint64_t ino, uint64_t off, const uint8_t* buf, size_t len, bool inplace_only, int64_t* written) {
    *written = 0;
    std::shared_ptr<Inode> in;
    int rc = get_inode(ino, &in); if (rc) return rc;
    if ((in->mode & S_FMT) == S_DIR) return -EISDIR;
    if ((in->mode & S_FMT) != S_REG) return -EINVAL;
    if (in->flags & 1) return -EOPNOTSUPP;   // リアルタイム
    if (len == 0) return 0;
    if (off + len < off || off + len > (1ULL << 62) / 2) return -EFBIG;
    if (inplace_only && off + len > in->size) return -EINVAL;
    rc = load_extents(in.get()); if (rc) return rc;
    std::vector<Ext> v = *in->exts;
    const uint64_t old_size = in->size;
    const bool reflink = (in->flags2 & kFlags2Reflink) != 0;
    const uint64_t fb0 = off / bs, fb1 = (off + len - 1) / bs;   // 含む範囲
    uint32_t ag = uint32_t(ino >> (agblklog + inopblog));
    int64_t new_blocks = 0;
    std::vector<uint8_t> blk(bs);
    // EOF をまたぐ穴のゼロ埋め: 旧 EOF が最終ブロックの途中で、そこから off までが同じブロック内
    if (off > old_size && (old_size % bs) != 0) {
      uint64_t lb = old_size / bs;
      const Ext* e = find_ext(v, lb);
      if (e && !e->unwritten) {
        uint64_t zend = std::min<uint64_t>(off, (lb + 1) * bs);
        uint64_t b; if (!fsb_to_blk(e->fsb + (lb - e->off), &b)) return -EUCLEAN;
        if (reflink) return -EOPNOTSUPP;
        std::vector<uint8_t> z(size_t(zend - old_size), 0);
        rc = dev_write_data(b * bs + (old_size % bs), z.data(), z.size()); if (rc) return rc;
      }
    }
    uint64_t fb = fb0;
    while (fb <= fb1) {
      const Ext* e = find_ext(v, fb);
      if (e) {
        if (e->unwritten) return -EOPNOTSUPP;
        if (reflink) return -EOPNOTSUPP;
        uint64_t upto = std::min<uint64_t>(fb1 + 1, e->off + e->len);
        uint64_t b; if (!fsb_to_blk(e->fsb + (fb - e->off), &b)) return -EUCLEAN;
        uint64_t pos = std::max<uint64_t>(off, fb * bs), end = std::min<uint64_t>(off + len, upto * bs);
        rc = dev_write_data(b * bs + (pos - fb * bs), buf + (pos - off), size_t(end - pos)); if (rc) return rc;
        fb = upto;
        continue;
      }
      if (inplace_only) return -EINVAL;
      uint64_t hole_end = std::min<uint64_t>(fb1 + 1, next_ext_start(v, fb));
      uint64_t want = hole_end - fb;
      while (want) {
        uint64_t hint = ~0ULL;
        // 直前の extent の直後を優先 (連続させる)
        if (fb > 0) { const Ext* prev = find_ext(v, fb - 1); if (prev && !prev->unwritten) hint = prev->fsb + (fb - prev->off); }
        uint64_t fsb; uint32_t got;
        rc = fs_alloc(ag, hint, uint32_t(std::min<uint64_t>(want, 1u << 20)), &fsb, &got); if (rc) return rc;
        { uint32_t a = uint32_t(fsb >> agblklog), bn = uint32_t(fsb & ((1ULL << agblklog) - 1));
          for (auto& x : busy) if (x.ag == a && bn < x.bno + x.len && x.bno < bn + got) return kRetryBusy; }
        // 新しいブロックの内容: 範囲外はゼロ
        for (uint32_t k = 0; k < got; k++) {
          uint64_t lb = fb + k;
          uint64_t b; if (!fsb_to_blk(fsb + k, &b)) return -EUCLEAN;
          std::fill(blk.begin(), blk.end(), 0);
          uint64_t lo = std::max<uint64_t>(off, lb * bs), hi = std::min<uint64_t>(off + len, (lb + 1) * bs);
          std::memcpy(blk.data() + (lo - lb * bs), buf + (lo - off), size_t(hi - lo));
          rc = dev_write_data(b * bs, blk.data(), bs); if (rc) return rc;
        }
        ext_add(v, Ext{fb, fsb, got, false});
        new_blocks += got;
        fb += got; want -= got;
      }
    }
    uint64_t new_size = std::max<uint64_t>(old_size, off + len);
    if (new_blocks) { rc = store_fork(ino, v, new_blocks); if (rc) return rc; }
    uint8_t* ip; rc = ino_raw(ino, &ip); if (rc) return rc;
    if (new_size != old_size) pb64(ip + 56, new_size);
    int64_t sec; uint32_t nsec; now_ts(&sec, &nsec);
    ino_times(ip, true, true, sec, nsec);
    *written = int64_t(len);
    return 0;
  }
  // データを書く (範囲はファイルシステム内であること)
  int dev_write_data(uint64_t off, const uint8_t* p, size_t n) {
    if (!range_ok(dev_size, off, n)) return -EIO;
#ifdef USBMEM_MUTATE_DATA_AFTER
    // 突然変異: データをコミットの後に書く (コミット済みのメタデータが、まだ書かれていないデータを指す窓ができる)
    deferred_data.push_back({off, std::vector<uint8_t>(p, p + n)});
    return 0;
#endif
    while (n) {
      size_t c = std::min<size_t>(n, 1u << 20);
      Status st = dev->write_at(off, p, c);
      if (!st.ok()) { fail_rw("データ書き込みエラー"); return -EIO; }
      off += c; p += c; n -= c;
    }
    return 0;
  }

  // ============ 操作の実行 (巻き戻し・コミット) ============
  template <class F>
  int run_op(F&& f) {
    if (!rw) return failed ? -EIO : -EROFS;
    for (int attempt = 0; attempt < 2; attempt++) {
      busy_mark = busy.size();
      op_begin();
      int rc = f();
      if (rc == kRetryBusy) { op_abort(); busy.resize(busy_mark); int c = commit(); busy.clear(); if (c) return c; continue; }
      if (rc < 0) { op_abort(); busy.resize(busy_mark); return rc; }
      op_end();
      int c = maybe_commit();
      if (c == 0 && wb.empty()) busy.clear();
      return c ? c : rc;
    }
    return -ENOSPC;
  }
