// SPDX-License-Identifier: Apache-2.0
#include "usbmem/c_api_partedit.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>

#include "usbmem/c_api_internal.h"
#include "usbmem/partition_edit.h"

using namespace usbmem;

struct usbmem_partedit {
  std::unique_ptr<PartitionEditor> ed;
  EditPlan plan;
  bool has_plan = false;
  std::vector<uint8_t> last_backup;
  std::string err_code, err_msg;
  std::mutex mu;
};

namespace {

thread_local std::string t_open_code, t_open_msg;

int to_rc(const EditResult& r) { return static_cast<int>(r.status.code); }

int finish(usbmem_partedit* h, const EditResult& r) {
  h->err_code = r.code;
  h->err_msg = r.message;
  return to_rc(r);
}

// 操作が成功したら計画は無効になる (確認トークンは計画の内容に紐づくため)
int op_result(usbmem_partedit* h, const EditResult& r) {
  if (r.ok()) h->has_plan = false;
  return finish(h, r);
}

int dup_string(const std::string& s, char** out) {
  char* p = static_cast<char*>(std::malloc(s.size() + 1));
  if (!p) return static_cast<int>(Errc::NoMemory);
  std::memcpy(p, s.c_str(), s.size() + 1);
  *out = p;
  return 0;
}

int dup_bytes(const std::vector<uint8_t>& v, unsigned char** out, size_t* len) {
  unsigned char* p = static_cast<unsigned char*>(std::malloc(v.empty() ? 1 : v.size()));
  if (!p) return static_cast<int>(Errc::NoMemory);
  if (!v.empty()) std::memcpy(p, v.data(), v.size());
  *out = p;
  *len = v.size();
  return 0;
}

bool guid_arg(const char* s, std::array<uint8_t, 16>* g) {
  if (!s || !*s) return true;  // 未指定 (全 0)
  return parse_guid(s, g);
}

void copy_out(const std::string& s, char* dst, size_t n) {
  if (!dst || n == 0) return;
  size_t k = s.size() < n - 1 ? s.size() : n - 1;
  std::memcpy(dst, s.data(), k);
  dst[k] = 0;
}

EditResult bad_arg(const char* what) {
  EditResult r;
  r.status = Status::Make(Errc::InvalidArgument);
  r.code = "bad-argument";
  r.message = what;
  return r;
}

// バックアップをファイルへ書く (fsync まで)。失敗したら適用を止める
Status write_backup_file(const std::string& path, const std::vector<uint8_t>& data) {
  int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) return Status::Make(Errc::Io, errno);
  size_t off = 0;
  while (off < data.size()) {
    ssize_t n = ::write(fd, data.data() + off, data.size() - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      int e = errno;
      ::close(fd);
      return Status::Make(Errc::Io, e);
    }
    off += static_cast<size_t>(n);
  }
  if (::fsync(fd) != 0) {
    int e = errno;
    ::close(fd);
    return Status::Make(Errc::Io, e);
  }
  if (::close(fd) != 0) return Status::Make(Errc::Io, errno);
  return Status::Ok();
}

std::string json_str(const std::string& s) {
  std::string o = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += static_cast<char>(c); }
    else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
    else o += static_cast<char>(c);
  }
  return o + "\"";
}

}  // namespace

