# SPDX-License-Identifier: Apache-2.0
# データ救出 (ddrescue 方式) とバックアップ / 復元 (Partclone 方式)。docs/recovery-backup.md
# core/CMakeLists.txt の末尾から include される。usbmem_core にだけ依存する (Android 非依存・PC でテスト可能)。
add_library(usbmem_recovery STATIC
  src/recovery.cpp src/backup_fs.cpp src/backup_image.cpp src/backup_restore.cpp src/backup_lz4.cpp src/c_api_recovery.cpp)
target_include_directories(usbmem_recovery PUBLIC include)
target_link_libraries(usbmem_recovery PUBLIC usbmem_core)
target_compile_options(usbmem_recovery PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wno-sign-conversion)

if(USBMEM_BUILD_TESTS)
  add_executable(usbmem_recovery_tests tests/test_main.cpp tests/test_recovery.cpp tests/test_backup.cpp)
  target_include_directories(usbmem_recovery_tests PRIVATE tests src)
  target_link_libraries(usbmem_recovery_tests PRIVATE usbmem_recovery)
  add_test(NAME recovery_unit COMMAND usbmem_recovery_tests)
  # 検証用 CLI (tools/check_recovery_tools.py が ddrescue / partclone と突き合わせる)
  add_executable(usbmem_recovery_cli tests/recovery_cli.cpp)
  target_include_directories(usbmem_recovery_cli PRIVATE tests src)
  target_link_libraries(usbmem_recovery_cli PRIVATE usbmem_recovery)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_FOUND)
    add_test(NAME recovery_tools COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_LIST_DIR}/../tools/check_recovery_tools.py $<TARGET_FILE:usbmem_recovery_cli>)
    set_tests_properties(recovery_tools PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 3600)
    if(USBMEM_SANITIZE)
      set_tests_properties(recovery_tools PROPERTIES ENVIRONMENT "USBMEM_RB_QUICK=1")
    endif()
  endif()
endif()
