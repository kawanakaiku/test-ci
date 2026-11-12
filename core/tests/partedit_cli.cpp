// SPDX-License-Identifier: Apache-2.0
// パーティション編集の検証用コマンドラインツール (テスト専用。製品には含めない)。
// tools/check_partedit_tools.py が、sfdisk / sgdisk / parted / fdisk / blkid との照合に使う。
//
//   usbmem_partedit_cli <image> [--sector N] info|inspect|labels|sfdisk [devname]
//   usbmem_partedit_cli <image> [--sector N] backup <file> | restore <file> [--force] [--dry]
//   usbmem_partedit_cli <image> [--sector N] [--dry] [--seed N] edit <op> [args] [+ <op> [args]]...
//     op: new-gpt [entries] | new-mbr | create k=v... | remove H | resize H N | move H N | type H T | name H S
//         attrs H N | boot H 0|1 | uuid H GUID | diskid ID | repair KIND
//     create の k=v: start count role=p|e|l type=HEX|GUID|esp|linux|msdata|swap name uuid attrs boot=1
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>

#include "usbmem/devices.h"
#include "usbmem/partition.h"
#include "usbmem/partition_edit.h"

using namespace usbmem;

namespace {

std::string json_escape(const std::string& s) {
  std::string o;
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += static_cast<char>(c); }
    else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
    else o += static_cast<char>(c);
  }
  return o;
}

std::array<uint8_t, 16> type_guid(const std::string& v) {
  std::array<uint8_t, 16> g{};
  if (v == "esp") parse_guid("C12A7328-F81F-11D2-BA4B-00A0C93EC93B", &g);
  else if (v == "linux") parse_guid("0FC63DAF-8483-4772-8E79-3D69D8477DE4", &g);
  else if (v == "msdata") parse_guid("EBD0A0A2-B9E5-4433-87C0-68B6B72699C7", &g);
  else if (v == "swap") parse_guid("0657FD6D-A4AB-43C4-84E5-0933C84B4F4F", &g);
  else if (v == "lvm") parse_guid("E6D6D379-F507-44C2-A23C-238F2A3DF928", &g);
  else parse_guid(v, &g);
  return g;
}

bool write_file(const std::string& path, const std::vector<uint8_t>& d) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f.write(reinterpret_cast<const char*>(d.data()), static_cast<std::streamsize>(d.size()));
  return f.good();
}
bool read_file(const std::string& path, std::vector<uint8_t>* d) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  d->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return true;
}

std::string result_json(const EditResult& r) {
  return "{\"ok\":" + std::string(r.ok() ? "true" : "false") + ",\"code\":\"" + json_escape(r.code) + "\",\"message\":\"" + json_escape(r.message) +
         "\",\"verified\":" + (r.verified ? "true" : "false") + ",\"rolledBack\":" + (r.rolled_back ? "true" : "false") +
         ",\"sectorsWritten\":" + std::to_string(r.sectors_written) + "}";
}

EditResult run_op(PartitionEditor& ed, const std::vector<std::string>& a, const PartitionTable& t) {
  const std::string& op = a[0];
  auto arg = [&](size_t i) { return i < a.size() ? a[i] : std::string(); };
  auto num = [&](size_t i) { return static_cast<uint64_t>(std::strtoull(arg(i).c_str(), nullptr, 0)); };
  bool gpt = t.kind == TableKind::Gpt;
  if (op == "new-gpt") return ed.new_table(TableKind::Gpt, a.size() > 1 ? static_cast<uint32_t>(num(1)) : 128, true);
  if (op == "new-mbr") return ed.new_table(TableKind::Mbr, 128, true);
  if (op == "create") {
    PartitionSpec s;
    bool is_gpt_target = gpt;
    for (size_t i = 1; i < a.size(); i++) {
      auto eq = a[i].find('=');
      std::string k = a[i].substr(0, eq), v = eq == std::string::npos ? "" : a[i].substr(eq + 1);
      if (k == "start") s.start = std::strtoull(v.c_str(), nullptr, 0);
      else if (k == "count") s.count = std::strtoull(v.c_str(), nullptr, 0);
      else if (k == "role") s.role = v == "p" ? PartRole::Primary : v == "e" ? PartRole::Extended : v == "l" ? PartRole::Logical : PartRole::Auto;
      else if (k == "type") { s.mbr_type = static_cast<uint8_t>(std::strtoul(v.c_str(), nullptr, 16)); s.type_guid = type_guid(v); }
      else if (k == "name") s.name = v;
      else if (k == "uuid") parse_guid(v, &s.unique_guid);
      else if (k == "attrs") s.attrs = std::strtoull(v.c_str(), nullptr, 0);
      else if (k == "boot") s.bootable = v != "0";
    }
    (void)is_gpt_target;
    uint32_t h = 0;
    EditResult r = ed.create(s, &h);
    if (r.ok()) r.message = std::to_string(h);
    return r;
  }
  uint32_t h = static_cast<uint32_t>(num(1));
  if (op == "remove") return ed.remove(h);
  if (op == "resize") return ed.resize(h, num(2));
  if (op == "move") return ed.move(h, num(2));
  if (op == "type") return gpt ? ed.set_gpt_type(h, type_guid(arg(2))) : ed.set_mbr_type(h, static_cast<uint8_t>(std::strtoul(arg(2).c_str(), nullptr, 16)));
  if (op == "name") return ed.set_name(h, arg(2));
  if (op == "attrs") return ed.set_attrs(h, num(2));
  if (op == "boot") return ed.set_bootable(h, arg(2) != "0");
  if (op == "uuid") { std::array<uint8_t, 16> g{}; parse_guid(arg(2), &g); return ed.set_unique_guid(h, g); }
  if (op == "diskid") return ed.set_disk_id(arg(1));
  if (op == "repair") {
    std::string k = arg(1);
    GptRepairKind kind = k == "copy-primary-to-backup" ? GptRepairKind::CopyPrimaryToBackup
                         : k == "copy-backup-to-primary" ? GptRepairKind::CopyBackupToPrimary
                         : k == "move-backup-to-end" ? GptRepairKind::MoveBackupToEnd
                                                     : GptRepairKind::RebuildProtectiveMbr;
    return ed.repair_gpt(kind);
  }
  EditResult r;
  r.status = Status::Make(Errc::InvalidArgument);
  r.code = "cli-unknown-op";
  r.message = op;
  return r;
}

