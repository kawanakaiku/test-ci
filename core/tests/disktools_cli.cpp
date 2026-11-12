// SPDX-License-Identifier: Apache-2.0
// DiskTools の検証用 CLI。tools/check_disktools.py が呼ぶ。
//   usbmem_disktools_cli check <image> [--fs NAME] [--range OFF LEN]   読み取り専用で検査して JSON を出す。
//     終了コード: 0 clean / 1 warnings / 2 errors / 3 unsupported / 4 incomplete / 10 I/O 失敗
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "usbmem/devices.h"
#include "usbmem/fscheck.h"

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
  std::fprintf(stderr, "usage: %s check <image> [--fs fat12|fat16|fat32|exfat|ext2|ext3|ext4|ntfs] [--range OFFSET LENGTH]\n", argv[0]);
  return 64;
}
