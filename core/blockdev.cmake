# SPDX-License-Identifier: Apache-2.0
# ディスクイメージ形式 (QCOW2/VHD/VHDX/VMDK)・COW オーバーレイ・NBD クライアント。core/CMakeLists.txt から include される。
# zlib (QCOW2 / VMDK の圧縮) に依存。ホストは find_package、Android NDK も sysroot の libz を見つける。
find_package(ZLIB REQUIRED)
target_sources(usbmem_core PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/src/blockdev_common.cpp
  ${CMAKE_CURRENT_LIST_DIR}/src/blockdev_qcow2.cpp
  ${CMAKE_CURRENT_LIST_DIR}/src/blockdev_vhd.cpp
  ${CMAKE_CURRENT_LIST_DIR}/src/blockdev_vhdx.cpp
  ${CMAKE_CURRENT_LIST_DIR}/src/blockdev_vmdk.cpp
  ${CMAKE_CURRENT_LIST_DIR}/src/blockdev_open.cpp
  ${CMAKE_CURRENT_LIST_DIR}/src/blockdev_overlay.cpp
  ${CMAKE_CURRENT_LIST_DIR}/src/blockdev_nbd.cpp
  ${CMAKE_CURRENT_LIST_DIR}/src/c_api_blockdev.cpp)
target_link_libraries(usbmem_core PUBLIC ZLIB::ZLIB)

if(USBMEM_BUILD_TESTS)
  add_executable(usbmem_blockdev_tests tests/test_main.cpp tests/test_blockdev.cpp tests/test_blockdev_capi.cpp tests/test_nbd.cpp)
  target_link_libraries(usbmem_blockdev_tests PRIVATE usbmem_core Threads::Threads)
  target_include_directories(usbmem_blockdev_tests PRIVATE src)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_FOUND)
    if(EXISTS /dev/shm)
      set(BD_FIX_DIR /dev/shm/usbmem_blockdev_$ENV{USER})
    else()
      set(BD_FIX_DIR ${CMAKE_CURRENT_BINARY_DIR}/blockdev_fixtures)
    endif()
    add_test(NAME blockdev_gen_fixtures COMMAND ${Python3_EXECUTABLE} -I
             ${CMAKE_CURRENT_LIST_DIR}/../tools/gen_blockdev_fixtures.py ${BD_FIX_DIR})
    add_test(NAME blockdev_unit COMMAND usbmem_blockdev_tests)
    add_test(NAME blockdev_fixtures COMMAND usbmem_blockdev_tests --fixtures ${BD_FIX_DIR})
    set_tests_properties(blockdev_fixtures PROPERTIES DEPENDS blockdev_gen_fixtures)
    set_tests_properties(blockdev_gen_fixtures PROPERTIES SKIP_RETURN_CODE 77)
  endif()
endif()
