// SPDX-License-Identifier: Apache-2.0
// c_api_partedit.cpp の不透明ハンドルの中身 (c_api_partscan.cpp が、探索で作った復元案を編集セッションへ積むために共有する。公開しない)。
#pragma once
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "usbmem/partition_edit.h"

struct usbmem_partedit {
  std::unique_ptr<usbmem::PartitionEditor> ed;
  usbmem::EditPlan plan;
  bool has_plan = false;
  std::vector<uint8_t> last_backup;
  std::string err_code, err_msg;
  std::mutex mu;
};
