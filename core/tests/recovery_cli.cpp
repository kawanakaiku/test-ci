// SPDX-License-Identifier: Apache-2.0
// 検証用 CLI。tools/check_recovery_tools.py が、ddrescue / partclone と同じ入力で突き合わせるために使う。
//   rescue  <in> <out> <map> [--sector N] [--cluster N] [--retries N] [--no-trim] [--no-scrape] [--no-skip] [--fault OFF:LEN]... [--max-reads N]
//           [--start N] [--length N] [--out-offset N]
//   backup  <src> <image> [--lz4] [--full]
//   info    <image> [--verify]
//   bits    <src> <out-file>             使用中ビットマップだけを書き出す (partclone のものと比べる)
//   restore <image> <target> [--no-verify] [--in-use]
//   plan    <image> <target>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "recovery_test_util.h"
#include "usbmem/c_api_recovery.h"

using namespace usbmem;

static std::string arg_value(int argc, char** argv, const char* name, const char* def = nullptr) {
  for (int i = 0; i + 1 < argc; i++) if (!std::strcmp(argv[i], name)) return argv[i + 1];
  return def ? def : "";
}
static bool has_flag(int argc, char** argv, const char* name) {
  for (int i = 0; i < argc; i++) if (!std::strcmp(argv[i], name)) return true;
  return false;
}

static int cmd_rescue(int argc, char** argv) {
  if (argc < 5) return 2;
  std::string in = argv[2], out = argv[3], map = argv[4];
  struct stat a, b;
  if (::stat(in.c_str(), &a) != 0) { std::printf("{\"stop\":\"bad-argument\",\"code\":\"no-input\"}\n"); return 3; }
  if (::stat(out.c_str(), &b) == 0 && a.st_dev == b.st_dev && a.st_ino == b.st_ino) {
    std::printf("{\"stop\":\"bad-argument\",\"code\":\"same-file\"}\n");
    return 4;
  }
  BlockDevicePtr raw;
  if (!RawImageDevice::open_path(in, true, 512, &raw).ok()) return 3;
  uint64_t start = std::strtoull(arg_value(argc, argv, "--start", "0").c_str(), nullptr, 0);
  uint64_t length = std::strtoull(arg_value(argc, argv, "--length", "0").c_str(), nullptr, 0);
  uint64_t out_off = std::strtoull(arg_value(argc, argv, "--out-offset", "0").c_str(), nullptr, 0);
  {  // 出力を必要な大きさにする
    int fd = ::open(out.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return 3;
    struct stat st;
    ::fstat(fd, &st);
    uint64_t need = out_off + (length ? length : raw->size() - start);
    if (static_cast<uint64_t>(st.st_size) < need) { if (::ftruncate(fd, static_cast<off_t>(need)) != 0) return 3; }
    ::close(fd);
  }
  BlockDevicePtr outd;
  if (!RawImageDevice::open_path(out, false, 512, &outd).ok()) return 3;
  auto fd = std::make_shared<rt::FaultDevice>(raw, 512);
  for (int i = 0; i + 1 < argc; i++) {
    if (!std::strcmp(argv[i], "--fault")) {
      unsigned long long off, len;
      if (std::sscanf(argv[i + 1], "%llu:%llu", &off, &len) == 2)
        for (uint64_t s = off / 512; s * 512 < off + len; s++) fd->bad.insert(s);
    }
  }
  int64_t max_reads = std::strtoll(arg_value(argc, argv, "--max-reads", "-1").c_str(), nullptr, 10);
  RescueControl ctl;
  if (max_reads >= 0) fd->on_read = [&](uint64_t i) { if (static_cast<int64_t>(i) >= max_reads) ctl.cancel = true; };
  RescueOptions opt;
  opt.start = start; opt.length = length; opt.out_offset = out_off;
  opt.sector_size = static_cast<uint32_t>(std::strtoul(arg_value(argc, argv, "--sector", "512").c_str(), nullptr, 0));
  opt.cluster_sectors = static_cast<uint32_t>(std::strtoul(arg_value(argc, argv, "--cluster", "64").c_str(), nullptr, 0));
  opt.retries = static_cast<uint32_t>(std::strtoul(arg_value(argc, argv, "--retries", "0").c_str(), nullptr, 0));
  opt.do_trim = !has_flag(argc, argv, "--no-trim");
  opt.do_scrape = !has_flag(argc, argv, "--no-scrape");
  opt.skip_on_error = !has_flag(argc, argv, "--no-skip");
  opt.checkpoint_every_ops = 1;
  Mapfile m(raw->size());
  MapMeta meta;
  {
    FILE* f = std::fopen(map.c_str(), "rb");
    if (f) {
      std::string t;
      char buf[65536];
      size_t n;
      while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) t.append(buf, n);
      std::fclose(f);
      if (!Mapfile::parse(t, raw->size(), &m, &meta).ok()) { std::printf("{\"stop\":\"bad-argument\",\"code\":\"bad-mapfile\"}\n"); return 5; }
    }
  }
  RescueEngine eng(fd, outd, &m, opt);
  RescueHooks hooks;
  hooks.persist = [&](const Mapfile& mm, const MapMeta& mt) {
    std::string tmp = map + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    std::string t = mm.to_text(mt);
    bool ok = std::fwrite(t.data(), 1, t.size(), f) == t.size();
    ok = (std::fclose(f) == 0) && ok;
    return ok && std::rename(tmp.c_str(), map.c_str()) == 0;
  };
  RescueResult r = eng.run(&ctl, hooks);
  outd->flush();
  std::printf("{\"stop\":%d,\"code\":\"%s\",\"finished\":%llu,\"bad\":%llu,\"nonTried\":%llu,\"nonTrimmed\":%llu,\"nonScraped\":%llu,\"readErrors\":%llu,\"inputWrites\":%llu}\n",
              static_cast<int>(r.stop), r.code.c_str(), (unsigned long long)m.count('+'), (unsigned long long)m.count('-'), (unsigned long long)m.count('?'),
              (unsigned long long)m.count('*'), (unsigned long long)m.count('/'), (unsigned long long)fd->errors, (unsigned long long)fd->writes);
  return r.stop == RescueStop::Finished ? 0 : 1;
}

