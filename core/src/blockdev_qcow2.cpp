// SPDX-License-Identifier: Apache-2.0
// QCOW2 (v2 / v3) 読み取り専用。仕様: QEMU docs/interop/qcow2.txt。
// 対応: L1/L2、クラスタサイズ 512 B〜2 MiB、zero フラグ (v3)、deflate 圧縮クラスタ、バッキングファイル (resolver 経由のチェーン)。
// 拒否 (Unsupported): 暗号化 (AES / LUKS)、外部データファイル、拡張 L2、zstd 圧縮、未知の非互換機能。corrupt ビットが立っていれば Corrupt。
#include <algorithm>

#include "blockdev_internal.h"

namespace usbmem {
namespace imgint {
namespace {

constexpr uint64_t kL1OffsetMask = 0x00fffffffffffe00ULL;
constexpr uint64_t kCompressedFlag = 1ULL << 62;
constexpr size_t kMaxL1Bytes = 32u << 20;
constexpr uint32_t kMinClusterBits = 9, kMaxClusterBits = 21;

class Qcow2Device final : public ImageDeviceBase {
 public:
  Qcow2Device(BlockDevicePtr file, uint64_t vsize, std::string desc, size_t cache_bytes)
      : ImageDeviceBase(std::move(file), vsize, 512, std::move(desc)), l2cache_(cache_bytes) {}

  uint32_t cluster_bits = 0, version = 2;
  uint64_t cluster_size = 0, file_size = 0, l2_entries = 0;
  std::vector<uint64_t> l1;
  BlockDevicePtr backing;

 protected:
  uint64_t unit_len(uint64_t off) const override { return cluster_size - (off & (cluster_size - 1)); }
  void on_close() override {
    if (backing) backing->close();
    backing.reset();
    l2cache_.clear();
  }

  Status read_unit(uint64_t off, uint8_t* out, size_t n, const IoOptions& io) override {
    uint64_t ci = off >> cluster_bits;
    uint64_t in_off = off & (cluster_size - 1);
    uint64_t l1i = ci / l2_entries;
    if (l1i >= l1.size()) return Status::Make(Errc::Corrupt);
    uint64_t l2off = l1[l1i] & kL1OffsetMask;
    if (l2off == 0) return unallocated(off, out, n, io);
    if ((l2off & (cluster_size - 1)) != 0 || l2off > file_size || cluster_size > file_size - l2off) return Status::Make(Errc::Corrupt);
    const std::vector<uint8_t>* t = l2cache_.get(l2off);
    if (!t) {
      std::vector<uint8_t> buf(static_cast<size_t>(cluster_size));
      Status st = file_->read_at(l2off, buf.data(), buf.size(), io);
      if (!st.ok()) return st;
      t = l2cache_.put(l2off, std::move(buf));
    }
    uint64_t entry = be64(t->data() + 8 * (ci % l2_entries));
    if (entry & kCompressedFlag) return read_compressed(entry, in_off, out, n, io);
    if (version >= 3 && (entry & 1)) { std::memset(out, 0, n); return Status::Ok(); }   // zero cluster
    uint64_t host = entry & kL1OffsetMask;
    if (host == 0) return unallocated(off, out, n, io);
    if ((host & (cluster_size - 1)) != 0) return Status::Make(Errc::Corrupt);
    uint64_t end;
    if (!add_ok(host + in_off, n, &end) || end > file_size) return Status::Make(Errc::Corrupt);
    return file_->read_at(host + in_off, out, n, io);
  }

 private:
  Status unallocated(uint64_t off, uint8_t* out, size_t n, const IoOptions& io) {
    if (backing) return read_zero_beyond(*backing, off, out, n, io);
    std::memset(out, 0, n);
    return Status::Ok();
  }

