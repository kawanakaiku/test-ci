// SPDX-License-Identifier: Apache-2.0
// F2FS 書き込み: 公開操作の本体 (f2fs_write.inc の続き。f2fs_volume.cpp から #include される)。

namespace {
inline bool inode_locked(const Inode& in) { return (in.flags & (kFlEncryptFl | kFlComprFl | 0x10u | 0x20u)) != 0 || in.compressed; }
}
int F2fsVolume::Impl::op_create(uint64_t dir64, const std::string& name, uint32_t mode, uint64_t* ino_out, uint16_t type, const std::string* target) {
  if (!rw) return -EROFS;
  if (broken) return -EIO;
  if (dir64 > 0xFFFFFFFFull) return -ENOENT;
  uint32_t dnid = static_cast<uint32_t>(dir64);
  int rc = check_name(name);
  if (rc) return rc;
  Inode d;
  rc = check_mutable_dir(dnid, &d);
  if (rc) return rc;
  bool found; uint32_t x; uint8_t ft;
  rc = dir_find(dnid, name, &x, &ft, &found);
  if (rc) return rc;
  if (found) return -EEXIST;
  if (target) {
    if (target->empty()) return -EINVAL;
    if (target->size() > 4095) return -ENAMETOOLONG;
  }
  if (!space_ok(8)) return -ENOSPC;
  uint32_t uid = le32(d.blk->data() + 4), gid = le32(d.blk->data() + 8);
  uint32_t nid;
  rc = make_inode(static_cast<uint16_t>(type | (mode & 07777)), dnid, name, uid, gid, &nid);
  if (rc) return rc;
  // ここから先で失敗したら、途中までの変更をコミットしないようにする
  auto fail = [&](int e) { broken = true; return e; };
  uint8_t dft = kFtReg;
  if (type == 0040000) {
    dft = kFtDir;
    std::vector<uint8_t>* pg;
    rc = dpage_mut(nid, 0, true, &pg);
    if (rc) return fail(rc);
    DBuf db = dbuf_blk(pg->data());
    db.put(0, 0, ".", nid, kFtDir);
    db.put(1, 0, "..", dnid, kFtDir);
    NodeVec iv, pv;
    if ((rc = mut_node(nid, 0, &iv)) || (rc = mut_node(dnid, 0, &pv))) return fail(rc);
    put64(iv->data() + 16, kBlk);
    put32(iv->data() + 72, 1);
    put32(pv->data() + 12, le32(pv->data() + 12) + 1);
  } else if (type == 0120000) {
    dft = kFtLnk;
    std::vector<uint8_t>* pg;
    rc = dpage_mut(nid, 0, true, &pg);
    if (rc) return fail(rc);
    memcpy(pg->data(), target->data(), target->size());
    NodeVec iv;
    if ((rc = mut_node(nid, 0, &iv))) return fail(rc);
    put64(iv->data() + 16, target->size());
  }
  rc = dir_add(dnid, name, nid, dft);
  if (rc) return fail(rc);
  rc = maybe_writeback();
  if (rc) return fail(rc);
  *ino_out = nid;
  return 0;
}
int F2fsVolume::Impl::op_unlink(uint64_t dir64, const std::string& name, bool want_dir) {
  if (!rw) return -EROFS;
  if (broken) return -EIO;
  if (dir64 > 0xFFFFFFFFull) return -ENOENT;
  uint32_t dnid = static_cast<uint32_t>(dir64);
  int rc = check_name(name);
  if (rc) return rc;
  Inode d;
  rc = check_mutable_dir(dnid, &d);
  if (rc) return rc;
  bool found; uint32_t ino; uint8_t ft;
  rc = dir_find(dnid, name, &ino, &ft, &found);
  if (rc) return rc;
  if (!found) return -ENOENT;
  Inode t;
  rc = load_inode(ino, &t);
  if (rc) return rc;
  if (t.flags & (0x10u | 0x20u)) return -EPERM;
  if (want_dir) {
    if (!t.is_dir()) return -ENOTDIR;
    bool empty;
    rc = dir_is_empty(ino, &empty);
    if (rc) return rc;
    if (!empty) return -ENOTEMPTY;
  } else if (t.is_dir()) return -EISDIR;
  auto fail = [&](int e) { broken = true; return e; };
  rc = dir_del(dnid, name);
  if (rc) return fail(rc);
  if (want_dir) {
    rc = free_inode(ino);
    if (rc) return fail(rc);
    NodeVec pv;
    if ((rc = mut_node(dnid, 0, &pv))) return fail(rc);
    uint32_t l = le32(pv->data() + 12);
    if (l > 2) put32(pv->data() + 12, l - 1);
  } else {
    uint32_t l = t.nlink;
    if (l <= 1) {
      rc = free_inode(ino);
      if (rc) return fail(rc);
    } else {
      NodeVec iv;
      if ((rc = mut_node(ino, 0, &iv))) return fail(rc);
      put32(iv->data() + 12, l - 1);
      if ((rc = touch(ino, false))) return fail(rc);
    }
  }
  dirty = true;
  return 0;
}
int F2fsVolume::Impl::op_link(uint64_t dir64, const std::string& name, uint64_t existing) {
  if (!rw) return -EROFS;
  if (broken) return -EIO;
  if (dir64 > 0xFFFFFFFFull || existing > 0xFFFFFFFFull) return -ENOENT;
  uint32_t dnid = static_cast<uint32_t>(dir64), ino = static_cast<uint32_t>(existing);
  int rc = check_name(name);
  if (rc) return rc;
  Inode d, t;
  rc = check_mutable_dir(dnid, &d);
  if (rc) return rc;
  rc = load_inode(ino, &t);
  if (rc) return rc;
  if (t.is_dir()) return -EPERM;
  if (t.flags & (0x10u | 0x20u)) return -EPERM;
  if (t.nlink >= 65000) return -EMLINK;
  bool found; uint32_t x; uint8_t ft;
  rc = dir_find(dnid, name, &x, &ft, &found);
  if (rc) return rc;
  if (found) return -EEXIST;
  if (!space_ok(4)) return -ENOSPC;
  auto fail = [&](int e) { broken = true; return e; };
  rc = dir_add(dnid, name, ino, t.is_lnk() ? kFtLnk : kFtReg);
  if (rc) return fail(rc);
  NodeVec iv;
  if ((rc = mut_node(ino, 0, &iv))) return fail(rc);
  put32(iv->data() + 12, t.nlink + 1);
  put32(iv->data() + 84, dnid);
  put32(iv->data() + 88, static_cast<uint32_t>(name.size()));
  memset(iv->data() + 92, 0, 255);
  memcpy(iv->data() + 92, name.data(), name.size());
  if ((rc = touch(ino, false))) return fail(rc);
  return 0;
}
int F2fsVolume::Impl::op_rename(uint64_t od64, const std::string& on, uint64_t nd64, const std::string& nn) {
  if (!rw) return -EROFS;
  if (broken) return -EIO;
  if (od64 > 0xFFFFFFFFull || nd64 > 0xFFFFFFFFull) return -ENOENT;
  uint32_t od = static_cast<uint32_t>(od64), nd = static_cast<uint32_t>(nd64);
  int rc = check_name(on);
  if (rc) return rc;
  rc = check_name(nn);
  if (rc) return rc;
  Inode odi, ndi;
  if ((rc = check_mutable_dir(od, &odi)) || (rc = check_mutable_dir(nd, &ndi))) return rc;
  bool found; uint32_t ino; uint8_t ft;
  rc = dir_find(od, on, &ino, &ft, &found);
  if (rc) return rc;
  if (!found) return -ENOENT;
  if (od == nd && on == nn) return 0;
  Inode src;
  rc = load_inode(ino, &src);
  if (rc) return rc;
  if (src.flags & (0x10u | 0x20u)) return -EPERM;
  if (src.is_dir()) {   // 自分自身の配下へは移せない
    uint32_t w = nd;
    for (int i = 0; i < 65536; i++) {
      if (w == ino) return -EINVAL;
      if (w == root_ino) break;
      Inode wi;
      rc = load_inode(w, &wi);
      if (rc) return rc;
      uint32_t up = le32(wi.blk->data() + 84);
      if (up == w || up < kReservedNodes) break;
      w = up;
    }
  }
  bool tfound; uint32_t tino = 0; uint8_t tft = 0;
  rc = dir_find(nd, nn, &tino, &tft, &tfound);
  if (rc) return rc;
  Inode tgt;
  if (tfound) {
    if (tino == ino) return 0;   // 同じ inode の別名どうし: 何もしない (POSIX)
    rc = load_inode(tino, &tgt);
    if (rc) return rc;
    if (tgt.flags & (0x10u | 0x20u)) return -EPERM;
    if (src.is_dir()) {
      if (!tgt.is_dir()) return -ENOTDIR;
      bool empty;
      rc = dir_is_empty(tino, &empty);
      if (rc) return rc;
      if (!empty) return -ENOTEMPTY;
    } else if (tgt.is_dir()) return -EISDIR;
  }
  if (!space_ok(6)) return -ENOSPC;
  auto fail = [&](int e) { broken = true; return e; };
  if (tfound) {
    rc = dir_del(nd, nn);
    if (rc) return fail(rc);
    if (tgt.is_dir()) {
      rc = free_inode(tino);
      if (rc) return fail(rc);
      NodeVec pv;
      if ((rc = mut_node(nd, 0, &pv))) return fail(rc);
      uint32_t l = le32(pv->data() + 12);
      if (l > 2) put32(pv->data() + 12, l - 1);
    } else if (tgt.nlink <= 1) {
      rc = free_inode(tino);
      if (rc) return fail(rc);
    } else {
      NodeVec tv;
      if ((rc = mut_node(tino, 0, &tv))) return fail(rc);
      put32(tv->data() + 12, tgt.nlink - 1);
    }
  }
  rc = dir_del(od, on);
  if (rc) return fail(rc);
  rc = dir_add(nd, nn, ino, ft);
  if (rc) return fail(rc);
  NodeVec iv;
  if ((rc = mut_node(ino, 0, &iv))) return fail(rc);
  put32(iv->data() + 84, nd);
  put32(iv->data() + 88, static_cast<uint32_t>(nn.size()));
  memset(iv->data() + 92, 0, 255);
  memcpy(iv->data() + 92, nn.data(), nn.size());
  if (src.is_dir() && od != nd) {
    rc = dir_set_ino(ino, "..", nd);
    if (rc) return fail(rc);
    NodeVec ov, nv;
    if ((rc = mut_node(od, 0, &ov)) || (rc = mut_node(nd, 0, &nv))) return fail(rc);
    uint32_t l = le32(ov->data() + 12);
    if (l > 2) put32(ov->data() + 12, l - 1);
    put32(nv->data() + 12, le32(nv->data() + 12) + 1);
  }
  if ((rc = touch(ino, false))) return fail(rc);
  dirty = true;
  return 0;
}

