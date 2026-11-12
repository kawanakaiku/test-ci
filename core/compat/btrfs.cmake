# SPDX-License-Identifier: Apache-2.0 (本ファイル) — btrfs_volume.cpp は自前のリーダー、btrfs_decomp.c と usbmem_kdecomp は GPL-2.0 系
# Btrfs の読み取り専用リーダー。core/compat/CMakeLists.txt の末尾から include される。docs/btrfs-port.md
# 他のファイルシステムのターゲットと衝突しないよう、このファイルに集める。
add_library(usbmem_btrfs STATIC ${CMAKE_CURRENT_LIST_DIR}/src/btrfs_volume.cpp ${CMAKE_CURRENT_LIST_DIR}/src/btrfs_decomp.c)
target_include_directories(usbmem_btrfs PUBLIC public)
target_compile_options(usbmem_btrfs PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-std=gnu11> $<$<COMPILE_LANGUAGE:C>:-w> $<$<COMPILE_LANGUAGE:C>:-fno-strict-aliasing> $<$<COMPILE_LANGUAGE:C>:-fwrapv>
  "$<$<COMPILE_LANGUAGE:C>:SHELL:-include stdint.h>" "$<$<COMPILE_LANGUAGE:C>:SHELL:-include stddef.h>" "$<$<COMPILE_LANGUAGE:C>:SHELL:-include endian.h>")
target_compile_definitions(usbmem_btrfs PRIVATE __KERNEL__)
target_link_libraries(usbmem_btrfs PUBLIC usbmem_kdecomp usbmem_compat_rt usbmem_transform usbmem_core)

if(USBMEM_BUILD_TESTS)
  add_executable(usbmem_btrfs_tests tests/test_btrfs.cpp)
  target_link_libraries(usbmem_btrfs_tests PRIVATE usbmem_btrfs)
  # mkfs.btrfs (チェックサム × 圧縮 × ノードサイズ × セクタサイズ × 機能 × サブボリューム) + tools/gen_btrfs_fixture.py。生成時に btrfs check --readonly と btrfs restore との一致を確認する。
  # btrfs_restore_xcheck は、リーダー (usbmem_btrfs_tests tree) の出力を btrfs restore の展開結果と全イメージで突き合わせる。docs/btrfs-port.md
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_FOUND AND EXISTS /dev/shm)
    set(BTRFS_FIX_DIR /dev/shm/usbmem_btrfs_$ENV{USER})
    set(BTRFS_TOOLS ${CMAKE_CURRENT_LIST_DIR}/../../tools)
    add_test(NAME btrfs_gen_fixture COMMAND ${CMAKE_COMMAND} -E env "PATH=$ENV{PATH}:/sbin:/usr/sbin" PYTHONDONTWRITEBYTECODE=1 ${Python3_EXECUTABLE} -I ${BTRFS_TOOLS}/gen_btrfs_fixture.py ${BTRFS_FIX_DIR})
    set_tests_properties(btrfs_gen_fixture PROPERTIES FIXTURES_SETUP btrfs_fixture TIMEOUT 1200)
    add_test(NAME btrfs_read COMMAND usbmem_btrfs_tests ${BTRFS_FIX_DIR})
    add_test(NAME btrfs_restore_xcheck COMMAND ${CMAKE_COMMAND} -E env "PATH=$ENV{PATH}:/sbin:/usr/sbin" PYTHONDONTWRITEBYTECODE=1 ${Python3_EXECUTABLE} -I ${BTRFS_TOOLS}/check_btrfs_restore.py ${BTRFS_FIX_DIR} $<TARGET_FILE:usbmem_btrfs_tests>)
    add_test(NAME btrfs_corrupt COMMAND usbmem_btrfs_tests --corrupt ${BTRFS_FIX_DIR})
    add_test(NAME btrfs_ioerr COMMAND usbmem_btrfs_tests --ioerr ${BTRFS_FIX_DIR})
    add_test(NAME btrfs_fuzz COMMAND usbmem_btrfs_tests --fuzz ${BTRFS_FIX_DIR} 150 1)
    set_tests_properties(btrfs_read btrfs_restore_xcheck btrfs_corrupt btrfs_ioerr btrfs_fuzz PROPERTIES FIXTURES_REQUIRED btrfs_fixture TIMEOUT 1800)
    set_tests_properties(btrfs_corrupt btrfs_ioerr btrfs_fuzz PROPERTIES ENVIRONMENT "ASAN_OPTIONS=verify_asan_link_order=0:detect_leaks=0")
    set_tests_properties(btrfs_read PROPERTIES ENVIRONMENT "ASAN_OPTIONS=verify_asan_link_order=0:detect_leaks=1")
  endif()
endif()
