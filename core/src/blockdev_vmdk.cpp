// SPDX-License-Identifier: Apache-2.0
// VMDK 読み取り専用。対応: monolithicSparse / streamOptimized (zlib 圧縮グレイン) / monolithicFlat + descriptor /
// twoGbMaxExtentSparse / twoGbMaxExtentFlat (複数エクステントは resolver で解決)、差分 (parentFileNameHint を resolver で解決)。
// 拒否 (Unsupported): VMFS / seSparse などの別形式、zlib 以外の圧縮。
#include <algorithm>
#include <map>
#include <sstream>

#include "blockdev_internal.h"

namespace usbmem {
namespace imgint {
namespace {

constexpr uint64_t kGdAtEnd = ~0ULL;
constexpr size_t kMaxGdBytes = 32u << 20;
constexpr uint64_t kMaxGrainSectors = 1u << 15;   // 16 MiB
constexpr size_t kMaxDescriptor = 2u << 20;

// 例外を投げない数値パーサ (descriptor はディスク上の入力なので不正な値が来る)
bool parse_dec(const std::string& s, uint64_t* v) {
  if (s.empty() || s.size() > 18) return false;
  uint64_t r = 0;
  for (char c : s) { if (c < '0' || c > '9') return false; r = r * 10 + static_cast<uint64_t>(c - '0'); }
  *v = r;
  return true;
}
bool parse_hex32(const std::string& s, uint32_t* v) {
  if (s.empty() || s.size() > 8) return false;
  uint32_t r = 0;
  for (char c : s) {
    uint32_t d;
    if (c >= '0' && c <= '9') d = static_cast<uint32_t>(c - '0');
    else if (c >= 'a' && c <= 'f') d = static_cast<uint32_t>(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') d = static_cast<uint32_t>(c - 'A' + 10);
    else return false;
    r = (r << 4) | d;
  }
  *v = r;
  return true;
}

struct SparseHeader {
  uint32_t version = 0, flags = 0, gtes_per_gt = 0, algo = 0;
  uint64_t capacity = 0, grain = 0, desc_off = 0, desc_size = 0, gd_off = 0, overhead = 0;
};

Status parse_sparse_header(BlockDevice& f, SparseHeader* h, std::string* why) {
  uint8_t b[512];
  if (f.size() < 512) { *why = "file too small for a VMDK header"; return Status::Make(Errc::Corrupt); }
  Status st = f.read_at(0, b, 512);
  if (!st.ok()) return st;
  if (std::memcmp(b, "KDMV", 4) != 0) { *why = "bad VMDK sparse magic"; return Status::Make(Errc::Corrupt); }
  h->version = le32(b + 4);
  h->flags = le32(b + 8);
  h->capacity = le64(b + 12);
  h->grain = le64(b + 20);
  h->desc_off = le64(b + 28);
  h->desc_size = le64(b + 36);
  h->gtes_per_gt = le32(b + 44);
  h->gd_off = le64(b + 56);
  h->overhead = le64(b + 64);
  h->algo = le16(b + 77);
  if (h->version < 1 || h->version > 3) { *why = "unsupported VMDK version"; return Status::Make(Errc::Unsupported); }
  if (h->flags & (1u << 16)) {   // 圧縮グレイン
    if (h->algo != 1) { *why = "unsupported VMDK compression"; return Status::Make(Errc::Unsupported); }
  }
  if (h->gd_off == kGdAtEnd) {   // streamOptimized: フッタに本物のヘッダがある
    uint64_t fs = f.size();
    if (fs < 1536) { *why = "streamOptimized file without a footer"; return Status::Make(Errc::Corrupt); }
    uint8_t ft[512];
    st = f.read_at(fs - 1024, ft, 512);
    if (!st.ok()) return st;
    if (std::memcmp(ft, "KDMV", 4) != 0) { *why = "bad VMDK footer"; return Status::Make(Errc::Corrupt); }
    h->capacity = le64(ft + 12);
    h->grain = le64(ft + 20);
    h->gtes_per_gt = le32(ft + 44);
    h->gd_off = le64(ft + 56);
  }
  if (h->grain < 8 || h->grain > kMaxGrainSectors || !is_pow2(h->grain)) { *why = "bad VMDK grain size"; return Status::Make(Errc::Corrupt); }
  if (h->gtes_per_gt == 0 || h->gtes_per_gt > 65536 || !is_pow2(h->gtes_per_gt)) { *why = "bad VMDK grain table size"; return Status::Make(Errc::Corrupt); }
  if (h->capacity == 0 || h->capacity > (1ULL << 53)) { *why = "bad VMDK capacity"; return Status::Make(Errc::Corrupt); }
  return Status::Ok();
}

struct Extent {
  enum Kind { Flat, Sparse, Zero } kind = Zero;
  uint64_t start = 0, sectors = 0;      // 仮想ディスク上の開始セクタと長さ
  BlockDevicePtr file;
  uint64_t flat_off = 0;                // Flat: ファイル内のバイトオフセット
  // Sparse
  uint64_t grain = 0, gtes = 0, file_size = 0;
  bool compressed = false;
  std::vector<uint32_t> gd;
  std::shared_ptr<TableCache> gtcache;
};

class VmdkDevice final : public ImageDeviceBase {
 public:
  VmdkDevice(uint64_t vsize, std::string desc) : ImageDeviceBase(nullptr, vsize, 512, std::move(desc)) {}
  std::vector<Extent> ext;
  BlockDevicePtr parent;

 protected:
  uint64_t unit_len(uint64_t off) const override {
    const Extent* e = find(off);
    if (!e) return 1;
    uint64_t in_ext = off - e->start * 512;
    uint64_t left = e->sectors * 512 - in_ext;
    if (e->kind == Extent::Sparse) return std::min<uint64_t>(left, e->grain * 512 - (in_ext % (e->grain * 512)));
    return std::min<uint64_t>(left, 1u << 20);
  }
  void on_close() override {
    for (auto& e : ext) if (e.file) e.file->close();
    ext.clear();
    if (parent) parent->close();
    parent.reset();
  }
  Status read_unit(uint64_t off, uint8_t* out, size_t n, const IoOptions& io) override {
    Extent* e = find(off);
    if (!e) return Status::Make(Errc::Corrupt);
    uint64_t in_ext = off - e->start * 512;
    switch (e->kind) {
      case Extent::Zero:
        std::memset(out, 0, n);
        return Status::Ok();
      case Extent::Flat: {
        uint64_t pos = e->flat_off + in_ext, end;
        if (!add_ok(pos, n, &end) || end > e->file->size()) return Status::Make(Errc::Corrupt);
        return e->file->read_at(pos, out, n, io);
      }
      case Extent::Sparse:
        return read_sparse(*e, off, in_ext, out, n, io);
    }
    return Status::Make(Errc::Corrupt);
  }

 private:
  Extent* find(uint64_t off) const {
    uint64_t s = off / 512;
    // エクステントは仮想セクタ順。個数は少ない (2 GB 分割でも数十〜数千) ので二分探索
    size_t lo = 0, hi = ext.size();
    while (lo < hi) {
      size_t mid = (lo + hi) / 2;
      if (ext[mid].start + ext[mid].sectors <= s) lo = mid + 1; else hi = mid;
    }
    if (lo < ext.size() && ext[lo].start <= s) return const_cast<Extent*>(&ext[lo]);
    return nullptr;
  }

  Status unalloc(uint64_t off, uint8_t* out, size_t n, const IoOptions& io) {
    if (parent) return read_zero_beyond(*parent, off, out, n, io);
    std::memset(out, 0, n);
    return Status::Ok();
  }

  Status read_sparse(Extent& e, uint64_t off, uint64_t in_ext, uint8_t* out, size_t n, const IoOptions& io) {
    uint64_t grain_bytes = e.grain * 512;
    uint64_t gi = in_ext / grain_bytes, in_grain = in_ext % grain_bytes;
    uint64_t gdi = gi / e.gtes, gti = gi % e.gtes;
    if (gdi >= e.gd.size()) return Status::Make(Errc::Corrupt);
    uint32_t gt_sector = e.gd[gdi];
    if (gt_sector == 0) return unalloc(off, out, n, io);
    uint64_t gt_off = static_cast<uint64_t>(gt_sector) * 512, gt_bytes = e.gtes * 4, end;
    if (!add_ok(gt_off, gt_bytes, &end) || end > e.file_size) return Status::Make(Errc::Corrupt);
    const std::vector<uint8_t>* t = e.gtcache->get(gt_off);
    if (!t) {
      std::vector<uint8_t> buf(static_cast<size_t>(gt_bytes));
      Status st = e.file->read_at(gt_off, buf.data(), buf.size(), io);
      if (!st.ok()) return st;
      t = e.gtcache->put(gt_off, std::move(buf));
    }
    uint32_t gte = le32(t->data() + 4 * gti);
    if (gte == 0) return unalloc(off, out, n, io);
    if (gte == 1) { std::memset(out, 0, n); return Status::Ok(); }   // ゼロ化済みグレイン
    uint64_t goff = static_cast<uint64_t>(gte) * 512;
    if (!e.compressed) {
      if (!add_ok(goff, grain_bytes, &end) || end > e.file_size) return Status::Make(Errc::Corrupt);
      return e.file->read_at(goff + in_grain, out, n, io);
    }
    // 圧縮グレイン: [lba u64][size u32][zlib データ]
    if (dcache_valid_ && dcache_ext_ == &e && dcache_key_ == goff) {
      std::memcpy(out, dcache_.data() + in_grain, n);
      return Status::Ok();
    }
    if (!add_ok(goff, 12, &end) || end > e.file_size) return Status::Make(Errc::Corrupt);
    uint8_t gh[12];
    Status st = e.file->read_at(goff, gh, 12, io);
    if (!st.ok()) return st;
    uint64_t lba = le64(gh);
    uint32_t csize = le32(gh + 8);
    if (lba != (gi * e.grain) || csize == 0 || csize > 2 * grain_bytes + 4096) return Status::Make(Errc::Corrupt);
    if (!add_ok(goff + 12, csize, &end) || end > e.file_size) return Status::Make(Errc::Corrupt);
    std::vector<uint8_t> comp(csize);
    st = e.file->read_at(goff + 12, comp.data(), comp.size(), io);
    if (!st.ok()) return st;
    dcache_.resize(static_cast<size_t>(grain_bytes));
    dcache_valid_ = false;
    st = inflate_to(comp.data(), comp.size(), dcache_.data(), dcache_.size(), /*zlib_header=*/true);
    if (!st.ok()) return st;
    dcache_valid_ = true;
    dcache_ext_ = &e;
    dcache_key_ = goff;
    std::memcpy(out, dcache_.data() + in_grain, n);
    return Status::Ok();
  }

  std::vector<uint8_t> dcache_;
  const Extent* dcache_ext_ = nullptr;
  uint64_t dcache_key_ = 0;
  bool dcache_valid_ = false;
};

// descriptor テキストの解析
struct Descriptor {
  std::string create_type, parent_hint;
  uint32_t cid = 0, parent_cid = 0xffffffff;
  bool have_cid = false;
  struct Ext { std::string access, type, file; uint64_t sectors = 0, offset = 0; };
  std::vector<Ext> extents;
};

bool parse_descriptor(const std::string& text, Descriptor* d, std::string* why) {
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    size_t b = line.find_first_not_of(" \t");
    if (b == std::string::npos) continue;
    line = line.substr(b);
    if (line[0] == '#') continue;
    if (line.compare(0, 2, "RW") == 0 || line.compare(0, 6, "RDONLY") == 0 || line.compare(0, 8, "NOACCESS") == 0) {
      Descriptor::Ext e;
      std::istringstream ls(line);
      std::string sec;
      ls >> e.access >> sec >> e.type;
      if (!parse_dec(sec, &e.sectors)) { *why = "bad extent line"; return false; }
      size_t q1 = line.find('"');
      if (q1 != std::string::npos) {
        size_t q2 = line.find('"', q1 + 1);
        if (q2 == std::string::npos) { *why = "bad extent file name"; return false; }
        e.file = line.substr(q1 + 1, q2 - q1 - 1);
        std::istringstream rest(line.substr(q2 + 1));
        std::string off;
        if (rest >> off) {
          if (!parse_dec(off, &e.offset)) { *why = "bad extent offset"; return false; }
        }
      }
      d->extents.push_back(e);
      continue;
    }
    size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string k = line.substr(0, eq), v = line.substr(eq + 1);
    while (!k.empty() && k.back() == ' ') k.pop_back();
    size_t vb = v.find_first_not_of(" \t");
    v = vb == std::string::npos ? "" : v.substr(vb);
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
    if (k == "createType") d->create_type = v;
    else if (k == "parentFileNameHint") d->parent_hint = v;
    else if (k == "CID") { if (parse_hex32(v, &d->cid)) d->have_cid = true; }
    else if (k == "parentCID") { if (!parse_hex32(v, &d->parent_cid)) d->parent_cid = 0xffffffffu; }
    if (d->extents.size() > 8192) { *why = "too many extents"; return false; }
  }
  return true;
}

Status load_sparse_extent(const SparseHeader& h, BlockDevicePtr file, uint64_t start, size_t cache, Extent* e, std::string* why) {
  uint64_t fs = file->size();
  e->kind = Extent::Sparse;
  e->start = start;
  e->sectors = h.capacity;
  e->file = file;
  e->file_size = fs;
  e->grain = h.grain;
  e->gtes = h.gtes_per_gt;
  e->compressed = (h.flags & (1u << 16)) != 0;
  uint64_t per_gt = h.grain * h.gtes_per_gt;
  uint64_t gd_entries = h.capacity / per_gt + ((h.capacity % per_gt) ? 1 : 0), gd_bytes, end;
  if (!mul_ok(gd_entries, 4, &gd_bytes) || gd_bytes > kMaxGdBytes) { *why = "grain directory too large"; return Status::Make(Errc::Corrupt); }
  uint64_t gdo = h.gd_off * 512;
  if (!add_ok(gdo, gd_bytes, &end) || end > fs) { *why = "grain directory outside the file"; return Status::Make(Errc::Corrupt); }
  std::vector<uint8_t> raw(static_cast<size_t>(gd_bytes));
  Status st = file->read_at(gdo, raw.data(), raw.size());
  if (!st.ok()) return st;
  e->gd.resize(static_cast<size_t>(gd_entries));
  for (size_t i = 0; i < e->gd.size(); i++) e->gd[i] = le32(raw.data() + 4 * i);
  e->gtcache = std::make_shared<TableCache>(cache);
  return Status::Ok();
}

}  // namespace

Status open_vmdk_depth(BlockDevicePtr file, const ImageOpenOptions& o, uint32_t depth, BlockDevicePtr* out, ImageInfo* info) {
  ImageInfo local;
  if (!info) info = &local;
  *info = ImageInfo();
  info->format = ImageFormat::Vmdk;
  const uint64_t fsize = file->size();
  if (fsize < 16) IMG_FAIL(info, Errc::Corrupt, "file too small for a VMDK");
  uint8_t magic[4];
  Status st = file->read_at(0, magic, 4);
  if (!st.ok()) return st;
  std::string why;
  Descriptor desc;
  std::vector<Extent> exts;
  uint64_t total_sectors = 0;
  BlockDevicePtr extent_owner_main;   // 単一スパースファイル形式なら file 自身

  if (std::memcmp(magic, "KDMV", 4) == 0) {
    SparseHeader h;
    st = parse_sparse_header(*file, &h, &why);
    if (!st.ok()) IMG_FAIL(info, st.code, why);
    if (h.desc_size > 0) {
      uint64_t dsz = h.desc_size * 512, doff = h.desc_off * 512, end;
      if (dsz > kMaxDescriptor || !add_ok(doff, dsz, &end) || end > fsize) IMG_FAIL(info, Errc::Corrupt, "descriptor outside the file");
      std::string text(static_cast<size_t>(dsz), '\0');
      st = file->read_at(doff, &text[0], text.size());
      if (!st.ok()) return st;
      text.resize(text.find('\0') == std::string::npos ? text.size() : text.find('\0'));
      if (!parse_descriptor(text, &desc, &why)) IMG_FAIL(info, Errc::Corrupt, why);
    }
    Extent e;
    st = load_sparse_extent(h, file, 0, o.max_cache_bytes, &e, &why);
    if (!st.ok()) IMG_FAIL(info, st.code, why);
    exts.push_back(std::move(e));
    total_sectors = h.capacity;
    info->subformat = "vmdk " + (desc.create_type.empty() ? std::string((h.flags & (1u << 16)) ? "streamOptimized" : "monolithicSparse") : desc.create_type);
    info->compressed = (h.flags & (1u << 16)) != 0;
  } else {
    // テキストの descriptor ファイル
    if (fsize > kMaxDescriptor) IMG_FAIL(info, Errc::Corrupt, "not a VMDK descriptor (too large)");
    std::string text(static_cast<size_t>(fsize), '\0');
    st = file->read_at(0, &text[0], text.size());
    if (!st.ok()) return st;
    if (text.find("# Disk DescriptorFile") == std::string::npos && text.find("createType") == std::string::npos) IMG_FAIL(info, Errc::Corrupt, "not a VMDK descriptor");
    if (!parse_descriptor(text, &desc, &why)) IMG_FAIL(info, Errc::Corrupt, why);
    if (desc.extents.empty()) IMG_FAIL(info, Errc::Corrupt, "VMDK descriptor without extents");
    info->subformat = "vmdk " + desc.create_type;
    uint64_t pos = 0;
    for (const auto& de : desc.extents) {
      if (de.sectors == 0) continue;
      Extent e;
      if (de.type == "ZERO") {
        e.kind = Extent::Zero; e.start = pos; e.sectors = de.sectors;
      } else if (de.type == "FLAT" || de.type == "VMFS") {
        if (de.file.empty()) IMG_FAIL(info, Errc::Corrupt, "flat extent without a file");
        ImageResolveRequest rq; rq.kind = ResolveKind::Extent; rq.name = norm_slashes(de.file); rq.format_hint = "raw";
        if (!o.resolver) IMG_FAIL(info, Errc::Unsupported, "VMDK extents are separate files but no resolver was provided");
        BlockDevicePtr f;
        st = o.resolver(rq, &f);
        if (!st.ok() || !f) IMG_FAIL(info, st.ok() ? Errc::Io : st.code, "cannot open extent '" + de.file + "'");
        uint64_t off_b, need;
        if (!mul_ok(de.offset, 512, &off_b) || !mul_ok(de.sectors, 512, &need) || !add_ok(off_b, need, &need) || need > f->size())
          IMG_FAIL(info, Errc::Corrupt, "flat extent '" + de.file + "' is shorter than declared");
        e.kind = Extent::Flat; e.start = pos; e.sectors = de.sectors; e.file = f; e.flat_off = off_b;
      } else if (de.type == "SPARSE") {
        if (de.file.empty()) IMG_FAIL(info, Errc::Corrupt, "sparse extent without a file");
        ImageResolveRequest rq; rq.kind = ResolveKind::Extent; rq.name = norm_slashes(de.file); rq.format_hint = "raw";
        if (!o.resolver) IMG_FAIL(info, Errc::Unsupported, "VMDK extents are separate files but no resolver was provided");
        BlockDevicePtr f;
        st = o.resolver(rq, &f);
        if (!st.ok() || !f) IMG_FAIL(info, st.ok() ? Errc::Io : st.code, "cannot open extent '" + de.file + "'");
        SparseHeader h;
        st = parse_sparse_header(*f, &h, &why);
        if (!st.ok()) IMG_FAIL(info, st.code, "extent '" + de.file + "': " + why);
        if (h.capacity != de.sectors) IMG_FAIL(info, Errc::Corrupt, "extent capacity does not match the descriptor");
        st = load_sparse_extent(h, f, pos, o.max_cache_bytes / std::max<size_t>(1, desc.extents.size()), &e, &why);
        if (!st.ok()) IMG_FAIL(info, st.code, why);
        info->compressed = info->compressed || e.compressed;
      } else {
        IMG_FAIL(info, Errc::Unsupported, "unsupported VMDK extent type '" + de.type + "'");
      }
      if (!add_ok(pos, de.sectors, &pos)) IMG_FAIL(info, Errc::Corrupt, "VMDK size overflow");
      exts.push_back(std::move(e));
    }
    total_sectors = pos;
  }
  uint64_t vsize;
  if (total_sectors == 0 || !mul_ok(total_sectors, 512, &vsize) || vsize > (1ULL << 62)) IMG_FAIL(info, Errc::Corrupt, "bad VMDK capacity");

  auto dev = std::make_shared<VmdkDevice>(vsize, "vmdk");
  dev->ext = std::move(exts);
  info->virtual_size = vsize;
  {
    char b[16];
    std::snprintf(b, sizeof b, "%08x", desc.cid);
    if (desc.have_cid) info->content_id = b;
  }
  if (desc.parent_cid != 0xffffffffu || !desc.parent_hint.empty()) {
    if (desc.parent_hint.empty()) IMG_FAIL(info, Errc::Corrupt, "differencing VMDK without parentFileNameHint");
    info->has_parent = true;
    info->parent_name = desc.parent_hint;
    ImageResolveRequest rq;
    rq.kind = ResolveKind::Backing;
    rq.name = norm_slashes(desc.parent_hint);
    rq.format_hint = "vmdk";
    BlockDevicePtr pd;
    ImageInfo pinfo;
    st = open_parent(o, depth, rq, &pd, &pinfo, info);
    if (!st.ok()) return st;
    if (!o.allow_parent_mismatch && pinfo.format == ImageFormat::Vmdk && !pinfo.content_id.empty()) {
      char b[16];
      std::snprintf(b, sizeof b, "%08x", desc.parent_cid);
      if (pinfo.content_id != b) IMG_FAIL(info, Errc::Corrupt, "parent VMDK CID does not match");
    }
    dev->parent = pd;
  }
  *out = dev;
  return Status::Ok();
}

}  // namespace imgint
}  // namespace usbmem