extern "C" {

int usbmem_partedit_open(usbmem_device* dev, unsigned sector_size, int in_use, unsigned long long rng_seed, usbmem_partedit** out) {
  if (!dev || !out) return static_cast<int>(Errc::InvalidArgument);
  *out = nullptr;
  try {
    auto* h = new (std::nothrow) usbmem_partedit();
    if (!h) return static_cast<int>(Errc::NoMemory);
    EditorOptions o;
    o.sector_size = sector_size;
    o.in_use = in_use != 0;
    o.rng_seed = rng_seed;
    EditResult r = PartitionEditor::open(dev->dev, o, &h->ed);
    if (!r.ok()) {
      t_open_code = r.code;
      t_open_msg = r.message;
      delete h;
      return to_rc(r);
    }
    *out = h;
    return 0;
  } catch (...) { return static_cast<int>(Errc::NoMemory); }
}

void usbmem_partedit_close(usbmem_partedit* h) { delete h; }

void usbmem_partedit_set_in_use(usbmem_partedit* h, int in_use) {
  if (h) h->ed->set_in_use(in_use != 0);
}

void usbmem_partedit_last_error(usbmem_partedit* h, char* code, size_t code_len, char* msg, size_t msg_len) {
  copy_out(h ? h->err_code : t_open_code, code, code_len);
  copy_out(h ? h->err_msg : t_open_msg, msg, msg_len);
}

int usbmem_partedit_table_json(usbmem_partedit* h, char** out) {
  if (!h || !out) return static_cast<int>(Errc::InvalidArgument);
  *out = nullptr;
  try { return dup_string(partition_table_to_json(h->ed->table()), out); } catch (...) { return static_cast<int>(Errc::NoMemory); }
}

int usbmem_partedit_gpt_json(usbmem_partedit* h, char** out) {
  if (!h || !out) return static_cast<int>(Errc::InvalidArgument);
  *out = nullptr;
  try { return dup_string(gpt_inspection_to_json(h->ed->gpt_inspection()), out); } catch (...) { return static_cast<int>(Errc::NoMemory); }
}

int usbmem_partedit_create(usbmem_partedit* h, int role, unsigned long long start, unsigned long long count, int mbr_type,
                           const char* type_guid, const char* unique_guid, const char* name, unsigned long long attrs, int bootable,
                           unsigned* out_handle) {
  if (!h) return static_cast<int>(Errc::InvalidArgument);
  try {
    PartitionSpec s;
    if (role < 0 || role > 3 || mbr_type < 0 || mbr_type > 255) return finish(h, bad_arg("role or MBR type out of range"));
    s.role = static_cast<PartRole>(role);
    s.start = start;
    s.count = count;
    s.mbr_type = static_cast<uint8_t>(mbr_type);
    if (!guid_arg(type_guid, &s.type_guid) || !guid_arg(unique_guid, &s.unique_guid)) return finish(h, bad_arg("not a GUID"));
    if (name) s.name = name;
    s.attrs = attrs;
    s.bootable = bootable != 0;
    uint32_t handle = 0;
    EditResult r = h->ed->create(s, &handle);
    if (r.ok() && out_handle) *out_handle = handle;
    return op_result(h, r);
  } catch (...) { return static_cast<int>(Errc::NoMemory); }
}

#define SIMPLE_OP(call) \
  if (!h) return static_cast<int>(Errc::InvalidArgument); \
  try { return op_result(h, call); } catch (...) { return static_cast<int>(Errc::NoMemory); }

int usbmem_partedit_remove(usbmem_partedit* h, unsigned handle) { SIMPLE_OP(h->ed->remove(handle)) }
int usbmem_partedit_resize(usbmem_partedit* h, unsigned handle, unsigned long long n) { SIMPLE_OP(h->ed->resize(handle, n)) }
int usbmem_partedit_move(usbmem_partedit* h, unsigned handle, unsigned long long s) { SIMPLE_OP(h->ed->move(handle, s)) }
int usbmem_partedit_set_mbr_type(usbmem_partedit* h, unsigned handle, int type) {
  if (type < 0 || type > 255) return h ? finish(h, bad_arg("type out of range")) : static_cast<int>(Errc::InvalidArgument);
  SIMPLE_OP(h->ed->set_mbr_type(handle, static_cast<uint8_t>(type)))
}
int usbmem_partedit_set_gpt_type(usbmem_partedit* h, unsigned handle, const char* guid) {
  std::array<uint8_t, 16> g{};
  if (!guid || !parse_guid(guid, &g)) return h ? finish(h, bad_arg("not a GUID")) : static_cast<int>(Errc::InvalidArgument);
  SIMPLE_OP(h->ed->set_gpt_type(handle, g))
}
int usbmem_partedit_set_name(usbmem_partedit* h, unsigned handle, const char* utf8) {
  if (!utf8) return h ? finish(h, bad_arg("null name")) : static_cast<int>(Errc::InvalidArgument);
  SIMPLE_OP(h->ed->set_name(handle, utf8))
}
int usbmem_partedit_set_attrs(usbmem_partedit* h, unsigned handle, unsigned long long a) { SIMPLE_OP(h->ed->set_attrs(handle, a)) }
int usbmem_partedit_set_bootable(usbmem_partedit* h, unsigned handle, int b) { SIMPLE_OP(h->ed->set_bootable(handle, b != 0)) }
int usbmem_partedit_set_unique_guid(usbmem_partedit* h, unsigned handle, const char* guid) {
  std::array<uint8_t, 16> g{};
  if (!guid || !parse_guid(guid, &g)) return h ? finish(h, bad_arg("not a GUID")) : static_cast<int>(Errc::InvalidArgument);
  SIMPLE_OP(h->ed->set_unique_guid(handle, g))
}
int usbmem_partedit_set_disk_id(usbmem_partedit* h, const char* id) {
  if (!id) return h ? finish(h, bad_arg("null id")) : static_cast<int>(Errc::InvalidArgument);
  SIMPLE_OP(h->ed->set_disk_id(id))
}
int usbmem_partedit_new_table(usbmem_partedit* h, int kind, unsigned gpt_entries, int overwrite_fs) {
  if (kind != 1 && kind != 2) return h ? finish(h, bad_arg("kind must be 1 (MBR) or 2 (GPT)")) : static_cast<int>(Errc::InvalidArgument);
  SIMPLE_OP(h->ed->new_table(kind == 2 ? TableKind::Gpt : TableKind::Mbr, gpt_entries ? gpt_entries : 128, overwrite_fs != 0))
}
int usbmem_partedit_repair_gpt(usbmem_partedit* h, int kind) {
  if (kind < 1 || kind > 4) return h ? finish(h, bad_arg("unknown repair")) : static_cast<int>(Errc::InvalidArgument);
  SIMPLE_OP(h->ed->repair_gpt(static_cast<GptRepairKind>(kind)))
}

void usbmem_partedit_clear_ops(usbmem_partedit* h) {
  if (!h) return;
  h->ed->clear_ops();
  h->has_plan = false;
}

int usbmem_partedit_plan_json(usbmem_partedit* h, int include_sectors, char** out) {
  if (!h || !out) return static_cast<int>(Errc::InvalidArgument);
  *out = nullptr;
  try {
    EditResult r = h->ed->plan(&h->plan);
    h->has_plan = r.ok();
    if (!r.ok()) return finish(h, r);
    return dup_string(edit_plan_to_json(h->plan, include_sectors != 0), out);
  } catch (...) { return static_cast<int>(Errc::NoMemory); }
}

int usbmem_partedit_make_token(usbmem_partedit* h, const char* typed, char** out) {
  if (!h || !typed || !out) return static_cast<int>(Errc::InvalidArgument);
  *out = nullptr;
  if (!h->has_plan) return finish(h, [] { EditResult r; r.status = Status::Make(Errc::InvalidArgument); r.code = "no-plan"; r.message = "make a plan first"; return r; }());
  try { return dup_string(PartitionEditor::make_confirm_token(h->plan, typed), out); } catch (...) { return static_cast<int>(Errc::NoMemory); }
}

int usbmem_partedit_apply(usbmem_partedit* h, const char* token, const char* backup_path, unsigned long long timestamp, char** out) {
  if (!h || !token || !out) return static_cast<int>(Errc::InvalidArgument);
  *out = nullptr;
  try {
    EditResult r;
    if (!h->has_plan) {
      r.status = Status::Make(Errc::InvalidArgument);
      r.code = "no-plan";
      r.message = "make a plan first";
    } else {
      ApplyOptions ao;
      ao.timestamp = timestamp;
      std::string path = backup_path ? backup_path : "";
      ao.backup_sink = [h, path](const std::vector<uint8_t>& b) {
        h->last_backup = b;
        return path.empty() ? Status::Ok() : write_backup_file(path, b);
      };
      r = h->ed->apply(h->plan, token, ao);
      if (r.ok() || r.code == "write-failed" || r.code == "flush-failed" || r.code == "verify-failed") h->has_plan = false;
    }
    h->err_code = r.code;
    h->err_msg = r.message;
    std::string j = "{\"ok\":" + std::string(r.ok() ? "true" : "false") + ",\"code\":" + json_str(r.code) + ",\"message\":" + json_str(r.message) +
                    ",\"sectorsWritten\":" + std::to_string(r.sectors_written) + ",\"rolledBack\":" + (r.rolled_back ? "true" : "false") +
                    ",\"verified\":" + (r.verified ? "true" : "false") + "}";
    int dr = dup_string(j, out);
    return dr ? dr : to_rc(r);
  } catch (...) { return static_cast<int>(Errc::NoMemory); }
}

int usbmem_partedit_backup(usbmem_partedit* h, unsigned long long timestamp, unsigned char** out, size_t* len) {
  if (!h || !out || !len) return static_cast<int>(Errc::InvalidArgument);
  *out = nullptr;
  try {
    std::vector<uint8_t> b;
    EditResult r = h->ed->backup(timestamp, &b);
    if (!r.ok()) return finish(h, r);
    return dup_bytes(b, out, len);
  } catch (...) { return static_cast<int>(Errc::NoMemory); }
}

int usbmem_partedit_last_backup(usbmem_partedit* h, unsigned char** out, size_t* len) {
  if (!h || !out || !len) return static_cast<int>(Errc::InvalidArgument);
  *out = nullptr;
  if (h->last_backup.empty()) return static_cast<int>(Errc::InvalidArgument);
  try { return dup_bytes(h->last_backup, out, len); } catch (...) { return static_cast<int>(Errc::NoMemory); }
}

int usbmem_partedit_backup_info_json(const unsigned char* data, size_t len, char** out) {
  if (!data || !out) return static_cast<int>(Errc::InvalidArgument);
  *out = nullptr;
  try {
    BackupInfo bi;
    Status st = parse_table_backup(std::vector<uint8_t>(data, data + len), &bi);
    if (!st.ok()) return static_cast<int>(st.code);
    const char* kinds[] = {"none", "mbr", "gpt"};
    std::string j = "{\"version\":" + std::to_string(bi.version) + ",\"sectorSize\":" + std::to_string(bi.sector_size) + ",\"tableKind\":\"" +
                    kinds[bi.table_kind > 2 ? 0 : bi.table_kind] + "\",\"diskBytes\":\"" + std::to_string(bi.disk_bytes) + "\",\"timestamp\":\"" +
                    std::to_string(bi.timestamp) + "\",\"diskId\":" + json_str(bi.disk_id) + ",\"sectorCount\":" + std::to_string(bi.sector_count) + "}";
    return dup_string(j, out);
  } catch (...) { return static_cast<int>(Errc::NoMemory); }
}

int usbmem_partedit_plan_restore(usbmem_partedit* h, const unsigned char* data, size_t len, char** out) {
  if (!h || !data || !out) return static_cast<int>(Errc::InvalidArgument);
  *out = nullptr;
  try {
    EditResult r = h->ed->plan_restore(std::vector<uint8_t>(data, data + len), &h->plan);
    h->has_plan = r.ok();
    if (!r.ok()) return finish(h, r);
    return dup_string(edit_plan_to_json(h->plan, false), out);
  } catch (...) { return static_cast<int>(Errc::NoMemory); }
}

int usbmem_partedit_export_sfdisk(usbmem_partedit* h, const char* device_name, char** out) {
  if (!h || !out) return static_cast<int>(Errc::InvalidArgument);
  *out = nullptr;
  try {
    std::string t;
    EditResult r = h->ed->export_sfdisk(&t, device_name ? device_name : "/dev/disk");
    if (!r.ok()) return finish(h, r);
    return dup_string(t, out);
  } catch (...) { return static_cast<int>(Errc::NoMemory); }
}

}  // extern "C"
