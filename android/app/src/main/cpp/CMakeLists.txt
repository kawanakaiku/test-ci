cmake_minimum_required(VERSION 3.22)
project(usbmem_jni LANGUAGES C CXX)
set(USBMEM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
# エディション (personal | public)。Gradle の -PusbmemEdition=public が -DUSBMEM_EDITION=public を渡す。core/CMakeLists.txt が同じ変数を定義する (docs/editions.md)
set(USBMEM_EDITION "personal" CACHE STRING "personal | public")
# AGP の Debug ビルドは CMAKE_BUILD_TYPE=Debug で、CMAKE_CXX_FLAGS_DEBUG が空 (NDK の clang 既定 = -O0)。実機検証 (assembleDebug) の native コードが
# 無最適化になり、AES / SHA-256 / Argon2 / crc32c / 展開 / FS 本体が 5〜10 倍遅かった (LUKS2 4.5 MB/s・BitLocker 0.5 MB/s・Btrfs 9 MB/s の主因。docs/crypto-volumes.md 6.4)。
# Debug でも -O2 をかける (-g は残るのでクラッシュのスタックは読める)。無最適化で試したいときは -DUSBMEM_DEBUG_NO_OPT=ON。
option(USBMEM_DEBUG_NO_OPT "Do not add -O2 to non-release native builds" OFF)
if(NOT USBMEM_DEBUG_NO_OPT AND NOT CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo|MinSizeRel)$")
  add_compile_options(-O2)
endif()
# kfsd (別プロセスの FS ドライバ。docs/kfsd.md): 本体側のクライアント (RpcVolume。Apache-2.0。公開版にも入る) をリンクする。個人版は kfsd の実行ファイル (libkfsd.so) も作れる
set(USBMEM_BUILD_KFSD ON CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../../../../core core)
add_library(usbmem_jni SHARED jni_kfsd.cpp jni_bridge.cpp jni_ntfs.cpp jni_partedit.cpp jni_transform.cpp jni_blockdev.cpp jni_disktools.cpp jni_recovery.cpp jni_partscan.cpp)
if(USBMEM_EDITION STREQUAL "public")
  # 公開版 (docs/editions.md): GPL 系を含まない。リンクする FS は自前の F2FS / Btrfs / XFS だけ (圧縮の展開は core の自前の展開器)。個人版と同じ JNI のコードを、USBMEM_FS_* の条件コンパイルで使う
  target_compile_definitions(usbmem_jni PRIVATE USBMEM_EDITION_PUBLIC=1 USBMEM_FS_F2FS=1 USBMEM_FS_BTRFS=1 USBMEM_FS_XFS=1)
  target_link_libraries(usbmem_jni PRIVATE usbmem_f2fs usbmem_btrfs usbmem_xfs usbmem_fsvol usbmem_transform usbmem_disktools usbmem_recovery usbmem_partscan usbmem_core log)
else()
  # jni_ntfs.cpp は GPL-2.0-or-later の NTFS (fs/ntfs) ・exFAT・ext2・ext4・FAT・isofs・udf・squashfs・erofs・btrfs・f2fs・xfs をリンクするため、この共有ライブラリ全体が GPL 扱い (docs/licenses.md)
  target_link_libraries(usbmem_jni PRIVATE usbmem_ntfs usbmem_exfat usbmem_ext2 usbmem_ext4 usbmem_fat usbmem_isofs usbmem_udf usbmem_squashfs usbmem_erofs usbmem_btrfs usbmem_f2fs usbmem_xfs usbmem_transform usbmem_disktools usbmem_recovery usbmem_partscan usbmem_core log)
endif()
# 削除ファイルの一覧と救出 / カービング (docs/undelete-carving.md)。自前コードだけなので公開版・個人版とも同じ
target_sources(usbmem_jni PRIVATE jni_undelete.cpp)
target_link_libraries(usbmem_jni PRIVATE usbmem_undelete)
target_link_libraries(usbmem_jni PRIVATE usbmem_kfsd)
if(NOT USBMEM_EDITION STREQUAL "public")
  target_link_libraries(usbmem_jni PRIVATE usbmem_kfsd_mount)   # jni_ntfs.cpp のプロセス内マウントは kfsd と同じ mount_auto を使う (振り分けの規則を 1 か所にした)
endif()
target_link_options(usbmem_jni PRIVATE -Wl,-z,max-page-size=16384)
# kfsd を起こす JNI (fork + exec で socketpair の端を子の fd 3 へ渡す。:kfsd サービスとプラグイン APK が使う。org.usbmem.app.kfsd.KfsdSpawnNative)
add_library(kfsdspawn SHARED ${CMAKE_CURRENT_LIST_DIR}/../../../../kfsd-common/src/main/cpp/kfsd_spawn.cpp)
target_link_libraries(kfsdspawn PRIVATE log)
target_link_options(kfsdspawn PRIVATE -Wl,-z,max-page-size=16384)
# 個人版: kfsd の実行ファイル (GPL-2.0-or-later。ドライバ + LinuxCompat をリンクした PIE) を libkfsd.so の名前で作る (Gradle の -PusbmemKfsd=true のとき = USBMEM_KFSD_EXE=ON)。
# AGP は *.so を APK の lib/<abi>/ へ入れ、extractNativeLibs=true (同じ -PusbmemKfsd が useLegacyPackaging にする) なら nativeLibraryDir へ展開される。そこから :kfsd サービスが exec する。
# 指定が無ければ名前は kfsd のまま (APK には入らない。AGP はリンクだけはするので、時間は少し増える)。
option(USBMEM_KFSD_EXE "Build kfsd as libkfsd.so (personal edition)" OFF)
if(NOT USBMEM_EDITION STREQUAL "public" AND TARGET usbmem_kfsd_bin)
  # AGP は EXCLUDE_FROM_ALL を見ず、すべてのターゲットをビルドするので、-PusbmemKfsd なしでもリンクが通るようにしておく (名前が *.so でなければ APK には入らない)
  target_link_libraries(usbmem_kfsd_bin PRIVATE log)   # LinuxCompat が __android_log_print を使う
  if(USBMEM_KFSD_EXE)
    set_target_properties(usbmem_kfsd_bin PROPERTIES OUTPUT_NAME "libkfsd.so" PREFIX "" SUFFIX "" POSITION_INDEPENDENT_CODE ON)
    target_compile_options(usbmem_kfsd_bin PRIVATE -fPIE)
    target_link_options(usbmem_kfsd_bin PRIVATE -pie -Wl,-z,max-page-size=16384)
  else()
    set_target_properties(usbmem_kfsd_bin PROPERTIES EXCLUDE_FROM_ALL ON)
  endif()
endif()
# ネスト (ループバック) マウントの JNI (docs/nested-mount.md)
target_sources(usbmem_jni PRIVATE jni_nested.cpp)
# 個人版の FUSE 層 (wimfuse) の本体側 (JNI + RPC クライアント)。Gradle の -PusbmemWimFuse=... のとき (-DUSBMEM_WIMFUSE=ON) だけ。docs/fuse-layer.md 10 章
# 公開版 (USBMEM_EDITION=public) では、指定されても読み込まない (wimlib は LGPL / GPL。公開版のツリーに FUSE 層は無い。docs/editions.md 12 章)
if(USBMEM_WIMFUSE AND NOT USBMEM_EDITION STREQUAL "public")
  include(${CMAKE_CURRENT_LIST_DIR}/../../../../../core/fuseshim/wimfuse/android/client.cmake)
endif()
