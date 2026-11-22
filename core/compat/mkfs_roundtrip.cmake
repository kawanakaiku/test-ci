# SPDX-License-Identifier: Apache-2.0 (本ファイル)
# 自前の mkfs (core/src/mkfs*.cpp) で作った FS を、アプリのドライバ (個人版だけ) で読み書きする往復テスト。docs/mkfs-extended.md
# core/compat/CMakeLists.txt から include される (各 FS ライブラリが定義された後)。usbmem_disktools は core/disktools.cmake (後で定義) を名前で参照する。
if(USBMEM_BUILD_TESTS AND TARGET usbmem_ntfs AND TARGET usbmem_exfat AND TARGET usbmem_ext2 AND TARGET usbmem_ext4 AND TARGET usbmem_fat)
  add_executable(usbmem_mkfs_roundtrip tests/test_mkfs_roundtrip.cpp)
  target_link_libraries(usbmem_mkfs_roundtrip PRIVATE usbmem_disktools usbmem_ntfs usbmem_exfat usbmem_ext2 usbmem_ext4 usbmem_fat usbmem_compat_rt usbmem_core Threads::Threads)
  if(EXISTS /dev/shm)
    add_test(NAME mkfs_roundtrip COMMAND sh ${CMAKE_CURRENT_LIST_DIR}/../../tools/run_mkfs_roundtrip.sh $<TARGET_FILE:usbmem_mkfs_roundtrip>)
    set_tests_properties(mkfs_roundtrip PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1800 ENVIRONMENT "ASAN_OPTIONS=verify_asan_link_order=0:detect_leaks=0")
  endif()
endif()
