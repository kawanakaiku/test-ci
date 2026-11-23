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
# エディション (docs/editions.md): personal = すべて入り (既定。配布しない自分用)。public = GPL 系 (カーネル由来の LinuxCompat・FS ドライバ・fuseshim) を一切含まない公開版。
# 公開版は書き出したツリー (tools/export_public.sh) でビルドする。compat/ には公開版用の小さな CMakeLists.txt (FsVolume 共通 IF と自前の F2FS だけ) が置かれる。
set(USBMEM_EDITION "personal" CACHE STRING "personal | public")
set_property(CACHE USBMEM_EDITION PROPERTY STRINGS personal public)
if(NOT USBMEM_EDITION MATCHES "^(personal|public)$")
  message(FATAL_ERROR "USBMEM_EDITION must be personal or public (got '${USBMEM_EDITION}')")
endif()
if(USBMEM_NTFS OR USBMEM_EDITION STREQUAL "public")
  find_package(Threads REQUIRED)
  add_subdirectory(compat)
endif()

find_package(Threads REQUIRED)
include(blockdev.cmake)  # 画像形式 / オーバーレイ / NBD (zlib 依存)
include(decomp.cmake)  # 圧縮データの展開 (zlib / LZ4 / LZO1X / Zstandard。Btrfs・F2FS の圧縮が使う。docs/decomp.md)

if(USBMEM_BUILD_TESTS)
  enable_testing()
  add_executable(usbmem_tests tests/test_main.cpp tests/test_devices.cpp tests/test_partition.cpp
                 tests/test_fixtures.cpp tests/test_usb_msc.cpp tests/test_usb_lun.cpp
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
# データ救出 / バックアップ (ddrescue / Partclone 方式)
include(${CMAKE_CURRENT_LIST_DIR}/recovery.cmake)
# パーティション探索 / ブートセクタの検査と修復 (TestDisk 相当)
include(${CMAKE_CURRENT_LIST_DIR}/partscan.cmake)
# 削除ファイルの一覧と救出 / カービング (PhotoRec 相当)
include(${CMAKE_CURRENT_LIST_DIR}/undelete.cmake)

if(NOT USBMEM_EDITION STREQUAL "public")
  # FUSE アダプター (xfstests 用。docs/xfstests.md)
  include(${CMAKE_CURRENT_LIST_DIR}/fuse.cmake)
  # fuseshim の試作 (ntfs-3g を libfuse 互換の層に載せる。既定 OFF。docs/fuse-layer.md)
  include(${CMAKE_CURRENT_LIST_DIR}/fuseshim.cmake)
endif()

# kfsd (docs/kfsd.md): カーネル由来の FS ドライバを別プロセス (実行ファイル) に分け、RPC だけで通信する。
#   usbmem_kfsd (Apache-2.0。プロトコル + クライアント RpcVolume + サーバの共通部分) は公開版にも入る。kfsd の実行ファイルとそのテストは個人版だけ (core/kfsd/CMakeLists.txt)。
# 既定: ホストの単独ビルドだけ ON。Android のビルドは明示したときだけ (docs/kfsd.md 段階 3)。
option(USBMEM_BUILD_KFSD "Build the kfsd protocol library (and, in the personal edition, the kfsd executable + tests)" ${_top})
if(USBMEM_BUILD_KFSD AND EXISTS ${CMAKE_CURRENT_LIST_DIR}/kfsd/CMakeLists.txt)
  add_subdirectory(kfsd)
endif()
