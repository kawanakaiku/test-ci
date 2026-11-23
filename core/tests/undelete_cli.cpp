// SPDX-License-Identifier: Apache-2.0
// 検証用 CLI。tools/check_undelete_tools.py / tools/check_carve_tools.py が、本物の mkfs.* / 外部ツールで作ったイメージに対して使う。
//   list    <image> [--range OFF LEN]                 削除ファイルの一覧 (JSON 1 行)
//   extract <image> <outdir> [--range OFF LEN]        一覧の全ファイルを <outdir>/<id> へ書き出し、SHA-256 を JSON で返す
//   carve   <image> [--types a,b] [--min-size N] [--align N] [--unalloc] [--out DIR] [--state FILE] [--max-hits N] [--range OFF LEN]
//                                                     カービングして結果を JSON で返す (--out があれば切り出したファイルも書く)
//   fuzz    <image> <回数> [seed]                      イメージの管理情報 (空でないセクタ) を少しずつ壊して list + extract を繰り返す (クラッシュ・書き込み・暴走が無いこと。ASan/UBSan のビルドで使う)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

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

// 読み取り専用の土台の上に「壊した数バイト」を重ねて見せるデバイス。書き込みが 1 度でも来たら数える (検査は読み取り専用でなければならない)
class OverlayDevice final : public BlockDevice {
 public:
  OverlayDevice(BlockDevicePtr base, const std::map<uint64_t, uint8_t>& patch) : base_(std::move(base)), patch_(patch) {}
  Status read_at(uint64_t off, void* buf, size_t len, const IoOptions& io = {}) override {
    Status s = base_->read_at(off, buf, len, io);
    if (!s.ok()) return s;
    auto it = patch_.lower_bound(off);
    for (; it != patch_.end() && it->first < off + len; ++it) static_cast<uint8_t*>(buf)[it->first - off] = it->second;
    return s;
  }
  Status write_at(uint64_t, const void*, size_t, const IoOptions& = {}) override { writes++; return Status::Make(Errc::ReadOnly); }
  Status flush() override { return Status::Ok(); }
  Status close() override { return Status::Ok(); }
  uint64_t size() const override { return base_->size(); }
  uint32_t logical_block_size() const override { return base_->logical_block_size(); }
  bool is_read_only() const override { return true; }
  std::string describe() const override { return "overlay"; }
  int writes = 0;

 private:
  BlockDevicePtr base_;
  const std::map<uint64_t, uint8_t>& patch_;
};

static int cmd_fuzz(int argc, char** argv) {
  if (argc < 4) return 2;
  BlockDevicePtr base;
  if (!RawImageDevice::open_path(argv[2], true, 512, &base).ok()) return 3;
  long iters = std::strtol(argv[3], nullptr, 10);
  std::mt19937_64 rng(argc > 4 ? std::strtoull(argv[4], nullptr, 0) : 1);
  // 空でないセクタ (管理情報の候補) を集める。大きなイメージは先頭から一定数
  std::vector<uint64_t> hot;
  std::vector<uint8_t> sec(512);
  for (uint64_t o = 0; o + 512 <= base->size() && hot.size() < 20000; o += 512) {
    if (!base->read_at(o, sec.data(), 512).ok()) break;
    for (uint8_t c : sec) if (c) { hot.push_back(o); break; }
  }
  if (hot.empty()) return 3;
  long listed = 0, extracted = 0, errors = 0;
  for (long it = 0; it < iters; it++) {
    std::map<uint64_t, uint8_t> patch;
    int n = 1 + static_cast<int>(rng() % 24);
    for (int k = 0; k < n; k++) {
      uint64_t o = hot[rng() % hot.size()] + rng() % 512;
      int kind = static_cast<int>(rng() % 5);
      uint8_t cur = 0;
      if (!base->read_at(o, &cur, 1).ok()) continue;
      uint8_t v = kind == 0 ? static_cast<uint8_t>(rng()) : kind == 1 ? static_cast<uint8_t>(cur ^ (1u << (rng() % 8))) : kind == 2 ? 0xFF : kind == 3 ? 0x00 : static_cast<uint8_t>(cur + 1);
      patch[o] = v;
      if (kind == 4) for (int b = 1; b < 4; b++) patch[o + b] = static_cast<uint8_t>(rng());
    }
    OverlayDevice dev(base, patch);
    UndeleteOptions opt;
    UndeleteResult r;
    Status st = list_deleted(dev, opt, &r);
    if (st.ok()) {
      listed++;
      int did = 0;
      for (const DeletedFile& f : r.files) {
        if (f.level == RecoverLevel::None || f.size > (32ull << 20) || did >= 6) continue;
        did++;
        ExtractResult er;
        extract_deleted(dev, f, [](const void*, size_t) { return Status::Ok(); }, nullptr, &er);
        extracted++;
      }
      std::string j = undelete_result_to_json(r);
      if (j.empty()) errors++;
    }
    if (dev.writes) { std::printf("{\"error\":\"write-attempted\",\"iteration\":%ld}\n", it); return 4; }
  }
  std::printf("{\"iterations\":%ld,\"listed\":%ld,\"extracted\":%ld,\"errors\":%ld}\n", iters, listed, extracted, errors);
  return errors ? 1 : 0;
}

int main(int argc, char** argv) {
  if (argc < 3) return 2;
  if (!std::strcmp(argv[1], "list")) return cmd_list(argc, argv);
  if (!std::strcmp(argv[1], "extract")) return cmd_extract(argc, argv);
  if (!std::strcmp(argv[1], "carve")) return cmd_carve(argc, argv);
  if (!std::strcmp(argv[1], "fuzz")) return cmd_fuzz(argc, argv);
  return 2;
}
