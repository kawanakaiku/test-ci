// SPDX-License-Identifier: Apache-2.0
// 削除ファイルの一覧と救出 / カービングの C 境界。ここでは JSON 化と、ファイル (出力・進捗の保存) の入出力だけを受け持つ。
#include "usbmem/c_api_undelete.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <new>
#include <string>
#include <unistd.h>

#include "usbmem/c_api_internal.h"
#include "usbmem/carve.h"
#include "usbmem/undelete.h"

using namespace usbmem;

struct usbmem_ujob {
  CancelToken cancel;
  std::mutex mu;
  std::string phase;
  unsigned long long done = 0, total = 0;
};
struct usbmem_undelete {
  BlockDevicePtr dev;
  UndeleteResult res;
};
struct usbmem_carve {
  BlockDevicePtr dev;
  CarveState state;
  CarveResult res;
};

namespace {
int emit(const std::string& j, char** out) {
  char* p = static_cast<char*>(std::malloc(j.size() + 1));
  if (!p) return static_cast<int>(Errc::NoMemory);
  std::memcpy(p, j.c_str(), j.size() + 1);
  *out = p;
  return 0;
}
int code_of(const Status& s) { return static_cast<int>(s.code); }
void set_progress(usbmem_ujob* j, const char* phase, unsigned long long d, unsigned long long t) {
  if (!j) return;
  std::lock_guard<std::mutex> l(j->mu);
  j->phase = phase ? phase : "";
  j->done = d;
  j->total = t;
}

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
    if (s.size() > (1ull << 30)) { std::fclose(f); return false; }
  }
  bool ok = !std::ferror(f);
  std::fclose(f);
  if (ok) *out = std::move(s);
  return ok;
}

// fd へ全部書く
Status write_all(int fd, const void* d, size_t n) {
  const uint8_t* p = static_cast<const uint8_t*>(d);
  size_t done = 0;
  while (done < n) {
    ssize_t w = ::write(fd, p + done, n - done);
    if (w < 0) { if (errno == EINTR) continue; return Status::Make(Errc::Io, errno); }
    done += static_cast<size_t>(w);
  }
  return Status::Ok();
}

std::string result_json(const Status& st, const ExtractResult& er) {
  return std::string("{\"ok\":") + (st.ok() ? "true" : "false") + ",\"code\":" + std::to_string(code_of(st)) + ",\"bytes\":" + std::to_string(er.bytes) + ",\"sha256\":\"" + er.sha256 + "\"}";
}

// パスへ書き出す共通処理 (新規作成。失敗なら消す)
template <class F>
int extract_to_path(const char* path, usbmem_ujob* job, char** out_json, F extract) {
  if (!path || !out_json) return static_cast<int>(Errc::InvalidArgument);
  *out_json = nullptr;
  int fd = ::open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0) {
    std::string j = "{\"ok\":false,\"code\":" + std::to_string(static_cast<int>(Errc::Io)) + ",\"bytes\":0,\"sha256\":\"\"}";
    emit(j, out_json);
    return static_cast<int>(Errc::Io);
  }
  ExtractResult er;
  Status st = extract(fd, &er);
  if (st.ok() && ::fsync(fd) != 0) st = Status::Make(Errc::Io, errno);
  if (::close(fd) != 0 && st.ok()) st = Status::Make(Errc::Io, errno);
  if (!st.ok()) ::unlink(path);
  (void)job;
  int rc = emit(result_json(st, er), out_json);
  return rc ? rc : code_of(st);
}
}  // namespace

