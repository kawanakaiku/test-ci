// SPDX-License-Identifier: Apache-2.0
// パーティション探索 / ブートセクタ修復の検証用コマンドラインツール (テスト専用。製品には含めない)。
// tools/check_partscan_tools.py が、sfdisk / mkfs.* / fsck.* / ntfsfix / testdisk との照合に使う。出力は JSON (標準出力)。
//
//   usbmem_partscan_cli <image> [--sector N] scan [--full] [--min-conf N]
//   usbmem_partscan_cli <image> [--sector N] proposals [--full]
//   usbmem_partscan_cli <image> [--sector N] apply-proposal <id> [--backup FILE] [--full]     (イメージへ書く)
//   usbmem_partscan_cli <image> boot-inspect [--fs F] [--offset O] [--length L]
//   usbmem_partscan_cli <image> boot-repair <repair-id> [--source N] [--fs F] [--offset O] [--length L] [--backup FILE] [--dry]   (イメージへ書く)
//   usbmem_partscan_cli <image> boot-restore <backup-file>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

#include "usbmem/bootfix.h"
#include "usbmem/devices.h"
#include "usbmem/partition_recover.h"
#include "usbmem/partition_scan.h"

using namespace usbmem;

namespace {

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

FsType parse_fs(const std::string& n) {
  if (n == "fat12") return FsType::Fat12;
  if (n == "fat16") return FsType::Fat16;
  if (n == "fat32") return FsType::Fat32;
  if (n == "exfat") return FsType::Exfat;
  if (n == "ext2") return FsType::Ext2;
  if (n == "ext3") return FsType::Ext3;
  if (n == "ext4") return FsType::Ext4;
  if (n == "ntfs") return FsType::Ntfs;
  return FsType::Unknown;
}

std::string esc(const std::string& s) {
  std::string o;
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += static_cast<char>(c); }
    else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
    else o += static_cast<char>(c);
  }
  return o;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) { std::fprintf(stderr, "usage: %s <image> [--sector N] <command> ...\n", argv[0]); return 64; }
  std::string image = argv[1];
  uint32_t ss = 0;
  std::string cmd, arg1, backup, fs_name;
  bool full = false, dry = false;
  int min_conf = 0;
  uint64_t offset = 0, length = 0, source = 0;
  for (int i = 2; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--sector") ss = static_cast<uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
    else if (a == "--full") full = true;
    else if (a == "--dry") dry = true;
    else if (a == "--min-conf") min_conf = std::atoi(next().c_str());
    else if (a == "--backup") backup = next();
    else if (a == "--fs") fs_name = next();
    else if (a == "--offset") offset = std::strtoull(next().c_str(), nullptr, 10);
    else if (a == "--length") length = std::strtoull(next().c_str(), nullptr, 10);
    else if (a == "--source") source = std::strtoull(next().c_str(), nullptr, 10);
    else if (cmd.empty()) cmd = a;
    else if (arg1.empty()) arg1 = a;
  }
  bool writes = cmd == "apply-proposal" || (cmd == "boot-repair" && !dry) || cmd == "boot-restore";
  BlockDevicePtr dev;
  Status st = RawImageDevice::open_path(image, !writes, ss ? ss : 512, &dev);
  if (!st.ok()) { std::fprintf(stderr, "open failed: %d\n", static_cast<int>(st.code)); return 66; }

  if (cmd == "scan" || cmd == "proposals" || cmd == "apply-proposal") {
    ScanOptions so;
    so.sector_size = ss;
    so.level = full ? ScanLevel::Full : ScanLevel::Quick;
    if (min_conf) so.min_confidence = min_conf;
    ScanResult r;
    st = scan_partitions(*dev, so, &r);
    if (!st.ok()) { std::fprintf(stderr, "scan failed: %d\n", static_cast<int>(st.code)); return 1; }
    if (cmd == "scan") { std::printf("%s\n", scan_result_to_json(r).c_str()); return 0; }
    std::vector<RecoveryProposal> props;
    st = propose_table_recovery(*dev, r, RecoveryOptions{}, &props);
    if (!st.ok()) { std::fprintf(stderr, "propose failed: %d\n", static_cast<int>(st.code)); return 1; }
    if (cmd == "proposals") { std::printf("%s\n", recovery_proposals_to_json(props).c_str()); return 0; }
    const RecoveryProposal* p = nullptr;
    for (const auto& x : props) if (x.id == arg1) p = &x;
    if (!p) { std::printf("{\"ok\":false,\"code\":\"no-such-proposal\"}\n"); return 2; }
    EditorOptions eo;
    eo.sector_size = ss;
    eo.rng_seed = 1;
    std::unique_ptr<PartitionEditor> ed;
    EditResult er = PartitionEditor::open(dev, eo, &ed);
    if (er.ok()) er = queue_recovery_proposal(*ed, *p);
    EditPlan plan;
    if (er.ok()) er = ed->plan(&plan);
    if (er.ok() && !plan.ok) { er.status = Status::Make(Errc::InvalidArgument); er.code = "plan-invalid"; }
    if (er.ok()) {
      ApplyOptions ao;
      ao.backup_sink = [&](const std::vector<uint8_t>& b) { return backup.empty() || write_file(backup, b) ? Status::Ok() : Status::Make(Errc::Io, 5); };
      er = ed->apply(plan, PartitionEditor::make_confirm_token(plan, plan.confirm_phrase), ao);
    }
    std::printf("{\"ok\":%s,\"code\":\"%s\",\"message\":\"%s\",\"verified\":%s}\n", er.ok() ? "true" : "false", esc(er.code).c_str(), esc(er.message).c_str(), er.verified ? "true" : "false");
    return er.ok() ? 0 : 3;
  }

  if (cmd == "boot-inspect") {
    BootInspection r;
    st = inspect_boot(*dev, offset, length, parse_fs(fs_name), BootInspectOptions{}, &r);
    if (!st.ok()) { std::fprintf(stderr, "inspect failed: %d\n", static_cast<int>(st.code)); return 1; }
    std::printf("%s\n", boot_inspection_to_json(r).c_str());
    return 0;
  }

  if (cmd == "boot-repair") {
    BootRepairPlan plan;
    st = plan_boot_repair(*dev, offset, length, parse_fs(fs_name), arg1, source, &plan);
    if (!st.ok()) { std::fprintf(stderr, "plan failed: %d\n", static_cast<int>(st.code)); return 1; }
    if (dry || !plan.ok) { std::printf("%s\n", boot_plan_to_json(plan).c_str()); return plan.ok ? 0 : 2; }
    BootApplyOptions ao;
    ao.timestamp = 1700000000;
    ao.backup_sink = [&](const std::vector<uint8_t>& b) { return backup.empty() || write_file(backup, b) ? Status::Ok() : Status::Make(Errc::Io, 5); };
    BootApplyResult r = apply_boot_repair(*dev, plan, make_boot_confirm_token(plan, plan.confirm_phrase), ao);
    std::printf("{\"ok\":%s,\"code\":\"%s\",\"message\":\"%s\",\"writesDone\":%zu,\"verified\":%s,\"rolledBack\":%s}\n", r.ok() ? "true" : "false", esc(r.code).c_str(), esc(r.message).c_str(),
                r.writes_done, r.verified ? "true" : "false", r.rolled_back ? "true" : "false");
    return r.ok() ? 0 : 3;
  }

  if (cmd == "boot-restore") {
    std::vector<uint8_t> data;
    if (!read_file(arg1, &data)) { std::fprintf(stderr, "cannot read %s\n", arg1.c_str()); return 66; }
    st = restore_boot_backup(*dev, data);
    std::printf("{\"ok\":%s}\n", st.ok() ? "true" : "false");
    return st.ok() ? 0 : 3;
  }
  std::fprintf(stderr, "unknown command: %s\n", cmd.c_str());
  return 64;
}