  Status read_compressed(uint64_t entry, uint64_t in_off, uint8_t* out, size_t n, const IoOptions& io) {
    uint32_t csize_shift = 62 - (cluster_bits - 8);
    uint64_t csize_mask = (1ULL << (cluster_bits - 8)) - 1;
    uint64_t coffset = entry & ((1ULL << csize_shift) - 1);
    uint64_t nsectors = ((entry >> csize_shift) & csize_mask) + 1;
    uint64_t csize = nsectors * 512 - (coffset & 511);
    uint64_t end;
    if (!add_ok(coffset, csize, &end) || end > file_size) return Status::Make(Errc::Corrupt);
    if (dcache_valid_ && dcache_key_ == coffset) {
      std::memcpy(out, dcache_.data() + in_off, n);
      return Status::Ok();
    }
    std::vector<uint8_t> comp(static_cast<size_t>(csize));
    Status st = file_->read_at(coffset, comp.data(), comp.size(), io);
    if (!st.ok()) return st;
    dcache_.resize(static_cast<size_t>(cluster_size));
    dcache_valid_ = false;
    st = inflate_to(comp.data(), comp.size(), dcache_.data(), dcache_.size(), /*zlib_header=*/false);
    if (!st.ok()) return st;
    dcache_valid_ = true;
    dcache_key_ = coffset;
    std::memcpy(out, dcache_.data() + in_off, n);
    return Status::Ok();
  }

