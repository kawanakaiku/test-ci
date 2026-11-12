# SPDX-License-Identifier: Apache-2.0 (本ファイル) — 取り込む fs/squashfs, fs/erofs, lib/{zlib_inflate,lz4,lzo,xz,zstd} は GPL-2.0 系
# 圧縮ファイルシステム (squashfs / erofs) とカーネルの展開ライブラリ。core/compat/CMakeLists.txt の末尾から include される。
# 他のファイルシステムのターゲットと衝突しないよう、すべてこのファイルに集める。docs/squashfs-port.md / docs/erofs-port.md
set(KLIB_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../third_party/kernel-lib)
set(SQUASHFS_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../third_party/squashfs)
set(EROFS_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../third_party/erofs)

# ---- 互換層の追加分 (bio / 待ち行列 / ページキャッシュ)
target_sources(usbmem_compat_rt PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src/rt_zfs.c)

# ---- カーネルの展開ライブラリ (無改変): zlib_inflate / lz4 / lzo1x / xz (LZMA2, BCJ) / zstd
file(GLOB KDECOMP_ZLIB ${KLIB_ROOT}/lib/zlib_inflate/*.c)
set(KDECOMP_SRC ${KDECOMP_ZLIB}
  ${KLIB_ROOT}/lib/lz4/lz4_decompress.c
  ${CMAKE_CURRENT_LIST_DIR}/src/zfs_lzo.c ${KLIB_ROOT}/lib/xxhash.c
  ${KLIB_ROOT}/lib/xz/xz_crc32.c ${KLIB_ROOT}/lib/xz/xz_dec_lzma2.c ${KLIB_ROOT}/lib/xz/xz_dec_stream.c ${KLIB_ROOT}/lib/xz/xz_dec_bcj.c
  ${KLIB_ROOT}/lib/zstd/zstd_decompress_module.c ${KLIB_ROOT}/lib/zstd/zstd_common_module.c
  ${KLIB_ROOT}/lib/zstd/common/debug.c ${KLIB_ROOT}/lib/zstd/common/entropy_common.c ${KLIB_ROOT}/lib/zstd/common/error_private.c
  ${KLIB_ROOT}/lib/zstd/common/fse_decompress.c ${KLIB_ROOT}/lib/zstd/common/zstd_common.c
  ${KLIB_ROOT}/lib/zstd/decompress/huf_decompress.c ${KLIB_ROOT}/lib/zstd/decompress/zstd_ddict.c
  ${KLIB_ROOT}/lib/zstd/decompress/zstd_decompress.c ${KLIB_ROOT}/lib/zstd/decompress/zstd_decompress_block.c)
add_library(usbmem_kdecomp STATIC ${KDECOMP_SRC})
target_include_directories(usbmem_kdecomp PUBLIC ${KLIB_ROOT}/include)
target_compile_options(usbmem_kdecomp PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-std=gnu11> $<$<COMPILE_LANGUAGE:C>:-w> $<$<COMPILE_LANGUAGE:C>:-fno-strict-aliasing> $<$<COMPILE_LANGUAGE:C>:-fwrapv>
  "$<$<COMPILE_LANGUAGE:C>:SHELL:-iquote ${KLIB_ROOT}/lib/lz4>" "$<$<COMPILE_LANGUAGE:C>:SHELL:-iquote ${KLIB_ROOT}/lib/lzo>"
  "$<$<COMPILE_LANGUAGE:C>:SHELL:-iquote ${KLIB_ROOT}/lib/xz>" "$<$<COMPILE_LANGUAGE:C>:SHELL:-iquote ${KLIB_ROOT}/lib/zlib_inflate>"
  "$<$<COMPILE_LANGUAGE:C>:SHELL:-iquote ${KLIB_ROOT}/lib/zstd>"
  # NDK では <linux/types.h> が UAPI 版 (uint8_t などを定義しない) に解決されるため、libc の基本型を先に入れる
  # zstd の mem.h は defined(__LITTLE_ENDIAN) で MEM_isLittleEndian() を決める (glibc は <endian.h> 経由で定義。bionic は明示的に入れないと未定義 → HUF テーブルが壊れる)
  "$<$<COMPILE_LANGUAGE:C>:SHELL:-include stdint.h>" "$<$<COMPILE_LANGUAGE:C>:SHELL:-include stddef.h>" "$<$<COMPILE_LANGUAGE:C>:SHELL:-include endian.h>")
target_compile_definitions(usbmem_kdecomp PRIVATE __KERNEL__ CONFIG_HAVE_EFFICIENT_UNALIGNED_ACCESS=1 CONFIG_X86_64=1 CONFIG_XZ_DEC_X86=1 CONFIG_XZ_DEC_ARM64=1 CONFIG_XZ_DEC_ARM=1)
if(USBMEM_SANITIZE)
  set_source_files_properties(${KDECOMP_SRC} PROPERTIES COMPILE_OPTIONS "-fno-sanitize=alignment;-fno-sanitize=shift-base;-fno-sanitize=nonnull-attribute")
endif()
target_link_libraries(usbmem_kdecomp PUBLIC usbmem_compat_rt)

# ---- squashfs (Linux fs/squashfs, 無改変)。読み取り専用。展開は usbmem_kdecomp (zlib / lz4 / lzo / xz / zstd)。
# 構成 (カーネルの Kconfig の選択に相当): FILE_DIRECT (データブロックを直接展開)、DECOMP_SINGLE (展開器は 1 つ。排他は mutex)、xattr なし。
set(SQUASHFS_SRC)
foreach(f block cache decompressor dir export file file_direct fragment id inode namei page_actor super symlink decompressor_single
          zlib_wrapper lz4_wrapper lzo_wrapper xz_wrapper zstd_wrapper)
  list(APPEND SQUASHFS_SRC ${SQUASHFS_ROOT}/fs-squashfs/${f}.c)
endforeach()
if(USBMEM_SANITIZE)
  set_source_files_properties(${SQUASHFS_SRC} PROPERTIES COMPILE_OPTIONS "-fno-sanitize=alignment;-fno-sanitize=shift-base;-fno-sanitize=nonnull-attribute")
endif()
add_library(usbmem_squashfs STATIC ${SQUASHFS_SRC} ${CMAKE_CURRENT_LIST_DIR}/src/rt_vfs_squashfs.c ${CMAKE_CURRENT_LIST_DIR}/src/squashfs_volume.cpp)
target_include_directories(usbmem_squashfs PUBLIC public)
target_compile_options(usbmem_squashfs PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-std=gnu11> $<$<COMPILE_LANGUAGE:C>:-w> $<$<COMPILE_LANGUAGE:C>:-fno-strict-aliasing> $<$<COMPILE_LANGUAGE:C>:-fwrapv>
  $<$<COMPILE_LANGUAGE:C>:-Wno-error=incompatible-pointer-types>   # 上流は u64 = unsigned long long 前提 (ホストの uint64_t は unsigned long)
  "$<$<COMPILE_LANGUAGE:C>:SHELL:-iquote ${SQUASHFS_ROOT}/fs-squashfs>")
target_compile_definitions(usbmem_squashfs PRIVATE CONFIG_HAVE_EFFICIENT_UNALIGNED_ACCESS=1 CONFIG_BLOCK=1 USBMEM_MODULE_INIT_NAME=usbmem_module_init_squashfs
  CONFIG_SQUASHFS_FILE_DIRECT=1 CONFIG_SQUASHFS_DECOMP_SINGLE=1 CONFIG_SQUASHFS_FRAGMENT_CACHE_SIZE=3
  CONFIG_SQUASHFS_ZLIB=1 CONFIG_SQUASHFS_LZ4=1 CONFIG_SQUASHFS_LZO=1 CONFIG_SQUASHFS_XZ=1 CONFIG_SQUASHFS_ZSTD=1)
target_link_libraries(usbmem_squashfs PUBLIC usbmem_kdecomp usbmem_compat_rt)

# ---- squashfs のテスト: mksquashfs (圧縮方式 × ブロックサイズ × フラグメント有無) + tools/gen_squashfs_fixture.py。unsquashfs で展開し直した結果との一致も生成時に確認する。ext2 とのコア共存も検査。
if(USBMEM_BUILD_TESTS)
  add_executable(usbmem_squashfs_tests tests/test_squashfs.cpp)
  target_link_libraries(usbmem_squashfs_tests PRIVATE usbmem_squashfs usbmem_ext2)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_FOUND AND EXISTS /dev/shm)
    set(SQUASHFS_FIX_DIR /dev/shm/usbmem_squashfs_$ENV{USER})
    add_test(NAME squashfs_gen_fixture COMMAND ${CMAKE_COMMAND} -E env "PATH=$ENV{PATH}:/sbin:/usr/sbin" ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_LIST_DIR}/../../tools/gen_squashfs_fixture.py ${SQUASHFS_FIX_DIR})
    set_tests_properties(squashfs_gen_fixture PROPERTIES FIXTURES_SETUP squashfs_fixture TIMEOUT 900)
    add_test(NAME squashfs_read COMMAND usbmem_squashfs_tests ${SQUASHFS_FIX_DIR})
    add_test(NAME squashfs_corrupt COMMAND usbmem_squashfs_tests --corrupt ${SQUASHFS_FIX_DIR})
    add_test(NAME squashfs_ioerr COMMAND usbmem_squashfs_tests --ioerr ${SQUASHFS_FIX_DIR})
    add_test(NAME squashfs_fuzz COMMAND usbmem_squashfs_tests --fuzz ${SQUASHFS_FIX_DIR} 400 1)
    add_test(NAME squashfs_coexist COMMAND usbmem_squashfs_tests --coexist ${SQUASHFS_FIX_DIR} /dev/shm/usbmem_ext2_$ENV{USER})
    set_tests_properties(squashfs_read squashfs_corrupt squashfs_ioerr squashfs_fuzz PROPERTIES FIXTURES_REQUIRED squashfs_fixture TIMEOUT 1800)
    set_tests_properties(squashfs_coexist PROPERTIES FIXTURES_REQUIRED "squashfs_fixture;ext2_fixture" TIMEOUT 600)
    # 失敗したマウントの後始末は inode 内部の確保物を解放しきれない場合がある (NTFS / exFAT / ext2 と同じ既知事項)。メモリ安全性 (UAF/OOB/ハング) の検査に集中する。
    set_tests_properties(squashfs_corrupt squashfs_ioerr squashfs_fuzz squashfs_coexist PROPERTIES ENVIRONMENT "ASAN_OPTIONS=verify_asan_link_order=0:detect_leaks=0")
    set_tests_properties(squashfs_read PROPERTIES ENVIRONMENT "ASAN_OPTIONS=verify_asan_link_order=0:detect_leaks=1")
  endif()
endif()
