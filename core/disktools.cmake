# SPDX-License-Identifier: Apache-2.0
# DiskTools: 読み取り専用の整合性検査 (fsck 相当) と mkfs。docs/disk-tools.md
# core/CMakeLists.txt の末尾から include される。usbmem_core にだけ依存する (Android 非依存・PC でテスト可能)。
add_library(usbmem_disktools STATIC
  src/fscheck.cpp src/fscheck_fat.cpp src/fscheck_exfat.cpp src/fscheck_ext.cpp src/fscheck_ntfs.cpp
  src/mkfs.cpp src/mkfs_fat32.cpp src/mkfs_exfat.cpp src/mkfs_ext2.cpp)
target_include_directories(usbmem_disktools PUBLIC include)
target_link_libraries(usbmem_disktools PUBLIC usbmem_core)
target_compile_options(usbmem_disktools PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wno-sign-conversion)

if(USBMEM_BUILD_TESTS)
  # 検証用 CLI (tools/check_disktools.py が使う)。イメージを読み取り専用で検査する。
  add_executable(usbmem_disktools_cli tests/disktools_cli.cpp)
  target_link_libraries(usbmem_disktools_cli PRIVATE usbmem_disktools)
endif()
