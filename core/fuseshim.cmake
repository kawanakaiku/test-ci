# SPDX-License-Identifier: Apache-2.0 (本ファイル)
# fuseshim (プロセス内の libfuse 互換の層) の試作 (docs/fuse-layer.md)。既定は OFF。製品 (core / android) には入らない。
#   無改変の ntfs-3g のソースを、libfuse の代わりの shim と、unix_io.c の代わりの BlockDevice 接続 (ntfs3g_io.c) に載せて、
#   NTFS イメージを読む実行体 usbmem_fsdump_ntfs3g を作る。比較用に、自前の NTFS 実装で同じ出力をする usbmem_fsdump_core も作る。
# ntfs-3g のソースはリポジトリに入れない。tools/fuseshim/fetch_ntfs3g.sh が固定バージョンを sha256 検証つきで取得して configure する。
#   cmake -S core -B build -DUSBMEM_BUILD_FUSESHIM=ON -DUSBMEM_NTFS3G_SRC=<展開先> -DUSBMEM_NTFS3G_BUILD=<configure 済みのビルドディレクトリ (config.h がある)>
option(USBMEM_BUILD_FUSESHIM "fuseshim の試作 (ntfs-3g のソースが別途要る。docs/fuse-layer.md)" OFF)
if(USBMEM_BUILD_FUSESHIM)
  if(NOT USBMEM_NTFS)
    message(FATAL_ERROR "USBMEM_BUILD_FUSESHIM には USBMEM_NTFS=ON (比較対象の自前 NTFS) が要る")
  endif()
  set(USBMEM_NTFS3G_SRC "" CACHE PATH "展開済みの ntfs-3g のソース (tools/fuseshim/fetch_ntfs3g.sh の出力)")
  set(USBMEM_NTFS3G_BUILD "" CACHE PATH "configure 済みのビルドディレクトリ (config.h)")
  if(NOT EXISTS "${USBMEM_NTFS3G_SRC}/libntfs-3g/volume.c" OR NOT EXISTS "${USBMEM_NTFS3G_BUILD}/config.h")
    message(FATAL_ERROR "USBMEM_NTFS3G_SRC / USBMEM_NTFS3G_BUILD が正しくない (tools/fuseshim/fetch_ntfs3g.sh を先に実行する)")
  endif()
  set(FS_DIR ${CMAKE_CURRENT_LIST_DIR}/fuseshim)

  # libntfs-3g (libntfs-3g/Makefile.am の SOURCES から unix_io.c を除いたもの。その代わりが fuseshim/ntfs3g_io.c)
  set(N3G_LIB acls attrib attrlist bitmap bootsect cache collate compat compress debug device dir ea efs index inode ioctl lcnalloc
      logfile logging mft misc mst object_id realpath reparse runlist security unistr volume xattrs)
  set(N3G_FILES "")
  foreach(f ${N3G_LIB})
    list(APPEND N3G_FILES ${USBMEM_NTFS3G_SRC}/libntfs-3g/${f}.c)
  endforeach()
  add_library(fuseshim_ntfs3g STATIC
    ${N3G_FILES}
    ${USBMEM_NTFS3G_SRC}/src/ntfs-3g.c ${USBMEM_NTFS3G_SRC}/src/ntfs-3g_common.c
    ${FS_DIR}/ntfs3g_io.c ${FS_DIR}/fuseshim_fuse2.c)
  target_include_directories(fuseshim_ntfs3g
    PRIVATE ${USBMEM_NTFS3G_BUILD} ${USBMEM_NTFS3G_SRC}/include/ntfs-3g ${USBMEM_NTFS3G_SRC}/include/fuse-lite ${FS_DIR}
    PUBLIC ${FS_DIR})
  # FUSE_INTERNAL は config.h (--with-fuse=internal) が定義する。ntfs-3g.c は <fuse.h> の libfuse-lite 版のヘッダに書かれた API で動く
  target_compile_definitions(fuseshim_ntfs3g PRIVATE HAVE_CONFIG_H FUSE_USE_VERSION=26 DISABLE_PLUGINS=1)
  # main は同じ実行体に入るので改名する (ソースは無改変)。daemon(3) は何もしない関数に差し替える (同一プロセスで fork させない)
  set_source_files_properties(${USBMEM_NTFS3G_SRC}/src/ntfs-3g.c PROPERTIES COMPILE_DEFINITIONS "main=ntfs3g_main;daemon=fuseshim_daemon_stub")
  target_compile_options(fuseshim_ntfs3g PRIVATE -w -fno-strict-aliasing)
  target_link_libraries(fuseshim_ntfs3g PUBLIC Threads::Threads)

  # shim 越しの ntfs-3g でツリーを書き出す / 性能を測る (自前の NTFS と同じ walker。両者は 1 つの実行体にできない: ntfs_* のシンボルが衝突する)
  add_executable(usbmem_fsdump_ntfs3g ${FS_DIR}/fsdump.cpp ${FS_DIR}/fuseshim_volume.cpp ${FS_DIR}/ntfs3g_session.cpp ${FS_DIR}/rpc_volume.cpp)
  target_compile_definitions(usbmem_fsdump_ntfs3g PRIVATE USBMEM_FSDUMP_NTFS3G=1)
  target_include_directories(usbmem_fsdump_ntfs3g PRIVATE ${FS_DIR} ${CMAKE_CURRENT_LIST_DIR}/compat/public)
  target_link_libraries(usbmem_fsdump_ntfs3g PRIVATE fuseshim_ntfs3g usbmem_core Threads::Threads)

  add_executable(usbmem_fsdump_core ${FS_DIR}/fsdump.cpp)
  target_include_directories(usbmem_fsdump_core PRIVATE ${CMAKE_CURRENT_LIST_DIR}/compat/public)
  target_link_libraries(usbmem_fsdump_core PRIVATE usbmem_ntfs usbmem_core)

  # ---- wimlib の wimmount (libfuse 3.x 系の shim)。任意: USBMEM_WIMLIB_SRC / USBMEM_WIMLIB_LIB (tools/fuseshim/fetch_wimlib.sh の出力) があるときだけ
  set(USBMEM_WIMLIB_SRC "" CACHE PATH "展開済みの wimlib のソース (include/wimlib.h)")
  set(USBMEM_WIMLIB_LIB "" CACHE FILEPATH "ビルド済みの libwim.a")
  if(USBMEM_WIMLIB_SRC AND EXISTS "${USBMEM_WIMLIB_LIB}")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FUSE3 REQUIRED fuse3)  # ヘッダだけ使う。libfuse3 はリンクしない (shim が代わり)
    add_library(fuseshim_fuse3 STATIC ${FS_DIR}/fuseshim_fuse3.c)
    target_include_directories(fuseshim_fuse3 PRIVATE ${FUSE3_INCLUDE_DIRS} ${FS_DIR})
    target_compile_definitions(fuseshim_fuse3 PRIVATE FUSE_USE_VERSION=30)
    target_link_libraries(fuseshim_fuse3 PUBLIC Threads::Threads)
    add_executable(usbmem_fsdump_wim ${FS_DIR}/fsdump.cpp ${FS_DIR}/fuseshim_volume.cpp ${FS_DIR}/wim_session.cpp ${FS_DIR}/rpc_volume.cpp)
    target_compile_definitions(usbmem_fsdump_wim PRIVATE USBMEM_FSDUMP_WIM=1)
    target_include_directories(usbmem_fsdump_wim PRIVATE ${FS_DIR} ${CMAKE_CURRENT_LIST_DIR}/compat/public ${USBMEM_WIMLIB_SRC}/include)
    target_link_libraries(usbmem_fsdump_wim PRIVATE ${USBMEM_WIMLIB_LIB} fuseshim_fuse3 usbmem_core rt Threads::Threads)
  endif()
endif()
