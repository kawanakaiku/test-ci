// SPDX-License-Identifier: GPL-2.0-or-later
#include "wim_session.h"

#include "wimlib.h"

namespace usbmem {

std::unique_ptr<FuseShimSession> open_wim(const std::string& wim_path, int image, std::string* err) {
  int rc = wimlib_global_init(WIMLIB_INIT_FLAG_ASSUME_UTF8);
  WIMStruct* wim = nullptr;
  if (!rc) rc = wimlib_open_wim(wim_path.c_str(), 0, &wim);
  if (rc) {
    if (err) *err = std::string("wimlib_open_wim: ") + wimlib_get_error_string(static_cast<enum wimlib_error_code>(rc));
    return nullptr;
  }
  // 読み取り専用 (WIMLIB_MOUNT_FLAG_READWRITE なし) では staging ディレクトリは作られない (mount_image.c: make_staging_dir は READWRITE のときだけ)。
  // マウントポイントは fuse_main に渡されるだけで、shim は使わない。
  auto main_fn = [wim, image]() -> int { return wimlib_mount_image(wim, image, "/mnt/usbmem-wim", WIMLIB_MOUNT_FLAG_STREAM_INTERFACE_NONE, nullptr); };
  int exit_code = 0;
  auto s = FuseShimSession::start(main_fn, "wimfs", &exit_code);
  if (!s && err) *err = "wimlib_mount_image が fuse_main に入る前に終了した (戻り値 " + std::to_string(exit_code) + ")";
  return s;
}

}  // namespace usbmem
