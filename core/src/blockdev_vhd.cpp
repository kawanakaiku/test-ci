// SPDX-License-Identifier: Apache-2.0
// VHD (Microsoft Virtual Hard Disk, 仕様 v1.0) 読み取り専用: 固定 / 動的 / 差分。数値はすべてビッグエンディアン。
// 差分は親を resolver 経由で開き、セクタビットマップで「このファイルにあるセクタ / 親から読むセクタ」を切り替える。
#include <algorithm>

#include "blockdev_internal.h"

namespace usbmem {
namespace imgint {
namespace {

constexpr uint64_t kBatMaxBytes = 64u << 20;
constexpr uint32_t kUnallocated = 0xFFFFFFFFu;

bool checksum_ok(const uint8_t* p, size_t len, size_t csum_off) {
  uint32_t sum = 0;
  for (size_t i = 0; i < len; i++) if (i < csum_off || i >= csum_off + 4) sum += p[i];
  return ~sum == be32(p + csum_off);
}

struct Footer {
  uint64_t data_offset = 0, orig_size = 0, cur_size = 0;
  uint32_t disk_type = 0;
  uint8_t uuid[16] = {0};
};

bool parse_footer(const uint8_t* f, bool strict, Footer* out, std::string* why) {
  if (std::memcmp(f, "conectix", 8) != 0) { *why = "no VHD cookie"; return false; }
  if (strict && !checksum_ok(f, 512, 64)) { *why = "VHD footer checksum mismatch"; return false; }
  out->data_offset = be64(f + 16);
  out->orig_size = be64(f + 40);
  out->cur_size = be64(f + 48);
  out->disk_type = be32(f + 60);
  std::memcpy(out->uuid, f + 68, 16);
  return true;
}

class VhdDevice final : public ImageDeviceBase {
 public:
  VhdDevice(BlockDevicePtr file, uint64_t vsize, std::string desc)
      : ImageDeviceBase(std::move(file), vsize, 512, std::move(desc)) {}
  uint32_t type = 2;                // 2 固定 / 3 動的 / 4 差分
  uint64_t block_size = 0, bitmap_bytes = 0, file_size = 0;
  std::vector<uint32_t> bat;
  BlockDevicePtr parent;

 protected:
  uint64_t unit_len(uint64_t off) const override {
    if (type == 2) return std::min<uint64_t>(vsize_ - off, 1u << 20);
    return block_size - (off % block_size);
  }
  void on_close() override { if (parent) parent->close(); parent.reset(); }

  Status read_unit(uint64_t off, uint8_t* out, size_t n, const IoOptions& io) override {
    if (type == 2) {
      uint64_t end;
      if (!add_ok(off, n, &end) || end > file_size) return Status::Make(Errc::Corrupt);
      return file_->read_at(off, out, n, io);
    }
    uint64_t bi = off / block_size, in_blk = off % block_size;
    if (bi >= bat.size()) return Status::Make(Errc::Corrupt);
    uint32_t e = bat[bi];
    if (e == kUnallocated) return from_parent(off, out, n, io);
    uint64_t blk_off = static_cast<uint64_t>(e) * 512;
    uint64_t data_start = blk_off + bitmap_bytes;
    uint64_t end;
    if (!add_ok(data_start, block_size, &end) || end > file_size) return Status::Make(Errc::Corrupt);
    if (type == 3) return file_->read_at(data_start + in_blk, out, n, io);
    // 差分: セクタごとにビットマップ (MSB 先頭) を見て、まとめて読む
    std::vector<uint8_t> bm(static_cast<size_t>(bitmap_bytes));
    Status st = file_->read_at(blk_off, bm.data(), bm.size(), io);
    if (!st.ok()) return st;
    size_t done = 0;
    while (done < n) {
      uint64_t s = (in_blk + done) / 512;
      bool present = (bm[s >> 3] >> (7 - (s & 7))) & 1;
      size_t run = 0;
      // 同じ状態が続く範囲
      while (done + run < n) {
        uint64_t s2 = (in_blk + done + run) / 512;
        bool p2 = (bm[s2 >> 3] >> (7 - (s2 & 7))) & 1;
        if (p2 != present) break;
        size_t step = static_cast<size_t>(512 - ((in_blk + done + run) % 512));
        run += std::min(step, n - done - run);
      }
      if (present) st = file_->read_at(data_start + in_blk + done, out + done, run, io);
      else st = from_parent(off + done, out + done, run, io);
      if (!st.ok()) return st;
      done += run;
    }
    return Status::Ok();
  }

