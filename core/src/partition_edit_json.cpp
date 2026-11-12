// SPDX-License-Identifier: Apache-2.0
// 計画 / GPT 検査結果の JSON 出力 (C API / JNI 用)。64 ビット値は文字列で出す (JavaScript 系の 53 ビット制限対策)。
#include <cstdio>

#include "partition_edit_internal.h"
#include "usbmem/sha256.h"

namespace usbmem {
namespace {

void js(std::string& o, const std::string& s) {
  o += '"';
  for (unsigned char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += static_cast<char>(c);
    }
  }
  o += '"';
}
const char* sev(Severity s) { return s == Severity::Error ? "error" : s == Severity::Warning ? "warning" : "info"; }
std::string u64(uint64_t v) { return "\"" + std::to_string(v) + "\""; }
std::string boolean(bool b) { return b ? "true" : "false"; }

void diags(std::string& o, const std::vector<Diagnostic>& v) {
  o += '[';
  for (size_t i = 0; i < v.size(); i++) {
    if (i) o += ',';
    o += "{\"severity\":"; js(o, sev(v[i].severity));
    o += ",\"code\":"; js(o, v[i].code);
    o += ",\"message\":"; js(o, v[i].message);
    o += '}';
  }
  o += ']';
}

void copy_json(std::string& o, const GptCopyInfo& c) {
  o += "{\"present\":" + boolean(c.present) + ",\"headerValid\":" + boolean(c.header_valid) + ",\"entriesValid\":" + boolean(c.entries_valid);
  o += ",\"reason\":"; js(o, c.reason);
  o += ",\"lba\":" + u64(c.lba) + ",\"altLba\":" + u64(c.alt_lba) + ",\"firstUsable\":" + u64(c.first_usable) +
       ",\"lastUsable\":" + u64(c.last_usable) + ",\"entriesLba\":" + u64(c.entries_lba);
  o += ",\"numEntries\":" + std::to_string(c.num_entries) + ",\"entrySize\":" + std::to_string(c.entry_size);
  o += ",\"diskGuid\":"; js(o, c.header_valid ? guid_to_string(c.disk_guid) : "");
  o += '}';
}

}  // namespace

std::string gpt_inspection_to_json(const GptInspection& g) {
  std::string o = "{\"sectorSize\":" + std::to_string(g.sector_size) + ",\"diskSectors\":" + u64(g.disk_sectors);
  o += ",\"primary\":"; copy_json(o, g.primary);
  o += ",\"backup\":"; copy_json(o, g.backup);
  o += ",\"backupAtEnd\":" + boolean(g.backup_at_end) + ",\"diskGrew\":" + boolean(g.disk_grew) +
       ",\"backupBeyondDisk\":" + boolean(g.backup_beyond_disk) + ",\"copiesAgree\":" + boolean(g.copies_agree) +
       ",\"mbrPresent\":" + boolean(g.mbr_present) + ",\"mbrProtective\":" + boolean(g.mbr_protective) +
       ",\"mbrProtectiveOk\":" + boolean(g.mbr_protective_ok) + ",\"mbrHybrid\":" + boolean(g.mbr_hybrid);
  o += ",\"diagnostics\":"; diags(o, g.diagnostics);
  o += ",\"suggestedRepairs\":[";
  for (size_t i = 0; i < g.suggested_repairs.size(); i++) {
    if (i) o += ',';
    js(o, gpt_repair_name(g.suggested_repairs[i]));
  }
  o += "]}";
  return o;
}

std::string edit_plan_to_json(const EditPlan& p, bool include_sectors) {
  std::string o = "{\"ok\":" + boolean(p.ok) + ",\"destructive\":" + boolean(p.destructive) + ",\"requiresForce\":" + boolean(p.requires_force) +
                  ",\"sectorSize\":" + std::to_string(p.sector_size);
  o += ",\"deviceId\":"; js(o, p.device_id);
  o += ",\"planHash\":"; js(o, p.plan_hash);
  o += ",\"confirmPhrase\":"; js(o, p.confirm_phrase);
  o += ",\"sectorCount\":" + std::to_string(p.sector_count());
  o += ",\"problems\":"; diags(o, p.problems);
  o += ",\"changes\":[";
  for (size_t i = 0; i < p.changes.size(); i++) { if (i) o += ','; js(o, p.changes[i]); }
  o += "],\"stages\":[";
  for (size_t i = 0; i < p.stages.size(); i++) {
    if (i) o += ',';
    o += "{\"name\":"; js(o, p.stages[i].name);
    o += ",\"writes\":[";
    for (size_t k = 0; k < p.stages[i].writes.size(); k++) {
      const auto& w = p.stages[i].writes[k];
      if (k) o += ',';
      o += "{\"lba\":" + u64(w.lba) + ",\"what\":"; js(o, w.what);
      if (include_sectors) {
        o += ",\"data\":"; js(o, hex_encode(w.data.data(), w.data.size()));
        o += ",\"before\":"; js(o, hex_encode(w.before.data(), w.before.size()));
      }
      o += '}';
    }
    o += "]}";
  }
  o += "],\"before\":" + (p.before.kind == TableKind::None && p.before.partitions.empty() ? std::string("null") : partition_table_to_json(p.before));
  o += ",\"after\":" + (p.stages.empty() ? std::string("null") : partition_table_to_json(p.after));
  o += '}';
  return o;
}

}  // namespace usbmem
