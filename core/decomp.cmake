# SPDX-License-Identifier: Apache-2.0
# 圧縮データの展開器 (zlib = システムの libz / LZ4 ブロック / LZO1X / Zstandard の自前の展開)。core/CMakeLists.txt から include される。docs/decomp.md
# Btrfs の圧縮 extent と F2FS の圧縮クラスタ (core/compat) と、バックアップの LZ4 (src/backup_lz4.cpp) が使う。blockdev.cmake の後 (zlib を見つけた後) に読み込む。
target_sources(usbmem_core PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src/decomp.cpp ${CMAKE_CURRENT_LIST_DIR}/src/decomp_zstd.cpp)

if(USBMEM_BUILD_TESTS)
  add_executable(usbmem_decomp_tests ${CMAKE_CURRENT_LIST_DIR}/tests/test_decomp.cpp)
  target_link_libraries(usbmem_decomp_tests PRIVATE usbmem_core)
  add_test(NAME decomp_unit COMMAND usbmem_decomp_tests --unit)
  set_tests_properties(decomp_unit PROPERTIES ENVIRONMENT "ASAN_OPTIONS=verify_asan_link_order=0:detect_leaks=1")
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_FOUND AND EXISTS /dev/shm)
    # 本物の圧縮器 (zstd / lzop コマンド・Python の zlib) の出力を展開して元データと比べる。壊れた入力のファジングは ASan / UBSan のビルド (USBMEM_SANITIZE) で意味を持つ
    set(DECOMP_FIX_DIR /dev/shm/usbmem_decomp_$ENV{USER})
    add_test(NAME decomp_gen_fixture COMMAND ${CMAKE_COMMAND} -E env "PATH=$ENV{PATH}:/sbin:/usr/sbin" ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_LIST_DIR}/../tools/gen_decomp_fixtures.py ${DECOMP_FIX_DIR} --quick)
    set_tests_properties(decomp_gen_fixture PROPERTIES FIXTURES_SETUP decomp_fixture TIMEOUT 1200 RESOURCE_LOCK decomp_fix_dir)
    add_test(NAME decomp_real COMMAND usbmem_decomp_tests ${DECOMP_FIX_DIR})
    add_test(NAME decomp_fuzz COMMAND usbmem_decomp_tests --fuzz ${DECOMP_FIX_DIR} 4000 1)
    set_tests_properties(decomp_real decomp_fuzz PROPERTIES FIXTURES_REQUIRED decomp_fixture TIMEOUT 1800 ENVIRONMENT "ASAN_OPTIONS=verify_asan_link_order=0:detect_leaks=1")
  endif()
endif()
