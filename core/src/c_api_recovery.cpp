// SPDX-License-Identifier: Apache-2.0
// データ救出・バックアップ・復元の C 境界。ここでは JSON 化と、ファイル (mapfile / 画像) の入出力だけを受け持つ。
#include "usbmem/c_api_recovery.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <new>
#include <string>
#include <unistd.h>

#include "usbmem/backup.h"
#include "usbmem/c_api_internal.h"
#include "usbmem/recovery.h"

using namespace usbmem;

struct usbmem_job {
  RescueControl ctl;
  CancelToken cancel;
  std::mutex mu;
  std::string progress = "{}";
};
struct usbmem_restore_plan {
  RestorePlan plan;
};

namespace {
void jstr(std::string& o, const std::string& s) {
  o += '"';
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += static_cast<char>(c); }
    else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
    else o += static_cast<char>(c);
  }
  o += '"';
}
std::string jnum(unsigned long long v) { return std::to_string(v); }
std::string jbool(bool b) { return b ? "true" : "false"; }
std::string jarr(const std::vector<std::string>& v) {
  std::string o = "[";
  for (size_t i = 0; i < v.size(); i++) { if (i) o += ','; jstr(o, v[i]); }
  return o + "]";
}
int emit(const std::string& j, char** out) {
  char* p = static_cast<char*>(std::malloc(j.size() + 1));
  if (!p) return static_cast<int>(Errc::NoMemory);
  std::memcpy(p, j.c_str(), j.size() + 1);
  *out = p;
  return 0;
}
int code_of(const Status& s) { return static_cast<int>(s.code); }

// mapfile を一時ファイルに書いて rename する (途中で電源が落ちても、前の mapfile か新しい mapfile のどちらかが残る)
bool save_text_atomic(const std::string& path, const std::string& text) {
  std::string tmp = path + ".tmp";
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) return false;
  size_t done = 0;
  while (done < text.size()) {
    ssize_t n = ::write(fd, text.data() + done, text.size() - done);
    if (n < 0) { if (errno == EINTR) continue; ::close(fd); ::unlink(tmp.c_str()); return false; }
    done += static_cast<size_t>(n);
  }
  bool ok = ::fsync(fd) == 0;
  ok = (::close(fd) == 0) && ok;
  if (ok) ok = ::rename(tmp.c_str(), path.c_str()) == 0;
  if (!ok) ::unlink(tmp.c_str());
  return ok;
}
bool load_text(const std::string& path, std::string* out, bool* exists) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) { *exists = errno != ENOENT; return false; }
  *exists = true;
  std::string s;
  char buf[65536];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
    s.append(buf, n);
    if (s.size() > (1ull << 31)) { std::fclose(f); return false; }
  }
  bool ok = !std::ferror(f);
  std::fclose(f);
  if (ok) *out = std::move(s);
  return ok;
}

std::string progress_json(const RescueProgress& p, uint64_t done = 0, uint64_t total = 0) {
  std::string o = "{\"phase\":";
  jstr(o, p.phase ? p.phase : "");
  o += ",\"pos\":" + jnum(p.pos) + ",\"size\":" + jnum(p.size) + ",\"finished\":" + jnum(p.finished) + ",\"nonTried\":" + jnum(p.non_tried) +
       ",\"nonTrimmed\":" + jnum(p.non_trimmed) + ",\"nonScraped\":" + jnum(p.non_scraped) + ",\"bad\":" + jnum(p.bad) + ",\"readOps\":" + jnum(p.read_ops) +
       ",\"readErrors\":" + jnum(p.read_errors) + ",\"bytesCopied\":" + jnum(p.bytes_copied) + ",\"msSinceLastGood\":" + jnum(p.ms_since_last_good) +
       ",\"done\":" + jnum(done) + ",\"total\":" + jnum(total) + "}";
  return o;
}
void set_progress(usbmem_job* j, const std::string& s) {
  if (!j) return;
  std::lock_guard<std::mutex> l(j->mu);
  j->progress = s;
}

