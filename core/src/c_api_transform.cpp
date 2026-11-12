// SPDX-License-Identifier: Apache-2.0
// 暗号ボリューム (BitLocker / LUKS) の C 境界。資格情報は Secret にコピーして使い、戻る前にゼロ化される (Credentials の破棄)。
#include "usbmem/c_api_transform.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>

#include "usbmem/c_api_internal.h"
#include "usbmem/transform.h"

using namespace usbmem;

namespace {
void json_str(std::string& o, const std::string& s) {
  o += '"';
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += char(c); }
    else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
    else o += char(c);
  }
  o += '"';
}
std::string info_json(const VolumeInfo& i) {
  std::string o = "{\"kind\":" + std::to_string(int(i.kind)) + ",\"format\":";
  json_str(o, i.format);
  o += ",\"cipher\":";
  json_str(o, i.cipher);
  auto b = [&](const char* k, bool v) { o += std::string(",\"") + k + "\":" + (v ? "true" : "false"); };
  b("supported", i.supported); b("password", i.can_password); b("recovery", i.can_recovery_key); b("key_file", i.can_key_file);
  b("no_secret", i.unlocked_without_secret); b("tpm_only", i.tpm_only); b("in_progress", i.encryption_in_progress);
  o += ",\"note\":";
  json_str(o, i.note);
  o += "}";
  return o;
}
int emit(const std::string& j, char** out) {
  char* p = static_cast<char*>(std::malloc(j.size() + 1));
  if (!p) return static_cast<int>(Errc::NoMemory);
  std::memcpy(p, j.c_str(), j.size() + 1);
  *out = p;
  return 0;
}
}  // namespace

extern "C" {

int usbmem_transform_probe(usbmem_device* h, char** out_json) {
  if (!h || !h->dev || !out_json) return static_cast<int>(Errc::InvalidArgument);
  *out_json = nullptr;
  try {
    VolumeInfo info;
    Status st = probe_crypto_volume(*h->dev, &info);
    if (!st.ok()) return static_cast<int>(st.code);
    return emit(info_json(info), out_json);
  } catch (...) { return static_cast<int>(Errc::NoMemory); }
}

int usbmem_transform_open(usbmem_device* h, int kind, const unsigned char* pw, size_t pw_len, const unsigned char* rk, size_t rk_len,
                          const unsigned char* kf, size_t kf_len, unsigned flags, usbmem_device** out, char** out_info_json) {
  if (out_info_json) *out_info_json = nullptr;
  if (!h || !h->dev || !out) return static_cast<int>(Errc::InvalidArgument);
  *out = nullptr;
  if (kind < 0 || kind > 2 || (pw_len && !pw) || (rk_len && !rk) || (kf_len && !kf)) return static_cast<int>(Errc::InvalidArgument);
  if (pw_len > (1u << 16) || rk_len > 1024 || kf_len > (1u << 20)) return static_cast<int>(Errc::InvalidArgument);
  try {
    Credentials cred;  // 破棄時にゼロ化される
    if (pw_len) cred.password.assign(pw, pw_len);
    if (rk_len) cred.recovery_key.assign(rk, rk_len);
    if (kf_len) cred.key_file.assign(kf, kf_len);
    OpenOptions opt;
    opt.allow_partial_encryption = (flags & USBMEM_TRANSFORM_ALLOW_PARTIAL) != 0;
    BlockDevicePtr d;
    VolumeInfo info;
    Status st = open_crypto_volume(h->dev, static_cast<CryptoKind>(kind), cred, opt, &d, &info);
    if (out_info_json && (st.ok() || !info.format.empty())) emit(info_json(info), out_info_json);
    if (!st.ok()) return static_cast<int>(st.code);
    auto* w = new (std::nothrow) usbmem_device{std::move(d)};
    if (!w) return static_cast<int>(Errc::NoMemory);
    *out = w;
    return 0;
  } catch (...) { return static_cast<int>(Errc::NoMemory); }
}

}  // extern "C"
