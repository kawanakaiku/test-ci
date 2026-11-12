# SPDX-License-Identifier: Apache-2.0 (本ファイル) — btrfs_volume.cpp は自前のリーダー、btrfs_decomp.c と usbmem_kdecomp は GPL-2.0 系
# Btrfs の読み取り専用リーダー。core/compat/CMakeLists.txt の末尾から include される。docs/btrfs-port.md
# 他のファイルシステムのターゲットと衝突しないよう、このファイルに集める。
add_library(usbmem_btrfs STATIC ${CMAKE_CURRENT_LIST_DIR}/src/btrfs_volume.cpp ${CMAKE_CURRENT_LIST_DIR}/src/btrfs_decomp.c)
target_include_directories(usbmem_btrfs PUBLIC public)
target_compile_options(usbmem_btrfs PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-std=gnu11> $<$<COMPILE_LANGUAGE:C>:-w> $<$<COMPILE_LANGUAGE:C>:-fno-strict-aliasing> $<$<COMPILE_LANGUAGE:C>:-fwrapv>
  "$<$<COMPILE_LANGUAGE:C>:SHELL:-include stdint.h>" "$<$<COMPILE_LANGUAGE:C>:SHELL:-include stddef.h>" "$<$<COMPILE_LANGUAGE:C>:SHELL:-include endian.h>")
target_compile_definitions(usbmem_btrfs PRIVATE __KERNEL__)
target_link_libraries(usbmem_btrfs PUBLIC usbmem_kdecomp usbmem_compat_rt usbmem_transform usbmem_core)

if(USBMEM_BUILD_TESTS)
  add_executable(usbmem_btrfs_tests tests/test_btrfs.cpp)
  target_link_libraries(usbmem_btrfs_tests PRIVATE usbmem_btrfs)
endif()