const char* stop_name(RescueStop s) {
  switch (s) {
    case RescueStop::Finished: return "finished";
    case RescueStop::Cancelled: return "cancelled";
    case RescueStop::SafetyAbort: return "safety-abort";
    case RescueStop::InputGone: return "input-gone";
    case RescueStop::OutputError: return "output-error";
    case RescueStop::MapfileError: return "mapfile-error";
    case RescueStop::BadArgument: return "bad-argument";
  }
  return "?";
}

// 画像ファイルの先頭を見て LZ4 なら展開の層を挟む
class PrefixSource final : public ByteSource {
 public:
  PrefixSource(std::unique_ptr<FileSource> f, const uint8_t* p, size_t n) : f_(std::move(f)), n_(n) { std::memcpy(pre_, p, n); }
  Status read(void* buf, size_t len, size_t* got) override {
    size_t k = 0;
    uint8_t* o = static_cast<uint8_t*>(buf);
    if (pos_ < n_) { k = std::min(len, n_ - pos_); std::memcpy(o, pre_ + pos_, k); pos_ += k; }
    size_t g = 0;
    if (k < len) { Status s = f_->read(o + k, len - k, &g); if (!s.ok()) return s; }
    *got = k + g;
    return Status::Ok();
  }
 private:
  std::unique_ptr<FileSource> f_;
  uint8_t pre_[4];
  size_t n_, pos_ = 0;
};
class OwningLz4Source final : public ByteSource {
 public:
  explicit OwningLz4Source(std::unique_ptr<ByteSource> raw) : raw_(std::move(raw)), lz_(make_lz4_source(raw_.get())) {}
  Status read(void* buf, size_t len, size_t* got) override { return lz_->read(buf, len, got); }
 private:
  std::unique_ptr<ByteSource> raw_;
  std::unique_ptr<ByteSource> lz_;
};
std::unique_ptr<ByteSource> open_image(const std::string& path) {
  std::unique_ptr<FileSource> f;
  if (!FileSource::open(path, &f).ok()) return nullptr;
  uint8_t head[4] = {0, 0, 0, 0};
  size_t got = 0;
  if (!f->read(head, 4, &got).ok()) return nullptr;
  std::unique_ptr<ByteSource> raw(new PrefixSource(std::move(f), head, got));
  if (got == 4 && looks_like_lz4(head)) return std::unique_ptr<ByteSource>(new OwningLz4Source(std::move(raw)));
  return raw;
}

// 書き出し側: lz4 の層 + ファイル
class FileLz4Sink final : public ByteSink {
 public:
  static std::unique_ptr<FileLz4Sink> make(const std::string& path, bool lz4, Status* st) {
    std::unique_ptr<FileSink> f;
    *st = FileSink::create(path, &f);
    if (!st->ok()) return nullptr;
    std::unique_ptr<FileLz4Sink> s(new FileLz4Sink());
    s->file_ = std::move(f);
    if (lz4) s->lz_ = make_lz4_sink(s->file_.get());
    return s;
  }
  Status write(const void* d, size_t n) override { return lz_ ? lz_->write(d, n) : file_->write(d, n); }
  Status finish() override { return lz_ ? lz_->finish() : file_->finish(); }
 private:
  std::unique_ptr<FileSink> file_;
  std::unique_ptr<ByteSink> lz_;
};

std::string image_info_json(const ImageInfo& i) {
  std::string o = "{\"fs\":";
  jstr(o, i.fs);
  o += ",\"version\":";
  jstr(o, i.version);
  o += ",\"createdBy\":";
  jstr(o, i.ptc_version);
  o += ",\"blockSize\":" + jnum(i.block_size) + ",\"deviceSize\":" + jnum(i.device_size) + ",\"totalBlocks\":" + jnum(i.total_blocks) +
       ",\"usedBlocks\":" + jnum(i.counted_used) + ",\"blocksPerChecksum\":" + jnum(i.blocks_per_checksum) + ",\"checksum\":" + jbool(i.checksum_mode != 0) + "}";
  return o;
}
}  // namespace

