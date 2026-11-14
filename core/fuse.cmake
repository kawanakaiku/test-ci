# SPDX-License-Identifier: Apache-2.0 (本ファイル)
# usbmem_fuse: core の FS API を FUSE (libfuse3) でホストに見せるアダプター。xfstests (fstests) で自前実装を試すために使う (docs/xfstests.md)。
# 既定は OFF (libfuse3-dev が要る)。書き込みを見せるには -DUSBMEM_NTFS_WRITE=ON も要る。
option(USBMEM_BUILD_FUSE "usbmem_fuse (libfuse3 アダプター) をビルドする" OFF)
if(USBMEM_BUILD_FUSE)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(FUSE3 REQUIRED IMPORTED_TARGET fuse3)
  add_executable(usbmem_fuse ${CMAKE_CURRENT_LIST_DIR}/../tools/fuse/usbmem_fuse.cpp)
  target_link_libraries(usbmem_fuse PRIVATE
    usbmem_ntfs usbmem_exfat usbmem_ext2 usbmem_ext4 usbmem_fat usbmem_xfs usbmem_btrfs usbmem_f2fs
    usbmem_isofs usbmem_udf usbmem_squashfs usbmem_erofs usbmem_compat_rt usbmem_core PkgConfig::FUSE3 Threads::Threads)
endif()
