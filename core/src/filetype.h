// SPDX-License-Identifier: Apache-2.0
// ファイル形式の先頭 (マジックナンバー) による判定と、ファイル名の拡張子との整合の検査。削除ファイルの確度 (undelete) が使う。
// マジックナンバーは各形式の公開仕様による。
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace usbmem {

// 先頭 n バイト (n >= 16 が望ましい) から形式名を返す。分からなければ空文字列。
std::string sniff_type(const uint8_t* p, size_t n);

enum class SigCheck { Match, Mismatch, Unknown };
const char* sig_check_name(SigCheck c);
// ファイル名の拡張子が既知の形式なら、先頭がそれと整合するか。テキスト系の拡張子は印字可能文字の割合で見る。
// 拡張子が未知なら、先頭が全部ゼロ / 全部 0xFF (中身が空) のときだけ Mismatch、それ以外は Unknown。
SigCheck check_head_for_name(const std::string& name, const uint8_t* head, size_t n, uint64_t file_size);

}  // namespace usbmem