// インラインデータのファイルを通常のブロックに変換する (サイズが inline に収まらなくなったとき)
int F2fsVolume::Impl::convert_inline_file(uint32_t ino) {
  Inode in;
  int rc = load_inode(ino, &in);
  if (rc) return rc;
  NodeVec iv;
  rc = mut_node(ino, 0, &iv);
  if (rc) return rc;
  uint64_t n = std::min<uint64_t>(in.size, in.max_inline);
  std::vector<uint8_t> data(iv->data() + 360 + 4 * in.extra_words + 4, iv->data() + 360 + 4 * in.extra_words + 4 + n);
  uint32_t cur_words = kDefAddrsPerInode - in.extra_words;
  uint32_t xw = in.xattr_words;
  memset(iv->data() + 360 + 4 * in.extra_words, 0, 4 * (cur_words - xw));
  iv->data()[3] = static_cast<uint8_t>(iv->data()[3] & ~(kInlData | kInlDataExist));
  if (n > 0) {
    std::vector<uint8_t>* pg;
    rc = dpage_mut(ino, 0, true, &pg);
    if (rc) return rc;
    memcpy(pg->data(), data.data(), n);
  }
  return 0;
}
int64_t F2fsVolume::Impl::op_write(uint64_t ino64, uint64_t off, const void* buf, size_t len, bool inplace) {
  if (!rw) return -EROFS;
  if (broken) return -EIO;
  if (ino64 > 0xFFFFFFFFull) return -ENOENT;
  uint32_t ino = static_cast<uint32_t>(ino64);
  Inode in;
  int rc = load_inode(ino, &in);
  if (rc) return rc;
  if (in.is_dir()) return -EISDIR;
  if (!in.is_reg()) return -EINVAL;
  if (inode_locked(in)) return -EPERM;
  if (inplace) {
    if (off >= in.size) return 0;
    len = static_cast<size_t>(std::min<uint64_t>(len, in.size - off));
  }
  if (len == 0) return 0;
  const uint64_t kMaxFile = (1ull << 41);
  if (off > kMaxFile || len > kMaxFile - off) return -EFBIG;
  uint64_t end = off + len;
  auto fail = [&](int e) -> int64_t { broken = true; return e; };
  if (in.inline_data()) {
    if (end <= in.max_inline) {
      NodeVec iv;
      rc = mut_node(ino, 0, &iv);
      if (rc) return rc;
      uint8_t* r = iv->data() + 360 + 4 * in.extra_words + 4;
      if (off > in.size) memset(r + in.size, 0, off - in.size);
      memcpy(r + off, buf, len);
      iv->data()[3] = static_cast<uint8_t>(iv->data()[3] | kInlDataExist);
      if (end > in.size) put64(iv->data() + 16, end);
      touch(ino, true);
      return static_cast<int64_t>(len);
    }
    rc = convert_inline_file(ino);
    if (rc) return fail(rc);
    rc = load_inode(ino, &in);
    if (rc) return fail(rc);
  }
  uint64_t nblocks = (end + kBlk - 1) / kBlk - off / kBlk;
  if (!space_ok(nblocks)) return -ENOSPC;
  const uint8_t* src = static_cast<const uint8_t*>(buf);
  size_t done = 0;
  while (done < len) {
    uint64_t pos = off + done, fblk = pos / kBlk;
    size_t boff = static_cast<size_t>(pos % kBlk), n = std::min<size_t>(len - done, kBlk - boff);
    std::vector<uint8_t>* pg;
    rc = dpage_mut(ino, fblk, boff == 0 && n == kBlk, &pg);
    if (rc) return fail(rc);
    memcpy(pg->data() + boff, src + done, n);
    done += n;
    if (dpages.size() > kMaxDirtyPages) { rc = writeback_pages(); if (rc) return fail(rc); }
  }
  NodeVec iv;
  rc = mut_node(ino, 0, &iv);
  if (rc) return fail(rc);
  if (end > le64(iv->data() + 16)) put64(iv->data() + 16, end);
  touch(ino, true);
  return static_cast<int64_t>(len);
}
int F2fsVolume::Impl::op_truncate(uint64_t ino64, uint64_t size) {
  if (!rw) return -EROFS;
  if (broken) return -EIO;
  if (ino64 > 0xFFFFFFFFull) return -ENOENT;
  uint32_t ino = static_cast<uint32_t>(ino64);
  Inode in;
  int rc = load_inode(ino, &in);
  if (rc) return rc;
  if (in.is_dir()) return -EISDIR;
  if (!in.is_reg()) return -EINVAL;
  if (inode_locked(in)) return -EPERM;
  if (size > (1ull << 41)) return -EFBIG;
  if (size == in.size) return 0;
  auto fail = [&](int e) { broken = true; return e; };
  if (in.inline_data()) {
    if (size <= in.max_inline) {
      NodeVec iv;
      rc = mut_node(ino, 0, &iv);
      if (rc) return rc;
      uint8_t* r = iv->data() + 360 + 4 * in.extra_words + 4;
      if (size < in.size) memset(r + size, 0, std::min<uint64_t>(in.size, in.max_inline) - size);
      put64(iv->data() + 16, size);
      touch(ino, true);
      return 0;
    }
    rc = convert_inline_file(ino);
    if (rc) return fail(rc);
    rc = load_inode(ino, &in);
    if (rc) return fail(rc);
  }
  NodeVec iv;
  if (size > in.size) {
    rc = mut_node(ino, 0, &iv);
    if (rc) return rc;
    put64(iv->data() + 16, size);
    touch(ino, true);
    return 0;
  }
  if (!space_ok(2)) return -ENOSPC;
  if (size % kBlk) {   // 最後の(部分)ブロックの EOF より後ろをゼロにする (あとで拡張したとき古い内容が見えないように)
    uint64_t fb = size / kBlk;
    uint32_t addr;
    rc = bmap(in, fb, &addr);
    if (rc) return rc;
    if (addr != kNullAddr || dirty_page(ino, fb)) {
      std::vector<uint8_t>* pg;
      rc = dpage_mut(ino, fb, false, &pg);
      if (rc) return fail(rc);
      memset(pg->data() + size % kBlk, 0, kBlk - size % kBlk);
    }
  }
  uint64_t from = (size + kBlk - 1) / kBlk;
  drop_dpages(ino, from);
  rc = truncate_blocks(ino, from);
  if (rc) return fail(rc);
  rc = mut_node(ino, 0, &iv);
  if (rc) return fail(rc);
  put64(iv->data() + 16, size);
  touch(ino, true);
  return 0;
}
