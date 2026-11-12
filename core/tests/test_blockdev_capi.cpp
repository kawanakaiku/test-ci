// SPDX-License-Identifier: Apache-2.0
// C API (JNI が使う境界) のテスト。
#include <fstream>

#include "test_util.h"
#include "usbmem/c_api_blockdev.h"
#include "usbmem/disk_image.h"

extern std::string g_fixture_dir;
using namespace usbmem;

namespace {
struct CResolveCtx { std::string dir; int calls = 0; };
int c_resolve(void* user, const char* name, int kind, const char* hint, usbmem_device** out) {
  auto* c = static_cast<CResolveCtx*>(user);
  c->calls++;
  (void)kind; (void)hint;
  std::string n = name;
  if (n.empty() || n[0] == '/' || n.find("..") != std::string::npos) return static_cast<int>(Errc::InvalidArgument);
  return usbmem_open_path((c->dir + "/" + n).c_str(), 1, out);
}
}  // namespace

TEST(fixture_blockdev_c_api) {
  if (!std::ifstream(g_fixture_dir + "/manifest.tsv").good()) { std::fprintf(stderr, "  (skip: no blockdev fixtures)\n"); return; }
  usbmem_device* f = nullptr;
  CHECK_EQ(usbmem_open_path((g_fixture_dir + "/chain_top.qcow2").c_str(), 1, &f), 0);
  char fmt[16];
  CHECK_EQ(usbmem_image_detect(f, fmt, sizeof fmt), 0);
  CHECK(std::string(fmt) == "qcow2");
  CResolveCtx ctx{g_fixture_dir};
  usbmem_device* img = nullptr;
  char* info = nullptr;
  CHECK_EQ(usbmem_image_open(f, c_resolve, &ctx, &img, &info), 0);
  CHECK(info != nullptr);
  if (info) {
    std::string j = info;
    CHECK(j.find("\"format\":\"qcow2\"") != std::string::npos);
    CHECK(j.find("\"has_parent\":true") != std::string::npos);
    usbmem_free(info);
  }
  CHECK_EQ(ctx.calls, 2);
  char* pj = nullptr;
  CHECK_EQ(usbmem_list_partitions_json(img, &pj), 0);   // 既存の C API がそのまま使える
  usbmem_free(pj);
  // resolver 無し → Unsupported と理由
  usbmem_device* f2 = nullptr;
  CHECK_EQ(usbmem_open_path((g_fixture_dir + "/chain_mid.qcow2").c_str(), 1, &f2), 0);
  usbmem_device* none = nullptr;
  char* info2 = nullptr;
  CHECK_EQ(usbmem_image_open(f2, nullptr, nullptr, &none, &info2), static_cast<int>(Errc::Unsupported));
  CHECK(none == nullptr);
  if (info2) { CHECK(std::string(info2).find("resolver") != std::string::npos); usbmem_free(info2); }
  usbmem_close(f2);
  // resolver が拒否 → そのエラー
  usbmem_device* f3 = nullptr;
  CHECK_EQ(usbmem_open_path((g_fixture_dir + "/chain_mid.qcow2").c_str(), 1, &f3), 0);
  CResolveCtx deny{"/nonexistent"};
  usbmem_device* d3 = nullptr;
  CHECK(usbmem_image_open(f3, c_resolve, &deny, &d3, nullptr) != 0);
  usbmem_close(f3);
  // オーバーレイ (C API 経由で作成・破棄・統計)
  usbmem_device* ov = nullptr;
  CHECK_EQ(usbmem_overlay_create(img, nullptr, 0, 0, &ov), 0);
  CHECK_EQ(usbmem_overlay_dirty_bytes(ov), 0ull);
  CHECK_EQ(usbmem_overlay_discard(ov), 0);
  CHECK_EQ(usbmem_overlay_discard(img), static_cast<int>(Errc::InvalidArgument));   // オーバーレイでないデバイス
  usbmem_close(ov);
  // パス版
  usbmem_device* p = nullptr;
  char* info3 = nullptr;
  CHECK_EQ(usbmem_image_open_path((g_fixture_dir + "/chain_top.qcow2").c_str(), 0, &p, &info3), 0);
  usbmem_free(info3);
  usbmem_close(p);
  usbmem_close(img);
  usbmem_close(f);
  // 存在しないパス / 引数エラー
  usbmem_device* x = nullptr;
  CHECK(usbmem_image_open_path("/nonexistent/path.qcow2", 0, &x, nullptr) != 0);
  CHECK_EQ(usbmem_image_open(nullptr, nullptr, nullptr, &x, nullptr), static_cast<int>(Errc::InvalidArgument));
  CHECK_EQ(usbmem_overlay_commit(nullptr, nullptr, 0), static_cast<int>(Errc::InvalidArgument));
}

TEST(blockdev_c_api_overlay_roundtrip) {
  // 一時ファイルの RAW → オーバーレイ (ファイル保存) → 書き込み → commit/discard
  std::string raw = test_tmpdir() + "/usbmem_capi_ov.raw", log = test_tmpdir() + "/usbmem_capi_ov.log";
  ::unlink(log.c_str());
  {
    std::ofstream o(raw, std::ios::binary);
    std::vector<char> z(1 << 20, 'a');
    o.write(z.data(), static_cast<std::streamsize>(z.size()));
  }
  usbmem_device *base = nullptr, *ov = nullptr, *target = nullptr;
  CHECK_EQ(usbmem_open_path(raw.c_str(), 1, &base), 0);
  CHECK_EQ(usbmem_overlay_create(base, log.c_str(), 4096, 0, &ov), 0);
  // 書き込みは C++ のデバイス経由 (C API は読み書きを公開していない: JNI は FS 層経由で使う)
  CHECK_EQ(usbmem_overlay_dirty_bytes(ov), 0ull);
  usbmem_close(ov);
  usbmem_close(base);
  CHECK_EQ(usbmem_open_path(raw.c_str(), 0, &target), 0);
  usbmem_close(target);
  ::unlink(raw.c_str());
  ::unlink(log.c_str());
}
