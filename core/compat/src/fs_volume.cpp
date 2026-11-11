// SPDX-License-Identifier: GPL-2.0-or-later
#include "usbmem/fs_volume.h"

namespace usbmem {

int FsVolume::resolve(const std::string& path, uint64_t* ino) {
  uint64_t cur = root();
  size_t i = 0;
  while (i < path.size()) {
    while (i < path.size() && path[i] == '/') i++;
    size_t j = path.find('/', i);
    if (j == std::string::npos) j = path.size();
    if (j > i) {
      int rc = lookup(cur, path.substr(i, j - i), &cur);
      if (rc) return rc;
    }
    i = j;
  }
  *ino = cur;
  return 0;
}

}  // namespace usbmem
