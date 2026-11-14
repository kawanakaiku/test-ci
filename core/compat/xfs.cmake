# SPDX-License-Identifier: Apache-2.0 (本ファイル) — xfs_volume.cpp は GPL-2.0-or-later (ディスク形式を Linux fs/xfs の形式定義を仕様として独立に実装。カーネルのソースは取り込まない)
# XFS の読み取り専用リーダ (docs/xfs-port.md)。core/compat/CMakeLists.txt の末尾から include される。
add_library(usbmem_xfs STATIC ${CMAKE_CURRENT_LIST_DIR}/src/xfs_volume.cpp)
set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/src/xfs_volume.cpp PROPERTIES OBJECT_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/src/xfs_write.inc;${CMAKE_CURRENT_LIST_DIR}/src/xfs_write_alloc.inc;${CMAKE_CURRENT_LIST_DIR}/src/xfs_write_ops.inc;${CMAKE_CURRENT_LIST_DIR}/src/xfs_write_full.inc;${CMAKE_CURRENT_LIST_DIR}/src/xfs_write_node.inc")
if(USBMEM_NTFS_WRITE)
  # 書き込み経路 (src/xfs_write*.inc) を有効にする。無効なら書き込み入口は全て EROFS。docs/xfs-write.md
  target_compile_definitions(usbmem_xfs PRIVATE USBMEM_NTFS_WRITE=1)
endif()
target_include_directories(usbmem_xfs PUBLIC ${CMAKE_CURRENT_LIST_DIR}/public)
target_link_libraries(usbmem_xfs PUBLIC usbmem_compat_rt usbmem_core)

# ---- XFS のテスト: mkfs.xfs -p <dir> (ルート権限不要) で作ったイメージ (v5/v4 × ブロックサイズ × ディレクトリブロック × inode サイズ × 機能) を全項目検証する。
# tools/gen_xfs_fixture.py は作成後に xfs_repair -n で検査する。ログの未再生・未知の機能ビット・破壊・I/O エラー・fuzz も。
if(USBMEM_BUILD_TESTS)
  add_executable(usbmem_xfs_tests ${CMAKE_CURRENT_LIST_DIR}/tests/test_xfs.cpp)
  target_link_libraries(usbmem_xfs_tests PRIVATE usbmem_xfs usbmem_core)
  add_executable(usbmem_xfs_dump ${CMAKE_CURRENT_LIST_DIR}/tests/xfs_dump.cpp)
  target_link_libraries(usbmem_xfs_dump PRIVATE usbmem_xfs usbmem_core)
  if(USBMEM_NTFS_WRITE)
    # 書き込みのテスト本体。独立実装 (xfs_repair -n / xfs_db / xfs_logprint / カーネルの再生) との突き合わせは tools/run_xfs_write_tests.sh。docs/xfs-write.md
    add_executable(usbmem_xfs_write_tests ${CMAKE_CURRENT_LIST_DIR}/tests/test_xfs_write.cpp)
    target_include_directories(usbmem_xfs_write_tests PRIVATE ${CMAKE_CURRENT_LIST_DIR}/tests)
    target_link_libraries(usbmem_xfs_write_tests PRIVATE usbmem_xfs usbmem_core)
  endif()
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_FOUND AND EXISTS /dev/shm)
    set(XFS_FIX_DIR /dev/shm/usbmem_xfs_$ENV{USER})
    add_test(NAME xfs_gen_fixture COMMAND ${CMAKE_COMMAND} -E env "PATH=$ENV{PATH}:/sbin:/usr/sbin" ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_LIST_DIR}/../../tools/gen_xfs_fixture.py ${XFS_FIX_DIR})
    set_tests_properties(xfs_gen_fixture PROPERTIES FIXTURES_SETUP xfs_fixture TIMEOUT 3600)
    add_test(NAME xfs_read COMMAND usbmem_xfs_tests ${XFS_FIX_DIR})
    add_test(NAME xfs_features COMMAND usbmem_xfs_tests --features ${XFS_FIX_DIR})
    add_test(NAME xfs_corrupt COMMAND usbmem_xfs_tests --corrupt ${XFS_FIX_DIR})
    add_test(NAME xfs_ioerr COMMAND usbmem_xfs_tests --ioerr ${XFS_FIX_DIR})
    add_test(NAME xfs_fuzz COMMAND usbmem_xfs_tests --fuzz ${XFS_FIX_DIR} 150 1)
    # シード 2 は extent の論理オフセットのオーバーフロー (穴の長さが 0 になりハング) を見つけた回帰テスト
    add_test(NAME xfs_fuzz_seed2 COMMAND usbmem_xfs_tests --fuzz ${XFS_FIX_DIR} 400 2)
    set_tests_properties(xfs_read xfs_features xfs_corrupt xfs_ioerr xfs_fuzz xfs_fuzz_seed2 PROPERTIES FIXTURES_REQUIRED xfs_fixture TIMEOUT 3600)
    set_tests_properties(xfs_features xfs_corrupt xfs_ioerr xfs_fuzz xfs_fuzz_seed2 PROPERTIES ENVIRONMENT "ASAN_OPTIONS=verify_asan_link_order=0:detect_leaks=0")
    set_tests_properties(xfs_read PROPERTIES ENVIRONMENT "ASAN_OPTIONS=verify_asan_link_order=0:detect_leaks=1")
    if(USBMEM_NTFS_WRITE)
      # 書き込み: 決まった操作列・乱数操作 (モデル)・拒否・満杯・ファジングを xfs_repair -n / xfs_logprint / 実カーネルと突き合わせ (xfs_write)、電源断の模擬 (xfs_crash)。docs/xfs-write.md
      set(XFS_TOOLS ${CMAKE_CURRENT_LIST_DIR}/../../tools)
      add_test(NAME xfs_write COMMAND sh ${XFS_TOOLS}/run_xfs_write_tests.sh $<TARGET_FILE:usbmem_xfs_write_tests> 1)
      add_test(NAME xfs_crash COMMAND sh ${XFS_TOOLS}/run_xfs_crash_tests.sh $<TARGET_FILE:usbmem_xfs_write_tests> 40)   # 軽量版。フル版は docs/xfs-write.md
      # 作業場所は /tmp (ディスク)。/dev/shm は他の ctest (失敗した fixture の残骸など) と共有で、CI では quota 超過 (ENOSPC) になった
      set_tests_properties(xfs_write xfs_crash PROPERTIES TIMEOUT 3600 SKIP_RETURN_CODE 77 ENVIRONMENT "ASAN_OPTIONS=verify_asan_link_order=0:detect_leaks=1;USBMEM_WORK_DIR=/tmp")
    endif()
  endif()
endif()
