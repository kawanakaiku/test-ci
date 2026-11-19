# SPDX-License-Identifier: Apache-2.0 (本ファイル) — パッチの対象 (fs/ntfs) は GPL-2.0-or-later
# 上流 (third_party/ntfs-resurrection/fs-ntfs) を書き換えずに、ビルドディレクトリの作業コピーへ当てる小さなパッチ。
# `patch` コマンドに依存しない (NDK のホスト・CI のどこでも動く) よう、1 行の完全一致の置換で書く。
# 置換元が見つからない / 複数ある (= 上流を更新して前提が変わった) ときは configure を失敗させ、黙って未適用にならないようにする。
# 経緯・根拠・検証は docs/ntfs-mftmirr.md、一覧は third_party/ntfs-resurrection/patches/README.md。

# usbmem_ntfs_replace_once(<変数名 (本文)> <置換元> <置換先> <説明>)
function(usbmem_ntfs_replace_once var anchor repl what)
  set(body "${${var}}")
  string(FIND "${body}" "${anchor}" p1)
  if(p1 EQUAL -1)
    message(FATAL_ERROR "ntfs patch (${what}): anchor not found. upstream changed? anchor: ${anchor}")
  endif()
  string(LENGTH "${anchor}" alen)
  math(EXPR rest_from "${p1} + ${alen}")
  string(SUBSTRING "${body}" ${rest_from} -1 rest)
  string(FIND "${rest}" "${anchor}" p2)
  if(NOT p2 EQUAL -1)
    message(FATAL_ERROR "ntfs patch (${what}): anchor is not unique. anchor: ${anchor}")
  endif()
  string(REPLACE "${anchor}" "${repl}" body "${body}")
  set(${var} "${body}" PARENT_SCOPE)
endfunction()

# super.c: $MFTMirr の比較範囲を MFT レコード 0-3 ($MFT / $MFTMirr / $LogFile / $Volume) だけにする (0001 に相当する番号は patches/README.md)。
# 上流 check_mft_mirror() は mftmirr_size (クラスタが大きいと 1 クラスタ分 = 64KiB クラスタで 64 レコード) を比較するが、Windows が同期するのは先頭の少数だけで、
# 後続は古いまま。偽陽性で書き込みマウントが自動的に読み取り専用に落ちる。ミラーの実体 (mftmirr_size、書き込み時の同期範囲、ランリストの検査) は上流のまま。
function(usbmem_ntfs_patch_super src dst)
  file(READ "${src}" body)
  usbmem_ntfs_replace_once(body
    "} while (++i < vol->mftmirr_size);"
    "} while (++i < (vol->mftmirr_size < 4 ? vol->mftmirr_size : 4)); /* usbmem: compare only MFT records 0-3 (docs/ntfs-mftmirr.md) */"
    "mftmirr compare range")
  set(old "")
  if(EXISTS "${dst}")
    file(READ "${dst}" old)
  endif()
  if(NOT old STREQUAL body)   # 同じ内容なら触らない (mtime を変えて全再ビルドになるのを避ける)
    get_filename_component(dir "${dst}" DIRECTORY)
    file(MAKE_DIRECTORY "${dir}")
    file(WRITE "${dst}" "${body}")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${src}" "${CMAKE_CURRENT_FUNCTION_LIST_FILE}")
endfunction()
