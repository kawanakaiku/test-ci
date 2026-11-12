# SPDX-License-Identifier: Apache-2.0
# F2FS の読み取り専用リーダー (オンディスク形式からの独自実装。docs/f2fs-port.md)。圧縮クラスタの展開だけ usbmem_kdecomp (GPL 系) を使う。
# core/compat/CMakeLists.txt の末尾から include される。
add_library(usbmem_f2fs STATIC ${CMAKE_CURRENT_LIST_DIR}/src/f2fs_volume.cpp ${CMAKE_CURRENT_LIST_DIR}/src/f2fs_decomp.c)
target_include_directories(usbmem_f2fs PUBLIC public)
target_compile_options(usbmem_f2fs PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-std=gnu11> $<$<COMPILE_LANGUAGE:C>:-w>
  "$<$<COMPILE_LANGUAGE:C>:SHELL:-include stdint.h>" "$<$<COMPILE_LANGUAGE:C>:SHELL:-include stddef.h>" "$<$<COMPILE_LANGUAGE:C>:SHELL:-include endian.h>")
target_link_libraries(usbmem_f2fs PUBLIC usbmem_kdecomp usbmem_core)

# ---- F2FS のテスト: mkfs.f2fs + sload.f2fs (機能 × 圧縮 × 大きさ) と、本物のカーネルが書いたイメージ (tools/f2fs_vm) を、リーダーで全項目検証する。fsck.f2fs で検査済み。
if(USBMEM_BUILD_TESTS)
  add_executable(usbmem_f2fs_tests tests/test_f2fs.cpp)
  target_link_libraries(usbmem_f2fs_tests PRIVATE usbmem_f2fs usbmem_core)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_FOUND AND EXISTS /dev/shm)
    set(F2FS_FIX_DIR /dev/shm/usbmem_f2fs_$ENV{USER})
    add_test(NAME f2fs_gen_fixture COMMAND ${CMAKE_COMMAND} -E env "PATH=$ENV{PATH}:/sbin:/usr/sbin" ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_LIST_DIR}/../../tools/gen_f2fs_fixture.py ${F2FS_FIX_DIR})
    set_tests_properties(f2fs_gen_fixture PROPERTIES FIXTURES_SETUP f2fs_fixture TIMEOUT 1200 RESOURCE_LOCK f2fs_fix_dir)
    # 本物のカーネルが書くイメージ (k_*。QEMU/TCG の小さな VM。カーネル・qemu・gcc などが無ければ SKIP=終了コード 77。約 2 分)
    add_test(NAME f2fs_gen_kfixture COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_LIST_DIR}/../../tools/f2fs_vm/gen_k_fixtures.py ${F2FS_FIX_DIR})
    set_tests_properties(f2fs_gen_kfixture PROPERTIES FIXTURES_SETUP f2fs_fixture TIMEOUT 1800 SKIP_RETURN_CODE 77 RESOURCE_LOCK f2fs_fix_dir)
    add_test(NAME f2fs_read COMMAND usbmem_f2fs_tests ${F2FS_FIX_DIR})
    add_test(NAME f2fs_corrupt COMMAND usbmem_f2fs_tests --corrupt ${F2FS_FIX_DIR})
    add_test(NAME f2fs_ioerr COMMAND usbmem_f2fs_tests --ioerr ${F2FS_FIX_DIR})
    add_test(NAME f2fs_fuzz COMMAND usbmem_f2fs_tests --fuzz ${F2FS_FIX_DIR} 600 1)
    add_test(NAME f2fs_noload COMMAND usbmem_f2fs_tests --noload ${F2FS_FIX_DIR})
    set_tests_properties(f2fs_read f2fs_corrupt f2fs_ioerr f2fs_fuzz f2fs_noload PROPERTIES FIXTURES_REQUIRED f2fs_fixture TIMEOUT 1800)
    set_tests_properties(f2fs_corrupt f2fs_ioerr f2fs_fuzz f2fs_noload PROPERTIES ENVIRONMENT "ASAN_OPTIONS=verify_asan_link_order=0:detect_leaks=0")
    set_tests_properties(f2fs_read PROPERTIES ENVIRONMENT "ASAN_OPTIONS=verify_asan_link_order=0:detect_leaks=1")
  endif()
endif()
