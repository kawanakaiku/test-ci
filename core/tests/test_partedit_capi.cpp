// SPDX-License-Identifier: Apache-2.0
// C API (usbmem_partedit_*) のテスト: 実ファイル (tmpfs) を usbmem_open_path で開き、JNI/Kotlin が通るのと同じ経路で編集する。
#include <fcntl.h>
#include <sys/stat.h>

#include <fstream>
#include <string>

#include "partedit_test_util.h"
#include "usbmem/c_api_partedit.h"

namespace {

std::string make_image(const char* name, size_t bytes) {
  std::string p = test_tmpdir() + "/usbmem_capi_" + name + ".img";
  ::unlink(p.c_str());
  int fd = ::open(p.c_str(), O_CREAT | O_RDWR, 0600);
  if (fd >= 0) { (void)!::ftruncate(fd, static_cast<off_t>(bytes)); ::close(fd); }
  return p;
}

std::string take(char* s) {
  std::string r = s ? s : "";
  usbmem_free(s);
  return r;
}

std::string json_field(const std::string& j, const std::string& key) {
  size_t k = j.find("\"" + key + "\":\"");
  if (k == std::string::npos) return "";
  k += key.size() + 4;
  return j.substr(k, j.find('"', k) - k);
}

std::string last_code(usbmem_partedit* h) {
  char c[64] = {}, m[256] = {};
  usbmem_partedit_last_error(h, c, sizeof c, m, sizeof m);
  return c;
}

std::vector<uint8_t> slurp(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), {});
}

}  // namespace

