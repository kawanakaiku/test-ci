// SPDX-License-Identifier: Apache-2.0
// Partclone イメージ形式 (format 0002、ビットマップは BIT、チェックサムは CRC32) の書き出し・読み取り・検査。
// 形式は partclone 0.3.49 が実際に作ったイメージから解読して確かめた (docs/recovery-backup.md)。
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "backup_internal.h"

namespace usbmem {
using namespace backup_detail;

// ---------------------------------------------------------------------------------------------------- 入出力
Status MemorySource::read(void* buf, size_t len, size_t* got) {
  size_t n = std::min(len, d_->size() - pos_);
  if (n) std::memcpy(buf, d_->data() + pos_, n);
  pos_ += n;
  *got = n;
  return Status::Ok();
}

Status FileSink::create(const std::string& path, std::unique_ptr<FileSink>* out) {
  int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);  // 画像には私的なデータが入る
  if (fd < 0) return Status::Make(Errc::Io, errno);
  FILE* f = ::fdopen(fd, "wb");
  if (!f) { int e = errno; ::close(fd); return Status::Make(Errc::Io, e); }
  std::unique_ptr<FileSink> s(new FileSink());
  s->f_ = f;
  *out = std::move(s);
  return Status::Ok();
}
FileSink::~FileSink() { if (f_) std::fclose(f_); }
Status FileSink::write(const void* d, size_t n) {
  if (!f_) return Status::Make(Errc::Closed);
  if (n && std::fwrite(d, 1, n, f_) != n) return Status::Make(Errc::Io, errno ? errno : EIO);
  return Status::Ok();
}
Status FileSink::finish() {
  if (!f_) return Status::Ok();
  if (std::fflush(f_) != 0) return Status::Make(Errc::Io, errno ? errno : EIO);
  if (::fsync(::fileno(f_)) != 0) return Status::Make(Errc::Io, errno ? errno : EIO);
  int rc = std::fclose(f_);
  f_ = nullptr;
  return rc == 0 ? Status::Ok() : Status::Make(Errc::Io, errno ? errno : EIO);
}

Status FileSource::open(const std::string& path, std::unique_ptr<FileSource>* out) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return Status::Make(Errc::Io, errno);
  std::unique_ptr<FileSource> s(new FileSource());
  s->f_ = f;
  *out = std::move(s);
  return Status::Ok();
}
FileSource::~FileSource() { if (f_) std::fclose(f_); }
Status FileSource::read(void* buf, size_t len, size_t* got) {
  *got = std::fread(buf, 1, len, f_);
  if (*got < len && std::ferror(f_)) return Status::Make(Errc::Io, errno ? errno : EIO);
  return Status::Ok();
}

// ---------------------------------------------------------------------------------------------------- 書き出し
namespace backup_detail {
uint32_t default_blocks_per_checksum(uint32_t bs) { return std::max<uint32_t>(1, (1u << 20) / std::max<uint32_t>(bs, 1)); }

Bytes make_image_head(const UsedMap& m, uint32_t bpc) {
  Bytes h(kHeaderBytes, 0);
  std::memcpy(h.data(), "partclone-image", 15);
  std::memcpy(h.data() + 16, "usbmem1", 7);       // ptc_version (14)
  std::memcpy(h.data() + 30, "0002", 4);          // version
  put_le(h, 34, 0xC0DE, 2);                       // endianness
  std::memcpy(h.data() + 36, m.fs.data(), std::min<size_t>(m.fs.size(), 15));  // fs[16]
  put_le(h, 0x34, m.device_size, 8);
  put_le(h, 0x3c, m.total_blocks, 8);
  put_le(h, 0x44, m.used_blocks, 8);              // usedblocks
  put_le(h, 0x4c, m.used_blocks, 8);              // used_bitmap
  put_le(h, 0x54, m.block_size, 4);
  put_le(h, 0x58, 18, 4);                         // feature_size (options + CRC)
  put_le(h, 0x5c, 2, 2);                          // image_version
  put_le(h, 0x5e, 64, 2);                         // cpu_bits
  put_le(h, 0x60, kCsmCrc32, 2);                  // checksum_mode
  put_le(h, 0x62, 4, 2);                          // checksum_size
  put_le(h, 0x64, bpc, 4);                        // blocks_per_checksum
  h[0x68] = 1;                                    // reseed_checksum
  h[0x69] = kBitmapBit;                           // bitmap_mode
  put_le(h, 0x6a, pc_crc(h.data(), 0x6a), 4);
  return h;
}
}  // namespace backup_detail

