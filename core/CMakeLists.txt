# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.22)
project(usbmem_core LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(USBMEM_SANITIZE "Build with ASan+UBSan" OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(_top ON)
else()
  set(_top OFF)
endif()
option(USBMEM_BUILD_TESTS "Build unit tests" ${_top})

add_library(usbmem_core STATIC
  src/devices.cpp src/usb_msc.cpp src/usbdevfs_transport.cpp src/crc32.cpp src/fs_probe.cpp src/partition.cpp src/c_api.cpp)
# パーティション編集 / ラベル / バックアップ (docs/partition-edit.md)
target_sources(usbmem_core PRIVATE
  src/sha256.cpp src/fs_label.cpp src/gpt_inspect.cpp src/partition_backup.cpp src/partition_edit.cpp src/partition_edit_json.cpp src/c_api_partedit.cpp)
target_include_directories(usbmem_core PUBLIC include)
target_compile_definitions(usbmem_core PUBLIC _FILE_OFFSET_BITS=64)
target_compile_options(usbmem_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wno-sign-conversion)
if(USBMEM_SANITIZE)
  target_compile_options(usbmem_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
  target_link_options(usbmem_core PUBLIC -fsanitize=address,undefined)
endif()
# Android 16KB ページ対応 (NDK r28+ は既定だが旧 NDK でも守る)
if(ANDROID)
  target_link_options(usbmem_core PUBLIC -Wl,-z,max-page-size=16384)
endif()

if(USBMEM_BUILD_TESTS)
  enable_testing()  # compat/ のテストも拾うため add_subdirectory より前に呼ぶ
endif()
option(USBMEM_NTFS "Build NTFS (LinuxCompat + fs/ntfs, GPL-2.0-or-later)" ON)
if(USBMEM_NTFS)
  find_package(Threads REQUIRED)
  add_subdirectory(compat)
endif()

find_package(Threads REQUIRED)
include(blockdev.cmake)  # 画像形式 / オーバーレイ / NBD (zlib 依存)

if(USBMEM_BUILD_TESTS)
  enable_testing()
  add_executable(usbmem_tests tests/test_main.cpp tests/test_devices.cpp tests/test_partition.cpp
                 tests/test_fixtures.cpp tests/test_usb_msc.cpp
                 tests/test_partedit.cpp tests/test_partedit_fault.cpp tests/test_fslabel.cpp tests/test_partedit_capi.cpp)
  target_link_libraries(usbmem_tests PRIVATE usbmem_core)
  add_test(NAME unit COMMAND usbmem_tests)
  add_executable(usbmem_partedit_cli tests/partedit_cli.cpp)  # 検証用 CLI (tools/check_partedit_tools.py が使う)
  target_link_libraries(usbmem_partedit_cli PRIVATE usbmem_core)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_FOUND)
    # フィクスチャ(ディスクイメージ)は tmpfs に生成する
    if(EXISTS /dev/shm)
      set(FIX_DIR /dev/shm/usbmem_fixtures_$ENV{USER})
    else()
      set(FIX_DIR ${CMAKE_CURRENT_BINARY_DIR}/fixtures)
    endif()
    find_program(USBMEM_SFDISK sfdisk PATHS /sbin /usr/sbin)
    if(USBMEM_SFDISK)
      add_test(NAME partedit_tools COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/../tools/check_partedit_tools.py $<TARGET_FILE:usbmem_partedit_cli>)
    endif()
    add_test(NAME gen_fixtures COMMAND ${Python3_EXECUTABLE} -I
             ${CMAKE_CURRENT_SOURCE_DIR}/../tools/gen_fixtures.py ${FIX_DIR})
    add_test(NAME fixtures COMMAND usbmem_tests --fixtures ${FIX_DIR})
    set_tests_properties(fixtures PROPERTIES DEPENDS gen_fixtures)
  endif()
endif()

# BlockTransform (BitLocker / LUKS)
include(${CMAKE_CURRENT_LIST_DIR}/transform.cmake)

# DiskTools (整合性検査 / mkfs)
include(${CMAKE_CURRENT_LIST_DIR}/disktools.cmake)