TEST(partedit_capi_full_cycle) {
  std::string img = make_image("cycle", 32u << 20), bk = img + ".bak";
  usbmem_device* dev = nullptr;
  CHECK_EQ(usbmem_open_path(img.c_str(), 0, &dev), 0);
  usbmem_partedit* h = nullptr;
  CHECK_EQ(usbmem_partedit_open(dev, 0, 0, 5, &h), 0);

  // 2 つ目の書き込み可エディタは拒否
  usbmem_partedit* h2 = nullptr;
  CHECK(usbmem_partedit_open(dev, 0, 0, 5, &h2) != 0);
  CHECK(h2 == nullptr);
  char c[64] = {}, m[128] = {};
  usbmem_partedit_last_error(nullptr, c, sizeof c, m, sizeof m);
  CHECK(std::string(c) == std::string("busy"));

  CHECK_EQ(usbmem_partedit_new_table(h, 2, 128, 0), 0);
  unsigned a = 0, b = 0;
  CHECK_EQ(usbmem_partedit_create(h, 0, 2048, 8192, 0, "C12A7328-F81F-11D2-BA4B-00A0C93EC93B", nullptr, "EFI 日本語", 0, 0, &a), 0);
  CHECK_EQ(usbmem_partedit_create(h, 0, 0, 0, 0, nullptr, nullptr, "data", 4, 0, &b), 0);
  CHECK(a != b);

  char* pj = nullptr;
  CHECK_EQ(usbmem_partedit_plan_json(h, 0, &pj), 0);
  std::string plan = take(pj);
  CHECK(plan.find("\"ok\":true") != std::string::npos);
  std::string phrase = json_field(plan, "confirmPhrase");
  CHECK(phrase.rfind("WRITE ", 0) == 0);

  // 間違った確認文字列
  char* tok = nullptr;
  CHECK_EQ(usbmem_partedit_make_token(h, "yes", &tok), 0);
  std::string bad = take(tok);
  char* res = nullptr;
  CHECK(usbmem_partedit_apply(h, bad.c_str(), bk.c_str(), 1700000000, &res) != 0);
  CHECK(take(res).find("\"ok\":false") != std::string::npos);
  CHECK(last_code(h) == std::string("bad-token"));
  CHECK(slurp(img) == std::vector<uint8_t>(32u << 20, 0));  // 何も書かれていない

  CHECK_EQ(usbmem_partedit_make_token(h, phrase.c_str(), &tok), 0);
  std::string good = take(tok);
  CHECK_EQ(usbmem_partedit_apply(h, good.c_str(), bk.c_str(), 1700000000, &res), 0);
  std::string r = take(res);
  CHECK(r.find("\"ok\":true") != std::string::npos && r.find("\"verified\":true") != std::string::npos);

  // 適用後の計画は無効
  CHECK(usbmem_partedit_apply(h, good.c_str(), nullptr, 0, &res) != 0);
  take(res);
  CHECK(last_code(h) == std::string("no-plan"));

  // バックアップファイルは適用前 (空ディスク) の状態を持ち、検査できる
  auto bytes = slurp(bk);
  CHECK(bytes.size() > 64);
  char* ij = nullptr;
  CHECK_EQ(usbmem_partedit_backup_info_json(bytes.data(), bytes.size(), &ij), 0);
  std::string info = take(ij);
  CHECK(info.find("\"version\":1") != std::string::npos);
  CHECK(info.find("\"sectorSize\":512") != std::string::npos);
  bytes[70] ^= 1;
  CHECK(usbmem_partedit_backup_info_json(bytes.data(), bytes.size(), &ij) != 0);

  // 新しいテーブルが読める (open 時のスナップショットなので開き直す)
  usbmem_partedit_close(h);
  CHECK_EQ(usbmem_partedit_open(dev, 0, 0, 5, &h), 0);
  char* tj = nullptr;
  CHECK_EQ(usbmem_partedit_table_json(h, &tj), 0);
  std::string t = take(tj);
  CHECK(t.find("\"kind\":\"gpt\"") != std::string::npos);
  CHECK(t.find("EFI 日本語") != std::string::npos);
  char* gj = nullptr;
  CHECK_EQ(usbmem_partedit_gpt_json(h, &gj), 0);
  CHECK(take(gj).find("\"suggestedRepairs\":[]") != std::string::npos);

  // sfdisk 互換出力
  char* sf = nullptr;
  CHECK_EQ(usbmem_partedit_export_sfdisk(h, "/dev/sdx", &sf), 0);
  std::string dump = take(sf);
  CHECK(dump.find("label: gpt") != std::string::npos && dump.find("/dev/sdx1 :") != std::string::npos);

  // 現在の表のバックアップ → 表を消す → 復元計画 → 適用 → 元通り
  unsigned char* bp = nullptr;
  size_t bl = 0;
  CHECK_EQ(usbmem_partedit_backup(h, 1, &bp, &bl), 0);
  std::vector<uint8_t> full(bp, bp + bl);
  usbmem_free(bp);
  auto before = slurp(img);
  CHECK_EQ(usbmem_partedit_remove(h, a), 0);
  CHECK_EQ(usbmem_partedit_plan_json(h, 0, &pj), 0);
  plan = take(pj);
  phrase = json_field(plan, "confirmPhrase");
  CHECK_EQ(usbmem_partedit_make_token(h, phrase.c_str(), &tok), 0);
  good = take(tok);
  CHECK_EQ(usbmem_partedit_apply(h, good.c_str(), nullptr, 0, &res), 0);
  take(res);
  CHECK(slurp(img) != before);
  unsigned char* lb = nullptr;
  size_t lbl = 0;
  CHECK_EQ(usbmem_partedit_last_backup(h, &lb, &lbl), 0);  // 適用前の表が残っている
  usbmem_free(lb);

  usbmem_partedit_close(h);
  CHECK_EQ(usbmem_partedit_open(dev, 0, 0, 5, &h), 0);
  CHECK_EQ(usbmem_partedit_plan_restore(h, full.data(), full.size(), &pj), 0);
  plan = take(pj);
  phrase = json_field(plan, "confirmPhrase");
  CHECK_EQ(usbmem_partedit_make_token(h, phrase.c_str(), &tok), 0);
  good = take(tok);
  CHECK_EQ(usbmem_partedit_apply(h, good.c_str(), nullptr, 0, &res), 0);
  CHECK(take(res).find("\"ok\":true") != std::string::npos);
  CHECK(slurp(img) == before);

  usbmem_partedit_close(h);
  usbmem_close(dev);
  ::unlink(img.c_str());
  ::unlink(bk.c_str());
}