static int cmd_backup(int argc, char** argv) {
  if (argc < 4) return 2;
  usbmem_device* d = nullptr;
  if (usbmem_open_path(argv[2], 1, &d) != 0) return 3;
  char* json = nullptr;
  int rc = usbmem_backup_run(d, argv[3], has_flag(argc, argv, "--lz4"), has_flag(argc, argv, "--full"), nullptr, &json);
  if (json) { std::printf("%s\n", json); usbmem_free(json); }
  usbmem_close(d);
  return rc;
}

static int cmd_info(int argc, char** argv) {
  if (argc < 3) return 2;
  char* json = nullptr;
  int rc = usbmem_backup_inspect_json(argv[2], has_flag(argc, argv, "--verify"), &json);
  if (json) { std::printf("%s\n", json); usbmem_free(json); }
  return rc;
}

static int cmd_bits(int argc, char** argv) {
  if (argc < 4) return 2;
  BlockDevicePtr dev;
  if (!RawImageDevice::open_path(argv[2], true, 512, &dev).ok()) return 3;
  UsedMap m;
  ExtractOptions eo;
  eo.full_if_unclean = has_flag(argc, argv, "--full");
  FsStatus f = build_used_map(*dev, eo, &m);
  if (!f.ok()) { std::printf("{\"ok\":false,\"code\":\"%s\",\"message\":\"%s\"}\n", f.code.c_str(), f.message.c_str()); return 1; }
  FILE* o = std::fopen(argv[3], "wb");
  if (!o) return 3;
  std::fwrite(m.bits.data(), 1, m.bits.size(), o);
  std::fclose(o);
  std::printf("{\"ok\":true,\"fs\":\"%s\",\"blockSize\":%u,\"totalBlocks\":%llu,\"usedBlocks\":%llu,\"deviceSize\":%llu}\n", m.fs.c_str(), m.block_size,
              (unsigned long long)m.total_blocks, (unsigned long long)m.used_blocks, (unsigned long long)m.device_size);
  return 0;
}

static int cmd_restore(int argc, char** argv, bool apply) {
  if (argc < 4) return 2;
  usbmem_device* d = nullptr;
  bool in_use = has_flag(argc, argv, "--in-use");
  if (usbmem_open_path(argv[3], apply ? 0 : 1, &d) != 0) { std::printf("{\"ok\":false,\"code\":\"open-failed\"}\n"); return 3; }
  usbmem_restore_plan* plan = nullptr;
  char* pj = nullptr;
  int rc = usbmem_restore_plan_new(argv[2], d, in_use, !has_flag(argc, argv, "--no-verify"), &plan, &pj);
  if (pj) { std::printf("%s\n", pj); }
  if (rc != 0 || !apply) { usbmem_free(pj); usbmem_restore_plan_free(plan); usbmem_close(d); return rc; }
  // 計画 JSON から確認文字列を取り出して打ち込む (人の代わり)
  std::string js = pj ? pj : "";
  usbmem_free(pj);
  size_t at = js.find("\"confirmPhrase\":\"");
  std::string phrase;
  if (at != std::string::npos) { at += 17; phrase = js.substr(at, js.find('"', at) - at); }
  char* tok = nullptr;
  usbmem_restore_token(plan, phrase.c_str(), &tok);
  char* rj = nullptr;
  rc = usbmem_restore_apply(plan, argv[2], d, tok ? tok : "", in_use, nullptr, &rj);
  if (rj) { std::printf("%s\n", rj); usbmem_free(rj); }
  usbmem_free(tok);
  usbmem_restore_plan_free(plan);
  usbmem_close(d);
  return rc;
}

int main(int argc, char** argv) {
  if (argc < 2) return 2;
  std::string c = argv[1];
  if (c == "rescue") return cmd_rescue(argc, argv);
  if (c == "backup") return cmd_backup(argc, argv);
  if (c == "info") return cmd_info(argc, argv);
  if (c == "bits") return cmd_bits(argc, argv);
  if (c == "restore") return cmd_restore(argc, argv, true);
  if (c == "plan") return cmd_restore(argc, argv, false);
  return 2;
}