extern "C" {

usbmem_job* usbmem_job_new(void) { try { return new usbmem_job(); } catch (...) { return nullptr; } }
void usbmem_job_cancel(usbmem_job* j) { if (j) { j->ctl.cancel.store(true); j->cancel.cancel(); } }
void usbmem_job_pause(usbmem_job* j, int p) { if (j) j->ctl.pause.store(p != 0); }
int usbmem_job_progress_json(usbmem_job* j, char** out) {
  if (!j || !out) return static_cast<int>(Errc::InvalidArgument);
  std::string s;
  { std::lock_guard<std::mutex> l(j->mu); s = j->progress; }
  return emit(s, out);
}
void usbmem_job_free(usbmem_job* j) { delete j; }

int usbmem_rescue_run(usbmem_device* in, usbmem_device* out, const char* mapfile_path, const usbmem_rescue_options* o, usbmem_job* job, char** out_json) {
  if (!in || !in->dev || !out || !out->dev || !out_json) return static_cast<int>(Errc::InvalidArgument);
  *out_json = nullptr;
  try {
    RescueOptions opt;
    if (o) {
      opt.start = o->start; opt.length = o->length; opt.out_offset = o->out_offset;
      opt.sector_size = o->sector_size; opt.cluster_sectors = o->cluster_sectors ? o->cluster_sectors : 64; opt.retries = o->retries;
      opt.do_trim = !o->no_trim; opt.do_scrape = !o->no_scrape; opt.skip_on_error = !o->no_skip;
      opt.read_timeout_ms = o->read_timeout_ms;
      opt.max_consecutive_errors = o->max_consecutive_errors; opt.max_errors = o->max_errors;
      opt.error_window_ms = o->error_window_ms; opt.max_errors_in_window = o->max_errors_in_window;
      opt.no_progress_timeout_ms = o->no_progress_timeout_ms; opt.pause_between_reads_ms = o->pause_between_reads_ms;
    }
    Mapfile map(in->dev->size());
    MapMeta meta;
    std::string path = mapfile_path ? mapfile_path : "";
    if (!path.empty()) {
      std::string text;
      bool exists = false;
      if (load_text(path, &text, &exists)) {
        Status st = Mapfile::parse(text, in->dev->size(), &map, &meta);
        if (!st.ok()) {  // 壊れた mapfile は上書きしない (中の進捗を失うため)
          std::string j = "{\"stop\":\"bad-argument\",\"code\":\"bad-mapfile\",\"message\":\"the mapfile is corrupt or does not match the input; it was not changed\",\"errc\":" + std::to_string(code_of(st)) + "}";
          emit(j, out_json);
          return code_of(st);
        }
      } else if (exists) {
        std::string j = "{\"stop\":\"bad-argument\",\"code\":\"mapfile-unreadable\",\"message\":\"the mapfile exists but could not be read\",\"errc\":2}";
        emit(j, out_json);
        return static_cast<int>(Errc::Io);
      }
    }
    RescueEngine eng(in->dev, out->dev, &map, opt);
    RescueHooks hooks;
    if (!path.empty()) hooks.persist = [&](const Mapfile& m, const MapMeta& mm) { return save_text_atomic(path, m.to_text(mm)); };
    hooks.on_progress = [&](const RescueProgress& p) { set_progress(job, progress_json(p)); };
    RescueResult r = eng.run(job ? &job->ctl : nullptr, hooks);
    std::string j = "{\"stop\":";
    jstr(j, stop_name(r.stop));
    j += ",\"code\":";
    jstr(j, r.code);
    j += ",\"message\":";
    jstr(j, r.message);
    j += ",\"errc\":" + std::to_string(code_of(r.status)) + ",\"finished\":" + jnum(map.count(kFinished)) + ",\"nonTried\":" + jnum(map.count(kNonTried)) +
         ",\"nonTrimmed\":" + jnum(map.count(kNonTrimmed)) + ",\"nonScraped\":" + jnum(map.count(kNonScraped)) + ",\"bad\":" + jnum(map.count(kBad)) +
         ",\"readOps\":" + jnum(r.last.read_ops) + ",\"readErrors\":" + jnum(r.last.read_errors) + ",\"bytesCopied\":" + jnum(r.last.bytes_copied) + "}";
    int rc = emit(j, out_json);
    if (rc) return rc;
    return r.stop == RescueStop::Finished ? 0 : (code_of(r.status) ? code_of(r.status) : static_cast<int>(Errc::Cancelled));
  } catch (...) { return static_cast<int>(Errc::NoMemory); }
}

int usbmem_backup_run(usbmem_device* src, const char* image_path, int lz4, int full_if_unclean, usbmem_job* job, char** out_json) {
  if (!src || !src->dev || !image_path || !out_json) return static_cast<int>(Errc::InvalidArgument);
  *out_json = nullptr;
  try {
    Status st;
    auto sink = FileLz4Sink::make(image_path, lz4 != 0, &st);
    if (!sink) {
      std::string j = "{\"ok\":false,\"code\":\"output-open-failed\",\"message\":\"could not create the image file\",\"errc\":" + std::to_string(code_of(st)) + "}";
      emit(j, out_json);
      return code_of(st);
    }
    BackupOptions bo;
    bo.extract.full_if_unclean = full_if_unclean != 0;
    if (job) {
      bo.cancel = &job->cancel;
      bo.progress = [job](uint64_t d, uint64_t t) { set_progress(job, "{\"phase\":\"backup\",\"done\":" + jnum(d) + ",\"total\":" + jnum(t) + "}"); };
    }
    BackupResult r = backup_filesystem(*src->dev, *sink, bo);
    sink.reset();
    std::string j = "{\"ok\":" + jbool(r.status.ok()) + ",\"code\":";
    jstr(j, r.status.code);
    j += ",\"message\":";
    jstr(j, r.status.message);
    j += ",\"errc\":" + std::to_string(code_of(r.status.st));
    if (r.status.ok()) {
      j += ",\"fs\":";
      jstr(j, r.map.fs);
      j += ",\"blockSize\":" + jnum(r.map.block_size) + ",\"totalBlocks\":" + jnum(r.map.total_blocks) + ",\"usedBlocks\":" + jnum(r.map.used_blocks) +
           ",\"fullCopy\":" + jbool(r.map.full_copy) + ",\"imageBytes\":" + jnum(r.image_bytes) + ",\"imageSha256\":";
      jstr(j, r.image_sha256);
      j += ",\"warnings\":" + jarr(r.map.warnings);
    } else {
      ::unlink(image_path);  // 不完全な画像を残さない
    }
    j += "}";
    int rc = emit(j, out_json);
    return rc ? rc : code_of(r.status.st);
  } catch (...) { return static_cast<int>(Errc::NoMemory); }
}

int usbmem_backup_inspect_json(const char* image_path, int verify, char** out_json) {
  if (!image_path || !out_json) return static_cast<int>(Errc::InvalidArgument);
  *out_json = nullptr;
  try {
    std::unique_ptr<ByteSource> src = open_image(image_path);
    if (!src) return static_cast<int>(Errc::Io);
    ImageInfo info;
    FsStatus f;
    std::string extra;
    if (verify) {
      VerifyResult v = verify_image(*src);
      info = v.info;
      f = v.status;
      if (f.ok()) extra = ",\"verified\":true,\"imageSha256\":\"" + v.image_sha256 + "\",\"imageBytes\":" + jnum(v.image_bytes);
    } else {
      f = read_image_info(*src, &info, nullptr);
      if (f.ok()) extra = ",\"verified\":false";
    }
    std::string j = "{\"ok\":" + jbool(f.ok()) + ",\"code\":";
    jstr(j, f.code);
    j += ",\"message\":";
    jstr(j, f.message);
    if (f.ok()) j += ",\"info\":" + image_info_json(info) + extra;
    j += "}";
    int rc = emit(j, out_json);
    return rc ? rc : code_of(f.st);
  } catch (...) { return static_cast<int>(Errc::NoMemory); }
}

int usbmem_restore_plan_new(const char* image_path, usbmem_device* target, int in_use, int verify_all, usbmem_restore_plan** out_plan, char** out_json) {
  if (!image_path || !target || !target->dev || !out_plan || !out_json) return static_cast<int>(Errc::InvalidArgument);
  *out_plan = nullptr;
  *out_json = nullptr;
  try {
    std::string path = image_path;
    RestorePlanOptions po;
    po.target_in_use = in_use != 0;
    po.verify_all = verify_all != 0;
    std::unique_ptr<usbmem_restore_plan> h(new usbmem_restore_plan());
    h->plan = plan_restore([path] { return open_image(path); }, *target->dev, po);
    const RestorePlan& p = h->plan;
    std::string j = "{\"ok\":" + jbool(p.ok) + ",\"code\":";
    jstr(j, p.code);
    j += ",\"message\":";
    jstr(j, p.message);
    j += ",\"fs\":";
    jstr(j, p.image.fs);
    j += ",\"blockSize\":" + jnum(p.image.block_size) + ",\"usedBlocks\":" + jnum(p.blocks_to_write) + ",\"bytesToWrite\":" + jnum(p.bytes_to_write) +
         ",\"targetSize\":" + jnum(p.target_size) + ",\"targetExistingFs\":";
    jstr(j, p.target_existing_fs);
    j += ",\"ntfsBackupBoot\":" + jbool(p.ntfs_backup_boot) + ",\"verified\":" + jbool(p.verified) + ",\"imageSha256\":";
    jstr(j, p.image_sha256);
    j += ",\"planHash\":";
    jstr(j, p.plan_hash);
    j += ",\"confirmPhrase\":";
    jstr(j, p.confirm_phrase);
    j += ",\"warnings\":" + jarr(p.warnings) + "}";
    int rc = emit(j, out_json);
    if (rc) return rc;
    *out_plan = h.release();
    return 0;
  } catch (...) { return static_cast<int>(Errc::NoMemory); }
}

int usbmem_restore_token(usbmem_restore_plan* plan, const char* typed, char** out_token) {
  if (!plan || !typed || !out_token) return static_cast<int>(Errc::InvalidArgument);
  try { return emit(make_restore_token(plan->plan, typed), out_token); } catch (...) { return static_cast<int>(Errc::NoMemory); }
}

int usbmem_restore_apply(usbmem_restore_plan* plan, const char* image_path, usbmem_device* target, const char* token, int in_use, usbmem_job* job, char** out_json) {
  if (!plan || !image_path || !target || !target->dev || !token || !out_json) return static_cast<int>(Errc::InvalidArgument);
  *out_json = nullptr;
  try {
    std::string path = image_path;
    RestoreOptions ro;
    ro.target_in_use = in_use != 0;
    if (job) {
      ro.cancel = &job->cancel;
      ro.progress = [job](const char* ph, uint64_t d, uint64_t t) { set_progress(job, std::string("{\"phase\":\"") + ph + "\",\"done\":" + jnum(d) + ",\"total\":" + jnum(t) + "}"); };
    }
    RestoreResult r = apply_restore([path] { return open_image(path); }, *target->dev, plan->plan, token, ro);
    std::string j = "{\"ok\":" + jbool(r.status.ok()) + ",\"code\":";
    jstr(j, r.status.code);
    j += ",\"message\":";
    jstr(j, r.status.message);
    j += ",\"errc\":" + std::to_string(code_of(r.status.st)) + ",\"blocksWritten\":" + jnum(r.blocks_written) + ",\"verified\":" + jbool(r.verified) +
         ",\"targetTouched\":" + jbool(r.target_touched) + "}";
    int rc = emit(j, out_json);
    return rc ? rc : code_of(r.status.st);
  } catch (...) { return static_cast<int>(Errc::NoMemory); }
}

void usbmem_restore_plan_free(usbmem_restore_plan* p) { delete p; }

}  // extern "C"
