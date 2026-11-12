// SPDX-License-Identifier: Apache-2.0
#include "usbmem/c_api_disktools.h"

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>

#include "usbmem/c_api_internal.h"
#include "usbmem/fscheck.h"
#include "usbmem/mkfs.h"

using namespace usbmem;

struct usbmem_job {
  CancelToken cancel;
  std::mutex mu;
  std::string phase;
  std::atomic<uint64_t> done{0}, total{0};
  void set(const char* ph, uint64_t d, uint64_t t) {
    {
      std::lock_guard<std::mutex> l(mu);
      if (phase != ph) phase = ph;
    }
    done.store(d, std::memory_order_relaxed);
    total.store(t, std::memory_order_relaxed);
  }
};

struct usbmem_mkfs {
  BlockDevicePtr dev;
  MkfsPlan plan;
  bool has_plan = false;
  std::vector<uint8_t> last_backup;
  std::string err_code, err_msg;
};

namespace {

thread_local std::string t_open_code, t_open_msg;

int dup_string(const std::string& s, char** out) {
  char* p = static_cast<char*>(std::malloc(s.size() + 1));
  if (!p) return static_cast<int>(Errc::NoMemory);
  std::memcpy(p, s.c_str(), s.size() + 1);
  *out = p;
  return 0;
}

int dup_bytes(const std::vector<uint8_t>& v, unsigned char** out, size_t* len) {
  unsigned char* p = static_cast<unsigned char*>(std::malloc(v.empty() ? 1 : v.size()));
  if (!p) return static_cast<int>(Errc::NoMemory);
  if (!v.empty()) std::memcpy(p, v.data(), v.size());
  *out = p;
  *len = v.size();
  return 0;
}

void copy_out(const std::string& s, char* dst, size_t n) {
  if (!dst || n == 0) return;
  size_t k = s.size() < n - 1 ? s.size() : n - 1;
  std::memcpy(dst, s.data(), k);
  dst[k] = 0;
}

FsType parse_fs(const char* n) {
  std::string s = n ? n : "";
  if (s == "fat12") return FsType::Fat12;
  if (s == "fat16") return FsType::Fat16;
  if (s == "fat32") return FsType::Fat32;
  if (s == "exfat") return FsType::Exfat;
  if (s == "ext2") return FsType::Ext2;
  if (s == "ext3") return FsType::Ext3;
  if (s == "ext4") return FsType::Ext4;
  if (s == "ntfs") return FsType::Ntfs;
  return FsType::Unknown;
}

Status write_backup_file(const std::string& path, const std::vector<uint8_t>& data) {
  int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) return Status::Make(Errc::Io, errno);
  size_t off = 0;
  while (off < data.size()) {
    ssize_t n = ::write(fd, data.data() + off, data.size() - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      int e = errno;
      ::close(fd);
      return Status::Make(Errc::Io, e);
    }
    off += static_cast<size_t>(n);
  }
  if (::fsync(fd) != 0) {
    int e = errno;
    ::close(fd);
    return Status::Make(Errc::Io, e);
  }
  if (::close(fd) != 0) return Status::Make(Errc::Io, errno);
  return Status::Ok();
}

}  // namespace

