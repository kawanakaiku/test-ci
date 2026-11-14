cmake_minimum_required(VERSION 3.22)
project(usbmem_jni LANGUAGES C CXX)
set(USBMEM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
# AGP の Debug ビルドは CMAKE_BUILD_TYPE=Debug で、CMAKE_CXX_FLAGS_DEBUG が空 (NDK の clang 既定 = -O0)。実機検証 (assembleDebug) の native コードが
# 無最適化になり、AES / SHA-256 / Argon2 / crc32c / 展開 / FS 本体が 5〜10 倍遅かった (LUKS2 4.5 MB/s・BitLocker 0.5 MB/s・Btrfs 9 MB/s の主因。docs/crypto-volumes.md 6.4)。
# Debug でも -O2 をかける (-g は残るのでクラッシュのスタックは読める)。無最適化で試したいときは -DUSBMEM_DEBUG_NO_OPT=ON。
option(USBMEM_DEBUG_NO_OPT "Do not add -O2 to non-release native builds" OFF)
if(NOT USBMEM_DEBUG_NO_OPT AND NOT CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo|MinSizeRel)$")
  add_compile_options(-O2)
endif()
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../../../../core core)
add_library(usbmem_jni SHARED jni_bridge.cpp jni_ntfs.cpp jni_partedit.cpp jni_transform.cpp jni_blockdev.cpp jni_disktools.cpp jni_recovery.cpp)
# jni_ntfs.cpp は GPL-2.0-or-later の NTFS (fs/ntfs) ・exFAT・ext2・ext4・FAT・isofs・udf・squashfs・erofs・btrfs・f2fs・xfs をリンクするため、この共有ライブラリ全体が GPL 扱い (docs/licenses.md)
target_link_libraries(usbmem_jni PRIVATE usbmem_ntfs usbmem_exfat usbmem_ext2 usbmem_ext4 usbmem_fat usbmem_isofs usbmem_udf usbmem_squashfs usbmem_erofs usbmem_btrfs usbmem_f2fs usbmem_xfs usbmem_transform usbmem_disktools usbmem_recovery usbmem_core log)
target_link_options(usbmem_jni PRIVATE -Wl,-z,max-page-size=16384)
# ネスト (ループバック) マウントの JNI (docs/nested-mount.md)
target_sources(usbmem_jni PRIVATE jni_nested.cpp)
