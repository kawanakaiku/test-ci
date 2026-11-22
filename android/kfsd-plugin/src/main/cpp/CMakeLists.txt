# SPDX-License-Identifier: GPL-2.0-or-later
# kfsd のプラグイン APK のネイティブ部 (docs/kfsd.md 6 章 (b)): libkfsd.so (カーネル由来のドライバ + LinuxCompat をリンクした PIE の実行ファイル) と、起こす側の JNI (libkfsdspawn.so)。
cmake_minimum_required(VERSION 3.22)
project(usbmem_kfsd_plugin LANGUAGES C CXX)
set(USBMEM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(USBMEM_BUILD_KFSD ON CACHE BOOL "" FORCE)
set(USBMEM_EDITION "personal" CACHE STRING "")
# AGP の Debug ビルドは無最適化 (-O0) になり FS が 5〜10 倍遅いので、Debug でも -O2 (app/src/main/cpp/CMakeLists.txt と同じ)
if(NOT CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo|MinSizeRel)$")
  add_compile_options(-O2)
endif()
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../../../../core core)
set_target_properties(usbmem_kfsd_bin PROPERTIES OUTPUT_NAME "libkfsd.so" PREFIX "" SUFFIX "" POSITION_INDEPENDENT_CODE ON)
target_compile_options(usbmem_kfsd_bin PRIVATE -fPIE)
target_link_options(usbmem_kfsd_bin PRIVATE -pie -Wl,-z,max-page-size=16384)
target_link_libraries(usbmem_kfsd_bin PRIVATE log)   # LinuxCompat が __android_log_print を使う
add_library(kfsdspawn SHARED ${CMAKE_CURRENT_LIST_DIR}/../../../../kfsd-common/src/main/cpp/kfsd_spawn.cpp)
target_link_libraries(kfsdspawn PRIVATE log)
target_link_options(kfsdspawn PRIVATE -Wl,-z,max-page-size=16384)
