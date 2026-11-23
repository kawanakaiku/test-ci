# SPDX-License-Identifier: Apache-2.0
# 削除ファイルの一覧と救出 (undelete) / シグネチャによるファイルの切り出し (カービング)。docs/undelete-carving.md
# core/CMakeLists.txt の末尾から include される。usbmem_core と usbmem_recovery (割り当てビットマップ) に依存する。Android 非依存・PC でテスト可能。
add_library(usbmem_undelete STATIC
  src/filetype.cpp src/undelete.cpp src/undelete_fat.cpp src/undelete_exfat.cpp src/undelete_ext.cpp src/undelete_ntfs.cpp
  src/carve.cpp src/carve_formats.cpp src/c_api_undelete.cpp)
target_include_directories(usbmem_undelete PUBLIC include)
target_link_libraries(usbmem_undelete PUBLIC usbmem_recovery usbmem_core)
target_compile_options(usbmem_undelete PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wno-sign-conversion)

if(USBMEM_BUILD_TESTS)
  # 単体テスト (手組みのイメージ・形式ごとの検査・ファジング・C API)
  add_executable(usbmem_undelete_tests tests/test_main.cpp tests/test_undelete.cpp tests/test_carve.cpp)
  target_include_directories(usbmem_undelete_tests PRIVATE tests src)
  target_link_libraries(usbmem_undelete_tests PRIVATE usbmem_undelete)
  add_test(NAME undelete_unit COMMAND usbmem_undelete_tests)
  # 検証用 CLI (tools/check_undelete_tools.py / tools/check_carve_tools.py が本物の mkfs.* / 外部ツールと突き合わせる)
  add_executable(usbmem_undelete_cli tests/undelete_cli.cpp)
  target_link_libraries(usbmem_undelete_cli PRIVATE usbmem_undelete)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_FOUND)
    add_test(NAME undelete_tools COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_LIST_DIR}/../tools/check_undelete_tools.py $<TARGET_FILE:usbmem_undelete_cli>)
    add_test(NAME carve_tools COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_LIST_DIR}/../tools/check_carve_tools.py $<TARGET_FILE:usbmem_undelete_cli>)
    set_tests_properties(undelete_tools carve_tools PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 3600)
  endif()
endif()
