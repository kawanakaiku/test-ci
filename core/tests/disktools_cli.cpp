// SPDX-License-Identifier: Apache-2.0
// DiskTools の検証用 CLI。tools/check_disktools.py が呼ぶ。
//   usbmem_disktools_cli check <image> [--fs NAME] [--range OFF LEN]   読み取り専用で検査して JSON を出す。
//     終了コード: 0 clean / 1 warnings / 2 errors / 3 unsupported / 4 incomplete / 10 I/O 失敗
//   usbmem_disktools_cli mkfs <image> --fs fat32|exfat|ext2 [--label L] [--cluster N] [--sector N] [--seed N] [--timestamp T]
//                         [--ratio N] [--inode-size N] [--range OFF LEN] [--plan-only] [--backup FILE]
//     イメージファイルに計画→(自動確認)→適用して {"plan":..,"result":..} を出す。検証専用 (実デバイスには使わない)。
//     終了コード: 0 作成して検証まで通った / 2 計画が不可 / 3 適用に失敗 / 10 I/O 失敗
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "usbmem/devices.h"
#include "usbmem/fscheck.h"
#include "usbmem/mkfs.h"

using namespace usbmem;

int main(int argc, char** argv) {
  if (argc >= 3 && !std::strcmp(argv[1], "check")) {
    BlockDevicePtr dev;
    Status st = RawImageDevice::open_path(argv[2], true, 512, &dev);
    if (!st.ok()) { std::fprintf(stderr, "open failed: %s\n", errc_name(st.code)); return 10; }
    uint64_t off = 0, len = dev->size();
    FsType fs = FsType::Unknown;
    for (int i = 3; i < argc; i++) {
      if (!std::strcmp(argv[i], "--fs") && i + 1 < argc) {
        std::string n = argv[++i];
        for (int t = 1; t <= 17; t++)
          if (n == "fat12" && t == static_cast<int>(FsType::Fat12)) fs = FsType::Fat12;
          else if (n == "fat16" && t == static_cast<int>(FsType::Fat16)) fs = FsType::Fat16;
          else if (n == "fat32" && t == static_cast<int>(FsType::Fat32)) fs = FsType::Fat32;
          else if (n == "exfat" && t == static_cast<int>(FsType::Exfat)) fs = FsType::Exfat;
          else if (n == "ext2" && t == static_cast<int>(FsType::Ext2)) fs = FsType::Ext2;
          else if (n == "ext3" && t == static_cast<int>(FsType::Ext3)) fs = FsType::Ext3;
          else if (n == "ext4" && t == static_cast<int>(FsType::Ext4)) fs = FsType::Ext4;
          else if (n == "ntfs" && t == static_cast<int>(FsType::Ntfs)) fs = FsType::Ntfs;
      } else if (!std::strcmp(argv[i], "--range") && i + 2 < argc) {
        off = std::strtoull(argv[++i], nullptr, 0);
        len = std::strtoull(argv[++i], nullptr, 0);
      }
    }
    FsCheckReport rep;
    FsCheckOptions opt;
    st = check_filesystem(*dev, off, len, fs, opt, &rep);
    std::puts(fscheck_report_to_json(rep).c_str());
    if (!st.ok()) { std::fprintf(stderr, "check failed: %s\n", errc_name(st.code)); return 10; }
    return static_cast<int>(rep.verdict);
  }
  if (argc >= 3 && !std::strcmp(argv[1], "mkfs")) {
    MkfsOptions mo;
    bool plan_only = false;
    const char* backup_path = nullptr;
    uint32_t bs = 512;
    for (int i = 3; i < argc; i++) {
      std::string a = argv[i];
      auto num = [&](uint64_t* v) { if (i + 1 < argc) *v = std::strtoull(argv[++i], nullptr, 0); };
      uint64_t v = 0;
      if (a == "--fs" && i + 1 < argc) {
        std::string n = argv[++i];
        mo.fs = n == "fat32" ? FsType::Fat32 : n == "exfat" ? FsType::Exfat : n == "ext2" ? FsType::Ext2 : FsType::Unknown;
      } else if (a == "--label" && i + 1 < argc) mo.label = argv[++i];
      else if (a == "--cluster") { num(&v); mo.cluster_size = static_cast<uint32_t>(v); }
      else if (a == "--sector") { num(&v); mo.sector_size = static_cast<uint32_t>(v); bs = static_cast<uint32_t>(v); }
      else if (a == "--seed") num(&mo.seed);
      else if (a == "--timestamp") num(&mo.timestamp);
      else if (a == "--ratio") { num(&v); mo.inode_ratio = static_cast<uint32_t>(v); }
      else if (a == "--inode-size") { num(&v); mo.inode_size = static_cast<uint32_t>(v); }
      else if (a == "--range" && i + 2 < argc) { mo.offset = std::strtoull(argv[++i], nullptr, 0); mo.length = std::strtoull(argv[++i], nullptr, 0); }
      else if (a == "--plan-only") plan_only = true;
      else if (a == "--backup" && i + 1 < argc) backup_path = argv[++i];
    }
    BlockDevicePtr dev;
    Status st = RawImageDevice::open_path(argv[2], false, bs, &dev);
    if (!st.ok()) { std::fprintf(stderr, "open failed: %s\n", errc_name(st.code)); return 10; }
    MkfsPlan plan;
    MkfsResult pr = Mkfs::plan(*dev, mo, &plan);
    if (!pr.ok()) { std::fprintf(stderr, "plan failed: %s %s\n", pr.code.c_str(), pr.message.c_str()); return 10; }
    std::string out = "{\"plan\":" + mkfs_plan_to_json(plan);
    if (!plan.ok || plan_only) { std::puts((out + "}").c_str()); return plan.ok ? 0 : 2; }
    MkfsApplyOptions ao;
    ao.backup_sink = [&](const std::vector<uint8_t>& blob) {
      if (!backup_path) return Status::Ok();
      FILE* f = std::fopen(backup_path, "wb");
      if (!f) return Status::Make(Errc::Io);
      size_t w = std::fwrite(blob.data(), 1, blob.size(), f);
      std::fclose(f);
      return w == blob.size() ? Status::Ok() : Status::Make(Errc::Io);
    };
    MkfsResult r = Mkfs::apply(*dev, plan, Mkfs::make_confirm_token(plan, plan.confirm_phrase), ao);
    std::puts((out + ",\"result\":" + mkfs_result_to_json(r) + "}").c_str());
    return r.ok() && r.verified ? 0 : 3;
  }
  std::fprintf(stderr, "usage: %s check <image> [--fs fat12|fat16|fat32|exfat|ext2|ext3|ext4|ntfs] [--range OFFSET LENGTH]\n       %s mkfs <image> --fs fat32|exfat|ext2 ...\n", argv[0], argv[0]);
  return 64;
}
