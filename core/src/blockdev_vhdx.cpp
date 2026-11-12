// SPDX-License-Identifier: Apache-2.0
// VHDX (MS-VHDX) 読み取り専用: 固定 / 動的 / 差分。数値はリトルエンディアン、チェックサムは CRC-32C。
// ログ (LogGuid != 0) は再生しない: 既定では未再生の可能性があるイメージを拒否する (opts.allow_unreplayed_log で読み取りだけ許す)。
#include <algorithm>
#include <array>

#include "blockdev_internal.h"

namespace usbmem {
namespace imgint {
namespace {

constexpr uint64_t kMiB = 1ULL << 20;
using Guid = std::array<uint8_t, 16>;

// GUID の文字列 "XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX" をディスク上のバイト列 (先頭 3 フィールドがリトルエンディアン) にする
Guid G(const char* s) {
  Guid g{};
  std::string h;
  for (const char* p = s; *p; p++) if (*p != '-') h += *p;
  uint8_t b[16];
  for (int i = 0; i < 16; i++) b[i] = static_cast<uint8_t>(std::stoi(h.substr(2 * i, 2), nullptr, 16));
  static const int map[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
  for (int i = 0; i < 16; i++) g[i] = b[map[i]];
  return g;
}
bool eq(const uint8_t* p, const Guid& g) { return std::memcmp(p, g.data(), 16) == 0; }

constexpr uint32_t PAYLOAD_NOT_PRESENT = 0, PAYLOAD_UNDEFINED = 1, PAYLOAD_ZERO = 2, PAYLOAD_UNMAPPED = 3,
                   PAYLOAD_FULLY_PRESENT = 6, PAYLOAD_PARTIALLY_PRESENT = 7;
constexpr uint32_t BITMAP_PRESENT = 6;

class VhdxDevice final : public ImageDeviceBase {
 public:
  VhdxDevice(BlockDevicePtr file, uint64_t vsize, uint32_t lbs, std::string desc, size_t cache)
      : ImageDeviceBase(std::move(file), vsize, lbs, std::move(desc)), cache_(cache) {}
  uint64_t block_size = 0, chunk_ratio = 0, bat_off = 0, bat_len = 0, file_size = 0, payload_blocks = 0;
  bool has_parent = false;
  BlockDevicePtr parent;

 protected:
  uint64_t unit_len(uint64_t off) const override { return block_size - (off % block_size); }
  void on_close() override { if (parent) parent->close(); parent.reset(); cache_.clear(); }

  Status read_unit(uint64_t off, uint8_t* out, size_t n, const IoOptions& io) override {
    uint64_t bi = off / block_size, in_blk = off % block_size;
    if (bi >= payload_blocks) return Status::Make(Errc::Corrupt);
    uint64_t e;
    Status st = bat_entry(bi + bi / chunk_ratio, &e, io);
    if (!st.ok()) return st;
    uint32_t state = static_cast<uint32_t>(e & 7);
    uint64_t foff = (e >> 20) * kMiB;
    switch (state) {
      case PAYLOAD_FULLY_PRESENT: {
        uint64_t end;
        if (!add_ok(foff + in_blk, n, &end) || end > file_size) return Status::Make(Errc::Corrupt);
        return file_->read_at(foff + in_blk, out, n, io);
      }
      case PAYLOAD_PARTIALLY_PRESENT: {
        if (!has_parent) return Status::Make(Errc::Corrupt);
        return read_partial(bi, foff, in_blk, off, out, n, io);
      }
      case PAYLOAD_NOT_PRESENT:
      case PAYLOAD_UNDEFINED:
        if (has_parent && parent) return read_zero_beyond(*parent, off, out, n, io);
        std::memset(out, 0, n);
        return Status::Ok();
      case PAYLOAD_ZERO:
      case PAYLOAD_UNMAPPED:
        std::memset(out, 0, n);
        return Status::Ok();
      default:
        return Status::Make(Errc::Corrupt);
    }
  }

 private:
  // BAT は 4 KiB ページ単位でキャッシュする
  Status bat_entry(uint64_t idx, uint64_t* e, const IoOptions& io) {
    uint64_t byte;
    if (!mul_ok(idx, 8, &byte) || byte + 8 > bat_len) return Status::Make(Errc::Corrupt);
    uint64_t page = byte / 4096, in_page = byte % 4096;
    const std::vector<uint8_t>* t = cache_.get(page);
    if (!t) {
      size_t n = static_cast<size_t>(std::min<uint64_t>(4096, bat_len - page * 4096));
      std::vector<uint8_t> buf(n);
      Status st = file_->read_at(bat_off + page * 4096, buf.data(), n, io);
      if (!st.ok()) return st;
      t = cache_.put(page, std::move(buf));
    }
    if (in_page + 8 > t->size()) return Status::Make(Errc::Corrupt);
    *e = le64(t->data() + in_page);
    return Status::Ok();
  }

  Status read_partial(uint64_t bi, uint64_t foff, uint64_t in_blk, uint64_t off, uint8_t* out, size_t n, const IoOptions& io) {
    // このブロックを含むチャンクのセクタビットマップ
    uint64_t chunk = bi / chunk_ratio;
    uint64_t be_idx = (chunk + 1) * chunk_ratio + chunk;
    uint64_t be;
    Status st = bat_entry(be_idx, &be, io);
    if (!st.ok()) return st;
    if ((be & 7) != BITMAP_PRESENT) {   // ビットマップが無い = このチャンクはすべて親から
      return read_zero_beyond(*parent, off, out, n, io);
    }
    uint64_t bmoff = (be >> 20) * kMiB;
    uint64_t end;
    if (!add_ok(bmoff, kMiB, &end) || end > file_size) return Status::Make(Errc::Corrupt);
    uint64_t lss = lbs_;
    uint64_t first_sector_in_chunk = (bi % chunk_ratio) * (block_size / lss);
    size_t done = 0;
    while (done < n) {
      uint64_t s = first_sector_in_chunk + (in_blk + done) / lss;
      if (s / 8 >= kMiB) return Status::Make(Errc::Corrupt);
      uint8_t byte;
      st = file_->read_at(bmoff + s / 8, &byte, 1, io);
      if (!st.ok()) return st;
      bool present = (byte >> (s & 7)) & 1;   // VHDX はビット 0 が若いセクタ
      size_t run = 0;
      while (done + run < n) {
        uint64_t s2 = first_sector_in_chunk + (in_blk + done + run) / lss;
        uint8_t b2 = byte;
        if (s2 / 8 != s / 8) { st = file_->read_at(bmoff + s2 / 8, &b2, 1, io); if (!st.ok()) return st; }
        if (((b2 >> (s2 & 7)) & 1) != present) break;
        size_t step = static_cast<size_t>(lss - ((in_blk + done + run) % lss));
        run += std::min(step, n - done - run);
        if (run >= (1u << 20)) break;
      }
      if (present) {
        uint64_t e2;
        if (!add_ok(foff + in_blk + done, run, &e2) || e2 > file_size) return Status::Make(Errc::Corrupt);
        st = file_->read_at(foff + in_blk + done, out + done, run, io);
      } else st = read_zero_beyond(*parent, off + done, out + done, run, io);
      if (!st.ok()) return st;
      done += run;
    }
    return Status::Ok();
  }

  TableCache cache_;
};

struct Header { bool valid = false; uint64_t seq = 0; uint8_t log_guid[16] = {0}; uint32_t log_length = 0; uint64_t log_offset = 0; };

// ヘッダが無効 (署名・CRC 不一致) なら h->valid=false で Ok を返す。I/O エラーはそのまま返す (無効扱いにして握りつぶさない)。
Status read_header(BlockDevice& f, uint64_t off, bool strict, Header* h) {
  *h = Header();
  if (off + 4096 > f.size()) return Status::Ok();
  std::vector<uint8_t> b(4096);
  Status st = f.read_at(off, b.data(), b.size());
  if (!st.ok()) return st;
  if (std::memcmp(b.data(), "head", 4) != 0) return Status::Ok();
  uint32_t want = le32(b.data() + 4);
  std::vector<uint8_t> c = b;
  std::memset(c.data() + 4, 0, 4);
  if (strict && crc32c(c.data(), c.size()) != want) return Status::Ok();
  if (le16(b.data() + 66) != 1) return Status::Ok();   // version
  h->valid = true;
  h->seq = le64(b.data() + 8);
  std::memcpy(h->log_guid, b.data() + 48, 16);
  h->log_length = le32(b.data() + 68);
  h->log_offset = le64(b.data() + 72);
  return Status::Ok();
}

}  // namespace

Status open_vhdx_depth(BlockDevicePtr file, const ImageOpenOptions& o, uint32_t depth, BlockDevicePtr* out, ImageInfo* info) {
  ImageInfo local;
  if (!info) info = &local;
  *info = ImageInfo();
  info->format = ImageFormat::Vhdx;
  const uint64_t fsize = file->size();
  if (fsize < 1 * kMiB) IMG_FAIL(info, Errc::Corrupt, "file too small for a VHDX");
  uint8_t sig[8];
  Status st = file->read_at(0, sig, 8);
  if (!st.ok()) return st;
  if (std::memcmp(sig, "vhdxfile", 8) != 0) IMG_FAIL(info, Errc::Corrupt, "bad VHDX file type identifier");
  Header h1, h2;
  st = read_header(*file, 64 * 1024, o.strict_checksums, &h1);
  if (!st.ok()) return st;
  st = read_header(*file, 128 * 1024, o.strict_checksums, &h2);
  if (!st.ok()) return st;
  if (!h1.valid && !h2.valid) IMG_FAIL(info, Errc::Corrupt, "no valid VHDX header");
  const Header& h = (h1.valid && (!h2.valid || h1.seq >= h2.seq)) ? h1 : h2;
  bool zero_log = true;
  for (int i = 0; i < 16; i++) if (h.log_guid[i]) zero_log = false;
  if (!zero_log) {
    if (!o.allow_unreplayed_log) IMG_FAIL(info, Errc::Unsupported, "VHDX log is not empty (the image was not closed cleanly); log replay is not supported");
    info->detail = "VHDX log not replayed; contents may be stale";
  }

  // リージョンテーブル (192 KiB, 64 KiB)
  std::vector<uint8_t> rt(64 * 1024);
  if (192 * 1024 + rt.size() > fsize) IMG_FAIL(info, Errc::Corrupt, "region table outside the file");
  st = file->read_at(192 * 1024, rt.data(), rt.size());
  if (!st.ok()) return st;
  if (std::memcmp(rt.data(), "regi", 4) != 0) IMG_FAIL(info, Errc::Corrupt, "bad region table signature");
  if (o.strict_checksums) {
    std::vector<uint8_t> c = rt;
    std::memset(c.data() + 4, 0, 4);
    if (crc32c(c.data(), c.size()) != le32(rt.data() + 4)) IMG_FAIL(info, Errc::Corrupt, "region table checksum mismatch");
  }
  uint32_t count = le32(rt.data() + 8);
  if (count > 2047) IMG_FAIL(info, Errc::Corrupt, "bad region count");
  static const Guid kBat = G("2DC27766-F623-4200-9D64-115E9BFD4A08"), kMeta = G("8B7CA206-4790-4B9A-B8FE-575F050F886E");
  uint64_t bat_off = 0, bat_len = 0, meta_off = 0, meta_len = 0;
  bool have_bat = false, have_meta = false;
  for (uint32_t i = 0; i < count; i++) {
    const uint8_t* e = rt.data() + 16 + 32 * static_cast<size_t>(i);
    uint64_t off = le64(e + 16);
    uint32_t len = le32(e + 24), required = le32(e + 28);
    uint64_t end;
    if (!add_ok(off, len, &end) || end > fsize || (off % kMiB) != 0) IMG_FAIL(info, Errc::Corrupt, "region outside the file or misaligned");
    if (eq(e, kBat)) { bat_off = off; bat_len = len; have_bat = true; }
    else if (eq(e, kMeta)) { meta_off = off; meta_len = len; have_meta = true; }
    else if (required & 1) IMG_FAIL(info, Errc::Unsupported, "unknown required VHDX region");
  }
  if (!have_bat || !have_meta) IMG_FAIL(info, Errc::Corrupt, "VHDX is missing the BAT or metadata region");
  if (meta_len < 64 * 1024 || meta_len > (16u << 20)) IMG_FAIL(info, Errc::Corrupt, "bad metadata region size");

  // メタデータ
  std::vector<uint8_t> md(static_cast<size_t>(meta_len));
  st = file->read_at(meta_off, md.data(), md.size());
  if (!st.ok()) return st;
  if (std::memcmp(md.data(), "metadata", 8) != 0) IMG_FAIL(info, Errc::Corrupt, "bad metadata table signature");
  uint32_t mcount = le16(md.data() + 10);
  if (mcount > 2047) IMG_FAIL(info, Errc::Corrupt, "bad metadata entry count");
  static const Guid kFileParams = G("CAA16737-FA36-4D43-B3B6-33F0AA44E76B"), kVSize = G("2FA54224-CD1B-4876-B211-5DBED83BF4B8"),
                    kVId = G("BECA12AB-B2E6-4523-93EF-C309E000C746"), kLSS = G("8141BF1D-A96F-4709-BA47-F233A8FAAB5F"),
                    kPSS = G("CDA348C7-445D-4471-9CC9-E9885251C556"), kParent = G("A8D35F2D-B30B-454D-ABF7-D3D84834AB0C");
  uint32_t block_size = 0, fparams_flags = 0, lss = 0;
  uint64_t vsize = 0, parent_item_off = 0, parent_item_len = 0;
  bool got_fp = false, got_vs = false, got_lss = false, got_parent = false;
  uint8_t vid[16] = {0};
  for (uint32_t i = 0; i < mcount; i++) {
    const uint8_t* e = md.data() + 32 + 32 * static_cast<size_t>(i);
    uint32_t off = le32(e + 16), len = le32(e + 20), flags = le32(e + 24);
    uint64_t end;
    if (!add_ok(off, len, &end) || end > md.size()) IMG_FAIL(info, Errc::Corrupt, "metadata item outside the region");
    const uint8_t* d = md.data() + off;
    if (eq(e, kFileParams) && len >= 8) { block_size = le32(d); fparams_flags = le32(d + 4); got_fp = true; }
    else if (eq(e, kVSize) && len >= 8) { vsize = le64(d); got_vs = true; }
    else if (eq(e, kVId) && len >= 16) std::memcpy(vid, d, 16);
    else if (eq(e, kLSS) && len >= 4) { lss = le32(d); got_lss = true; }
    else if (eq(e, kPSS)) {}
    else if (eq(e, kParent)) { parent_item_off = off; parent_item_len = len; got_parent = true; }
    else if (flags & 4) IMG_FAIL(info, Errc::Unsupported, "unknown required VHDX metadata item");
  }
  if (!got_fp || !got_vs || !got_lss) IMG_FAIL(info, Errc::Corrupt, "VHDX metadata is incomplete");
  if (!is_pow2(block_size) || block_size < 1 * kMiB || block_size > 256 * kMiB) IMG_FAIL(info, Errc::Corrupt, "bad VHDX block size");
  if (lss != 512 && lss != 4096) IMG_FAIL(info, Errc::Corrupt, "bad VHDX logical sector size");
  if (vsize == 0 || vsize > (1ULL << 62) || (vsize % lss)) IMG_FAIL(info, Errc::Corrupt, "bad VHDX virtual size");
  uint64_t chunk_ratio = ((1ULL << 23) * lss) / block_size;
  if (chunk_ratio == 0) IMG_FAIL(info, Errc::Corrupt, "bad VHDX chunk ratio");
  bool has_parent = (fparams_flags & 2) != 0;
  if (has_parent && !got_parent) IMG_FAIL(info, Errc::Corrupt, "differencing VHDX without a parent locator");
  uint64_t payload_blocks = vsize / block_size + ((vsize % block_size) ? 1 : 0);
  uint64_t need_entries = payload_blocks + (payload_blocks - 1) / chunk_ratio;
  if (has_parent) need_entries = ((payload_blocks + chunk_ratio - 1) / chunk_ratio) * (chunk_ratio + 1);   // 差分は最後のチャンクにもビットマップ項目がある
  uint64_t need_bytes;
  if (!mul_ok(need_entries, 8, &need_bytes) || need_bytes > bat_len || bat_len > (256u << 20)) IMG_FAIL(info, Errc::Corrupt, "BAT is too small for the virtual size");

  auto dev = std::make_shared<VhdxDevice>(file, vsize, lss, "vhdx", o.max_cache_bytes);
  dev->block_size = block_size;
  dev->chunk_ratio = chunk_ratio;
  dev->bat_off = bat_off;
  dev->bat_len = bat_len;
  dev->file_size = fsize;
  dev->payload_blocks = payload_blocks;
  dev->has_parent = has_parent;
  info->virtual_size = vsize;
  info->logical_block_size = lss;
  info->content_id = hex(vid, 16);
  info->subformat = has_parent ? "vhdx differencing" : ((fparams_flags & 1) ? "vhdx fixed" : "vhdx dynamic");

  if (has_parent) {
    info->has_parent = true;
    // 親ロケータ: 型 GUID(16) 予約(2) 項目数(2) { key_off(4) value_off(4) key_len(2) value_len(2) } ... 文字列は UTF-16LE
    if (parent_item_len < 20) IMG_FAIL(info, Errc::Corrupt, "short parent locator");
    const uint8_t* pl = md.data() + parent_item_off;
    uint32_t kv = le16(pl + 18);
    if (20 + static_cast<uint64_t>(kv) * 12 > parent_item_len) IMG_FAIL(info, Errc::Corrupt, "bad parent locator");
    std::vector<std::string> names;
    for (uint32_t i = 0; i < kv; i++) {
      const uint8_t* ke = pl + 20 + 12 * static_cast<size_t>(i);
      uint32_t ko = le32(ke), vo = le32(ke + 4), kl = le16(ke + 8), vl = le16(ke + 10);
      if (static_cast<uint64_t>(ko) + kl > parent_item_len || static_cast<uint64_t>(vo) + vl > parent_item_len) IMG_FAIL(info, Errc::Corrupt, "bad parent locator entry");
      std::string key = utf16_to_utf8(pl + ko, kl, false), val = utf16_to_utf8(pl + vo, vl, false);
      if (key == "relative_path" || key == "volume_path" || key == "absolute_win32_path") names.push_back(norm_slashes(val));
    }
    if (names.empty()) IMG_FAIL(info, Errc::Corrupt, "differencing VHDX without a parent path");
    ImageResolveRequest rq;
    rq.kind = ResolveKind::Backing;
    rq.name = names[0];
    rq.alt_names.assign(names.begin() + 1, names.end());
    rq.format_hint = "vhdx";
    info->parent_name = rq.name;
    BlockDevicePtr pd;
    ImageInfo pinfo;
    st = open_parent(o, depth, rq, &pd, &pinfo, info);
    if (!st.ok()) return st;
    dev->parent = pd;
  }
  *out = dev;
  return Status::Ok();
}

}  // namespace imgint
}  // namespace usbmem
