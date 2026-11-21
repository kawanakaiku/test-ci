# SPDX-License-Identifier: Apache-2.0 (本ファイル)
# libwimfuse.so: wimlib の wimmount を載せた別プロセスの実行ファイル (Android / bionic、PIE)。docs/fuse-layer.md 10 章。
# AGP の externalNativeBuild には載せない (tools/wimfuse/build_android.sh が NDK のツールチェーンで単独に作り、jniLibs/<abi>/ に置く。
# AGP は jniLibs の *.so をそのまま APK の lib/<abi>/ へ入れ、extractNativeLibs=true なら nativeLibraryDir へ展開される)。
cmake_minimum_required(VERSION 3.22)
project(wimfuse C CXX)

set(WIMLIB_SRC "" CACHE PATH "展開済みの wimlib のソース (include/wimlib.h)")
set(WIMLIB_LIB "" CACHE FILEPATH "Android 向けにビルドした libwim.a (tools/wimfuse/build_android.sh)")
set(FUSE3_INCLUDE "" CACHE PATH "libfuse3 のヘッダ (fuse.h。コンパイル時だけ使う。libfuse3 はリンクしない: shim が代わり)")
set(WIMFUSE_OUT "${CMAKE_BINARY_DIR}" CACHE PATH "libwimfuse.so の出力先")
if(NOT EXISTS "${WIMLIB_SRC}/include/wimlib.h" OR NOT EXISTS "${WIMLIB_LIB}" OR NOT EXISTS "${FUSE3_INCLUDE}/fuse.h")
  message(FATAL_ERROR "WIMLIB_SRC / WIMLIB_LIB / FUSE3_INCLUDE が正しくない")
endif()

get_filename_component(FS_DIR "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)      # core/fuseshim
get_filename_component(CORE_DIR "${FS_DIR}/.." ABSOLUTE)                         # core
set(WF_DIR ${FS_DIR}/wimfuse)

add_library(wimfuse_fuse3 STATIC ${FS_DIR}/fuseshim_fuse3.c)
target_include_directories(wimfuse_fuse3 PRIVATE ${FUSE3_INCLUDE} ${FS_DIR})
target_compile_definitions(wimfuse_fuse3 PRIVATE FUSE_USE_VERSION=30)
set_target_properties(wimfuse_fuse3 PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_executable(wimfuse
  ${WF_DIR}/wimfuse_main.cpp ${WF_DIR}/wimfuse_io.c
  ${FS_DIR}/rpc_volume.cpp ${FS_DIR}/fuseshim_volume.cpp ${FS_DIR}/wim_session.cpp)
target_include_directories(wimfuse PRIVATE ${WF_DIR} ${WF_DIR}/stubs ${FS_DIR} ${CORE_DIR}/compat/public ${CORE_DIR}/include ${WIMLIB_SRC}/include)
target_compile_options(wimfuse PRIVATE -O2 -fPIE -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0)
target_compile_features(wimfuse PRIVATE cxx_std_17)
target_link_libraries(wimfuse PRIVATE wimfuse_fuse3 ${WIMLIB_LIB} m)
# wimlib の WIM ファイルの入出力 (open / pread / read / lseek / fstat / realpath / close) を RPC に差し替える (wimfuse_io.c)。
# 16 KiB ページ整列 (Android 15 以降の 16 KiB ページの端末)。-pie: アプリの nativeLibraryDir から exec できる位置独立実行ファイル
target_link_options(wimfuse PRIVATE -pie -Wl,-z,max-page-size=16384
  -Wl,--wrap=open -Wl,--wrap=close -Wl,--wrap=pread -Wl,--wrap=read -Wl,--wrap=lseek -Wl,--wrap=fstat -Wl,--wrap=realpath)
# APK の jniLibs に入れる名前 (lib*.so) にする。実体は共有ライブラリではなく ET_DYN の実行ファイル
set_target_properties(wimfuse PROPERTIES OUTPUT_NAME "libwimfuse.so" PREFIX "" SUFFIX "" RUNTIME_OUTPUT_DIRECTORY "${WIMFUSE_OUT}")