BackupResult backup_filesystem(BlockDevice& dev, ByteSink& raw_sink, const BackupOptions& opt) {
  BackupResult res;
  UsedMap m;
  res.status = build_used_map(dev, opt.extract, &m);
  if (!res.status.ok()) return res;
  auto sfail = [&](const Status& s, const char* code, const std::string& msg) {
    res.status.st = s;
    res.status.code = code;
    res.status.message = msg;
    return res;
  };
  const uint32_t bs = m.block_size;
  const uint32_t bpc = default_blocks_per_checksum(bs);
  HashingSink sink(raw_sink);

  Bytes head = make_image_head(m, bpc);
  Status st = sink.write(head.data(), head.size());
  if (st.ok()) {
    size_t nbytes = static_cast<size_t>((m.total_blocks + 7) / 8);
    st = sink.write(m.bits.data(), nbytes);
    uint8_t bcrc[4];
    uint32_t c = pc_crc(m.bits.data(), nbytes);
    for (int i = 0; i < 4; i++) bcrc[i] = static_cast<uint8_t>(c >> (8 * i));
    if (st.ok()) st = sink.write(bcrc, 4);
  }
  if (!st.ok()) return sfail(st, "write-error", "could not write the image header");

  const uint64_t max_blocks_per_read = std::max<uint64_t>(1, opt.read_chunk_bytes / bs);
  Bytes buf;
  PcCrc crc;
  uint64_t in_chunk = 0, done = 0, total = m.total_blocks;
  uint64_t b = 0;
  while (b < total) {
    // 次の使用中ブロックの連なり [b0, b1)
    while (b < total && (b & 7) == 0 && b + 8 <= total && m.bits[b >> 3] == 0) b += 8;
    while (b < total && !m.test(b)) b++;
    if (b >= total) break;
    uint64_t b0 = b;
    while (b < total && m.test(b)) b++;
    for (uint64_t pos = b0; pos < b;) {
      if (opt.cancel && opt.cancel->cancelled()) return sfail(Status::Make(Errc::Cancelled), "cancelled", "the backup was cancelled");
      uint64_t n = std::min(b - pos, max_blocks_per_read);
      buf.resize(static_cast<size_t>(n * bs));
      st = dev.read_at(pos * bs, buf.data(), buf.size());
      if (!st.ok()) {
        res.status.st = st;
        res.status.code = "read-error";
        res.status.message = "could not read block " + std::to_string(pos) + " of the source (use data rescue first, then back up the rescued image)";
        return res;
      }
      for (uint64_t k = 0; k < n;) {
        uint64_t take = std::min(n - k, bpc - in_chunk);
        const uint8_t* p = buf.data() + k * bs;
        crc.update(p, static_cast<size_t>(take * bs));
        st = sink.write(p, static_cast<size_t>(take * bs));
        if (!st.ok()) return sfail(st, "write-error", "could not write the image");
        k += take;
        in_chunk += take;
        if (in_chunk == bpc) {
          uint8_t c4[4];
          uint32_t cv = crc.value();
          for (int i = 0; i < 4; i++) c4[i] = static_cast<uint8_t>(cv >> (8 * i));
          st = sink.write(c4, 4);
          if (!st.ok()) return sfail(st, "write-error", "could not write the image");
          in_chunk = 0;
          crc.reset();
        }
      }
      pos += n;
      done += n;
      if (opt.progress) opt.progress(done, m.used_blocks);
    }
  }
  if (in_chunk) {
    uint8_t c4[4];
    uint32_t cv = crc.value();
    for (int i = 0; i < 4; i++) c4[i] = static_cast<uint8_t>(cv >> (8 * i));
    st = sink.write(c4, 4);
    if (!st.ok()) return sfail(st, "write-error", "could not write the image");
  }
  st = sink.finish();
  if (!st.ok()) return sfail(st, "write-error", "could not finish the image");
  res.image_sha256 = sink.digest();
  res.image_bytes = sink.bytes();
  m.bits.clear();
  m.bits.shrink_to_fit();
  res.map = std::move(m);
  return res;
}

// ---------------------------------------------------------------------------------------------------- 読み取り
namespace {
FsStatus ifail(Errc e, const char* code, const std::string& msg) {
  FsStatus f;
  f.st = Status::Make(e);
  f.code = code;
  f.message = msg;
  return f;
}
}  // namespace