extern "C" {

usbmem_ujob* usbmem_ujob_new(void) { try { return new usbmem_ujob(); } catch (...) { return nullptr; } }
void usbmem_ujob_cancel(usbmem_ujob* j) { if (j) j->cancel.cancel(); }
void usbmem_ujob_progress(usbmem_ujob* j, char* phase, size_t phase_len, unsigned long long* done, unsigned long long* total) {
  if (done) *done = 0;
  if (total) *total = 0;
  if (phase && phase_len) phase[0] = 0;
  if (!j) return;
  std::lock_guard<std::mutex> l(j->mu);
  if (phase && phase_len) { std::strncpy(phase, j->phase.c_str(), phase_len - 1); phase[phase_len - 1] = 0; }
  if (done) *done = j->done;
  if (total) *total = j->total;
}
void usbmem_ujob_free(usbmem_ujob* j) { delete j; }

// ---- 削除ファイル
int usbmem_undelete_scan(usbmem_device* dev, int include_empty, usbmem_ujob* job, usbmem_undelete** out) {
  if (!dev || !dev->dev || !out) return static_cast<int>(Errc::InvalidArgument);
  *out = nullptr;
  try {
    std::unique_ptr<usbmem_undelete> h(new usbmem_undelete());
    h->dev = dev->dev;
    UndeleteOptions opt;
    opt.include_empty = include_empty != 0;
    if (job) opt.cancel = &job->cancel;
    opt.progress = [job](const char* ph, uint64_t d, uint64_t t) { set_progress(job, ph, d, t); };
    Status st = list_deleted(*dev->dev, opt, &h->res);
    if (!st.ok() && st.code != Errc::Cancelled) return code_of(st);
    *out = h.release();
    return code_of(st);
  } catch (...) { return static_cast<int>(Errc::NoMemory); }
}
void usbmem_undelete_close(usbmem_undelete* h) { delete h; }
int usbmem_undelete_result_json(usbmem_undelete* h, char** out) {
  if (!h || !out) return static_cast<int>(Errc::InvalidArgument);
  try { return emit(undelete_result_to_json(h->res), out); } catch (...) { return static_cast<int>(Errc::NoMemory); }
}
int usbmem_undelete_extract_fd(usbmem_undelete* h, unsigned id, int fd, usbmem_ujob* job, char** out) {
  if (!h || !out || fd < 0 || id >= h->res.files.size()) return static_cast<int>(Errc::InvalidArgument);
  *out = nullptr;
  try {
    const DeletedFile& f = h->res.files[id];
    ExtractResult er;
    uint64_t done = 0;
    Status st = extract_deleted(*h->dev, f, [&](const void* d, size_t n) {
      Status w = write_all(fd, d, n);
      done += n;
      set_progress(job, "extract", done, f.size);
      return w;
    }, job ? &job->cancel : nullptr, &er);
    int rc = emit(result_json(st, er), out);
    return rc ? rc : code_of(st);
  } catch (...) { return static_cast<int>(Errc::NoMemory); }
}
int usbmem_undelete_extract_path(usbmem_undelete* h, unsigned id, const char* path, usbmem_ujob* job, char** out) {
  if (!h || id >= h->res.files.size()) return static_cast<int>(Errc::InvalidArgument);
  try { return extract_to_path(path, job, out, [&](int fd, ExtractResult* er) {
    const DeletedFile& f = h->res.files[id];
    uint64_t done = 0;
    return extract_deleted(*h->dev, f, [&](const void* d, size_t n) {
      Status w = write_all(fd, d, n);
      done += n;
      set_progress(job, "extract", done, f.size);
      return w;
    }, job ? &job->cancel : nullptr, er);
  }); } catch (...) { return static_cast<int>(Errc::NoMemory); }
}

// ---- カービング
int usbmem_carve_types_json(char** out) {
  if (!out) return static_cast<int>(Errc::InvalidArgument);
  std::string o = "[";
  bool first = true;
  for (const CarveTypeInfo& t : carve_types()) {
    if (!first) o += ',';
    first = false;
    o += "{\"id\":\"" + t.id + "\",\"extensions\":\"" + t.extensions + "\",\"description\":\"" + t.description + "\"}";
  }
  o += "]";
  return emit(o, out);
}

int usbmem_carve_run(usbmem_device* dev, const char* types_csv, const usbmem_carve_options* o, const char* state_path, const char* out_dir, usbmem_ujob* job,
                     usbmem_carve** out) {
  if (!dev || !dev->dev || !out) return static_cast<int>(Errc::InvalidArgument);
  *out = nullptr;
  try {
    std::unique_ptr<usbmem_carve> h(new usbmem_carve());
    h->dev = dev->dev;
    CarveOptions opt;
    if (types_csv) {
      std::string t = types_csv;
      size_t p = 0;
      while (p < t.size()) {
        size_t q = t.find(',', p);
        if (q == std::string::npos) q = t.size();
        if (q > p) opt.types.insert(t.substr(p, q - p));
        p = q + 1;
      }
    }
    if (o) {
      opt.min_size = o->min_size;
      opt.max_size = o->max_size;
      opt.align = o->align;
      opt.unallocated_only = o->unallocated_only != 0;
      if (o->max_hits) opt.max_hits = static_cast<size_t>(o->max_hits);
    }
    if (job) opt.cancel = &job->cancel;
    std::string sp = state_path ? state_path : "";
    if (!sp.empty()) {
      std::string text;
      bool exists = false;
      if (load_text(sp, &text, &exists)) {
        if (!carve_state_parse(text, &h->state)) return static_cast<int>(Errc::Corrupt);  // 壊れた進捗は上書きしない
      } else if (exists) {
        return static_cast<int>(Errc::Io);
      }
    }
    std::string dir = out_dir ? out_dir : "";
    opt.progress = [job](const char* ph, uint64_t d, uint64_t t) { set_progress(job, ph, d, t); };
    bool save_failed = false;
    opt.checkpoint = [&](const CarveState& s) {
      if (!sp.empty() && !save_failed && !save_text_atomic(sp, carve_state_to_text(s))) save_failed = true;
    };
    BlockDevicePtr devp = dev->dev;
    if (!dir.empty()) {
      opt.on_hit = [&](const CarveHit& hit) -> Status {
        std::string path = dir + "/" + carve_hit_filename(hit);
        int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (fd < 0) return Status::Make(Errc::Io, errno);
        Status st = carve_extract(*devp, hit, [&](const void* d, size_t n) { return write_all(fd, d, n); }, opt.cancel, nullptr);
        if (st.ok() && ::fsync(fd) != 0) st = Status::Make(Errc::Io, errno);
        if (::close(fd) != 0 && st.ok()) st = Status::Make(Errc::Io, errno);
        if (!st.ok()) ::unlink(path.c_str());
        return st;
      };
    }
    Status st = carve_scan(*dev->dev, opt, &h->state, &h->res);
    if (save_failed && st.ok()) h->res.warnings.push_back("the state file could not be saved");
    if (st.code == Errc::InvalidArgument || st.code == Errc::Unsupported) return code_of(st);  // 設定の誤り。途中の結果は無い
    *out = h.release();
    return code_of(st);
  } catch (...) { return static_cast<int>(Errc::NoMemory); }
}
void usbmem_carve_close(usbmem_carve* h) { delete h; }
int usbmem_carve_result_json(usbmem_carve* h, char** out) {
  if (!h || !out) return static_cast<int>(Errc::InvalidArgument);
  try { return emit(carve_result_to_json(h->res, h->state), out); } catch (...) { return static_cast<int>(Errc::NoMemory); }
}
int usbmem_carve_extract_fd(usbmem_carve* h, unsigned idx, int fd, usbmem_ujob* job, char** out) {
  if (!h || !out || fd < 0 || idx >= h->state.hits.size()) return static_cast<int>(Errc::InvalidArgument);
  *out = nullptr;
  try {
    const CarveHit& hit = h->state.hits[idx];
    ExtractResult er;
    uint64_t done = 0;
    Status st = carve_extract(*h->dev, hit, [&](const void* d, size_t n) {
      Status w = write_all(fd, d, n);
      done += n;
      set_progress(job, "extract", done, hit.length);
      return w;
    }, job ? &job->cancel : nullptr, &er);
    int rc = emit(result_json(st, er), out);
    return rc ? rc : code_of(st);
  } catch (...) { return static_cast<int>(Errc::NoMemory); }
}
int usbmem_carve_extract_path(usbmem_carve* h, unsigned idx, const char* path, usbmem_ujob* job, char** out) {
  if (!h || idx >= h->state.hits.size()) return static_cast<int>(Errc::InvalidArgument);
  try { return extract_to_path(path, job, out, [&](int fd, ExtractResult* er) {
    const CarveHit& hit = h->state.hits[idx];
    uint64_t done = 0;
    return carve_extract(*h->dev, hit, [&](const void* d, size_t n) {
      Status w = write_all(fd, d, n);
      done += n;
      set_progress(job, "extract", done, hit.length);
      return w;
    }, job ? &job->cancel : nullptr, er);
  }); } catch (...) { return static_cast<int>(Errc::NoMemory); }
}

}  // extern "C"