extern "C" {

usbmem_job* usbmem_job_new(void) { return new (std::nothrow) usbmem_job(); }
void usbmem_job_free(usbmem_job* j) { delete j; }
void usbmem_job_cancel(usbmem_job* j) { if (j) j->cancel.cancel(); }
void usbmem_job_progress(usbmem_job* j, char* phase, size_t phase_len, unsigned long long* done, unsigned long long* total) {
  if (!j) return;
  if (phase) {
    std::lock_guard<std::mutex> l(j->mu);
    copy_out(j->phase, phase, phase_len);
  }
  if (done) *done = j->done.load(std::memory_order_relaxed);
  if (total) *total = j->total.load(std::memory_order_relaxed);
}

int usbmem_fscheck_supported(const char* fs_name) {
  FsType fs = parse_fs(fs_name);
  return fs != FsType::Unknown && fscheck_supported(fs) ? 1 : 0;
}

int usbmem_fscheck_run(usbmem_device* dev, unsigned long long offset, unsigned long long length, const char* fs_name, unsigned max_findings,
                       unsigned long long memory_limit, usbmem_job* job, char** out_json) {
  if (!dev || !dev->dev || !out_json) return static_cast<int>(Errc::InvalidArgument);
  *out_json = nullptr;
  try {
    FsType fs = FsType::Unknown;
    if (fs_name && *fs_name) {
      fs = parse_fs(fs_name);
      if (fs == FsType::Unknown) return static_cast<int>(Errc::InvalidArgument);
    }
    FsCheckOptions o;
    if (max_findings) o.max_findings = max_findings;
    if (memory_limit) o.memory_limit = memory_limit;
    if (job) {
      o.cancel = &job->cancel;
      o.progress = [job](const char* ph, uint64_t d, uint64_t t) { job->set(ph, d, t); };
    }
    uint64_t len = length ? length : (offset <= dev->dev->size() ? dev->dev->size() - offset : 0);
    FsCheckReport rep;
    Status st = check_filesystem(*dev->dev, offset, len, fs, o, &rep);
    if (!st.ok() && st.code != Errc::Cancelled) return static_cast<int>(st.code);
    int dr = dup_string(fscheck_report_to_json(rep), out_json);
    return dr ? dr : static_cast<int>(st.code);
  } catch (...) { return static_cast<int>(Errc::NoMemory); }
}

int usbmem_mkfs_open(usbmem_device* dev, const char* fs_name, unsigned long long offset, unsigned long long length, const char* label,
                     unsigned cluster_size, unsigned sector_size, unsigned inode_ratio, unsigned inode_size, int in_use, unsigned long long seed,
                     unsigned long long timestamp, usbmem_mkfs** out) {
  if (!dev || !dev->dev || !out) return static_cast<int>(Errc::InvalidArgument);
  *out = nullptr;
  try {
    MkfsOptions o;
    o.fs = parse_fs(fs_name);
    if (!Mkfs::supported(o.fs)) {
      t_open_code = "unsupported-fs";
      t_open_msg = "only ext2, exFAT and FAT32 can be created";
      return static_cast<int>(Errc::Unsupported);
    }
    o.offset = offset;
    o.length = length;
    if (label) o.label = label;
    o.cluster_size = cluster_size;
    o.sector_size = sector_size;
    o.inode_ratio = inode_ratio;
    o.inode_size = inode_size;
    o.in_use = in_use != 0;
    o.seed = seed;
    o.timestamp = timestamp;
    auto* h = new (std::nothrow) usbmem_mkfs();
    if (!h) return static_cast<int>(Errc::NoMemory);
    h->dev = dev->dev;
    MkfsResult r = Mkfs::plan(*h->dev, o, &h->plan);
    if (!r.ok()) {
      t_open_code = r.code;
      t_open_msg = r.message;
      delete h;
      return static_cast<int>(r.status.code);
    }
    h->has_plan = h->plan.ok;
    *out = h;
    return 0;
  } catch (...) { return static_cast<int>(Errc::NoMemory); }
}

void usbmem_mkfs_close(usbmem_mkfs* h) { delete h; }

void usbmem_mkfs_last_error(usbmem_mkfs* h, char* code, size_t code_len, char* msg, size_t msg_len) {
  copy_out(h ? h->err_code : t_open_code, code, code_len);
  copy_out(h ? h->err_msg : t_open_msg, msg, msg_len);
}

int usbmem_mkfs_plan_json(usbmem_mkfs* h, char** out) {
  if (!h || !out) return static_cast<int>(Errc::InvalidArgument);
  *out = nullptr;
  try { return dup_string(mkfs_plan_to_json(h->plan), out); } catch (...) { return static_cast<int>(Errc::NoMemory); }
}

int usbmem_mkfs_make_token(usbmem_mkfs* h, const char* typed, char** out) {
  if (!h || !typed || !out) return static_cast<int>(Errc::InvalidArgument);
  *out = nullptr;
  if (!h->has_plan) {
    h->err_code = "no-plan";
    h->err_msg = "there is no applicable plan";
    return static_cast<int>(Errc::InvalidArgument);
  }
  try { return dup_string(Mkfs::make_confirm_token(h->plan, typed), out); } catch (...) { return static_cast<int>(Errc::NoMemory); }
}

int usbmem_mkfs_apply(usbmem_mkfs* h, const char* token, const char* backup_path, unsigned long long timestamp, int in_use, usbmem_job* job,
                      char** out) {
  if (!h || !token || !out) return static_cast<int>(Errc::InvalidArgument);
  *out = nullptr;
  try {
    MkfsResult r;
    if (!h->has_plan) {
      r.status = Status::Make(Errc::InvalidArgument);
      r.code = "no-plan";
      r.message = "there is no applicable plan";
    } else {
      MkfsApplyOptions ao;
      ao.timestamp = timestamp;
      ao.in_use = in_use != 0;
      std::string path = backup_path ? backup_path : "";
      ao.backup_sink = [h, path](const std::vector<uint8_t>& b) {
        h->last_backup = b;
        return path.empty() ? Status::Ok() : write_backup_file(path, b);
      };
      if (job) {
        ao.cancel = &job->cancel;
        ao.progress = [job](const char* ph, uint64_t d, uint64_t t) { job->set(ph, d, t); };
      }
      r = Mkfs::apply(*h->dev, h->plan, token, ao);
      // 書き込みに入った後の失敗・完了では、この計画はもう使えない (ディスクの内容が変わっている)
      if (r.ok() || r.code == "write-failed" || r.code == "flush-failed" || r.code == "verify-failed" || r.code == "cancelled") h->has_plan = false;
    }
    h->err_code = r.code;
    h->err_msg = r.message;
    int dr = dup_string(mkfs_result_to_json(r), out);
    return dr ? dr : static_cast<int>(r.status.code);
  } catch (...) { return static_cast<int>(Errc::NoMemory); }
}

unsigned long long usbmem_mkfs_min_size(const char* fs_name, unsigned sector_size) { return Mkfs::min_size(parse_fs(fs_name), sector_size); }

int usbmem_mkfs_last_backup(usbmem_mkfs* h, unsigned char** out, size_t* len) {
  if (!h || !out || !len) return static_cast<int>(Errc::InvalidArgument);
  *out = nullptr;
  if (h->last_backup.empty()) return static_cast<int>(Errc::InvalidArgument);
  try { return dup_bytes(h->last_backup, out, len); } catch (...) { return static_cast<int>(Errc::NoMemory); }
}

int usbmem_mkfs_backup_info_json(const unsigned char* data, size_t len, char** out) {
  if (!data || !out) return static_cast<int>(Errc::InvalidArgument);
  *out = nullptr;
  try {
    MkfsBackupInfo bi;
    Status st = parse_mkfs_backup(std::vector<uint8_t>(data, data + len), &bi);
    if (!st.ok()) return static_cast<int>(st.code);
    std::string j = "{\"version\":" + std::to_string(bi.version) + ",\"timestamp\":\"" + std::to_string(bi.timestamp) + "\",\"rangeOffset\":\"" +
                    std::to_string(bi.range_offset) + "\",\"rangeLength\":\"" + std::to_string(bi.range_length) + "\",\"regions\":" +
                    std::to_string(bi.regions) + ",\"bytes\":\"" + std::to_string(bi.bytes) + "\"}";
    return dup_string(j, out);
  } catch (...) { return static_cast<int>(Errc::NoMemory); }
}

int usbmem_mkfs_restore_backup(usbmem_device* dev, const unsigned char* data, size_t len) {
  if (!dev || !dev->dev || !data) return static_cast<int>(Errc::InvalidArgument);
  try { return static_cast<int>(mkfs_restore_backup(*dev->dev, std::vector<uint8_t>(data, data + len)).code); } catch (...) { return static_cast<int>(Errc::NoMemory); }
}

}  // extern "C"