 private:
  Status from_parent(uint64_t off, uint8_t* out, size_t n, const IoOptions& io) {
    if (type == 4 && parent) return read_zero_beyond(*parent, off, out, n, io);
    std::memset(out, 0, n);
    return Status::Ok();
  }
};

}  // namespace

Status open_vhd_depth(BlockDevicePtr file, const ImageOpenOptions& o, uint32_t depth, BlockDevicePtr* out, ImageInfo* info) {
  ImageInfo local;
  if (!info) info = &local;
  *info = ImageInfo();
  info->format = ImageFormat::Vhd;
  const uint64_t fsize = file->size();
  if (fsize < 512) IMG_FAIL(info, Errc::Corrupt, "file too small for a VHD footer");
  // フッタは末尾 512 B (古い Virtual PC は 511 B) 。動的/差分は先頭にもコピーがある。
  uint8_t f[512];
  Footer ft;
  std::string why;
  bool got = false;
  Status st = file->read_at(fsize - 512, f, 512);
  if (!st.ok()) return st;
  got = parse_footer(f, o.strict_checksums, &ft, &why);
  if (!got && fsize >= 511) {
    uint8_t g[512] = {0};
    st = file->read_at(fsize - 511, g, 511);
    if (!st.ok()) return st;
    if (parse_footer(g, o.strict_checksums, &ft, &why)) got = true;
  }
  if (!got && fsize >= 1024) {   // 末尾のフッタが壊れていれば先頭のコピー (動的/差分のみ)
    st = file->read_at(0, f, 512);
    if (!st.ok()) return st;
    std::string why2;
    if (parse_footer(f, o.strict_checksums, &ft, &why2) && (ft.disk_type == 3 || ft.disk_type == 4)) got = true;
  }
  if (!got) IMG_FAIL(info, Errc::Corrupt, why);
  if (ft.disk_type != 2 && ft.disk_type != 3 && ft.disk_type != 4) IMG_FAIL(info, Errc::Unsupported, "unknown VHD disk type " + std::to_string(ft.disk_type));
  uint64_t vsize = ft.cur_size;
  if (vsize == 0 || vsize > (1ULL << 62) || (vsize & 511)) IMG_FAIL(info, Errc::Corrupt, "bad VHD size");
  info->virtual_size = vsize;
  info->content_id = hex(ft.uuid, 16);

  auto dev = std::make_shared<VhdDevice>(file, vsize, "vhd");
  dev->type = ft.disk_type;
  dev->file_size = fsize;
  if (ft.disk_type == 2) {
    info->subformat = "vhd fixed";
    if (fsize < 512 || vsize > fsize - 511) IMG_FAIL(info, Errc::Corrupt, "fixed VHD is shorter than its declared size");
    *out = dev;
    return Status::Ok();
  }

  // 動的ディスクヘッダ (1024 B)
  uint64_t dho = ft.data_offset;
  uint64_t dh_end;
  if (dho == ~0ULL || (dho & 511) || !add_ok(dho, 1024, &dh_end) || dh_end > fsize) IMG_FAIL(info, Errc::Corrupt, "bad dynamic disk header offset");
  std::vector<uint8_t> dh(1024);
  st = file->read_at(dho, dh.data(), dh.size());
  if (!st.ok()) return st;
  if (std::memcmp(dh.data(), "cxsparse", 8) != 0) IMG_FAIL(info, Errc::Corrupt, "bad dynamic disk header cookie");
  if (o.strict_checksums && !checksum_ok(dh.data(), 1024, 36)) IMG_FAIL(info, Errc::Corrupt, "dynamic disk header checksum mismatch");
  if ((be32(dh.data() + 24) >> 16) != 1) IMG_FAIL(info, Errc::Unsupported, "unknown dynamic header version");
  uint64_t table_off = be64(dh.data() + 16);
  uint32_t max_entries = be32(dh.data() + 28);
  uint32_t block_size = be32(dh.data() + 32);
  if (block_size < 512 || block_size > (64u << 20) || !is_pow2(block_size)) IMG_FAIL(info, Errc::Corrupt, "bad VHD block size");
  uint64_t need = vsize / block_size + ((vsize % block_size) ? 1 : 0);
  if (max_entries < need) IMG_FAIL(info, Errc::Corrupt, "BAT too small for the virtual size");
  uint64_t bat_bytes;
  if (!mul_ok(max_entries, 4, &bat_bytes) || bat_bytes > kBatMaxBytes) IMG_FAIL(info, Errc::Corrupt, "BAT too large");
  uint64_t bat_end;
  if ((table_off & 511) || !add_ok(table_off, bat_bytes, &bat_end) || bat_end > fsize) IMG_FAIL(info, Errc::Corrupt, "BAT outside the file");
  dev->block_size = block_size;
  uint64_t bm_sectors = (block_size / 512 + 7) / 8;
  dev->bitmap_bytes = ((bm_sectors + 511) / 512) * 512;
  std::vector<uint8_t> raw(static_cast<size_t>(bat_bytes));
  if (!raw.empty()) {
    st = file->read_at(table_off, raw.data(), raw.size());
    if (!st.ok()) return st;
  }
  dev->bat.resize(max_entries);
  for (uint32_t i = 0; i < max_entries; i++) {
    uint32_t e = be32(raw.data() + 4 * static_cast<size_t>(i));
    if (e != kUnallocated) {
      uint64_t bo = static_cast<uint64_t>(e) * 512, end;
      if (i < need && (!add_ok(bo, dev->bitmap_bytes + block_size, &end) || end > fsize)) IMG_FAIL(info, Errc::Corrupt, "VHD block outside the file");
    }
    dev->bat[i] = e;
  }
  info->subformat = ft.disk_type == 3 ? "vhd dynamic" : "vhd differencing";

  if (ft.disk_type == 4) {
    info->has_parent = true;
    ImageResolveRequest rq;
    rq.kind = ResolveKind::Backing;
    // 親の場所: ロケータ (W2ru 相対 / W2ku 絶対 / Wi2r / Wi2k) と親名 (UTF-16BE)
    std::vector<std::string> names;
    for (int i = 0; i < 8; i++) {
      const uint8_t* le = dh.data() + 576 + 24 * i;
      uint32_t code = be32(le), dlen = be32(le + 8);
      uint64_t doff = be64(le + 16);
      if (code == 0 || dlen == 0 || dlen > 4096) continue;
      uint64_t e;
      if ((code == 0x57327275u /*W2ru*/ || code == 0x57326b75u /*W2ku*/ || code == 0x57693272u /*Wi2r*/ || code == 0x5769326bu /*Wi2k*/) &&
          add_ok(doff, dlen, &e) && e <= fsize) {
        std::vector<uint8_t> d(dlen);
        st = file->read_at(doff, d.data(), d.size());
        if (!st.ok()) return st;
        bool utf16 = (code == 0x57327275u || code == 0x57326b75u);
        std::string s = utf16 ? utf16_to_utf8(d.data(), d.size(), false) : std::string(reinterpret_cast<char*>(d.data()), d.size());
        while (!s.empty() && s.back() == '\0') s.pop_back();
        if (!s.empty()) names.push_back(norm_slashes(s));
      }
    }
    std::string pn = utf16_to_utf8(dh.data() + 64, 512, true);
    if (!pn.empty()) names.push_back(norm_slashes(pn));
    if (names.empty()) IMG_FAIL(info, Errc::Corrupt, "differencing VHD without a parent path");
    rq.name = names[0];
    rq.alt_names.assign(names.begin() + 1, names.end());
    info->parent_name = rq.name;
    rq.format_hint = "vpc";
    BlockDevicePtr pd;
    ImageInfo pinfo;
    st = open_parent(o, depth, rq, &pd, &pinfo, info);
    if (!st.ok()) return st;
    if (!o.allow_parent_mismatch && pinfo.format == ImageFormat::Vhd && !pinfo.content_id.empty() &&
        pinfo.content_id != hex(dh.data() + 40, 16))
      IMG_FAIL(info, Errc::Corrupt, "parent VHD unique id does not match");
    dev->parent = pd;
  }
  *out = dev;
  return Status::Ok();
}

}  // namespace imgint
}  // namespace usbmem
