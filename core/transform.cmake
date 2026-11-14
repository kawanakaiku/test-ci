# SPDX-License-Identifier: Apache-2.0
# BlockTransform (BitLocker / LUKS の読み取り専用復号) と暗号プリミティブ。docs/crypto-volumes.md
# core/CMakeLists.txt の末尾から include される。usbmem_core にだけ依存する (Android 非依存・PC でテスト可能)。
add_library(usbmem_transform STATIC
  src/crypto_hash.cpp src/crypto_aes.cpp src/crypto_aes_hw.cpp src/crypto_sha_hw.cpp src/crypto_argon2.cpp
  src/transform.cpp src/transform_bitlocker.cpp src/transform_luks.cpp src/mini_json.cpp src/c_api_transform.cpp)
target_include_directories(usbmem_transform PUBLIC include)
target_link_libraries(usbmem_transform PUBLIC usbmem_core)
target_compile_options(usbmem_transform PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wno-sign-conversion)

if(USBMEM_BUILD_TESTS)
  add_executable(usbmem_transform_tests tests/test_main.cpp tests/test_crypto.cpp tests/test_transform.cpp)
  target_include_directories(usbmem_transform_tests PRIVATE tests src)
  target_link_libraries(usbmem_transform_tests PRIVATE usbmem_transform)
  add_test(NAME crypto_unit COMMAND usbmem_transform_tests)
  # 復号ボリュームの読み取り速度のマイクロベンチ (遅延つき模擬デバイス。ctest には入れない。docs/crypto-volumes.md 6.4)
  add_executable(usbmem_transform_bench tests/bench_transform.cpp)
  target_link_libraries(usbmem_transform_bench PRIVATE usbmem_transform)
  if(TARGET usbmem_btrfs)
    target_link_libraries(usbmem_transform_bench PRIVATE usbmem_btrfs)
    target_compile_definitions(usbmem_transform_bench PRIVATE USBMEM_BENCH_BTRFS=1)
  endif()
endif()

# 復号 → 既存 FS 実装への結合検証ツール (LinuxCompat 版 FS が有効なビルドのみ)
if(USBMEM_BUILD_TESTS AND TARGET usbmem_ntfs)
  add_executable(usbmem_cryptovol_tests compat/tests/test_cryptovol.cpp)
  target_include_directories(usbmem_cryptovol_tests PRIVATE compat/tests)
  target_link_libraries(usbmem_cryptovol_tests PRIVATE usbmem_transform usbmem_ntfs)
  target_compile_definitions(usbmem_cryptovol_tests PRIVATE USBMEM_CV_NTFS=1)
  foreach(_fs ext2 ext4 fat)
    if(TARGET usbmem_${_fs})
      string(TOUPPER ${_fs} _FS)
      target_link_libraries(usbmem_cryptovol_tests PRIVATE usbmem_${_fs})
      target_compile_definitions(usbmem_cryptovol_tests PRIVATE USBMEM_CV_${_FS}=1)
    endif()
  endforeach()
endif()

# 実フィクスチャ (tools/gen_luks_fixtures.py / tools/winvhd/make_bitlocker_matrix.sh が /dev/shm/usbmem_crypt_$USER に作る) があれば検証。無ければ SKIP。
if(USBMEM_BUILD_TESTS AND TARGET usbmem_cryptovol_tests)
  add_test(NAME crypto_volume_fixtures COMMAND sh ${CMAKE_CURRENT_LIST_DIR}/../tools/run_crypto_volume_tests.sh $<TARGET_FILE:usbmem_cryptovol_tests>)
  set_tests_properties(crypto_volume_fixtures PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 3600)
  if(USBMEM_SANITIZE)
    set_tests_properties(crypto_volume_fixtures PROPERTIES ENVIRONMENT "USBMEM_CV_QUICK=1")
  endif()
endif()
