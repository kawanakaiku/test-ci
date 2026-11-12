// SPDX-License-Identifier: Apache-2.0
// Partclone イメージの復元: 計画 → 確認トークン → 適用。画像の検査・書き込み・読み戻し検証を行う。
//  - 計画は何も書かない。画像全体を検査し (任意)、書き込み先の状態と合わせて「指紋」を作る。
//  - 適用は、指紋と確認トークンが合う場合だけ進む。画像か書き込み先が計画のあとで変わっていれば拒否する。
//  - 書き込みの順序: 先頭 4 KiB (ブートセクタ / スーパーブロック) はゼロで潰してから最後に書く。途中で止まっても、
//    「古い FS の先頭に新しい中身が混ざった、一見正常なボリューム」にはならず、FS として認識されない状態で終わる。
#include <algorithm>
#include <cstring>
#include <map>

#include "backup_internal.h"
#include "usbmem/fs_probe.h"

namespace usbmem {
using namespace backup_detail;

namespace {
RestorePlan plan_fail(RestorePlan p, const char* code, const std::string& msg) {
  p.ok = false;
  p.code = code;
  p.message = msg;
  return p;
}
const char* fs_label(FsType t) { return t == FsType::Unknown ? "" : fs_type_name(t); }
}  // namespace

std::string make_restore_token(const RestorePlan& plan, const std::string& typed) {
  Sha256 h;
  const std::string head = "usbmem-restore-v1";
  h.update(head.data(), head.size() + 1);
  h.update(plan.plan_hash.data(), plan.plan_hash.size() + 1);
  h.update(plan.target_id.data(), plan.target_id.size() + 1);
  h.update(typed.data(), typed.size());
  return hex_encode(h.finish());
}

RestorePlan plan_restore(const SourceFactory& image, BlockDevice& target, const RestorePlanOptions& opt) {
  RestorePlan p;
  p.opt_ntfs_backup_boot = opt.write_ntfs_backup_boot;
  p.target_size = target.size();
  p.target_id = target.describe();
  std::unique_ptr<ByteSource> src = image ? image() : nullptr;
  if (!src) return plan_fail(p, "image-open-failed", "could not open the image");
  if (opt.verify_all) {
    VerifyResult v = verify_image(*src, opt.cancel);
    if (!v.status.ok()) {
      p.image = v.info;
      return plan_fail(p, v.status.st.code == Errc::Cancelled ? "cancelled" : "image-corrupt", v.status.message + " [" + v.status.code + "]");
    }
    p.image = v.info;
    p.image_sha256 = v.image_sha256;
    p.verified = true;
  } else {
    FsStatus f = read_image_info(*src, &p.image, nullptr);
    if (!f.ok()) return plan_fail(p, "image-corrupt", f.message + " [" + f.code + "]");
  }
  const ImageInfo& im = p.image;
  p.blocks_to_write = im.counted_used;
  p.bytes_to_write = im.counted_used * im.block_size;

  if (target.is_read_only()) return plan_fail(p, "target-read-only", "the target is read-only");
  if (opt.target_in_use) return plan_fail(p, "target-in-use", "the target is in use (mounted or open); unmount it first");
  uint64_t need = im.total_blocks * im.block_size;
  if (p.target_size < need) return plan_fail(p, "target-too-small", "the target is smaller than the filesystem in the image");
  if (p.target_size > im.device_size) p.warnings.push_back("the target is larger than the original; the filesystem keeps its original size");

  bool want_ntfs_tail = opt.write_ntfs_backup_boot && im.fs == "NTFS" && im.device_size > need && p.target_size >= im.device_size;
  p.ntfs_backup_boot = want_ntfs_tail;
  if (im.fs == "NTFS" && im.device_size > need && p.target_size < im.device_size) p.warnings.push_back("the target has no room for the backup boot sector at its end");

  Status io;
  FsType et = probe_filesystem(target, 0, target.size(), &io);
  p.target_existing_fs = fs_label(et);
  if (!p.target_existing_fs.empty()) p.warnings.push_back("the target currently holds a " + p.target_existing_fs + " filesystem that will be destroyed");

  // 指紋
  std::string c = "fs=" + im.fs + ";bs=" + std::to_string(im.block_size) + ";total=" + std::to_string(im.total_blocks) + ";used=" + std::to_string(im.counted_used) +
                  ";dev=" + std::to_string(im.device_size) + ";hdr=" + im.header_sha256 + ";bm=" + im.bitmap_sha256 + ";img=" + p.image_sha256 +
                  ";verified=" + (p.verified ? "1" : "0") + ";tsize=" + std::to_string(p.target_size) + ";tid=" + p.target_id + ";ntfsboot=" + (p.ntfs_backup_boot ? "1" : "0");
  p.plan_hash = hex_encode(Sha256::hash(c.data(), c.size()));
  p.confirm_phrase = "RESTORE " + p.plan_hash.substr(0, 6);
  p.ok = true;
  p.code = "ok";
  return p;
}

RestoreResult apply_restore(const SourceFactory& image, BlockDevice& target, const RestorePlan& plan, const std::string& token, const RestoreOptions& opt) {
  RestoreResult res;
  auto fail = [&](Errc e, const char* code, const std::string& msg, int err = 0) {
    res.status.st = Status::Make(e, err);
    res.status.code = code;
    res.status.message = msg;
    return res;
  };
  if (!plan.ok) return fail(Errc::InvalidArgument, "bad-plan", "the plan is not valid: " + plan.code);
  if (token != make_restore_token(plan, plan.confirm_phrase)) return fail(Errc::InvalidArgument, "bad-token", "the confirmation token does not match this plan");
  // 計画をもう一度作って、画像と書き込み先が計画のときと同じことを確かめる (verified の計画なら、ここで画像全体も再検査される)
  RestorePlanOptions po;
  po.target_in_use = opt.target_in_use;
  po.verify_all = plan.verified;
  po.write_ntfs_backup_boot = plan.opt_ntfs_backup_boot;
  po.cancel = opt.cancel;
  if (opt.progress) opt.progress("verify", 0, plan.blocks_to_write);
  RestorePlan again = plan_restore(image, target, po);
  if (!again.ok) return fail(Errc::InvalidArgument, again.code.c_str(), again.message);
  if (again.plan_hash != plan.plan_hash) return fail(Errc::InvalidArgument, "plan-changed", "the image or the target changed since the plan was made");

  std::unique_ptr<ByteSource> src = image();
  if (!src) return fail(Errc::Io, "image-open-failed", "could not open the image");
  ImageInfo info;
  std::vector<uint8_t> bits;
  FsStatus f = read_image_info(*src, &info, &bits);
  if (!f.ok()) return fail(f.st.code, "image-corrupt", f.message + " [" + f.code + "]");
  const uint32_t bs = info.block_size;
  BlockStream stream(*src, info, bits);

  // 先頭 4 KiB に入るブロックは最後に書く。先に、その領域を潰しておく。
  const uint64_t held_n = std::max<uint64_t>(1, (4096 + bs - 1) / bs);
  const bool hold = info.counted_used > 0 && info.first_used < held_n;
  std::map<uint64_t, Bytes> held;
  if (hold) {
    Bytes zeros(static_cast<size_t>(held_n * bs), 0);
    Status st = target.write_at(0, zeros.data(), zeros.size());
    res.target_touched = true;
    if (!st.ok()) return fail(st.code, "write-error", "could not clear the start of the target", st.sys_errno);
  }

  std::vector<uint32_t> written_crc;  // 区間ごとに「書いた内容」の CRC (読み戻しの照合に使う)
  std::vector<uint64_t> blocks;
  Bytes data;
  uint64_t done = 0;
  for (;;) {
    if (opt.cancel && opt.cancel->cancelled()) return fail(Errc::Cancelled, "cancelled", "the restore was cancelled; the target is incomplete and must not be used");
    uint32_t crc;
    bool end;
    f = stream.next(&blocks, &data, &crc, &end);
    if (!f.ok()) return fail(f.st.code, f.code.c_str(), f.message + " (the target is incomplete and must not be used)");
    if (end) break;
    written_crc.push_back(pc_crc(data.data(), data.size()));
    // 連続するブロックをまとめて書く
    for (size_t i = 0; i < blocks.size();) {
      size_t j = i + 1;
      while (j < blocks.size() && blocks[j] == blocks[j - 1] + 1 && (j - i + 1) * bs <= opt.write_chunk_bytes) j++;
      for (size_t k = i; k < j; k++) {
        if (blocks[k] < held_n) held[blocks[k]].assign(data.begin() + static_cast<std::ptrdiff_t>(k * bs), data.begin() + static_cast<std::ptrdiff_t>((k + 1) * bs));
      }
      // 保留するブロックを除いて書く (連なりの途中に保留があれば分ける)
      size_t a = i;
      while (a < j) {
        while (a < j && blocks[a] < held_n) a++;
        size_t b = a;
        while (b < j && blocks[b] >= held_n) b++;
        if (b > a) {
          Status st = target.write_at(blocks[a] * bs, data.data() + a * bs, (b - a) * bs);
          res.target_touched = true;
          if (!st.ok()) return fail(st.code, "write-error", "writing to the target failed at block " + std::to_string(blocks[a]), st.sys_errno);
        }
        a = b;
      }
      i = j;
    }
    done += blocks.size();
    res.blocks_written = done;
    if (opt.progress) opt.progress("write", done, info.counted_used);
  }
  f = stream.expect_end();
  if (!f.ok()) return fail(f.st.code, f.code.c_str(), f.message);

  Status st = target.flush();
  if (!st.ok()) return fail(st.code, "flush-error", "could not flush the target before writing the boot area", st.sys_errno);
  // 保留したブロック (ブートセクタ / スーパーブロック) を最後に書く
  if (hold) {
    // 先頭領域は 1 回の書き込みにまとめる (使われていないブロックは、最初に潰したままのゼロ)
    Bytes region(static_cast<size_t>(held_n * bs), 0);
    for (auto& kv : held) std::memcpy(region.data() + kv.first * bs, kv.second.data(), kv.second.size());
    st = target.write_at(0, region.data(), region.size());
    res.target_touched = true;
    if (!st.ok()) return fail(st.code, "write-error", "writing the boot area failed", st.sys_errno);
  }
  res.blocks_written = done;
  Bytes tail_boot;
  if (plan.ntfs_backup_boot && held.count(0)) {
    uint32_t bps = static_cast<uint32_t>(get_le(&held[0][11], 2));
    if (bps >= 512 && bps <= 4096 && !(bps & (bps - 1)) && bps <= bs && info.device_size >= bps) {
      tail_boot.assign(held[0].begin(), held[0].begin() + bps);
      st = target.write_at(info.device_size - bps, tail_boot.data(), tail_boot.size());
      if (!st.ok()) return fail(st.code, "write-error", "writing the backup boot sector failed", st.sys_errno);
    } else {
      tail_boot.clear();
    }
  }
  st = target.flush();
  if (!st.ok()) return fail(st.code, "flush-error", "could not flush the target", st.sys_errno);

  // 読み戻し検証: 書いた区間と同じ区切りで読み、書いた内容の CRC と比べる
  uint64_t cursor = 0, chunk = 0, verified_blocks = 0;
  Bytes rb;
  std::vector<uint64_t> sel;
  for (;;) {
    if (opt.cancel && opt.cancel->cancelled()) return fail(Errc::Cancelled, "cancelled", "cancelled during the read-back; the data was written but not verified");
    BlockStream::select(bits, info.total_blocks, stream.per_chunk(), &cursor, &sel);
    if (sel.empty()) break;
    if (chunk >= written_crc.size()) return fail(Errc::Corrupt, "verify-failed", "internal: chunk count mismatch");
    PcCrc c;
    for (size_t i = 0; i < sel.size();) {
      size_t j = i + 1;
      while (j < sel.size() && sel[j] == sel[j - 1] + 1 && (j - i + 1) * bs <= opt.write_chunk_bytes) j++;
      rb.resize((j - i) * bs);
      st = target.read_at(sel[i] * bs, rb.data(), rb.size());
      if (!st.ok()) return fail(st.code, "verify-read-error", "could not read the target back at block " + std::to_string(sel[i]), st.sys_errno);
      c.update(rb.data(), rb.size());
      i = j;
    }
    if (c.value() != written_crc[static_cast<size_t>(chunk)]) return fail(Errc::Corrupt, "verify-failed", "the target does not hold what was written (chunk " + std::to_string(chunk) + "); the target is unreliable");
    verified_blocks += sel.size();
    chunk++;
    if (opt.progress) opt.progress("readback", verified_blocks, info.counted_used);
  }
  if (!tail_boot.empty()) {
    Bytes back(tail_boot.size());
    st = target.read_at(info.device_size - tail_boot.size(), back.data(), back.size());
    if (!st.ok() || back != tail_boot) return fail(Errc::Corrupt, "verify-failed", "the backup boot sector does not match");
  }
  res.verified = true;
  res.status.st = Status::Ok();
  res.status.code = "ok";
  return res;
}

}  // namespace usbmem
