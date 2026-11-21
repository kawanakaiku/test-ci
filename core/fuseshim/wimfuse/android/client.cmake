# SPDX-License-Identifier: Apache-2.0 (本ファイル)
# wimfuse の本体側 (JNI + RPC のクライアント) を usbmem_jni に足す。wimlib のコードは入らない (別プロセスの libwimfuse.so 側だけ)。
# android/app/src/main/cpp/CMakeLists.txt が、Gradle の -PusbmemWimFuse=<ビルド出力ディレクトリ> のとき (= USBMEM_WIMFUSE=ON) だけ include する。
get_filename_component(WIMFUSE_FS_DIR "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)   # core/fuseshim
target_sources(usbmem_jni PRIVATE
  ${WIMFUSE_FS_DIR}/wimfuse/android/jni_wimfuse.cpp
  ${WIMFUSE_FS_DIR}/wimfuse/wimfuse_client.cpp
  ${WIMFUSE_FS_DIR}/wimfuse/wim_volume.cpp
  ${WIMFUSE_FS_DIR}/rpc_volume.cpp)
# jni_vol.h (android/app/src/main/cpp) を jni_wimfuse.cpp から使う: include した側の CMakeLists のディレクトリ
target_include_directories(usbmem_jni PRIVATE ${WIMFUSE_FS_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(usbmem_jni PRIVATE USBMEM_WIMFUSE=1)
