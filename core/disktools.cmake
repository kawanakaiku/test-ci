# SPDX-License-Identifier: Apache-2.0
# DiskTools: 読み取り専用の整合性検査 (fsck 相当) と mkfs。docs/disk-tools.md
# core/CMakeLists.txt の末尾から include される。usbmem_core にだけ依存する (Android 非依存・PC でテスト可能)。
add_library(usbmem_disktools STATIC
  src/fscheck.cpp src/fscheck_fat.cpp src/fscheck_exfat.cpp src/fscheck_ext.cpp src/fscheck_ntfs.cpp
  src/mkfs.cpp src/mkfs_fat32.cpp src/mkfs_exfat.cpp src/mkfs_ext2.cpp
  src/c_api_disktools.cpp)
target_include_directories(usbmem_disktools PUBLIC include)
target_link_libraries(usbmem_disktools PUBLIC usbmem_core)
target_compile_options(usbmem_disktools PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wno-sign-conversion)

if(USBMEM_BUILD_TESTS)
  # 検証用 CLI (tools/check_disktools.py が使う)。イメージを読み取り専用で検査する。
  add_executable(usbmem_disktools_cli tests/disktools_cli.cpp)
  target_link_libraries(usbmem_disktools_cli PRIVATE usbmem_disktools)
  # 単体テスト (安全拒否・バックアップ・キャンセル・書き込み失敗・破損入力で落ちないこと)
  add_executable(usbmem_disktools_tests tests/test_main.cpp tests/test_disktools.cpp)
  target_link_libraries(usbmem_disktools_tests PRIVATE usbmem_disktools)
  add_test(NAME disktools_unit COMMAND usbmem_disktools_tests)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_FOUND)
    # 外部ツール (fsck.fat / fsck.exfat / e2fsck / ntfsfix / mkfs.*) との突き合わせ。ツールが無ければ SKIP (終了コード 77)。
    # 全件は tools/check_disktools.py <cli> all (数分かかる)。ここでは --quick。
    add_test(NAME disktools_tools COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/../tools/check_disktools.py
             $<TARGET_FILE:usbmem_disktools_cli> all --quick)
    set_tests_properties(disktools_tools PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1800)
  endif()
endif()