  TableCache l2cache_;
  std::vector<uint8_t> dcache_;
  uint64_t dcache_key_ = 0;
  bool dcache_valid_ = false;
};

}  // namespace

Status open_qcow2_depth(BlockDevicePtr file, const ImageOpenOptions& o, uint32_t depth, BlockDevicePtr* out, ImageInfo* info) {
  ImageInfo local;
  if (!info) info = &local;
  *info = ImageInfo();
  info->format = ImageFormat::Qcow2;
  const uint64_t fsize = file->size();
  if (fsize < 72) IMG_FAIL(info, Errc::Corrupt, "file too small for a qcow2 header");
  uint8_t h[112] = {0};
  Status st = file->read_at(0, h, static_cast<size_t>(std::min<uint64_t>(sizeof h, fsize)));
  if (!st.ok()) return st;
  if (be32(h) != 0x514649fbu) IMG_FAIL(info, Errc::Corrupt, "bad qcow2 magic");
  uint32_t version = be32(h + 4);
  if (version != 2 && version != 3) IMG_FAIL(info, Errc::Unsupported, "unsupported qcow2 version " + std::to_string(version));
  uint64_t backing_off = be64(h + 8);
  uint32_t backing_size = be32(h + 16);
  uint32_t cluster_bits = be32(h + 20);
  uint64_t vsize = be64(h + 24);
  uint32_t crypt = be32(h + 32);
  uint32_t l1_size = be32(h + 36);
  uint64_t l1_off = be64(h + 40);
  if (cluster_bits < kMinClusterBits || cluster_bits > kMaxClusterBits) IMG_FAIL(info, Errc::Corrupt, "bad cluster_bits");
  if (crypt != 0) IMG_FAIL(info, Errc::Unsupported, "encrypted qcow2 images are not supported");
  if (vsize > (1ULL << 62)) IMG_FAIL(info, Errc::Corrupt, "virtual size too large");
  const uint64_t cs = 1ULL << cluster_bits;

  uint64_t hdr_len = 72;
  std::string backing_fmt;
  bool compression_zstd = false;
  if (version == 3) {
    if (fsize < 104) IMG_FAIL(info, Errc::Corrupt, "short v3 header");
    uint64_t incompat = be64(h + 72);
    hdr_len = be32(h + 100);
    if (hdr_len < 104 || (hdr_len & 7) || hdr_len > cs) IMG_FAIL(info, Errc::Corrupt, "bad header_length");
    if (incompat & (1ULL << 1)) IMG_FAIL(info, Errc::Corrupt, "image is marked corrupt");
    if (incompat & (1ULL << 2)) IMG_FAIL(info, Errc::Unsupported, "external data files are not supported");
    if (incompat & (1ULL << 4)) IMG_FAIL(info, Errc::Unsupported, "extended L2 entries are not supported");
    if (incompat & ~0x1fULL) IMG_FAIL(info, Errc::Unsupported, "unknown incompatible feature bits");
    if (incompat & (1ULL << 0)) info->detail = "image was not closed cleanly (dirty bit); reading the mapped data only";
    if (incompat & (1ULL << 3)) {
      if (hdr_len < 105 || fsize < 105) IMG_FAIL(info, Errc::Corrupt, "missing compression type");
      uint8_t ct;
      st = file->read_at(104, &ct, 1);
      if (!st.ok()) return st;
      if (ct == 1) compression_zstd = true;
      else if (ct != 0) IMG_FAIL(info, Errc::Unsupported, "unknown compression type");
    }
  }
  if (compression_zstd) IMG_FAIL(info, Errc::Unsupported, "zstd-compressed qcow2 is not supported (deflate only)");

  // ヘッダ拡張 (backing format / 暗号化ヘッダ / データファイル名)
  {
    uint64_t pos = hdr_len;
    uint64_t limit = std::min<uint64_t>(fsize, cs);
    for (int guard = 0; guard < 256 && pos + 8 <= limit; guard++) {
      uint8_t eh[8];
      st = file->read_at(pos, eh, 8);
      if (!st.ok()) return st;
      uint32_t type = be32(eh), len = be32(eh + 4);
      if (type == 0) break;
      uint64_t padded = (static_cast<uint64_t>(len) + 7) & ~7ULL;
      if (pos + 8 + len > limit) IMG_FAIL(info, Errc::Corrupt, "header extension runs past the first cluster");
      if (type == 0xE2792ACAu && len <= 64) {
        std::string s(len, '\0');
        if (len) { st = file->read_at(pos + 8, &s[0], len); if (!st.ok()) return st; }
        backing_fmt = s;
      } else if (type == 0x0537be77u) IMG_FAIL(info, Errc::Unsupported, "full-disk encryption header present");
      pos += 8 + padded;
    }
  }

  uint64_t l2_entries = cs / 8;
  uint64_t clusters = vsize / cs + ((vsize % cs) ? 1 : 0);
  uint64_t need_l1 = clusters / l2_entries + ((clusters % l2_entries) ? 1 : 0);
  if (l1_size < need_l1) IMG_FAIL(info, Errc::Corrupt, "L1 table too small for the virtual size");
  uint64_t l1_bytes;
  if (!mul_ok(l1_size, 8, &l1_bytes) || l1_bytes > kMaxL1Bytes) IMG_FAIL(info, Errc::Corrupt, "L1 table too large");
  if (l1_size != 0) {
    uint64_t end;
    if ((l1_off & 511) || !add_ok(l1_off, l1_bytes, &end) || end > fsize) IMG_FAIL(info, Errc::Corrupt, "L1 table outside the file");
  }

  auto dev = std::make_shared<Qcow2Device>(file, vsize, "qcow2 v" + std::to_string(version), o.max_cache_bytes);
  dev->cluster_bits = cluster_bits;
  dev->cluster_size = cs;
  dev->version = version;
  dev->file_size = fsize;
  dev->l2_entries = l2_entries;
  dev->l1.resize(l1_size);
  if (l1_size) {
    std::vector<uint8_t> raw(static_cast<size_t>(l1_bytes));
    st = file->read_at(l1_off, raw.data(), raw.size());
    if (!st.ok()) return st;
    for (uint32_t i = 0; i < l1_size; i++) dev->l1[i] = be64(raw.data() + 8 * static_cast<size_t>(i));
  }

  info->subformat = "qcow2 v" + std::to_string(version);
  info->virtual_size = vsize;
  info->compressed = true;
  if (backing_off != 0 && backing_size != 0) {
    if (backing_size > 1023 || backing_off + backing_size > std::min<uint64_t>(fsize, cs)) IMG_FAIL(info, Errc::Corrupt, "bad backing file name");
    std::string name(backing_size, '\0');
    st = file->read_at(backing_off, &name[0], backing_size);
    if (!st.ok()) return st;
    info->has_parent = true;
    info->parent_name = name;
    ImageResolveRequest rq;
    rq.kind = ResolveKind::Backing;
    rq.name = norm_slashes(name);
    rq.format_hint = backing_fmt;
    BlockDevicePtr bd;
    ImageInfo pinfo;
    st = open_parent(o, depth, rq, &bd, &pinfo, info);
    if (!st.ok()) return st;
    dev->backing = bd;
  }
  *out = dev;
  return Status::Ok();
}

}  // namespace imgint
}  // namespace usbmem
