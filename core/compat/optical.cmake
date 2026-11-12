# SPDX-License-Identifier: Apache-2.0 (本ファイル) — 取り込む fs/isofs・fs/udf は GPL-2.0
# 光学メディア系ファイルシステム (ISO 9660 / UDF)。CMakeLists.txt の末尾から include される (他の FS の追加と衝突しないよう別ファイルにしてある)。
# 読み取り専用。docs/optical-port.md

set(ISOFS_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../third_party/isofs)
set(UDF_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../third_party/udf)

# ---- ISO 9660 (Joliet / Rock Ridge)。zisofs (compress.c) は入れない (圧縮ファイルは read で明示的に拒否)。
foreach(f namei inode dir util rock export joliet)
  list(APPEND ISOFS_SRC ${ISOFS_ROOT}/fs-isofs/${f}.c)
endforeach()
if(USBMEM_SANITIZE)
  set_source_files_properties(${ISOFS_SRC} PROPERTIES COMPILE_OPTIONS "-fno-sanitize=alignment;-fno-sanitize=shift-base;-fno-sanitize=nonnull-attribute")
endif()
add_library(usbmem_isofs STATIC ${ISOFS_SRC} src/rt_vfs_isofs.c src/isofs_volume.cpp)
target_include_directories(usbmem_isofs PUBLIC public)
target_include_directories(usbmem_isofs PRIVATE ${ISOFS_ROOT}/include/uapi)
target_compile_options(usbmem_isofs PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-std=gnu11>
  $<$<COMPILE_LANGUAGE:C>:-w>
  $<$<COMPILE_LANGUAGE:C>:-fno-strict-aliasing>
  $<$<COMPILE_LANGUAGE:C>:-fwrapv>
  "$<$<COMPILE_LANGUAGE:C>:SHELL:-include compat/optical.h>"
  "$<$<COMPILE_LANGUAGE:C>:SHELL:-iquote ${ISOFS_ROOT}/fs-isofs>")
target_compile_definitions(usbmem_isofs PRIVATE CONFIG_HAVE_EFFICIENT_UNALIGNED_ACCESS=1 CONFIG_BLOCK=1 CONFIG_JOLIET=1 "CONFIG_NLS_DEFAULT=\"utf8\""
  USBMEM_MODULE_INIT_NAME=usbmem_module_init_isofs)
# Joliet の UTF-16BE → UTF-8 は互換層の utf16s_to_utf8s (エンディアン無視) ではなく、rt_vfs_isofs.c の専用実装を使う
set_source_files_properties(${ISOFS_ROOT}/fs-isofs/joliet.c PROPERTIES COMPILE_DEFINITIONS "utf16s_to_utf8s=usbmem_utf16s_to_utf8s_be")
target_link_libraries(usbmem_isofs PUBLIC usbmem_compat_rt)

# ---- ISO 9660 のテスト: genisoimage / xorriso で作った 7 種のイメージを isoinfo / 7z で照合 (tools/gen_iso_fixture.py)、fs/isofs の読み取りを全項目検証
if(USBMEM_BUILD_TESTS)
  add_executable(usbmem_isofs_tests tests/test_isofs.cpp)
  target_link_libraries(usbmem_isofs_tests PRIVATE usbmem_isofs usbmem_ext2)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_FOUND AND EXISTS /dev/shm)
    set(ISO_FIX_DIR /dev/shm/usbmem_iso_$ENV{USER})
    add_test(NAME isofs_gen_fixture COMMAND ${CMAKE_COMMAND} -E env "PATH=$ENV{PATH}:/sbin:/usr/sbin" ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_LIST_DIR}/../../tools/gen_iso_fixture.py ${ISO_FIX_DIR})
    set_tests_properties(isofs_gen_fixture PROPERTIES FIXTURES_SETUP isofs_fixture TIMEOUT 900)
    add_test(NAME isofs_read COMMAND usbmem_isofs_tests ${ISO_FIX_DIR})
    add_test(NAME isofs_corrupt COMMAND usbmem_isofs_tests --corrupt ${ISO_FIX_DIR})
    add_test(NAME isofs_ioerr COMMAND usbmem_isofs_tests --ioerr ${ISO_FIX_DIR})
    add_test(NAME isofs_fuzz COMMAND usbmem_isofs_tests --fuzz ${ISO_FIX_DIR} 600 1)
    add_test(NAME isofs_coexist COMMAND usbmem_isofs_tests --coexist ${ISO_FIX_DIR} /dev/shm/usbmem_ext2_$ENV{USER})
    set_tests_properties(isofs_read isofs_corrupt isofs_ioerr isofs_fuzz PROPERTIES FIXTURES_REQUIRED isofs_fixture TIMEOUT 1800)
    set_tests_properties(isofs_coexist PROPERTIES FIXTURES_REQUIRED "isofs_fixture;ext2_fixture" TIMEOUT 600)
    # 失敗したマウントの後始末は inode 内部の確保物を解放しきれない (NTFS / exFAT / ext2 と同じ既知事項)。メモリ安全性 (UAF/OOB/ハング) の検査に集中する。
    set_tests_properties(isofs_corrupt isofs_ioerr isofs_fuzz isofs_coexist PROPERTIES ENVIRONMENT "ASAN_OPTIONS=verify_asan_link_order=0:detect_leaks=0")
    set_tests_properties(isofs_read PROPERTIES ENVIRONMENT "ASAN_OPTIONS=verify_asan_link_order=0:detect_leaks=1")
  endif()
endif()
