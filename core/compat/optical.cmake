# SPDX-License-Identifier: Apache-2.0 (本ファイル) — 取り込む fs/isofs・fs/udf は GPL-2.0
# 光学メディア系ファイルシステム (ISO 9660 / UDF)。CMakeLists.txt の末尾から include される (他の FS の追加と衝突しないよう別ファイルにしてある)。
# 読み取り専用。docs/isofs-port.md / docs/udf-port.md

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

# ---- UDF (fs/udf)。読み取り専用。書き込み側 (balloc/ialloc/truncate) も上流のまま一緒にビルドするが、ホスト入口は読み取り経路しか呼ばない。
# file.c (write_iter / mmap / ioctl / setattr) は読み取り専用では不要で、ホスト入口は i_fop を使わないので入れない (空の ops は rt_vfs_udf.c)
foreach(f balloc dir ialloc inode lowlevel namei partition super truncate symlink directory misc udftime unicode)
  list(APPEND UDF_SRC ${UDF_ROOT}/fs-udf/${f}.c)
endforeach()
if(USBMEM_SANITIZE)
  set_source_files_properties(${UDF_SRC} PROPERTIES COMPILE_OPTIONS "-fno-sanitize=alignment;-fno-sanitize=shift-base;-fno-sanitize=nonnull-attribute;-fno-sanitize=pointer-overflow")
  # pointer-overflow: 上流は NULL の s_vat_inode に UDF_I() (container_of) を先に適用し、使う前に範囲検査で戻る (partition.c の udf_get_pblock_virt15)。参照はしない
endif()
add_library(usbmem_udf STATIC ${UDF_SRC} src/rt_vfs_udf.c src/udf_volume.cpp)
target_include_directories(usbmem_udf PUBLIC public)
target_compile_options(usbmem_udf PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-std=gnu11>
  $<$<COMPILE_LANGUAGE:C>:-w>
  $<$<COMPILE_LANGUAGE:C>:-fno-strict-aliasing>
  $<$<COMPILE_LANGUAGE:C>:-fwrapv>
  $<$<COMPILE_LANGUAGE:C>:-Wno-incompatible-pointer-types>
  $<$<COMPILE_LANGUAGE:C>:-Wno-int-conversion>
  "$<$<COMPILE_LANGUAGE:C>:SHELL:-include compat/optical.h>"
  "$<$<COMPILE_LANGUAGE:C>:SHELL:-iquote ${UDF_ROOT}/fs-udf>")
target_compile_definitions(usbmem_udf PRIVATE CONFIG_HAVE_EFFICIENT_UNALIGNED_ACCESS=1 CONFIG_BLOCK=1 CONFIG_UDF_FS=1 "CONFIG_NLS_DEFAULT=\"utf8\""
  USBMEM_MODULE_INIT_NAME=usbmem_module_init_udf)
# 上流の udf_fiiter_release は iter->epos.bh (拡張割り当て記述子の続きブロック) を解放しない (参照漏れ)。directory.c の中の名前だけ変えて、他の翻訳単位からの呼び出しは
# rt_vfs_udf.c の同名の関数 (epos.bh を brelse してから本物を呼ぶ) に向ける。
set_source_files_properties(${UDF_ROOT}/fs-udf/directory.c PROPERTIES COMPILE_DEFINITIONS "udf_fiiter_release=usbmem_real_fiiter_release")
target_link_libraries(usbmem_udf PUBLIC usbmem_compat_rt)

# ---- UDF のテスト (tools/gen_udf_fixture.py: mkudffs + ホストの UDF ドライバで書いたイメージと genisoimage -udf のブリッジ)
if(USBMEM_BUILD_TESTS)
  add_executable(usbmem_udf_tests tests/test_udf.cpp)
  target_link_libraries(usbmem_udf_tests PRIVATE usbmem_udf usbmem_isofs usbmem_ext2)
  find_package(Python3 COMPONENTS Interpreter)
  find_program(USBMEM_MKUDFFS mkudffs PATHS /sbin /usr/sbin)
  find_program(USBMEM_UDISKSCTL udisksctl)
  if(Python3_FOUND AND EXISTS /dev/shm AND USBMEM_MKUDFFS AND USBMEM_UDISKSCTL)
    set(UDF_FIX_DIR /dev/shm/usbmem_udf_$ENV{USER})
    add_test(NAME udf_gen_fixture COMMAND ${CMAKE_COMMAND} -E env "PATH=$ENV{PATH}:/sbin:/usr/sbin" ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_LIST_DIR}/../../tools/gen_udf_fixture.py ${UDF_FIX_DIR})
    set_tests_properties(udf_gen_fixture PROPERTIES FIXTURES_SETUP udf_fixture TIMEOUT 1800)
    add_test(NAME udf_read COMMAND usbmem_udf_tests ${UDF_FIX_DIR})
    add_test(NAME udf_read_complex COMMAND ${CMAKE_COMMAND} -E env USBMEM_UDF_FORCE_COMPLEX_FOR_TEST=1 $<TARGET_FILE:usbmem_udf_tests> ${UDF_FIX_DIR})
    add_test(NAME udf_corrupt COMMAND usbmem_udf_tests --corrupt ${UDF_FIX_DIR})
    add_test(NAME udf_ioerr COMMAND usbmem_udf_tests --ioerr ${UDF_FIX_DIR})
    add_test(NAME udf_fuzz COMMAND usbmem_udf_tests --fuzz ${UDF_FIX_DIR} 400 1)
    add_test(NAME udf_coexist COMMAND usbmem_udf_tests --coexist ${UDF_FIX_DIR} /dev/shm/usbmem_ext2_$ENV{USER} /dev/shm/usbmem_iso_$ENV{USER})
    set_tests_properties(udf_read udf_read_complex udf_corrupt udf_ioerr udf_fuzz PROPERTIES FIXTURES_REQUIRED udf_fixture TIMEOUT 1800)
    set_tests_properties(udf_coexist PROPERTIES FIXTURES_REQUIRED "udf_fixture;isofs_fixture;ext2_fixture" TIMEOUT 600)
    set_tests_properties(udf_corrupt udf_ioerr udf_fuzz udf_coexist PROPERTIES ENVIRONMENT "ASAN_OPTIONS=verify_asan_link_order=0:detect_leaks=0")
    set_tests_properties(udf_read PROPERTIES ENVIRONMENT "ASAN_OPTIONS=verify_asan_link_order=0:detect_leaks=1")
  endif()
endif()
