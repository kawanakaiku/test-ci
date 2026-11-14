// SPDX-License-Identifier: GPL-2.0-or-later
// XFS 書き込み基盤 (xfs_volume.cpp の struct XfsVolume::Impl の中に #include される。USBMEM_NTFS_WRITE のビルドだけ)。docs/xfs-write.md
//
// 方針: メタデータの更新はすべて「バッファ (ブロック / セクタ単位の完全なイメージ)」をメモリで書き換え、コミットのときに
//  (1) 変更のあった 128 バイト単位の塊 (XFS_BLF_CHUNK) だけを XFS のログ (バッファログ項目 XFS_LI_BUF) に書く  → flush
//  (2) 別レコードでトランザクションのコミット操作を書く                                                        → flush
//  (3) 変更を本来の場所に書く                                                                                → flush
//  (4) アンマウントレコードを書いてログをクリーンに戻す (flush は次回の先頭 flush か close で)
// の順に進める。(2) の後で (4) の前に電源が切れても、Linux のカーネルが同じログを再生して (3) と同じ結果になる (再生はバッファの上書きなので冪等)。
// (1)(2) の間で切れればコミットされていないトランザクションとして捨てられ、(3) の最中なら再生で補われる。
// データブロックは新しく確保した領域にコミットより前に書く (どのメタデータからも指されていないので、切れても無害)。
// 書き込み可にするのは prepare_rw() のホワイトリストを満たしたときだけ。満たさなければ読み取り専用 (ro_reason に理由)。

  // ============ 状態 ============
  bool rw = false, failed = false;
  std::string ro_reason;
  uint8_t uuid[16] = {0};
  bool bigtime = false, nrext64 = false, has_finobt = false, has_inobtcount = false, has_spino = false, has_reflink = false;
  uint32_t imax_pct = 0, inoalignmt = 0, spino_align = 0;
  uint32_t agfl_slots = 0, blocks_per_cluster_unused = 0;
  uint64_t sb_icount = 0, sb_ifree = 0, sb_fdblocks = 0;
  // ログの位置 (log_state が確定する)
  bool lg_ok = false;
  uint64_t lg_head = 0, lg_rhead = 0, lg_prev = 0;
  uint32_t lg_cycle = 0;
  uint32_t tid_ctr = 0x1000;
  bool unmount_pending_flush = false;

  static constexpr uint8_t BLFT_BTREE = 4, BLFT_AGF = 5, BLFT_AGFL = 6, BLFT_AGI = 7, BLFT_DINO = 8, BLFT_SYMLINK = 9, BLFT_DIR_BLOCK = 10,
                           BLFT_DIR_DATA = 11, BLFT_DIR_FREE = 12, BLFT_DIR_LEAF1 = 13, BLFT_DIR_LEAFN = 14, BLFT_DA_NODE = 15, BLFT_SB = 18;
  static constexpr uint32_t kChunk = 128, kMaxRecBytes = 32768;

  struct WBuf {
    uint64_t off = 0;
    uint32_t len = 0;
    uint8_t blft = 0;
    bool fresh = false, dirty = false;
    std::vector<uint8_t> d, orig;
  };
  std::map<uint64_t, WBuf> wb;                 // キー = デバイス先頭からのバイト位置
  struct Undo { std::vector<uint8_t> d; bool dirty, fresh, existed; };
  std::map<uint64_t, Undo> undo;               // 現在の操作で最初に触ったときの内容 (失敗時に戻す)
  bool in_op = false;
  int64_t d_icount = 0, d_ifree = 0, d_fdblocks = 0;
  int64_t u_icount = 0, u_ifree = 0, u_fdblocks = 0;   // 操作の開始時点の差分 (巻き戻し用)
  size_t dirty_bytes = 0;

  // ============ 小物 ============
  static void pb16(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
  static void pb32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = uint8_t(v >> (24 - 8 * i)); }
  static void pb64(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = uint8_t(v >> (56 - 8 * i)); }
  static void ple16(uint8_t* p, uint32_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
  static void ple32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = uint8_t(v >> (8 * i)); }
  static void ple64(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = uint8_t(v >> (8 * i)); }
  static uint64_t rup(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

  // CRC フィールド (cksum_off から 4 バイト) を計算して書く
  static void set_crc(uint8_t* p, size_t len, size_t off) {
    ple32(p + off, 0);
    uint32_t c = crc_update(0xFFFFFFFFu, p, len);
    ple32(p + off, ~c);
  }

  // ============ バッファ ============
  int bget(uint64_t off, uint32_t len, uint8_t blft, WBuf** out) {
    auto it = wb.find(off);
    if (it != wb.end()) {
      if (it->second.len != len) return -EIO;   // 同じ位置を別の大きさで開いた (設計上起きない)
      *out = &it->second;
      return 0;
    }
    if (!range_ok(dev_size, off, len)) return -EIO;
    WBuf b;
    b.off = off; b.len = len; b.blft = blft;
    b.d.resize(len);
    int rc = dev_read(off, b.d.data(), len);
    if (rc) return rc;
    b.orig = b.d;
    *out = &wb.emplace(off, std::move(b)).first->second;
    return 0;
  }
  // 中身が不定の新しいバッファ (全体をログに書く)。ゼロで初期化して返す
  WBuf* bnew(uint64_t off, uint32_t len, uint8_t blft) {
    auto it = wb.find(off);
    if (in_op && !undo.count(off)) {
      Undo u;
      u.existed = it != wb.end();
      if (u.existed) { u.d = it->second.d; u.dirty = it->second.dirty; u.fresh = it->second.fresh; } else { u.dirty = false; u.fresh = false; }
      undo.emplace(off, std::move(u));
    }
    if (it == wb.end()) it = wb.emplace(off, WBuf()).first;
    WBuf& b = it->second;
    b.off = off; b.len = len; b.blft = blft; b.fresh = true; b.dirty = true;
    b.d.assign(len, 0);
    b.orig.clear();
    return &b;
  }
  // 書き換える前に呼ぶ (巻き戻し用の保存と dirty 印)。書き込み先のポインタを返す
  uint8_t* bw(WBuf* b) {
    if (in_op && !undo.count(b->off)) undo.emplace(b->off, Undo{b->d, b->dirty, b->fresh, true});
    b->dirty = true;
    return b->d.data();
  }
  // 解放したブロック [off, off+len) のバッファは、もうコミットしない (中身は参照されない。巻き戻し用に元の状態を保存する)
  void bdrop_range(uint64_t off, uint64_t len) {
    for (auto it = wb.lower_bound(off); it != wb.end() && it->first < off + len; ++it) {
      WBuf& b = it->second;
      if (!b.dirty) continue;
      if (in_op && !undo.count(b.off)) undo.emplace(b.off, Undo{b.d, b.dirty, b.fresh, true});
      b.dirty = false;
    }
  }
  void op_begin() { in_op = true; undo.clear(); u_icount = d_icount; u_ifree = d_ifree; u_fdblocks = d_fdblocks; }
  void op_end() { in_op = false; undo.clear(); }
  void op_abort() {
    for (auto& kv : undo) {
      auto it = wb.find(kv.first);
      if (it == wb.end()) continue;
      if (!kv.second.existed) { wb.erase(it); continue; }
      it->second.d = kv.second.d; it->second.dirty = kv.second.dirty; it->second.fresh = kv.second.fresh;
    }
    d_icount = u_icount; d_ifree = u_ifree; d_fdblocks = u_fdblocks;
    in_op = false; undo.clear();
    icache.clear(); dirmaps.clear();
  }

  // read_meta の上に、まだコミットしていない変更を重ねる
  void overlay(uint64_t off, uint8_t* out, size_t len) {
    if (wb.empty()) return;
    auto it = wb.upper_bound(off);
    if (it != wb.begin()) --it;
    for (; it != wb.end() && it->first < off + len; ++it) {
      WBuf& b = it->second;
      if (!b.dirty) continue;
      finalize(b);   // 読み出し側 (CRC を検証する) に、現在の内容の CRC を見せる
      uint64_t lo = std::max<uint64_t>(off, b.off), hi = std::min<uint64_t>(off + len, b.off + b.len);
      if (lo >= hi) continue;
      std::memcpy(out + (lo - off), b.d.data() + (lo - b.off), static_cast<size_t>(hi - lo));
    }
  }

  // ---- アドレス (FSB / AG 内ブロック ↔ バイト位置) ----
  uint64_t agb_off(uint32_t agno, uint64_t agbno) const { return (uint64_t(agno) * agblocks + agbno) * bs; }
  uint64_t fsb_of(uint32_t agno, uint64_t agbno) const { return (uint64_t(agno) << agblklog) | agbno; }
  uint32_t ag_len(uint32_t agno) const { return agno + 1 == agcount ? uint32_t(dblocks - uint64_t(agno) * agblocks) : agblocks; }

  // ============ CRC の仕上げ ============
  static bool is_short_btree_magic(uint32_t m) {
    return m == 0x41423342 || m == 0x41423343 || m == 0x49414233 || m == 0x46494233;
  }
  void finalize(WBuf& b) {
    uint8_t* p = b.d.data();
    switch (b.blft) {
      case BLFT_SB: set_crc(p, b.len, 224); break;
      case BLFT_AGF: set_crc(p, b.len, 216); break;
      case BLFT_AGI: set_crc(p, b.len, 312); break;
      case BLFT_AGFL: set_crc(p, b.len, 32); break;
      case BLFT_BTREE: set_crc(p, b.len, is_short_btree_magic(be32(p)) ? 52 : 64); break;
      case BLFT_DIR_BLOCK: case BLFT_DIR_DATA: case BLFT_DIR_FREE: set_crc(p, b.len, 4); break;
      case BLFT_DIR_LEAF1: case BLFT_DIR_LEAFN: case BLFT_DA_NODE: case BLFT_SYMLINK: set_crc(p, b.len, 12); break;
      case BLFT_DINO:
        for (uint32_t o = 0; o + inodesize <= b.len; o += inodesize) {
          if (!b.fresh && std::memcmp(p + o, b.orig.data() + o, inodesize) == 0) continue;
          if (be16(p + o) == kInodeMagic) set_crc(p + o, inodesize, kInodeCrcOff);
        }
        break;
      default: break;
    }
  }

  // ============ ログへの書き込み ============
  uint64_t log_nbb() const { return uint64_t(logblocks) * (bs / 512); }
  uint64_t log_base() const { uint64_t b = 0; fsb_to_blk(logstart, &b); return b * bs; }

  static void put_ophdr(std::vector<uint8_t>& v, uint32_t tid, uint32_t len, uint8_t client, uint8_t flags) {
    size_t o = v.size();
    v.resize(o + 12, 0);
    pb32(&v[o], tid); pb32(&v[o + 4], len);
    v[o + 8] = client; v[o + 9] = flags;
  }

  // 1 レコード書く。ops = 操作 (ヘッダ + 本体) の連結 (kMaxRecBytes 以下)。成功したらログの先頭位置を進める
  // tail_first = true なら tail_lsn にこのトランザクションの最初のレコードの lsn (lg_txn_lsn) を書く。カーネルは「最後のレコードの tail_lsn」から再生するので、
  // 複数レコードのトランザクションでは全レコードが同じ tail (= 先頭のレコード) を指していないと、先頭側の項目が再生されずに半端な状態になる。
  uint64_t lg_txn_lsn = 0;
#ifdef USBMEM_MUTATE_DATA_AFTER
  std::vector<std::pair<uint64_t, std::vector<uint8_t>>> deferred_data;
#endif
  int log_put(const std::vector<uint8_t>& ops, uint32_t nops, bool in_txn = false) {
    const uint64_t nbb = log_nbb();
    if (ops.size() > kMaxRecBytes) return -EINVAL;
    uint64_t dbb = (ops.size() + 511) / 512;
    if (lg_head + 1 + dbb == nbb) dbb++;   // レコードの終わりがログの終端ぴったりになる形は作らない
    if (dbb > 64 || 1 + dbb >= nbb) return -ENOSPC;
    std::vector<uint8_t> rec((1 + dbb) * 512, 0);
    std::memcpy(rec.data() + 512, ops.data(), ops.size());
    uint8_t* h = rec.data();
    uint64_t lsn = (uint64_t(lg_cycle) << 32) | lg_head;
    pb32(h, kLogMagic); pb32(h + 4, lg_cycle); pb32(h + 8, 2);
    pb32(h + 12, uint32_t(dbb * 512));
    pb64(h + 16, lsn);
    // tail_lsn = このトランザクションの先頭 (unmount レコードは自分): これより前は全て反映済み
#ifdef USBMEM_MUTATE_TAIL_OWN
    pb64(h + 24, lsn);   // 突然変異: tail を自分の先頭にする (複数レコードのトランザクションで先頭側が再生されない)
#else
    pb64(h + 24, in_txn ? lg_txn_lsn : lsn);
#endif
    pb32(h + 36, uint32_t(lg_prev)); pb32(h + 40, nops);
    for (uint64_t i = 0; i < dbb; i++) {
      uint8_t* p = rec.data() + 512 + i * 512;
      if (i < 64) std::memcpy(h + 44 + i * 4, p, 4);
      uint32_t cyc = lg_cycle + ((lg_head + 1 + i) >= nbb ? 1 : 0);
      pb32(p, cyc);
    }
    pb32(h + 300, 1);                              // XLOG_FMT_LINUX_LE
    std::memcpy(h + 304, uuid, 16);
    pb32(h + 320, kMaxRecBytes);                   // h_size
    uint32_t c = crc_update(0xFFFFFFFFu, h, 328);  // crc フィールドは 0 のまま
    c = crc_update(c, rec.data() + 512, size_t(dbb * 512));
    ple32(h + 32, ~c);
    // 書く (終端をまたぐなら 2 回)
    const uint64_t base = log_base();
    size_t done = 0, total = rec.size() / 512;
    uint64_t pos = lg_head;
    while (done < total) {
      uint64_t run = std::min<uint64_t>(total - done, nbb - pos);
      Status st = dev->write_at(base + pos * 512, rec.data() + done * 512, size_t(run * 512));
      if (!st.ok()) return -EIO;
      done += size_t(run); pos += run;
      if (pos >= nbb) pos -= nbb;
    }
    lg_prev = lg_head;
    uint64_t nh = lg_head + 1 + dbb;
    if (nh >= nbb) { nh -= nbb; lg_cycle++; }
    lg_head = nh;
    return 0;
  }
  int dev_flush() { return dev->flush().ok() ? 0 : -EIO; }

  int log_unmount_record() {
    std::vector<uint8_t> ops;
    put_ophdr(ops, ++tid_ctr, 8, 0xaa, 0x20);
    ops.resize(ops.size() + 8, 0);
    ple16(&ops[12], 0x556e);
    return log_put(ops, 1);
  }

  // ============ コミット ============
  struct LogItem { WBuf* b; std::vector<std::pair<uint32_t, uint32_t>> runs; uint32_t map_words; };

  // 変更の塊 (kChunk バイト単位) を求める
  void chunk_runs(const WBuf& b, std::vector<std::pair<uint32_t, uint32_t>>* runs) const {
    runs->clear();
    uint32_t n = b.len / kChunk;
    int cur = -1;
    for (uint32_t i = 0; i < n; i++) {
      bool dirty = b.fresh || std::memcmp(b.d.data() + size_t(i) * kChunk, b.orig.data() + size_t(i) * kChunk, kChunk) != 0;
      if (dirty && cur < 0) cur = int(i);
      if (!dirty && cur >= 0) { runs->push_back({uint32_t(cur), i}); cur = -1; }
    }
    if (cur >= 0) runs->push_back({uint32_t(cur), n});
  }

  void apply_sb_deltas() {
    if (!d_icount && !d_ifree && !d_fdblocks) return;
    WBuf* sbb = nullptr;
    if (bget(0, sectsize, BLFT_SB, &sbb)) { failed = true; return; }
    uint8_t* p = bw(sbb);
    pb64(p + 128, uint64_t(int64_t(be64(p + 128)) + d_icount));
    pb64(p + 136, uint64_t(int64_t(be64(p + 136)) + d_ifree));
    pb64(p + 144, uint64_t(int64_t(be64(p + 144)) + d_fdblocks));
    d_icount = d_ifree = d_fdblocks = 0;
  }

  // 溜まっている変更をログ経由でコミットする。0 = 成功 (または変更なし)
  int commit() {
    if (failed) return -EIO;
    apply_sb_deltas();
    if (failed) return -EIO;
    std::vector<LogItem> items;
    uint64_t log_bytes = 0;
    for (auto& kv : wb) {
      WBuf& b = kv.second;
      if (!b.dirty) continue;
#ifndef USBMEM_MUTATE_NO_CRC
      finalize(b);
#endif
      LogItem it; it.b = &b;
      chunk_runs(b, &it.runs);
      if (it.runs.empty()) { b.dirty = false; continue; }
      it.map_words = (b.len / kChunk + 31) / 32;
      for (auto& r : it.runs) log_bytes += 12 + uint64_t(r.second - r.first) * kChunk;
      log_bytes += 12 + 20 + 4 * it.map_words;
      items.push_back(std::move(it));
    }
    if (items.empty()) { wb.clear(); return 0; }
    // ログに収まるか (開始 / コミット / アンマウントのレコードと余裕を含む)
    if (log_bytes / 512 + items.size() / 8 + 16 >= log_nbb() / 2) { fail_rw("ログが小さすぎてコミットできない"); return -ENOSPC; }
    if (!lg_ok) { fail_rw("ログの位置が不明"); return -EIO; }
    const uint32_t tid = ++tid_ctr;
    std::vector<uint8_t> ops;
    uint32_t nops = 0;
    lg_txn_lsn = (uint64_t(lg_cycle) << 32) | lg_head;
    int recs_unflushed = 0;
    auto flush_rec = [&]() -> int {
      if (ops.empty()) return 0;
      int rc = log_put(ops, nops, true);
      ops.clear(); nops = 0;
      // 順序保証のないデバイスで、電源断のあとに「途中が欠けた」ログが 8 レコード (カーネルが先頭を探す窓) を超えて残らないよう、4 レコードごとに flush する
      if (!rc && ++recs_unflushed >= 4) { recs_unflushed = 0; rc = dev_flush(); }
      return rc;
    };
    put_ophdr(ops, tid, 0, 0x69, 0x01); nops++;                 // XLOG_START_TRANS
    put_ophdr(ops, tid, 16, 0x69, 0); nops++;                   // トランザクションヘッダ (ホストのバイト順)
    ops.resize(ops.size() + 16, 0);
    ple32(&ops[ops.size() - 16], 0x5452414e); ple32(&ops[ops.size() - 12], 0); ple32(&ops[ops.size() - 8], tid); ple32(&ops[ops.size() - 4], uint32_t(items.size()));
    for (LogItem& it : items) {
      WBuf& b = *it.b;
      size_t need = 12 + 20 + 4 * it.map_words;
      for (auto& r : it.runs) need += 12 + size_t(r.second - r.first) * kChunk;
      if (ops.size() + need > kMaxRecBytes) { int rc = flush_rec(); if (rc) { fail_rw("ログ書き込みエラー"); return rc; } }
      size_t fl = 20 + 4 * it.map_words;
      put_ophdr(ops, tid, uint32_t(fl), 0x69, 0); nops++;
      size_t o = ops.size();
      ops.resize(o + fl, 0);
      ple16(&ops[o], 0x123c); ple16(&ops[o + 2], uint32_t(1 + it.runs.size())); ple16(&ops[o + 4], uint32_t(b.blft) << 11);
      ple16(&ops[o + 6], b.len / 512); ple64(&ops[o + 8], b.off / 512); ple32(&ops[o + 16], it.map_words);
      for (auto& r : it.runs)
        for (uint32_t i = r.first; i < r.second; i++) ops[o + 20 + (i / 32) * 4 + (i % 32) / 8] |= uint8_t(1u << (i % 8));
      // data_map は host 順の 32 ビット語 (ビット i = 語 i/32 の i%32)。リトルエンディアンなのでバイト単位の上の式と等価
      for (auto& r : it.runs) {
        uint32_t n = (r.second - r.first) * kChunk;
        put_ophdr(ops, tid, n, 0x69, 0); nops++;
        o = ops.size();
        ops.resize(o + n);
        std::memcpy(&ops[o], b.d.data() + size_t(r.first) * kChunk, n);
      }
    }
    int rc = 0;
#ifdef USBMEM_MUTATE_INPLACE_FIRST
    // 突然変異: 本来の場所への書き込みをログより先にする (WAL 違反)
    for (LogItem& it : items) { WBuf& b = *it.b; for (uint32_t s = 0; s < b.len; s += sectsize) { if (!b.fresh && std::memcmp(b.d.data() + s, b.orig.data() + s, sectsize) == 0) continue; dev->write_at(b.off + s, b.d.data() + s, sectsize); } }
#endif
    rc = flush_rec();
    if (rc) { fail_rw("ログ書き込みエラー"); return rc; }
#ifndef USBMEM_MUTATE_NO_FLUSH_LOG
    if ((rc = dev_flush())) { fail_rw("flush エラー"); return rc; }
#endif
    put_ophdr(ops, tid, 0, 0x69, 0x02); nops++;                 // XLOG_COMMIT_TRANS (別レコード)
    if ((rc = flush_rec())) { fail_rw("ログ書き込みエラー"); return rc; }
#ifndef USBMEM_MUTATE_NO_FLUSH_COMMIT
    if ((rc = dev_flush())) { fail_rw("flush エラー"); return rc; }
#endif
#ifdef USBMEM_MUTATE_DATA_AFTER
    for (auto& d : deferred_data) dev->write_at(d.first, d.second.data(), d.second.size());
    deferred_data.clear();
#endif
    // 本来の場所へ (セクタ単位で差分だけ)
    for (LogItem& it : items) {
      WBuf& b = *it.b;
      for (uint32_t s = 0; s < b.len; s += sectsize) {
        if (!b.fresh && std::memcmp(b.d.data() + s, b.orig.data() + s, sectsize) == 0) continue;
        Status st = dev->write_at(b.off + s, b.d.data() + s, sectsize);
        if (!st.ok()) { fail_rw("メタデータ書き込みエラー"); return -EIO; }
      }
    }
#ifndef USBMEM_MUTATE_NO_FLUSH_INPLACE
    if ((rc = dev_flush())) { fail_rw("flush エラー"); return rc; }
#endif
    if ((rc = log_unmount_record())) { fail_rw("ログ書き込みエラー"); return rc; }
    unmount_pending_flush = true;
    wb.clear();
    pages.clear(); icache.clear(); dirmaps.clear();
    busy.clear();
    dirty_bytes = 0;
    return 0;
  }

  void fail_rw(const char* why) {
    failed = true; rw = false;
    ro_reason = std::string("書き込み中にエラー: ") + why;
    wb.clear(); undo.clear(); pages.clear(); icache.clear(); dirmaps.clear();
  }

  size_t wb_dirty_bytes() const {
    size_t n = 0;
    for (auto& kv : wb) if (kv.second.dirty) n += kv.second.len;
    return n;
  }
  // 操作の終わりに呼ぶ。溜まりすぎていたらコミットする
  int maybe_commit() {
    if (wb.size() < 2048 && wb_dirty_bytes() < (4u << 20) && wb_dirty_bytes() < log_nbb() * 512 / 8) return 0;
    return commit();
  }

  int do_sync() {
    if (!rw) return failed ? -EIO : 0;   // 読み取り専用なら同期するものが無い
    int rc = commit();
    if (rc) return rc;
    if (unmount_pending_flush) { rc = dev_flush(); if (rc) { fail_rw("flush エラー"); return rc; } unmount_pending_flush = false; }
    return 0;
  }

  // ============ 書き込みマウントの検査 ============
  // 書き込み可にできない理由を返す (空 = 可)
  std::string prepare_rw() {
    if (dev->is_read_only()) return "デバイスが読み取り専用";
    if (!v5) return "V4 (CRC なし) のボリュームは書き込み未対応";
    if (journal_ignored) return "ログが未再生 (noload で開いた)";
    if (!lg_ok) return "ログの位置を確定できない (未使用のログ / 解析不能)";
    if (dev->logical_block_size() > 512) return "デバイスの論理ブロックが 512 バイトより大きい (4Kn) ものは未対応";
    std::vector<uint8_t> sb(sectsize);
    if (dev_read(0, sb.data(), sectsize)) return "スーパーブロックを読めない";
    if (be32(sb.data()) != kSbMagic) return "スーパーブロックが不正";
    uint32_t comp_ro = be32(sb.data() + 212), incomp = be32(sb.data() + 216), log_incomp = be32(sb.data() + 220);
    if (log_incomp) return "ログに未知の機能 (features_log_incompat) がある";
    if (be32(sb.data() + 208)) return "未知の compat 機能がある";
    if (incomp & ~(kIncompatFtype | kIncompatSpinodes | kIncompatBigtime | kIncompatNrext64 | kIncompatExchrange))
      return "未対応の incompat 機能がある (meta_uuid / parent pointer / metadir など)";
    if (!has_ftype) return "ftype のないボリュームは未対応";
    if (comp_ro & ~(1u | 4u | 8u)) return "未対応の ro_compat 機能がある (rmapbt など)";
    if (versionnum & 0x4000) return "大文字小文字を区別しない (ascii-ci) ボリュームは未対応";
    if (be16(sb.data() + 176)) return "クォータが有効";
    if (be32(sb.data() + 184) || be32(sb.data() + 188)) return "ストライプ (sunit / swidth) の設定があるボリュームは未対応";
    if (be32(sb.data() + 196) > 1 || be16(sb.data() + 194) > 512) return "ログのストライプ / セクタサイズが 512 バイトを超える";
    if (dirblklog != 0) return "ディレクトリブロックがファイルシステムブロックより大きいボリュームは未対応";
    if (bs > 16384 || bs < 1024) return "ブロックサイズが 1 KiB〜16 KiB の範囲外";
    if (sectsize > 4096 || sectsize > bs) return "セクタサイズが範囲外";
    if (uint64_t(logblocks) * bs < (512u << 10)) return "ログが小さすぎる (512 KiB 未満)";
    if (log_nbb() >= (1ull << 30)) return "ログが大きすぎる";
    if (sb[127] > 100) return "imax_pct が不正";
    std::memcpy(uuid, sb.data() + 32, 16);
    bigtime = (incomp & kIncompatBigtime) != 0;
    nrext64 = (incomp & kIncompatNrext64) != 0;
    has_spino = (incomp & kIncompatSpinodes) != 0;
    has_finobt = (comp_ro & 1) != 0;
    has_reflink = (comp_ro & 4) != 0;
    has_inobtcount = (comp_ro & 8) != 0;
    imax_pct = sb[127];
    inoalignmt = be32(sb.data() + 180);
    spino_align = be32(sb.data() + 228);
    agfl_slots = (sectsize - 36) / 4;
    // 各 AG のヘッダ (AGF / AGI / AGFL) を検査
    for (uint32_t ag = 0; ag < agcount; ag++) {
      std::vector<uint8_t> h(sectsize);
      uint64_t base = agb_off(ag, 0);
      const uint32_t alen = ag_len(ag);
      if (dev_read(base + sectsize, h.data(), sectsize)) return "AGF を読めない";
      if (be32(h.data()) != 0x58414746 || be32(h.data() + 4) != 1 || be32(h.data() + 8) != ag || be32(h.data() + 12) != alen) return "AGF が不正 (ag " + std::to_string(ag) + ")";
      if (!cksum_ok(h.data(), sectsize, 216) || std::memcmp(h.data() + 64, uuid, 16)) return "AGF の CRC / UUID が不正 (ag " + std::to_string(ag) + ")";
      if (be32(h.data() + 24) != 0 || be32(h.data() + 36) != 0) return "rmapbt がある";
      uint32_t fl = be32(h.data() + 40), ll = be32(h.data() + 44), fc = be32(h.data() + 48);
      if (fl >= agfl_slots || ll >= agfl_slots || fc > agfl_slots) return "AGFL の位置が不正";
      if (be32(h.data() + 28) < 1 || be32(h.data() + 28) > 12 || be32(h.data() + 32) < 1 || be32(h.data() + 32) > 12) return "AGF の btree の高さが不正";
      if (dev_read(base + 2 * sectsize, h.data(), sectsize)) return "AGI を読めない";
      if (be32(h.data()) != 0x58414749 || be32(h.data() + 4) != 1 || be32(h.data() + 8) != ag || be32(h.data() + 12) != alen) return "AGI が不正 (ag " + std::to_string(ag) + ")";
      if (!cksum_ok(h.data(), sectsize, 312) || std::memcmp(h.data() + 296, uuid, 16)) return "AGI の CRC / UUID が不正 (ag " + std::to_string(ag) + ")";
      if (has_finobt && (be32(h.data() + 328) == 0 || be32(h.data() + 328) == 0xFFFFFFFFu)) return "finobt があるはずが AGI に無い";
      if (dev_read(base + 3 * sectsize, h.data(), sectsize)) return "AGFL を読めない";
      if (be32(h.data()) != 0x5841464c || be32(h.data() + 4) != ag || !cksum_ok(h.data(), sectsize, 32)) return "AGFL が不正 (ag " + std::to_string(ag) + ")";
    }
    sb_icount = be64(sb.data() + 128); sb_ifree = be64(sb.data() + 136); sb_fdblocks = be64(sb.data() + 144);
    return "";
  }
