// SPDX-License-Identifier: Apache-2.0
// LUKS2 のメタデータ JSON 用の小さなパーサ。深さ・ノード数・文字列長に上限を持ち、壊れた入力でも無制限に確保・再帰しない。
// 整数は int64 (小数・指数は「数値だが整数でない」として保持し、整数として取り出すと失敗)。
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace usbmem {
namespace tf {

struct JsonValue {
  enum Type { Null, Bool, Int, Real, String, Array, Object } type = Null;
  bool b = false;
  int64_t i = 0;
  std::string s;                                      // String
  std::vector<JsonValue> arr;                         // Array
  std::vector<std::pair<std::string, JsonValue>> obj; // Object (挿入順)

  const JsonValue* get(const std::string& key) const {  // Object でなければ nullptr
    if (type != Object) return nullptr;
    for (auto& kv : obj) if (kv.first == key) return &kv.second;
    return nullptr;
  }
  bool is_obj() const { return type == Object; }
  bool is_arr() const { return type == Array; }
  bool is_str() const { return type == String; }
  // 整数値: Int、または 10 進数の文字列 ("32768" — LUKS2 は 64 ビット値を文字列で持つ)。負数・桁あふれは失敗。
  bool as_u64(uint64_t* out) const;
};

// text[0, len) を解釈する。成功で true。失敗時 *err に短い説明 (入力の内容は含めない)。
bool json_parse(const char* text, size_t len, JsonValue* out, std::string* err);

// 標準 Base64 (パディングあり/なし両対応、空白無視)。不正なら false。
bool base64_decode(const std::string& in, std::vector<uint8_t>* out, size_t max_out);

}  // namespace tf
}  // namespace usbmem
