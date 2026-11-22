# SPDX-License-Identifier: Apache-2.0
# パーティション探索 (TestDisk 相当) / 失われた表の復元案 / ブートセクタの検査と修復。docs/partition-scan.md
# core/CMakeLists.txt の末尾から include される。usbmem_core にだけ依存する (Android 非依存・PC でテスト可能)。
add_library(usbmem_partscan STATIC
  src/partition_scan.cpp src/partition_scan_detect.cpp src/partition_recover.cpp src/bootfix.cpp src/c_api_partscan.cpp)
target_include_directories(usbmem_partscan PUBLIC include)
target_link_libraries(usbmem_partscan PUBLIC usbmem_core)
target_compile_options(usbmem_partscan PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wno-sign-conversion)

if(USBMEM_BUILD_TESTS)
  # 単体テスト (手組みのイメージ・乱数・破損入力・ブートセクタの修復・C API)
  add_executable(usbmem_partscan_tests tests/test_main.cpp tests/test_partscan.cpp tests/test_bootfix.cpp)
  target_include_directories(usbmem_partscan_tests PRIVATE tests src)
  target_link_libraries(usbmem_partscan_tests PRIVATE usbmem_partscan)
  add_test(NAME partscan_unit COMMAND usbmem_partscan_tests)
  # 検証用 CLI (tools/check_partscan_tools.py が sfdisk / mkfs.* / fsck.* / testdisk と突き合わせる)
  add_executable(usbmem_partscan_cli tests/partscan_cli.cpp)
  target_link_libraries(usbmem_partscan_cli PRIVATE usbmem_partscan)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_FOUND)
    add_test(NAME partscan_tools COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_LIST_DIR}/../tools/check_partscan_tools.py $<TARGET_FILE:usbmem_partscan_cli>)
    set_tests_properties(partscan_tools PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 3600)
  endif()
endif()