TEST(partedit_capi_safety_gates) {
  std::string img = make_image("gates", 8u << 20);
  usbmem_device* dev = nullptr;
  CHECK_EQ(usbmem_open_path(img.c_str(), 1, &dev), 0);  // 読み取り専用
  usbmem_partedit* h = nullptr;
  CHECK_EQ(usbmem_partedit_open(dev, 0, 0, 1, &h), 0);
  CHECK_EQ(usbmem_partedit_new_table(h, 1, 0, 0), 0);
  unsigned a = 0;
  CHECK_EQ(usbmem_partedit_create(h, 1, 2048, 2048, 0x83, nullptr, nullptr, nullptr, 0, 0, &a), 0);
  char* pj = nullptr;
  CHECK_EQ(usbmem_partedit_plan_json(h, 0, &pj), 0);
  std::string phrase = json_field(take(pj), "confirmPhrase");
  char* tok = nullptr;
  CHECK_EQ(usbmem_partedit_make_token(h, phrase.c_str(), &tok), 0);
  std::string t = take(tok);
  char* res = nullptr;
  CHECK(usbmem_partedit_apply(h, t.c_str(), nullptr, 0, &res) != 0);  // 読み取り専用デバイスは拒否
  take(res);
  CHECK(last_code(h) == std::string("device-read-only"));
  usbmem_partedit_close(h);
  usbmem_close(dev);

  // 範囲外・重なり・不正引数
  CHECK_EQ(usbmem_open_path(img.c_str(), 0, &dev), 0);
  CHECK_EQ(usbmem_partedit_open(dev, 0, 1, 1, &h), 0);  // in_use
  CHECK_EQ(usbmem_partedit_new_table(h, 1, 0, 0), 0);
  CHECK(usbmem_partedit_create(h, 1, 2048, 1ull << 40, 0x83, nullptr, nullptr, nullptr, 0, 0, &a) != 0);
  CHECK(usbmem_partedit_create(h, 1, 2048, 0, 0x83, nullptr, nullptr, nullptr, 0, 0, &a) == 0);
  CHECK(usbmem_partedit_create(h, 9, 0, 0, 0, nullptr, nullptr, nullptr, 0, 0, &a) != 0);
  CHECK(usbmem_partedit_set_gpt_type(h, a, "not-a-guid") != 0);
  CHECK(last_code(h) == std::string("bad-argument"));
  CHECK(usbmem_partedit_remove(h, 99) != 0);
  CHECK(usbmem_partedit_resize(nullptr, 1, 1) != 0);
  CHECK(usbmem_partedit_new_table(h, 7, 0, 0) != 0);
  CHECK_EQ(usbmem_partedit_plan_json(h, 0, &pj), 0);
  phrase = json_field(take(pj), "confirmPhrase");
  CHECK_EQ(usbmem_partedit_make_token(h, phrase.c_str(), &tok), 0);
  t = take(tok);
  CHECK(usbmem_partedit_apply(h, t.c_str(), nullptr, 0, &res) != 0);  // 使用中は拒否
  take(res);
  CHECK(last_code(h) == std::string("in-use"));
  usbmem_partedit_set_in_use(h, 0);
  CHECK_EQ(usbmem_partedit_apply(h, t.c_str(), "/nonexistent-dir/x.bak", 0, &res), static_cast<int>(usbmem::Errc::Io));  // バックアップを保存できなければ書かない
  take(res);
  CHECK(slurp(img) == std::vector<uint8_t>(8u << 20, 0));
  CHECK_EQ(usbmem_partedit_apply(h, t.c_str(), nullptr, 0, &res), 0);
  take(res);
  usbmem_partedit_close(h);
  usbmem_close(dev);
  ::unlink(img.c_str());
}
