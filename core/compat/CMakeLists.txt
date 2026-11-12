# SPDX-License-Identifier: Apache-2.0 (本ファイル) — 取り込む fs/ntfs は GPL-2.0-or-later
# LinuxCompat + NTFS Resurrection (無改変) を静的ライブラリ usbmem_ntfs としてビルドする。
set(NTFS_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../third_party/ntfs-resurrection)
set(EXFAT_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../third_party/exfat)
set(EXT2_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../third_party/ext2)
set(EXT4_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../third_party/ext4)
file(GLOB NTFS_SRC ${NTFS_ROOT}/fs-ntfs/*.c ${NTFS_ROOT}/fs-ntfs/lib/*.c)
file(GLOB EXFAT_SRC ${EXFAT_ROOT}/fs-exfat/*.c)
# ext2: カーネルの Makefile の ext2-y (xattr / ACL / security は CONFIG なし = 含めない)
foreach(f balloc dir file ialloc inode ioctl namei super symlink trace)
  list(APPEND EXT2_SRC ${EXT2_ROOT}/fs-ext2/${f}.c)
endforeach()
# ext4: 読み取り経路に必要なものだけをそのままコンパイルする (書き込み・journal・mballoc・ioctl・sysfs 等は src/rt_ext4.c の最小スタブ。docs/ext4-port.md)
foreach(f balloc bitmap block_validity dir ext4_jbd2 extents extents_status file fsync hash ialloc indirect inline inode namei readpage super symlink xattr xattr_hurd xattr_trusted xattr_user)
  list(APPEND EXT4_SRC ${EXT4_ROOT}/fs-ext4/${f}.c)
endforeach()

# ---- 共通ランタイム (LinuxCompat 本体: inode/ページキャッシュ/bio/buffer_head/VFS 風マウント)。FS ごとの上流コードはここに入れない。
set(COMPAT_RT_SRC src/rt_core.c src/rt_pagecache.c src/rt_write.c src/rt_stubs.c src/rt_bh.c src/rt_buffer.c src/bdev_bridge.cpp src/fs_volume.cpp)
add_library(usbmem_compat_rt STATIC ${COMPAT_RT_SRC})
target_include_directories(usbmem_compat_rt PUBLIC public)
# 互換シム(<linux/errno.h> 等)は C ソース専用。C++ に見せると glibc の <errno.h> が奪われ std ヘッダと衝突する。
target_compile_options(usbmem_compat_rt PUBLIC $<$<COMPILE_LANGUAGE:C>:-I${CMAKE_CURRENT_LIST_DIR}/include>)
target_compile_options(usbmem_compat_rt PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-std=gnu11>
  $<$<COMPILE_LANGUAGE:C>:-w>
  $<$<COMPILE_LANGUAGE:C>:-fno-strict-aliasing>
  $<$<COMPILE_LANGUAGE:C>:-fwrapv>)
target_link_libraries(usbmem_compat_rt PUBLIC usbmem_core Threads::Threads)

# 書き込み経路は既定で無効 (未検証)。ON でも実行時に read_only=0 で明示マウントしたときだけ有効。
option(USBMEM_NTFS_WRITE "NTFS 書き込み経路を有効にする (実験的・未検証)" OFF)
# ---- NTFS (NTFS Resurrection, 無改変)
add_library(usbmem_ntfs STATIC ${NTFS_SRC} src/rt_vfs.c src/ntfs_volume.cpp)
# 上流 fs/ntfs は CONFIG_HAVE_EFFICIENT_UNALIGNED_ACCESS 前提 (x86/arm64 は非整列アクセス可) で u16/u32 を直接読む。
# UBSan の alignment 検査だけ上流コードで外す (他の UB 検査は有効のまま)。互換層 (src/rt_*.c) には適用しない。
if(USBMEM_SANITIZE)
  set_source_files_properties(${NTFS_SRC} PROPERTIES COMPILE_OPTIONS "-fno-sanitize=alignment;-fno-sanitize=shift-base;-fno-sanitize=nonnull-attribute")
  # 破損 runlist の巨大値で符号付き左シフトが溢れる (上流は -fno-strict-overflow 前提 = wrap)。実カーネルのビルドと同じ扱いにする。
  set_source_files_properties(${EXT2_SRC} PROPERTIES COMPILE_OPTIONS "-fno-sanitize=alignment;-fno-sanitize=shift-base;-fno-sanitize=nonnull-attribute")
  set_source_files_properties(${EXT4_SRC} PROPERTIES COMPILE_OPTIONS "-fno-sanitize=alignment;-fno-sanitize=shift-base;-fno-sanitize=nonnull-attribute")
  set_source_files_properties(${EXFAT_SRC} PROPERTIES COMPILE_OPTIONS "-fno-sanitize=alignment;-fno-sanitize=shift-base;-fno-sanitize=nonnull-attribute")
endif()
target_include_directories(usbmem_ntfs PUBLIC public)
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
target_link_libraries(usbmem_ntfs PUBLIC usbmem_compat_rt)

# ---- exFAT (Linux fs/exfat, 無改変)。現状は読み取り専用マウントのみ (書き込みは未検証のため入口を用意しない)。
add_library(usbmem_exfat STATIC ${EXFAT_SRC} src/rt_vfs_exfat.c src/exfat_volume.cpp)
target_include_directories(usbmem_exfat PUBLIC public)
target_include_directories(usbmem_exfat PRIVATE ${EXFAT_ROOT}/include)
target_compile_options(usbmem_exfat PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-std=gnu11>
  $<$<COMPILE_LANGUAGE:C>:-w>
  $<$<COMPILE_LANGUAGE:C>:-fno-strict-aliasing>
  $<$<COMPILE_LANGUAGE:C>:-fwrapv>
  "$<$<COMPILE_LANGUAGE:C>:SHELL:-iquote ${EXFAT_ROOT}/fs-exfat>")
target_compile_definitions(usbmem_exfat PRIVATE CONFIG_HAVE_EFFICIENT_UNALIGNED_ACCESS=1
  USBMEM_MODULE_INIT_NAME=usbmem_module_init_exfat "CONFIG_EXFAT_DEFAULT_IOCHARSET=\"utf8\"")
if(USBMEM_NTFS_WRITE)
  target_compile_definitions(usbmem_exfat PRIVATE USBMEM_NTFS_WRITE=1)   # 書き込み入口 (rt_vfs_exfat.c) を有効にする。実行時にも mount(read_only=0) が必要
endif()
target_link_libraries(usbmem_exfat PUBLIC usbmem_compat_rt)

# ---- ext2 (Linux fs/ext2, 無改変)。読み取り専用マウントのみ (ext3/ext4 の機能を持つボリュームは上流が拒否する)。
add_library(usbmem_ext2 STATIC ${EXT2_SRC} src/rt_vfs_ext2.c src/ext2_volume.cpp)
target_include_directories(usbmem_ext2 PUBLIC public)
target_include_directories(usbmem_ext2 PRIVATE ${EXT2_ROOT}/include)
target_compile_options(usbmem_ext2 PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-std=gnu11>
  $<$<COMPILE_LANGUAGE:C>:-w>
  $<$<COMPILE_LANGUAGE:C>:-fno-strict-aliasing>
  $<$<COMPILE_LANGUAGE:C>:-fwrapv>
  "$<$<COMPILE_LANGUAGE:C>:SHELL:-iquote ${EXT2_ROOT}/fs-ext2>")
target_compile_definitions(usbmem_ext2 PRIVATE CONFIG_HAVE_EFFICIENT_UNALIGNED_ACCESS=1 USBMEM_MODULE_INIT_NAME=usbmem_module_init_ext2)
if(USBMEM_NTFS_WRITE)
  target_compile_definitions(usbmem_ext2 PRIVATE USBMEM_NTFS_WRITE=1)   # 書き込み入口 (rt_vfs_ext2.c) を有効にする。実行時にも mount(read_only=0) が必要
endif()
target_link_libraries(usbmem_ext2 PUBLIC usbmem_compat_rt)

# ---- ext4 (Linux fs/ext4, 無改変)。読み取り専用・journal 無し (noload 固定)。
add_library(usbmem_ext4 STATIC ${EXT4_SRC} src/rt_ext4.c src/rt_vfs_ext4.c src/ext4_volume.cpp)
target_include_directories(usbmem_ext4 PUBLIC public)
target_include_directories(usbmem_ext4 PRIVATE ${EXT4_ROOT}/include)
target_compile_options(usbmem_ext4 PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-std=gnu11>
  $<$<COMPILE_LANGUAGE:C>:-w>
  $<$<COMPILE_LANGUAGE:C>:-fno-strict-aliasing>
  $<$<COMPILE_LANGUAGE:C>:-fwrapv>
  "$<$<COMPILE_LANGUAGE:C>:SHELL:-iquote ${EXT4_ROOT}/fs-ext4>")
target_compile_definitions(usbmem_ext4 PRIVATE CONFIG_HAVE_EFFICIENT_UNALIGNED_ACCESS=1 CONFIG_BLOCK=1 __KERNEL__ USBMEM_RCU_DEFER=1 USBMEM_MODULE_INIT_NAME=usbmem_module_init_ext4)
target_link_libraries(usbmem_ext4 PUBLIC usbmem_compat_rt)

if(USBMEM_BUILD_TESTS)
  add_executable(usbmem_exfat_tests tests/test_exfat.cpp)
  target_link_libraries(usbmem_exfat_tests PRIVATE usbmem_exfat)
  # テスト用 exFAT イメージ: mkfs.exfat + tools/gen_exfat_fixture.py (Linux ドライバとは独立の Python 実装) で作り、fsck.exfat で検証。無ければ SKIP。
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_FOUND AND EXISTS /dev/shm)
    set(EXFAT_FIX_DIR /dev/shm/usbmem_exfat_$ENV{USER})
    add_test(NAME exfat_gen_fixture COMMAND ${CMAKE_COMMAND} -E env "PATH=$ENV{PATH}:/sbin:/usr/sbin" ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_LIST_DIR}/../../tools/gen_exfat_fixture.py ${EXFAT_FIX_DIR})
    set_tests_properties(exfat_gen_fixture PROPERTIES FIXTURES_SETUP exfat_fixture)
    add_test(NAME exfat_read COMMAND usbmem_exfat_tests ${EXFAT_FIX_DIR})
    add_test(NAME exfat_corrupt COMMAND usbmem_exfat_tests --corrupt ${EXFAT_FIX_DIR})
    add_test(NAME exfat_ioerr COMMAND usbmem_exfat_tests --ioerr ${EXFAT_FIX_DIR})
    add_test(NAME exfat_fuzz COMMAND usbmem_exfat_tests --fuzz ${EXFAT_FIX_DIR} 600 1)
    set_tests_properties(exfat_read exfat_corrupt exfat_ioerr exfat_fuzz PROPERTIES FIXTURES_REQUIRED exfat_fixture TIMEOUT 600)
    # 失敗したマウントの後始末は inode 内部の確保物を解放しきれない (NTFS と同じ既知事項)。メモリ安全性 (UAF/OOB/ハング) の検査に集中する。
    set_tests_properties(exfat_corrupt exfat_ioerr exfat_fuzz PROPERTIES ENVIRONMENT "ASAN_OPTIONS=verify_asan_link_order=0:detect_leaks=0")
    set_tests_properties(exfat_read PROPERTIES ENVIRONMENT "ASAN_OPTIONS=verify_asan_link_order=0:detect_leaks=1")
  endif()
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
  add_test(NAME ntfs_bitops COMMAND usbmem_ntfs_tests --bitops)
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
    # クラスタ 2048B (1 folio に 2 クラスタ): 実機の RAID (7.3 TiB) がこのサイズだった。電源断の模擬も同じ検査を通すこと
    add_test(NAME ntfs_gen_fixture_c2048 COMMAND ${CMAKE_COMMAND} -E env USBMEM_NTFS_CLUSTER=2048 ${CMAKE_CURRENT_LIST_DIR}/../../tools/gen_ntfs_fixture.sh /dev/shm/usbmem_ntfs_c2048_$ENV{USER})
    set_tests_properties(ntfs_gen_fixture_c2048 PROPERTIES SKIP_RETURN_CODE 77 FIXTURES_SETUP ntfs_fixture_c2048)
    add_test(NAME ntfs_crash_c2048 COMMAND ${CMAKE_CURRENT_LIST_DIR}/../../tools/run_crash_tests.sh $<TARGET_FILE:usbmem_ntfs_tests> /dev/shm/usbmem_ntfs_c2048_$ENV{USER} 8)
    set_tests_properties(ntfs_crash_c2048 PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900 FIXTURES_REQUIRED ntfs_fixture_c2048)
  endif()
  add_test(NAME ntfs_fuzz COMMAND usbmem_ntfs_tests --fuzz ${NTFS_FIX_DIR} 150 1)
  set_tests_properties(ntfs_fuzz PROPERTIES FIXTURES_REQUIRED ntfs_fixture TIMEOUT 600
    # 失敗したマウントの後始末は inode 内部の確保物を解放しきれない (既知)。メモリ安全性 (UAF/OOB/ハング) の検査に集中する。
    ENVIRONMENT "ASAN_OPTIONS=verify_asan_link_order=0:detect_leaks=0")
endif()

# ---- ext2 のテスト: mke2fs -d + tools/gen_ext2_fixture.py でイメージを作り (e2fsck -fn で clean を確認)、fs/ext2 の読み取りを全項目検証
if(USBMEM_BUILD_TESTS)
  add_executable(usbmem_ext2_tests tests/test_ext2.cpp)
  target_link_libraries(usbmem_ext2_tests PRIVATE usbmem_ext2)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_FOUND AND EXISTS /dev/shm)
    set(EXT2_FIX_DIR /dev/shm/usbmem_ext2_$ENV{USER})
    add_test(NAME ext2_gen_fixture COMMAND ${CMAKE_COMMAND} -E env "PATH=$ENV{PATH}:/sbin:/usr/sbin" ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_LIST_DIR}/../../tools/gen_ext2_fixture.py ${EXT2_FIX_DIR})
    set_tests_properties(ext2_gen_fixture PROPERTIES FIXTURES_SETUP ext2_fixture)
    add_test(NAME ext2_read COMMAND usbmem_ext2_tests ${EXT2_FIX_DIR})
    add_test(NAME ext2_corrupt COMMAND usbmem_ext2_tests --corrupt ${EXT2_FIX_DIR})
    add_test(NAME ext2_ioerr COMMAND usbmem_ext2_tests --ioerr ${EXT2_FIX_DIR})
    add_test(NAME ext2_fuzz COMMAND usbmem_ext2_tests --fuzz ${EXT2_FIX_DIR} 600 1)
    set_tests_properties(ext2_read ext2_corrupt ext2_ioerr ext2_fuzz PROPERTIES FIXTURES_REQUIRED ext2_fixture TIMEOUT 900)
    # 失敗したマウントの後始末は inode 内部の確保物を解放しきれない (NTFS / exFAT と同じ既知事項)。メモリ安全性 (UAF/OOB/ハング) の検査に集中する。
    set_tests_properties(ext2_corrupt ext2_ioerr ext2_fuzz PROPERTIES ENVIRONMENT "ASAN_OPTIONS=verify_asan_link_order=0:detect_leaks=0")
    set_tests_properties(ext2_read PROPERTIES ENVIRONMENT "ASAN_OPTIONS=verify_asan_link_order=0:detect_leaks=1")
  endif()
endif()

# ---- ext2 書き込みのテスト (USBMEM_NTFS_WRITE=ON のときだけ): 操作列 / モデルとの突き合わせ / 満杯 / 拒否 / 電源断。独立実装 (e2fsck・fuse2fs・debugfs) での検証は tools/run_ext2_write_tests.sh
if(USBMEM_BUILD_TESTS AND USBMEM_NTFS_WRITE)
  add_executable(usbmem_ext2_write_tests tests/test_ext2_write.cpp)
  target_link_libraries(usbmem_ext2_write_tests PRIVATE usbmem_ext2)
  add_test(NAME ext2_write COMMAND ${CMAKE_CURRENT_LIST_DIR}/../../tools/run_ext2_write_tests.sh $<TARGET_FILE:usbmem_ext2_write_tests>)
  set_tests_properties(ext2_write PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1800 ENVIRONMENT "ASAN_OPTIONS=verify_asan_link_order=0:detect_leaks=1")
  # 電源断の模擬 (軽量版: 8 点 x {順序保持,キャッシュ} x {全セクタ,途中切断} x {最後に 1 回 sync, 操作ごとに sync})。フル版は tools/run_ext2_crash_tests.sh <bin> <dir> 40
  add_test(NAME ext2_crash COMMAND ${CMAKE_CURRENT_LIST_DIR}/../../tools/run_ext2_crash_tests.sh $<TARGET_FILE:usbmem_ext2_write_tests> /dev/shm/usbmem_ext2_$ENV{USER} 8)
  set_tests_properties(ext2_crash PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)
endif()

# ---- exFAT 書き込みのテスト (USBMEM_NTFS_WRITE=ON のときだけ): 操作列 / モデル / 満杯 / 拒否 / 名前 / 電源断。独立実装 (fsck.exfat・exfat-fuse) での検証は tools/run_exfat_write_tests.sh
if(USBMEM_BUILD_TESTS AND USBMEM_NTFS_WRITE)
  add_executable(usbmem_exfat_write_tests tests/test_exfat_write.cpp)
  target_link_libraries(usbmem_exfat_write_tests PRIVATE usbmem_exfat)
  add_test(NAME exfat_write COMMAND ${CMAKE_CURRENT_LIST_DIR}/../../tools/run_exfat_write_tests.sh $<TARGET_FILE:usbmem_exfat_write_tests> /dev/shm/usbmem_exfat_$ENV{USER} 2)
  set_tests_properties(exfat_write PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1800 ENVIRONMENT "ASAN_OPTIONS=verify_asan_link_order=0:detect_leaks=1")
  add_test(NAME exfat_crash COMMAND ${CMAKE_CURRENT_LIST_DIR}/../../tools/run_exfat_crash_tests.sh $<TARGET_FILE:usbmem_exfat_write_tests> /dev/shm/usbmem_exfat_$ENV{USER} 8)
  set_tests_properties(exfat_crash PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)
endif()

# ---- ext4 のテスト: mke2fs -d + tools/gen_ext4_fixture.py (機能の組み合わせごと・e2fsck -fn で clean を確認)。ext2 とのコンテナ共存 (同一プロセスで両方マウント) も検査する。
if(USBMEM_BUILD_TESTS)
  add_executable(usbmem_ext4_tests tests/test_ext4.cpp)
  target_link_libraries(usbmem_ext4_tests PRIVATE usbmem_ext4 usbmem_ext2)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_FOUND AND EXISTS /dev/shm)
    set(EXT4_FIX_DIR /dev/shm/usbmem_ext4_$ENV{USER})
    add_test(NAME ext4_gen_fixture COMMAND ${CMAKE_COMMAND} -E env "PATH=$ENV{PATH}:/sbin:/usr/sbin" ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_LIST_DIR}/../../tools/gen_ext4_fixture.py ${EXT4_FIX_DIR})
    set_tests_properties(ext4_gen_fixture PROPERTIES FIXTURES_SETUP ext4_fixture TIMEOUT 900)
    add_test(NAME ext4_read COMMAND usbmem_ext4_tests ${EXT4_FIX_DIR})
    add_test(NAME ext4_corrupt COMMAND usbmem_ext4_tests --corrupt ${EXT4_FIX_DIR})
    add_test(NAME ext4_ioerr COMMAND usbmem_ext4_tests --ioerr ${EXT4_FIX_DIR})
    add_test(NAME ext4_fuzz COMMAND usbmem_ext4_tests --fuzz ${EXT4_FIX_DIR} 600 1)
    add_test(NAME ext4_coexist COMMAND usbmem_ext4_tests --coexist ${EXT4_FIX_DIR} /dev/shm/usbmem_ext2_$ENV{USER})
    set_tests_properties(ext4_read ext4_corrupt ext4_ioerr ext4_fuzz PROPERTIES FIXTURES_REQUIRED ext4_fixture TIMEOUT 1800)
    set_tests_properties(ext4_coexist PROPERTIES FIXTURES_REQUIRED "ext4_fixture;ext2_fixture" TIMEOUT 600)
    set_tests_properties(ext4_corrupt ext4_ioerr ext4_fuzz ext4_coexist PROPERTIES ENVIRONMENT "ASAN_OPTIONS=verify_asan_link_order=0:detect_leaks=0")
    set_tests_properties(ext4_read PROPERTIES ENVIRONMENT "ASAN_OPTIONS=verify_asan_link_order=0:detect_leaks=1")
  endif()
endif()

# ---- FAT12/16/32 + vfat (Linux fs/fat, 無改変)。読み取り専用マウントのみ。docs/fat-port.md
# FAT 専用の補助 (include_fat/ = 空シムと compat/fat_shim.h) は、このライブラリにだけ入る。
set(FAT_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../third_party/fat)
foreach(f cache dir fatent file inode misc namei_vfat)
  list(APPEND FAT_SRC ${FAT_ROOT}/fs-fat/${f}.c)
endforeach()
set(FAT_NLS_SRC ${FAT_ROOT}/nls/nls_cp437.c ${FAT_ROOT}/nls/nls_cp932.c)
add_library(usbmem_fat STATIC ${FAT_SRC} ${FAT_NLS_SRC} src/rt_vfs_fat.c src/fat_volume.cpp)
target_include_directories(usbmem_fat PUBLIC public)
target_include_directories(usbmem_fat PRIVATE ${CMAKE_CURRENT_LIST_DIR}/include_fat ${FAT_ROOT}/include)
target_compile_options(usbmem_fat PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-std=gnu11>
  $<$<COMPILE_LANGUAGE:C>:-w>
  $<$<COMPILE_LANGUAGE:C>:-Wno-incompatible-pointer-types>            # nls_cp*.c の表は char2uni を wchar_t * (16 ビット) で定義する。互換層の表の型 (void *) とは関数ポインタの引数型が違うだけ (GCC14 は -w でもエラーにする)
  $<$<COMPILE_LANGUAGE:C>:-Wno-incompatible-function-pointer-types>   # clang の同等のオプション
  $<$<COMPILE_LANGUAGE:C>:-fno-strict-aliasing>
  $<$<COMPILE_LANGUAGE:C>:-fwrapv>
  $<$<COMPILE_LANGUAGE:C>:-fshort-wchar>   # カーネルは wchar_t = 16 ビット (-fshort-wchar)。FAT / nls_cp*.c は UTF-16 の配列を wchar_t で持つ
  "$<$<COMPILE_LANGUAGE:C>:SHELL:-include compat/fat_shim.h>"
  "$<$<COMPILE_LANGUAGE:C>:SHELL:-iquote ${FAT_ROOT}/fs-fat>")
# USBMEM_RCU_DEFER: fat_put_super が sbi を call_rcu で解放した後に、残りの inode (fat_inode / fsinfo_inode) の evict が sbi を読むため、call_rcu を umount の最後まで遅らせる (ext4 と同じ)
target_compile_definitions(usbmem_fat PRIVATE USBMEM_RCU_DEFER=1 CONFIG_HAVE_EFFICIENT_UNALIGNED_ACCESS=1 CONFIG_FAT_DEFAULT_CODEPAGE=437 "CONFIG_FAT_DEFAULT_IOCHARSET=\"utf8\"")
# module_init は翻訳単位ごとに別名にする (src/rt_vfs_fat.c の usbmem_module_init_fat が順に呼ぶ)
set_source_files_properties(${FAT_ROOT}/fs-fat/inode.c PROPERTIES COMPILE_DEFINITIONS USBMEM_MODULE_INIT_NAME=usbmem_fat_init_core)
set_source_files_properties(${FAT_ROOT}/fs-fat/namei_vfat.c PROPERTIES COMPILE_DEFINITIONS USBMEM_MODULE_INIT_NAME=usbmem_fat_init_vfat)
set_source_files_properties(${FAT_ROOT}/nls/nls_cp437.c PROPERTIES COMPILE_DEFINITIONS USBMEM_MODULE_INIT_NAME=usbmem_fat_init_cp437)
set_source_files_properties(${FAT_ROOT}/nls/nls_cp932.c PROPERTIES COMPILE_DEFINITIONS USBMEM_MODULE_INIT_NAME=usbmem_fat_init_cp932)
if(USBMEM_SANITIZE)
  set_source_files_properties(${FAT_SRC} ${FAT_NLS_SRC} PROPERTIES COMPILE_OPTIONS "-fno-sanitize=alignment;-fno-sanitize=shift-base;-fno-sanitize=nonnull-attribute")
endif()
target_link_libraries(usbmem_fat PUBLIC usbmem_compat_rt)

# FAT のテスト: mtools (mformat / mcopy) で作ったイメージ (tools/gen_fat_fixture.py。7z と mtools の読み出しで検証済み) を、fs/fat の読み取りで全項目検証する
if(USBMEM_BUILD_TESTS)
  add_executable(usbmem_fat_tests tests/test_fat.cpp)
  target_link_libraries(usbmem_fat_tests PRIVATE usbmem_fat usbmem_exfat usbmem_ext2 usbmem_core)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_FOUND AND EXISTS /dev/shm)
    set(FAT_FIX_DIR /dev/shm/usbmem_fat_$ENV{USER})
    add_test(NAME fat_gen_fixture COMMAND ${CMAKE_COMMAND} -E env "PATH=$ENV{PATH}:/sbin:/usr/sbin" ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_LIST_DIR}/../../tools/gen_fat_fixture.py ${FAT_FIX_DIR})
    set_tests_properties(fat_gen_fixture PROPERTIES FIXTURES_SETUP fat_fixture SKIP_RETURN_CODE 77 TIMEOUT 900)
    add_test(NAME fat_read COMMAND usbmem_fat_tests ${FAT_FIX_DIR})
    add_test(NAME fat_corrupt COMMAND usbmem_fat_tests --corrupt ${FAT_FIX_DIR})
    add_test(NAME fat_ioerr COMMAND usbmem_fat_tests --ioerr ${FAT_FIX_DIR})
    add_test(NAME fat_fuzz COMMAND usbmem_fat_tests --fuzz ${FAT_FIX_DIR} 600 1)
    add_test(NAME fat_coexist COMMAND usbmem_fat_tests --coexist ${FAT_FIX_DIR} /dev/shm/usbmem_exfat_$ENV{USER} /dev/shm/usbmem_ext2_$ENV{USER})
    set_tests_properties(fat_read fat_corrupt fat_ioerr fat_fuzz PROPERTIES FIXTURES_REQUIRED fat_fixture TIMEOUT 1800)
    add_test(NAME fat_probe COMMAND usbmem_fat_tests --probe ${FAT_FIX_DIR} /dev/shm/usbmem_exfat_$ENV{USER} /dev/shm/usbmem_ntfs_$ENV{USER} /dev/shm/usbmem_ext2_$ENV{USER})
    set_tests_properties(fat_probe PROPERTIES FIXTURES_REQUIRED "fat_fixture;exfat_fixture;ntfs_fixture;ext2_fixture" TIMEOUT 300)
    set_tests_properties(fat_coexist PROPERTIES FIXTURES_REQUIRED "fat_fixture;exfat_fixture;ext2_fixture" TIMEOUT 600)
    # 失敗したマウントの後始末は inode 内部の確保物を解放しきれない (NTFS / exFAT / ext2 と同じ既知事項)。メモリ安全性 (UAF/OOB/ハング) の検査に集中する。
    set_tests_properties(fat_corrupt fat_ioerr fat_fuzz fat_coexist PROPERTIES ENVIRONMENT "ASAN_OPTIONS=verify_asan_link_order=0:detect_leaks=0")
    set_tests_properties(fat_read PROPERTIES ENVIRONMENT "ASAN_OPTIONS=verify_asan_link_order=0:detect_leaks=1")
  endif()
endif()
# ---- 光学メディア系 (ISO 9660 / UDF。読み取り専用)
include(${CMAKE_CURRENT_LIST_DIR}/optical.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/zfs.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/xfs.cmake)
