// SPDX-License-Identifier: Apache-2.0
// 共通エラーコード。C API / JNI 境界でも同じ数値を使うため値を固定する。
#pragma once
#include <cstdint>

namespace usbmem {

enum class Errc : int32_t {
  Ok = 0,
  InvalidArgument = 1,
  Io = 2,            // OS/デバイスの I/O エラー (sys_errno 参照)
  OutOfRange = 3,    // デバイス範囲外のアクセス
  ReadOnly = 4,      // 読み取り専用デバイスへの書き込み
  Cancelled = 5,
  Timeout = 6,
  Disconnected = 7,  // USB 切断など、デバイスが消えた
  Corrupt = 8,       // 入力データ(ディスク上の構造)が不正
  Unsupported = 9,
  NoMemory = 10,
  Closed = 11,       // close 済みデバイスへのアクセス
  AuthFailed = 12,   // 暗号ボリュームのパスワード / 回復キー / 鍵ファイルが合わない (BlockTransform)
};

struct Status {
  Errc code = Errc::Ok;
  int sys_errno = 0;  // code == Io のときの errno (それ以外は 0)

  constexpr bool ok() const { return code == Errc::Ok; }
  constexpr explicit operator bool() const { return ok(); }
  static constexpr Status Ok() { return {}; }
  static constexpr Status Make(Errc c, int e = 0) { return Status{c, e}; }
};

const char* errc_name(Errc c);

}  // namespace usbmem
