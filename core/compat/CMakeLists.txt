# SPDX-License-Identifier: Apache-2.0 (本ファイル) — 取り込む fs/ntfs は GPL-2.0-or-later
# LinuxCompat + NTFS Resurrection (無改変) を静的ライブラリ usbmem_ntfs としてビルドする。
set(NTFS_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../third_party/ntfs-resurrection)
file(GLOB NTFS_SRC ${NTFS_ROOT}/fs-ntfs/*.c ${NTFS_ROOT}/fs-ntfs/lib/*.c)
set(COMPAT_SRC src/rt_core.c src/rt_pagecache.c src/rt_stubs.c src/rt_vfs.c src/bdev_bridge.cpp src/ntfs_volume.cpp)

add_library(usbmem_ntfs STATIC ${NTFS_SRC} ${COMPAT_SRC})
target_include_directories(usbmem_ntfs PUBLIC public)
# 互換シム(<linux/errno.h> 等)は C ソース専用。C++ に見せると glibc の <errno.h> が奪われ std ヘッダと衝突する。
target_compile_options(usbmem_ntfs PRIVATE $<$<COMPILE_LANGUAGE:C>:-I${CMAKE_CURRENT_LIST_DIR}/include>)
target_include_directories(usbmem_ntfs PRIVATE ${NTFS_ROOT}/include)
# 上流のヘッダ検索: #include "x.h" はそのディレクトリ基準。-iquote で fs-ntfs を足す (-I だと <time.h> を奪う)
target_compile_options(usbmem_ntfs PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-std=gnu11>
  $<$<COMPILE_LANGUAGE:C>:-w>
  $<$<COMPILE_LANGUAGE:C>:-fno-strict-aliasing>
  "$<$<COMPILE_LANGUAGE:C>:SHELL:-iquote ${NTFS_ROOT}/fs-ntfs>"
  "$<$<COMPILE_LANGUAGE:C>:SHELL:-iquote ${NTFS_ROOT}/fs-ntfs/lib>")
target_compile_definitions(usbmem_ntfs PRIVATE CONFIG_NTFS_FS_WOF_COMPRESSION=1
  CONFIG_HAVE_EFFICIENT_UNALIGNED_ACCESS=1)
target_link_libraries(usbmem_ntfs PUBLIC usbmem_core Threads::Threads)

if(USBMEM_BUILD_TESTS)
  add_executable(usbmem_ntfs_tests tests/test_ntfs.cpp)
  target_link_libraries(usbmem_ntfs_tests PRIVATE usbmem_ntfs)
  # 実 NTFS イメージは tmpfs (/dev/shm) に生成する。mkntfs/ntfs-3g/FUSE が無ければ SKIP。
  if(EXISTS /dev/shm)
    set(NTFS_FIX_DIR /dev/shm/usbmem_ntfs_$ENV{USER})
  else()
    set(NTFS_FIX_DIR ${CMAKE_CURRENT_BINARY_DIR}/ntfs_fixture)
  endif()
  add_test(NAME ntfs_gen_fixture COMMAND ${CMAKE_CURRENT_LIST_DIR}/../../tools/gen_ntfs_fixture.sh ${NTFS_FIX_DIR})
  set_tests_properties(ntfs_gen_fixture PROPERTIES SKIP_RETURN_CODE 77 FIXTURES_SETUP ntfs_fixture)
  add_test(NAME ntfs_read COMMAND usbmem_ntfs_tests ${NTFS_FIX_DIR})
  set_tests_properties(ntfs_read PROPERTIES FIXTURES_REQUIRED ntfs_fixture)
endif()