FsStatus read_image_info(ByteSource& src, ImageInfo* info, std::vector<uint8_t>* bits_out, uint64_t max_bitmap_bytes) {
  Bytes h(kHeaderBytes);
  Status st = read_exact(src, h.data(), h.size());
  if (!st.ok()) return st.code == Errc::Corrupt ? ifail(Errc::Corrupt, "truncated", "the image is too short to hold a header") : FsStatus{st, "read-error", "could not read the image"};
  if (std::memcmp(h.data(), "partclone-image\0", 16) != 0) return ifail(Errc::Corrupt, "not-partclone", "this is not a Partclone image");
  std::string version(reinterpret_cast<char*>(&h[30]), 4);
  if (version == "0001") return ifail(Errc::Unsupported, "old-format", "Partclone image format 0001 is not supported (only 0002)");
  if (version != "0002") return ifail(Errc::Corrupt, "bad-version", "unknown image format version");
  if (get_le(&h[34], 2) != 0xC0DE) return ifail(Errc::Unsupported, "bad-endian", "the image was written on a machine with a different byte order");
  if (get_le(&h[0x58], 4) != 18 || get_le(&h[0x5c], 2) != 2) return ifail(Errc::Unsupported, "bad-options", "unknown image options layout");
  if (get_le(&h[0x6a], 4) != pc_crc(h.data(), 0x6a)) return ifail(Errc::Corrupt, "header-checksum", "the header checksum does not match");

  ImageInfo i;
  i.version = version;
  i.ptc_version.assign(reinterpret_cast<char*>(&h[16]), strnlen(reinterpret_cast<char*>(&h[16]), 14));
  i.fs.assign(reinterpret_cast<char*>(&h[36]), strnlen(reinterpret_cast<char*>(&h[36]), 16));
  i.device_size = get_le(&h[0x34], 8);
  i.total_blocks = get_le(&h[0x3c], 8);
  i.used_blocks = get_le(&h[0x44], 8);
  i.used_bitmap = get_le(&h[0x4c], 8);
  i.block_size = static_cast<uint32_t>(get_le(&h[0x54], 4));
  i.checksum_mode = static_cast<uint16_t>(get_le(&h[0x60], 2));
  i.checksum_size = static_cast<uint16_t>(get_le(&h[0x62], 2));
  i.blocks_per_checksum = static_cast<uint32_t>(get_le(&h[0x64], 4));
  i.reseed = h[0x68] != 0;
  i.bitmap_mode = h[0x69];
  i.header_bytes = kHeaderBytes;
  i.header_sha256 = hex_encode(Sha256::hash(h.data(), h.size()));

  if (i.bitmap_mode != kBitmapBit) return ifail(Errc::Unsupported, "bitmap-mode", "only the bit-per-block bitmap mode is supported");
  if (i.block_size < 512 || i.block_size > (1u << 20) || (i.block_size & (i.block_size - 1))) return ifail(Errc::Corrupt, "bad-block-size", "implausible block size");
  if (i.total_blocks == 0 || i.total_blocks > (1ull << 44) || i.total_blocks > i.device_size / i.block_size) return ifail(Errc::Corrupt, "bad-geometry", "block count and device size do not match");
  if (i.used_blocks > i.total_blocks || i.used_bitmap > i.total_blocks) return ifail(Errc::Corrupt, "bad-geometry", "used block count exceeds the total");
  if (i.checksum_mode == kCsmCrc32) {
    if (i.checksum_size != 4) return ifail(Errc::Unsupported, "checksum-size", "unsupported checksum size");
    if (i.blocks_per_checksum == 0 || i.blocks_per_checksum > (1u << 20) || static_cast<uint64_t>(i.blocks_per_checksum) * i.block_size > kMaxChunkBytes)
      return ifail(Errc::Corrupt, "bad-checksum-interval", "implausible checksum interval");
  } else if (i.checksum_mode != kCsmNone) {
    return ifail(Errc::Unsupported, "checksum-mode", "unsupported checksum algorithm");
  }
  i.bitmap_bytes = (i.total_blocks + 7) / 8;
  if (i.bitmap_bytes > max_bitmap_bytes) return ifail(Errc::NoMemory, "bitmap-too-large", "the bitmap would be too large");

  // ビットマップは届いた分だけ確保しながら読む (嘘のヘッダで巨大な確保をしない)
  Bytes bits;
  Sha256 bh;
  uint64_t left = i.bitmap_bytes;
  while (left) {
    size_t n = static_cast<size_t>(std::min<uint64_t>(left, 1u << 20));
    size_t at = bits.size();
    bits.resize(at + n);
    st = read_exact(src, bits.data() + at, n);
    if (!st.ok()) return st.code == Errc::Corrupt ? ifail(Errc::Corrupt, "truncated", "the image ends inside the bitmap") : FsStatus{st, "read-error", "could not read the image"};
    bh.update(bits.data() + at, n);
    left -= n;
  }
  i.bitmap_sha256 = hex_encode(bh.finish());
  uint8_t c4[4];
  st = read_exact(src, c4, 4);
  if (!st.ok()) return st.code == Errc::Corrupt ? ifail(Errc::Corrupt, "truncated", "the image ends before the bitmap checksum") : FsStatus{st, "read-error", "could not read the image"};
  if (get_le(c4, 4) != pc_crc(bits.data(), bits.size())) return ifail(Errc::Corrupt, "bitmap-checksum", "the bitmap checksum does not match");
  i.header_bytes = kHeaderBytes + i.bitmap_bytes + 4;

  // 末尾の余りビットは無視して数える
  uint64_t cnt = 0, first = UINT64_MAX, last = 0;
  for (uint64_t by = 0; by < bits.size(); by++) {
    uint8_t v = bits[static_cast<size_t>(by)];
    if (by == bits.size() - 1 && (i.total_blocks & 7)) v &= static_cast<uint8_t>((1u << (i.total_blocks & 7)) - 1);
    if (!v) continue;
    cnt += static_cast<uint64_t>(__builtin_popcount(v));
    if (first == UINT64_MAX) first = by * 8 + static_cast<uint64_t>(__builtin_ctz(v));
    last = by * 8 + 7 - static_cast<uint64_t>(__builtin_clz(static_cast<unsigned>(v) << 24));
  }
  i.counted_used = cnt;
  i.first_used = first == UINT64_MAX ? 0 : first;
  i.last_used = last;
  if (cnt != i.used_bitmap && cnt != i.used_blocks) return ifail(Errc::Corrupt, "used-count-mismatch", "the bitmap does not match the used block count in the header");
  *info = i;
  if (bits_out) *bits_out = std::move(bits);
  return FsStatus{Status::Ok(), "ok", ""};
}

