// SPDX-License-Identifier: Apache-2.0
#include "usbmem/c_api_partscan.h"

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

#include "c_api_partedit_internal.h"
#include "usbmem/bootfix.h"
#include "usbmem/c_api_internal.h"
#include "usbmem/partition_recover.h"
#include "usbmem/partition_scan.h"

using namespace usbmem;

struct usbmem_pjob {
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

struct usbmem_partscan {
  BlockDevicePtr dev;
  ScanResult result;
  std::vector<RecoveryProposal> proposals;
  bool have_proposals = false;
  uint32_t sector_size = 0;
};

struct usbmem_bootfix {
  BlockDevicePtr dev;
  uint64_t offset = 0, length = 0;
  FsType fs = FsType::Unknown;
  BootInspection insp;
  BootRepairPlan plan;
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

std::string json_str(const std::string& s) {
  std::string o = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += static_cast<char>(c); }
    else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
    else o += static_cast<char>(c);
  }
  return o + "\"";
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

}  // namespace

extern "C" {

usbmem_pjob* usbmem_pjob_new(void) {
  try { return new usbmem_pjob(); } catch (...) { return nullptr; }
}
void usbmem_pjob_free(usbmem_pjob* j) { delete j; }
void usbmem_pjob_cancel(usbmem_pjob* j) { if (j) j->cancel.cancel(); }
void usbmem_pjob_progress(usbmem_pjob* j, char* phase, size_t phase_len, unsigned long long* done, unsigned long long* total) {
  if (!j) {
    copy_out("", phase, phase_len);
    if (done) *done = 0;
    if (total) *total = 0;
    return;
  }
  std::string ph;
  {
    std::lock_guard<std::mutex> l(j->mu);
    ph = j->phase;
  }
  copy_out(ph, phase, phase_len);
  if (done) *done = j->done.load(std::memory_order_relaxed);
  if (total) *total = j->total.load(std::memory_order_relaxed);
}

// ------------------------------------------------------------------ 探索

int usbmem_partscan_run(usbmem_device* dev, unsigned sector_size, int level, int min_confidence, usbmem_pjob* job, usbmem_partscan** out) {
  if (!dev || !out) return static_cast<int>(Errc::InvalidArgument);
  *out = nullptr;
  try {
    auto* h = new (std::nothrow) usbmem_partscan();
    if (!h) return static_cast<int>(Errc::NoMemory);
    h->dev = dev->dev;
    ScanOptions o;
    o.sector_size = sector_size;
    o.level = level == 1 ? ScanLevel::Full : ScanLevel::Quick;
    if (min_confidence > 0) o.min_confidence = min_confidence > 100 ? 100 : min_confidence;
    if (job) {
      o.cancel = &job->cancel;
      o.progress = [job](const char* ph, uint64_t d, uint64_t t) { job->set(ph, d, t); };
    }
    Status st = scan_partitions(*dev->dev, o, &h->result);
    h->sector_size = h->result.sector_size;
    if (!st.ok() && st.code != Errc::Cancelled) {
      delete h;
      return static_cast<int>(st.code);
    }
    *out = h;
    return static_cast<int>(st.code);
  } catch (...) { return static_cast<int>(Errc::NoMemory); }
}

void usbmem_partscan_close(usbmem_partscan* h) { delete h; }

int usbmem_partscan_result_json(usbmem_partscan* h, char** out) {
  if (!h || !out) return static_cast<int>(Errc::InvalidArgument);
  *out = nullptr;
  try { return dup_string(scan_result_to_json(h->result), out); } catch (...) { return static_cast<int>(Errc::NoMemory); }
}

int usbmem_partscan_proposals_json(usbmem_partscan* h, char** out) {
  if (!h || !out) return static_cast<int>(Errc::InvalidArgument);
  *out = nullptr;
  try {
    if (!h->have_proposals) {
      RecoveryOptions ro;
      Status st = propose_table_recovery(*h->dev, h->result, ro, &h->proposals);
      if (!st.ok()) return static_cast<int>(st.code);
      h->have_proposals = true;
    }
    return dup_string(recovery_proposals_to_json(h->proposals), out);
  } catch (...) { return static_cast<int>(Errc::NoMemory); }
}

int usbmem_partscan_queue_proposal(usbmem_partscan* h, int idx, usbmem_partedit* ed) {
  if (!h || !ed || !h->have_proposals || idx < 0 || static_cast<size_t>(idx) >= h->proposals.size()) return static_cast<int>(Errc::InvalidArgument);
  try {
    EditResult r = queue_recovery_proposal(*ed->ed, h->proposals[static_cast<size_t>(idx)]);
    ed->has_plan = false;
    ed->err_code = r.code;
    ed->err_msg = r.message;
    return static_cast<int>(r.status.code);
  } catch (...) { return static_cast<int>(Errc::NoMemory); }
}

// ------------------------------------------------------------------ ブートセクタ

int usbmem_bootfix_open(usbmem_device* dev, unsigned long long offset, unsigned long long length, const char* fs_name, usbmem_pjob* job, usbmem_bootfix** out) {
  if (!dev || !out) return static_cast<int>(Errc::InvalidArgument);
  *out = nullptr;
  try {
    auto* h = new (std::nothrow) usbmem_bootfix();
    if (!h) return static_cast<int>(Errc::NoMemory);
    h->dev = dev->dev;
    h->offset = offset;
    h->length = length;
    h->fs = parse_fs(fs_name);
    BootInspectOptions io;
    if (job) io.cancel = &job->cancel;
    Status st = inspect_boot(*dev->dev, offset, length, h->fs, io, &h->insp);
    if (!st.ok()) {
      t_open_code = st.code == Errc::Cancelled ? "cancelled" : "io-error";
      t_open_msg = "inspection failed";
      delete h;
      return static_cast<int>(st.code);
    }
    *out = h;
    return 0;
  } catch (...) { return static_cast<int>(Errc::NoMemory); }
}

void usbmem_bootfix_close(usbmem_bootfix* h) { delete h; }

void usbmem_bootfix_last_error(usbmem_bootfix* h, char* code, size_t code_len, char* msg, size_t msg_len) {
  copy_out(h ? h->err_code : t_open_code, code, code_len);
  copy_out(h ? h->err_msg : t_open_msg, msg, msg_len);
}

int usbmem_bootfix_inspection_json(usbmem_bootfix* h, char** out) {
  if (!h || !out) return static_cast<int>(Errc::InvalidArgument);
  *out = nullptr;
  try { return dup_string(boot_inspection_to_json(h->insp), out); } catch (...) { return static_cast<int>(Errc::NoMemory); }
}

int usbmem_bootfix_plan_json(usbmem_bootfix* h, const char* repair_id, unsigned long long source, char** out) {
  if (!h || !repair_id || !out) return static_cast<int>(Errc::InvalidArgument);
  *out = nullptr;
  try {
    h->has_plan = false;
    Status st = plan_boot_repair(*h->dev, h->offset, h->length, h->fs == FsType::Unknown ? h->insp.fs : h->fs, repair_id, source, &h->plan);
    if (!st.ok()) return static_cast<int>(st.code);
    h->has_plan = h->plan.ok;
    if (!h->plan.ok && !h->plan.problems.empty()) { h->err_code = h->plan.problems[0].code; h->err_msg = h->plan.problems[0].message; }
    return dup_string(boot_plan_to_json(h->plan), out);
  } catch (...) { return static_cast<int>(Errc::NoMemory); }
}

int usbmem_bootfix_make_token(usbmem_bootfix* h, const char* typed, char** out) {
  if (!h || !typed || !out) return static_cast<int>(Errc::InvalidArgument);
  *out = nullptr;
  if (!h->has_plan) {
    h->err_code = "no-plan";
    h->err_msg = "make a plan first";
    return static_cast<int>(Errc::InvalidArgument);
  }
  try { return dup_string(make_boot_confirm_token(h->plan, typed), out); } catch (...) { return static_cast<int>(Errc::NoMemory); }
}

int usbmem_bootfix_apply(usbmem_bootfix* h, const char* token, const char* backup_path, unsigned long long timestamp, int in_use, usbmem_pjob* job, char** out) {
  if (!h || !token || !out) return static_cast<int>(Errc::InvalidArgument);
  *out = nullptr;
  try {
    BootApplyResult r;
    if (!h->has_plan) {
      r.status = Status::Make(Errc::InvalidArgument);
      r.code = "no-plan";
      r.message = "make a plan first";
    } else {
      BootApplyOptions ao;
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
      r = apply_boot_repair(*h->dev, h->plan, token, ao);
      if (r.ok() || r.code == "write-failed" || r.code == "verify-failed" || r.code == "cancelled") h->has_plan = false;
    }
    h->err_code = r.code;
    h->err_msg = r.message;
    std::string j = "{\"ok\":" + std::string(r.ok() ? "true" : "false") + ",\"code\":" + json_str(r.code) + ",\"message\":" + json_str(r.message) +
                    ",\"writesDone\":" + std::to_string(r.writes_done) + ",\"rolledBack\":" + (r.rolled_back ? "true" : "false") +
                    ",\"verified\":" + (r.verified ? "true" : "false") + ",\"verifyFindings\":[";
    for (size_t i = 0; i < r.verify_findings.size(); i++) {
      if (i) j += ',';
      const auto& f = r.verify_findings[i];
      j += "{\"severity\":" + json_str(f.severity == Severity::Error ? "error" : "warning") + ",\"code\":" + json_str(f.code) + ",\"message\":" + json_str(f.message) + "}";
    }
    j += "]}";
    int dr = dup_string(j, out);
    return dr ? dr : static_cast<int>(r.status.code);
  } catch (...) { return static_cast<int>(Errc::NoMemory); }
}

int usbmem_bootfix_last_backup(usbmem_bootfix* h, unsigned char** out, size_t* len) {
  if (!h || !out || !len) return static_cast<int>(Errc::InvalidArgument);
  *out = nullptr;
  if (h->last_backup.empty()) return static_cast<int>(Errc::InvalidArgument);
  try { return dup_bytes(h->last_backup, out, len); } catch (...) { return static_cast<int>(Errc::NoMemory); }
}

int usbmem_bootfix_backup_info_json(const unsigned char* data, size_t len, char** out) {
  if (!data || !out) return static_cast<int>(Errc::InvalidArgument);
  *out = nullptr;
  try {
    BootBackupInfo bi;
    Status st = parse_boot_backup(std::vector<uint8_t>(data, data + len), &bi);
    if (!st.ok()) return static_cast<int>(st.code);
    std::string j = "{\"version\":" + std::to_string(bi.version) + ",\"fs\":" + json_str(fs_type_name(static_cast<FsType>(bi.fs))) + ",\"timestamp\":\"" +
                    std::to_string(bi.timestamp) + "\",\"offset\":\"" + std::to_string(bi.offset) + "\",\"length\":\"" + std::to_string(bi.length) +
                    "\",\"records\":" + std::to_string(bi.record_count) + ",\"bytes\":\"" + std::to_string(bi.bytes) + "\"}";
    return dup_string(j, out);
  } catch (...) { return static_cast<int>(Errc::NoMemory); }
}

int usbmem_bootfix_restore_backup(usbmem_device* dev, const unsigned char* data, size_t len) {
  if (!dev || !data) return static_cast<int>(Errc::InvalidArgument);
  try { return static_cast<int>(restore_boot_backup(*dev->dev, std::vector<uint8_t>(data, data + len)).code); } catch (...) { return static_cast<int>(Errc::NoMemory); }
}

}  // extern "C"