int apply_plan(PartitionEditor& ed, const EditPlan& plan, bool dry, bool force_ok, const std::string& backup_out) {
  std::printf("{\"plan\":%s", edit_plan_to_json(plan).c_str());
  if (dry || !plan.ok) { std::printf("}\n"); return plan.ok ? 0 : 3; }
  if (plan.requires_force && !force_ok) { std::printf(",\"result\":{\"ok\":false,\"code\":\"force-required\"}}\n"); return 4; }
  ApplyOptions ao;
  ao.timestamp = 1700000000;
  ao.backup_sink = [&](const std::vector<uint8_t>& b) {
    if (backup_out.empty()) return Status::Ok();
    return write_file(backup_out, b) ? Status::Ok() : Status::Make(Errc::Io, 5);
  };
  EditResult r = ed.apply(plan, PartitionEditor::make_confirm_token(plan, plan.confirm_phrase), ao);
  std::printf(",\"result\":%s}\n", result_json(r).c_str());
  return r.ok() ? 0 : 2;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) { std::fprintf(stderr, "usage: %s <image> [options] <command>\n", argv[0]); return 64; }
  std::string image = argv[1];
  uint32_t ss = 0;
  bool dry = false, force = false;
  uint64_t seed = 0;
  std::string pre_backup;
  std::vector<std::string> rest;
  for (int i = 2; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--sector" && i + 1 < argc) ss = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 0));
    else if (a == "--dry") dry = true;
    else if (a == "--force") force = true;
    else if (a == "--seed" && i + 1 < argc) seed = std::strtoull(argv[++i], nullptr, 0);
    else if (a == "--pre-backup" && i + 1 < argc) pre_backup = argv[++i];
    else rest.push_back(a);
  }
  if (rest.empty()) return 64;
  const std::string cmd = rest[0];
  bool read_only = cmd == "info" || cmd == "inspect" || cmd == "labels" || cmd == "sfdisk" || cmd == "backup" || dry;
  BlockDevicePtr dev;
  Status st = RawImageDevice::open_path(image, read_only, ss ? ss : 512, &dev);
  if (!st.ok()) { std::fprintf(stderr, "open failed: %d\n", static_cast<int>(st.code)); return 66; }

  if (cmd == "info" || cmd == "labels") {
    PartitionTable t;
    ParseOptions po;
    po.sector_size = ss;
    st = read_partition_table(*dev, &t, po);
    if (!st.ok()) return 65;
    std::printf("%s\n", partition_table_to_json(t).c_str());
    return 0;
  }
  EditorOptions eo;
  eo.sector_size = ss;
  eo.rng_seed = seed;
  std::unique_ptr<PartitionEditor> ed;
  EditResult r = PartitionEditor::open(dev, eo, &ed);
  if (!r.ok()) { std::printf("%s\n", result_json(r).c_str()); return 2; }

  if (cmd == "inspect") { std::printf("%s\n", gpt_inspection_to_json(ed->gpt_inspection()).c_str()); return 0; }
  if (cmd == "sfdisk") {
    std::string out;
    r = ed->export_sfdisk(&out, rest.size() > 1 ? rest[1] : "/dev/loop0");
    if (!r.ok()) { std::fprintf(stderr, "%s\n", r.code.c_str()); return 2; }
    std::fputs(out.c_str(), stdout);
    return 0;
  }
  if (cmd == "backup") {
    std::vector<uint8_t> b;
    r = ed->backup(1700000000, &b);
    if (!r.ok() || rest.size() < 2 || !write_file(rest[1], b)) return 2;
    return 0;
  }
  if (cmd == "restore") {
    std::vector<uint8_t> b;
    if (rest.size() < 2 || !read_file(rest[1], &b)) return 66;
    EditPlan plan;
    r = ed->plan_restore(b, &plan);
    if (!r.ok()) { std::printf("%s\n", result_json(r).c_str()); return 2; }
    return apply_plan(*ed, plan, dry, force, pre_backup);
  }
  if (cmd == "edit") {
    std::vector<std::string> cur;
    std::vector<std::vector<std::string>> ops;
    for (size_t i = 1; i < rest.size(); i++) {
      if (rest[i] == "+") { if (!cur.empty()) ops.push_back(cur); cur.clear(); }
      else cur.push_back(rest[i]);
    }
    if (!cur.empty()) ops.push_back(cur);
    PartitionTable t = ed->table();
    for (auto& op : ops) {
      r = run_op(*ed, op, t);
      if (!r.ok()) { std::printf("{\"opError\":%s,\"op\":\"%s\"}\n", result_json(r).c_str(), json_escape(op[0]).c_str()); return 2; }
      if (op[0] == "new-gpt") t.kind = TableKind::Gpt;
      if (op[0] == "new-mbr") t.kind = TableKind::Mbr;
    }
    EditPlan plan;
    r = ed->plan(&plan);
    if (!r.ok()) { std::printf("%s\n", result_json(r).c_str()); return 2; }
    return apply_plan(*ed, plan, dry, force, pre_backup);
  }
  std::fprintf(stderr, "unknown command\n");
  return 64;
}