namespace backup_detail {
void BlockStream::select(const std::vector<uint8_t>& bits, uint64_t total, uint64_t per_chunk, uint64_t* cursor, std::vector<uint64_t>* blocks) {
  blocks->clear();
  while (blocks->size() < per_chunk && *cursor < total) {
    uint64_t c = *cursor;
    if ((c & 7) == 0 && c + 8 <= total && bits[c >> 3] == 0) { *cursor += 8; continue; }
    if ((bits[c >> 3] >> (c & 7)) & 1) blocks->push_back(c);
    ++*cursor;
  }
}

FsStatus BlockStream::next(std::vector<uint64_t>* blocks, Bytes* data, uint32_t* crc, bool* done) {
  *done = false;
  select(bits_, info_.total_blocks, per_chunk_, &cursor_, blocks);
  if (blocks->empty()) { *done = true; return FsStatus{Status::Ok(), "ok", ""}; }
  produced_ += blocks->size();
  size_t bytes = static_cast<size_t>(blocks->size() * info_.block_size);
  data->resize(bytes);
  Status st = read_exact(src_, data->data(), bytes);
  if (!st.ok()) return st.code == Errc::Corrupt ? ifail(Errc::Corrupt, "truncated", "the image ends inside the block data") : FsStatus{st, "read-error", "could not read the image"};
  *crc = 0;
  if (info_.checksum_mode == kCsmCrc32) {
    uint8_t c4[4];
    st = read_exact(src_, c4, 4);
    if (!st.ok()) return st.code == Errc::Corrupt ? ifail(Errc::Corrupt, "truncated", "the image ends before a block checksum") : FsStatus{st, "read-error", "could not read the image"};
    if (info_.reseed || !running_init_) running_.reset();
    running_init_ = true;
    running_.update(data->data(), bytes);
    *crc = running_.value();
    if (get_le(c4, 4) != *crc) return ifail(Errc::Corrupt, "checksum-mismatch", "the data checksum of chunk " + std::to_string(chunk_) + " does not match");
  }
  chunk_++;
  return FsStatus{Status::Ok(), "ok", ""};
}

FsStatus BlockStream::expect_end() {
  uint8_t x;
  size_t got = 0;
  Status st = src_.read(&x, 1, &got);
  if (!st.ok()) return FsStatus{st, "read-error", "could not read the image"};
  if (got != 0) return ifail(Errc::Corrupt, "trailing-data", "the image has extra data after the last block");
  return FsStatus{Status::Ok(), "ok", ""};
}
}  // namespace backup_detail

VerifyResult verify_image(ByteSource& src, const CancelToken* cancel, const std::function<void(uint64_t, uint64_t)>& progress) {
  VerifyResult r;
  HashingSource hs(src);
  std::vector<uint8_t> bits;
  r.status = read_image_info(hs, &r.info, &bits);
  if (!r.status.ok()) return r;
  BlockStream bsr(hs, r.info, bits);
  std::vector<uint64_t> blocks;
  Bytes data;
  for (;;) {
    if (cancel && cancel->cancelled()) { r.status = ifail(Errc::Cancelled, "cancelled", "the verification was cancelled"); return r; }
    uint32_t crc;
    bool done;
    r.status = bsr.next(&blocks, &data, &crc, &done);
    if (!r.status.ok()) return r;
    if (done) break;
    r.blocks_checked += blocks.size();
    r.checksums_checked++;
    if (progress) progress(r.blocks_checked, r.info.counted_used);
  }
  r.status = bsr.expect_end();
  if (!r.status.ok()) return r;
  r.image_sha256 = hs.digest();
  r.image_bytes = hs.bytes();
  return r;
}

}  // namespace usbmem
