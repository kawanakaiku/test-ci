# SPDX-License-Identifier: Apache-2.0 (本ファイル) — 取り込む fs/ntfs は GPL-2.0-or-later
# LinuxCompat + NTFS Resurrection (無改変) を静的ライブラリ usbmem_ntfs としてビルドする。
set(NTFS_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../third_party/ntfs-resurrection)
file(GLOB NTFS_SRC ${NTFS_ROOT}/fs-ntfs/*.c ${NTFS_ROOT}/fs-ntfs/lib/*.c)
set(COMPAT_SRC src/rt_core.c src/rt_pagecache.c src/rt_write.c src/rt_stubs.c src/rt_vfs.c src/bdev_bridge.cpp src/ntfs_volume.cpp)

# 書き込み経路は既定で無効 (未検証)。ON でも実行時に read_only=0 で明示マウントしたときだけ有効。
option(USBMEM_NTFS_WRITE "NTFS 書き込み経路を有効にする (実験的・未検証)" OFF)
add_library(usbmem_ntfs STATIC ${NTFS_SRC} ${COMPAT_SRC})
# 上流 fs/ntfs は CONFIG_HAVE_EFFICIENT_UNALIGNED_ACCESS 前提 (x86/arm64 は非整列アクセス可) で u16/u32 を直接読む。
# UBSan の alignment 検査だけ上流コードで外す (他の UB 検査は有効のまま)。互換層 (src/rt_*.c) には適用しない。
if(USBMEM_SANITIZE)
  set_source_files_properties(${NTFS_SRC} PROPERTIES COMPILE_OPTIONS "-fno-sanitize=alignment;-fno-sanitize=shift-base;-fno-sanitize=nonnull-attribute")
  # 破損 runlist の巨大値で符号付き左シフトが溢れる (上流は -fno-strict-overflow 前提 = wrap)。実カーネルのビルドと同じ扱いにする。
endif()
target_include_directories(usbmem_ntfs PUBLIC public)
# 互換シム(<linux/errno.h> 等)は C ソース専用。C++ に見せると glibc の <errno.h> が奪われ std ヘッダと衝突する。
target_compile_options(usbmem_ntfs PRIVATE $<$<COMPILE_LANGUAGE:C>:-I${CMAKE_CURRENT_LIST_DIR}/include>)
target_include_directories(usbmem_ntfs PRIVATE ${NTFS_ROOT}/include)
# 上流のヘッダ検索: #include "x.h" はそのディレクトリ基準。-iquote で fs-ntfs を足す (-I だと <time.h> を奪う)
target_compile_options(usbmem_ntfs PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-std=gnu11>
  $<$<COMPILE_LANGUAGE:C>:-w>
  $<$<COMPILE_LANGUAGE:C>:-fno-strict-aliasing>
  $<$<COMPILE_LANGUAGE:C>:-fwrapv>   # カーネルは -fno-strict-overflow でビルドされる
  "$<$<COMPILE_LANGUAGE:C>:SHELL:-iquote ${NTFS_ROOT}/fs-ntfs>"
  "$<$<COMPILE_LANGUAGE:C>:SHELL:-iquote ${NTFS_ROOT}/fs-ntfs/lib>")
target_compile_definitions(usbmem_ntfs PRIVATE CONFIG_NTFS_FS_WOF_COMPRESSION=1
  CONFIG_HAVE_EFFICIENT_UNALIGNED_ACCESS=1)
if(USBMEM_NTFS_WRITE)
  target_compile_definitions(usbmem_ntfs PRIVATE USBMEM_NTFS_WRITE=1)
endif()
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
  # 3 TiB の疎イメージで、マウントが $Bitmap 全体を読まないこと (実機の 7.3 TiB USB ディスクで 3 分以上かかった不具合の回帰)
  add_test(NAME ntfs_large_volume COMMAND ${CMAKE_CURRENT_LIST_DIR}/../../tools/run_large_volume_test.sh $<TARGET_FILE:usbmem_ntfs_tests>)
  set_tests_properties(ntfs_large_volume PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300 ENVIRONMENT "ASAN_OPTIONS=verify_asan_link_order=0:detect_leaks=0")
  set_tests_properties(ntfs_read PROPERTIES FIXTURES_REQUIRED ntfs_fixture)
  # Windows (WinPE) が作ったイメージがあれば検証 (無ければ SKIP)。tools/winpe/make_windows_fixtures.sh で作る。
  add_test(NAME ntfs_windows_images COMMAND ${CMAKE_CURRENT_LIST_DIR}/../../tools/run_win_fixture_tests.sh $<TARGET_FILE:usbmem_ntfs_tests>)
  set_tests_properties(ntfs_windows_images PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900
    ENVIRONMENT "ASAN_OPTIONS=verify_asan_link_order=0:detect_leaks=0")
  if(USBMEM_NTFS_WRITE)
    add_test(NAME ntfs_write COMMAND ${CMAKE_CURRENT_LIST_DIR}/../../tools/run_write_tests.sh $<TARGET_FILE:usbmem_ntfs_tests>)
    set_tests_properties(ntfs_write PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600 ENVIRONMENT "ASAN_OPTIONS=verify_asan_link_order=0:detect_leaks=1")
    # 電源断の模擬 (軽量版: 8 点 x {順序保持,キャッシュ} x {全セクタ,途中切断})。フル版は tools/run_crash_tests.sh <bin> <dir> 40
    add_test(NAME ntfs_crash COMMAND ${CMAKE_CURRENT_LIST_DIR}/../../tools/run_crash_tests.sh $<TARGET_FILE:usbmem_ntfs_tests> /dev/shm/usbmem_ntfs_$ENV{USER} 8)
    set_tests_properties(ntfs_crash PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)
  endif()
  add_test(NAME ntfs_fuzz COMMAND usbmem_ntfs_tests --fuzz ${NTFS_FIX_DIR} 150 1)
  set_tests_properties(ntfs_fuzz PROPERTIES FIXTURES_REQUIRED ntfs_fixture TIMEOUT 600
    # 失敗したマウントの後始末は inode 内部の確保物を解放しきれない (既知)。メモリ安全性 (UAF/OOB/ハング) の検査に集中する。
    ENVIRONMENT "ASAN_OPTIONS=verify_asan_link_order=0:detect_leaks=0")
endif()
