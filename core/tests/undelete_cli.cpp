// SPDX-License-Identifier: Apache-2.0
// 検証用 CLI。tools/check_undelete_tools.py / tools/check_carve_tools.py が、本物の mkfs.* / 外部ツールで作ったイメージに対して使う。
//   list    <image> [--range OFF LEN]                 削除ファイルの一覧 (JSON 1 行)
//   extract <image> <outdir> [--range OFF LEN]        一覧の全ファイルを <outdir>/<id> へ書き出し、SHA-256 を JSON で返す
//   carve   <image> [--types a,b] [--min-size N] [--align N] [--unalloc] [--out DIR] [--state FILE] [--max-hits N] [--range OFF LEN]
//                                                     カービングして結果を JSON で返す (--out があれば切り出したファイルも書く)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "usbmem/carve.h"
#include "usbmem/devices.h"
#include "usbmem/undelete.h"

using namespace usbmem;

static bool has_flag(int argc, char** argv, const char* n) {
  for (int i = 0; i < argc; i++) if (!std::strcmp(argv[i], n)) return true;
  return false;
}
static std::string arg_value(int argc, char** argv, const char* n, const char* def = "") {
  for (int i = 0; i + 1 < argc; i++) if (!std::strcmp(argv[i], n)) return argv[i + 1];
  return def;
}

static BlockDevicePtr open_dev(int argc, char** argv, const char* path) {
  BlockDevicePtr raw;
  if (!RawImageDevice::open_path(path, true, 512, &raw).ok()) return nullptr;
  for (int i = 0; i + 2 < argc; i++) {
    if (!std::strcmp(argv[i], "--range")) {
      uint64_t off = std::strtoull(argv[i + 1], nullptr, 0), len = std::strtoull(argv[i + 2], nullptr, 0);
      BlockDevicePtr sub;
      if (!SubRangeDevice::create(raw, off, len, true, &sub).ok()) return nullptr;
      return sub;
    }
  }
  return raw;
}

static int cmd_list(int argc, char** argv) {
  BlockDevicePtr dev = open_dev(argc, argv, argv[2]);
  if (!dev) return 3;
  UndeleteOptions opt;
  UndeleteResult r;
  Status st = list_deleted(*dev, opt, &r);
  std::printf("%s\n", st.ok() ? undelete_result_to_json(r).c_str() : ("{\"error\":" + std::to_string(static_cast<int>(st.code)) + "}").c_str());
  return st.ok() ? 0 : 1;
}

static int cmd_extract(int argc, char** argv) {
  if (argc < 4) return 2;
  BlockDevicePtr dev = open_dev(argc, argv, argv[2]);
  if (!dev) return 3;
  UndeleteOptions opt;
  UndeleteResult r;
  Status st = list_deleted(*dev, opt, &r);
  if (!st.ok()) { std::printf("{\"error\":%d}\n", static_cast<int>(st.code)); return 1; }
  std::string o = "{\"results\":[";
  bool first = true;
  for (const DeletedFile& f : r.files) {
    if (f.level == RecoverLevel::None) continue;
    std::string path = std::string(argv[3]) + "/" + std::to_string(f.id);
    FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) return 3;
    ExtractResult er;
    Status es = extract_deleted(*dev, f, [&](const void* d, size_t n) { return std::fwrite(d, 1, n, fp) == n ? Status::Ok() : Status::Make(Errc::Io); }, nullptr, &er);
    std::fclose(fp);
    if (!first) o += ',';
    first = false;
    o += "{\"id\":" + std::to_string(f.id) + ",\"status\":" + std::to_string(static_cast<int>(es.code)) + ",\"bytes\":" + std::to_string(er.bytes) + ",\"sha256\":\"" + er.sha256 + "\"}";
  }
  o += "]}";
  std::printf("%s\n", o.c_str());
  return 0;
}

static int cmd_carve(int argc, char** argv) {
  BlockDevicePtr dev = open_dev(argc, argv, argv[2]);
  if (!dev) return 3;
  CarveOptions opt;
  std::string types = arg_value(argc, argv, "--types");
  size_t p = 0;
  while (p < types.size()) {
    size_t q = types.find(',', p);
    if (q == std::string::npos) q = types.size();
    if (q > p) opt.types.insert(types.substr(p, q - p));
    p = q + 1;
  }
  opt.min_size = std::strtoull(arg_value(argc, argv, "--min-size", "0").c_str(), nullptr, 0);
  opt.align = static_cast<uint32_t>(std::strtoul(arg_value(argc, argv, "--align", "0").c_str(), nullptr, 0));
  opt.unallocated_only = has_flag(argc, argv, "--unalloc");
  opt.max_hits = std::strtoull(arg_value(argc, argv, "--max-hits", "1000000").c_str(), nullptr, 0);
  std::string out_dir = arg_value(argc, argv, "--out");
  CarveState state;
  std::string state_path = arg_value(argc, argv, "--state");
  if (!state_path.empty()) {
    FILE* f = std::fopen(state_path.c_str(), "rb");
    if (f) {
      std::string t;
      char buf[65536];
      size_t n;
      while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) t.append(buf, n);
      std::fclose(f);
      if (!carve_state_parse(t, &state)) { std::printf("{\"error\":\"bad-state\"}\n"); return 5; }
    }
  }
  CancelToken cancel;
  opt.cancel = &cancel;
  int64_t cancel_after = std::strtoll(arg_value(argc, argv, "--cancel-after-mb", "-1").c_str(), nullptr, 10);
  opt.progress = [&](const char*, uint64_t done, uint64_t) { if (cancel_after >= 0 && done >= static_cast<uint64_t>(cancel_after) << 20) cancel.cancel(); };
  opt.checkpoint = [&](const CarveState& s) {
    if (state_path.empty()) return;
    std::string tmp = state_path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return;
    std::string t = carve_state_to_text(s);
    std::fwrite(t.data(), 1, t.size(), f);
    std::fclose(f);
    std::rename(tmp.c_str(), state_path.c_str());
  };
  opt.on_hit = [&](const CarveHit& h) -> Status {
    if (out_dir.empty()) return Status::Ok();
    std::string path = out_dir + "/" + carve_hit_filename(h);
    FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) return Status::Make(Errc::Io);
    Status es = carve_extract(*dev, h, [&](const void* d, size_t n) { return std::fwrite(d, 1, n, fp) == n ? Status::Ok() : Status::Make(Errc::Io); }, nullptr, nullptr);
    std::fclose(fp);
    return es;
  };
  CarveResult r;
  auto t0 = std::chrono::steady_clock::now();
  Status st = carve_scan(*dev, opt, &state, &r);
  double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::string j = carve_result_to_json(r, state);
  j.pop_back();
  char b[128];
  std::snprintf(b, sizeof b, ",\"seconds\":%.3f,\"status\":%d}", secs, static_cast<int>(st.code));
  std::printf("%s%s\n", j.c_str(), b);
  return st.ok() ? 0 : 1;
}

int main(int argc, char** argv) {
  if (argc < 3) return 2;
  if (!std::strcmp(argv[1], "list")) return cmd_list(argc, argv);
  if (!std::strcmp(argv[1], "extract")) return cmd_extract(argc, argv);
  if (!std::strcmp(argv[1], "carve")) return cmd_carve(argc, argv);
  return 2;
}
