// SPDX-License-Identifier: Apache-2.0
// ブートセクタの検査と修復。docs/partition-scan.md
#include "usbmem/bootfix.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <mutex>
#include <set>

#include "partition_scan_internal.h"
#include "usbmem/crc32.h"
#include "usbmem/sha256.h"

namespace usbmem {

const char* boot_repair_name(BootRepairKind k) {
  switch (k) {
    case BootRepairKind::FatRestorePrimaryFromBackup: return "fat32-restore-primary-from-backup";
    case BootRepairKind::FatRefreshBackupFromPrimary: return "fat32-refresh-backup-from-primary";
    case BootRepairKind::FatRebuildFsInfo: return "fat32-rebuild-fsinfo";
    case BootRepairKind::NtfsRestorePrimaryFromBackup: return "ntfs-restore-primary-from-backup";
    case BootRepairKind::NtfsRefreshBackupFromPrimary: return "ntfs-refresh-backup-from-primary";
    case BootRepairKind::ExfatRestoreMainFromBackup: return "exfat-restore-main-from-backup";
    case BootRepairKind::ExfatRefreshBackupFromMain: return "exfat-refresh-backup-from-main";
    case BootRepairKind::ExfatRecomputeChecksums: return "exfat-recompute-checksums";
    case BootRepairKind::ExtRestoreSuperblock: return "ext-restore-superblock-from-backup";
    case BootRepairKind::ExtRestoreGdt: return "ext-restore-gdt-from-backup";
  }
  return "";
}

size_t BootInspection::error_count() const {
  size_t n = 0;
  for (const auto& f : findings)
    if (f.severity == Severity::Error) n++;
  return n;
}

namespace {

using namespace ps;
using Bytes = std::vector<uint8_t>;

// 範囲 [off, off+len) の読み取りビュー。範囲外は false。I/O エラーは最初の 1 つを残す
struct Vr {
  BlockDevice& d;
  uint64_t off, len;
  Status io{};
  bool rd(uint64_t rel, void* b, size_t n) {
    if (!range_ok(len, rel, n) || !io.ok()) return false;
    Status st = d.read_at(off + rel, b, n);
    if (!st.ok()) { io = st; return false; }
    return true;
  }
  Bytes get(uint64_t rel, size_t n) {
    Bytes b(n);
    if (!rd(rel, b.data(), n)) b.clear();
    return b;
  }
};

struct Writes {
  std::map<std::string, std::vector<BootWrite>> by_id;  // 修復の id → 書き込み (offset は範囲の先頭から)
};

struct Ctx {
  Vr* v;
  BootInspection* out;
  Writes* w;  // null なら計画用の書き込みを作らない
  void add(Severity s, const char* code, const std::string& msg) { out->findings.push_back({s, code, msg}); }
  void detail(const char* k, const std::string& val) { out->details.emplace_back(k, val); }
  void detail(const char* k, uint64_t val) { out->details.emplace_back(k, std::to_string(val)); }
  void repair(BootRepairKind k, const std::string& desc, uint64_t source, std::vector<BootWrite> writes) {
    BootRepairOption o;
    o.kind = k;
    o.id = boot_repair_name(k);
    o.description = desc;
    o.source = source;
    out->repairs.push_back(o);
    if (w) {
      // ext は源 (予備のブロック) ごとに別の書き込みになる
      std::string key = o.id + (source ? "@" + std::to_string(source) : "");
      w->by_id[key] = std::move(writes);
      if (source && !w->by_id.count(o.id)) w->by_id[o.id] = w->by_id[key];
    }
  }
};

BootWrite mk_write(uint64_t rel, Bytes data, const char* what) {
  BootWrite b;
  b.offset = rel;
  b.data = std::move(data);
  b.what = what;
  return b;
}

std::string hex(uint64_t v) {
  char b[24];
  std::snprintf(b, sizeof b, "0x%llX", static_cast<unsigned long long>(v));
  return b;
}

// ------------------------------------------------------------------ FAT

bool fsinfo_valid(const uint8_t* s) { return le32(s) == 0x41615252u && le32(s + 484) == 0x61417272u && le32(s + 508) == 0xAA550000u; }

Bytes make_fsinfo(uint32_t bps) {
  Bytes b(bps, 0);
  auto p32 = [&](size_t o, uint32_t v) { for (int i = 0; i < 4; i++) b[o + i] = static_cast<uint8_t>(v >> (8 * i)); };
  p32(0, 0x41615252u);
  p32(484, 0x61417272u);
  p32(488, 0xFFFFFFFFu);  // 空きクラスタ数: 不明
  p32(492, 0xFFFFFFFFu);  // 次に探すクラスタ: 不明
  p32(508, 0xAA550000u);
  return b;
}

bool fat_same(const uint8_t* a, const uint8_t* b) {
  return std::memcmp(a + 11, b + 11, 0x41 - 11) == 0 && std::memcmp(a + 0x42, b + 0x42, 0x5A - 0x42) == 0;
}

// FAT12/16 の BPB が壊れているとき、FAT の並び・パーティションの大きさから BPB を推定する (読み取りのみ。書かない)
void propose_fat_bpb(Vr& v, Ctx& c) {
  const uint32_t bps = 512;
  uint64_t tot = v.len / bps;
  if (tot < 64) return;
  uint32_t rsvd = 0;
  uint8_t s[512];
  uint32_t media = 0;
  bool fat16 = false;
  for (uint32_t r = 1; r < 64 && !rsvd; r++) {
    if (!v.rd(static_cast<uint64_t>(r) * bps, s, 512)) break;
    if ((s[0] == 0xF0 || s[0] >= 0xF8) && s[1] == 0xFF && s[2] == 0xFF) {
      // 2 つ目の FAT が見つかること (同じ先頭のセクタ) を確かめて採用する
      uint8_t t[512];
      for (uint32_t d = 1; d <= 260 && static_cast<uint64_t>(r + d) < tot; d++) {
        if (!v.rd(static_cast<uint64_t>(r + d) * bps, t, 512)) break;
        if (std::memcmp(s, t, 512) == 0 && t[0] == s[0]) {
          rsvd = r;
          media = s[0];
          fat16 = s[3] == 0xFF;
          // FAT のサイズ d で整合するクラスタあたりセクタ数を探す
          const uint32_t root_ent = 512;
          uint64_t root_secs = (root_ent * 32ull + bps - 1) / bps;
          for (uint32_t spc = 1; spc <= 128; spc <<= 1) {
            if (tot < rsvd + 2ull * d + root_secs + spc) continue;
            uint64_t clusters = (tot - rsvd - 2ull * d - root_secs) / spc;
            uint64_t need = fat16 ? ((clusters + 2) * 2 + bps - 1) / bps : ((clusters + 2) * 3 / 2 + bps - 1) / bps;
            bool type_ok = fat16 ? (clusters >= 4085 && clusters < 65525) : clusters < 4085;
            if (need == d && type_ok) {
              c.out->proposed_bpb = {{"type", fat16 ? "fat16" : "fat12"},
                                     {"bytesPerSector", "512"},
                                     {"sectorsPerCluster", std::to_string(spc)},
                                     {"reservedSectors", std::to_string(rsvd)},
                                     {"numFats", "2"},
                                     {"rootEntries", std::to_string(root_ent)},
                                     {"totalSectors", std::to_string(tot)},
                                     {"mediaDescriptor", hex(media)},
                                     {"sectorsPerFat", std::to_string(d)},
                                     {"assumptions", "512-byte sectors, 2 FATs, 512 root entries, total sectors = partition length"}};
              return;
            }
          }
          return;  // FAT の並びは見つかったが、大きさが合う組み合わせが無い
        }
      }
    }
  }
}

bool inspect_fat(Ctx& c, bool probing) {
  Vr& v = *c.v;
  BootInspection& o = *c.out;
  uint8_t s[512];
  Fat f;
  bool prim = v.rd(0, s, 512) && parse_fat(s, &f);
  if (!prim) {
    // FAT32 のバックアップブートセクタ (セクタ 6)
    uint8_t b[512];
    Fat g;
    if (v.rd(6ull * 512, b, 512) && parse_fat(b, &g) && g.fat32 && g.bps == 512 && g.bk == 6) {
      uint8_t e[16];
      if (!v.rd(static_cast<uint64_t>(g.rsvd) * 512, e, 16) || !(e[0] == g.media && e[1] == 0xFF && e[2] == 0xFF)) {
        if (probing) return false;
      }
      o.fs = FsType::Fat32;
      o.recognized = true;
      o.has_backup_structure = true;
      o.boot_ok = false;
      o.backup_ok = true;
      c.add(Severity::Error, "fat32-boot-invalid", "The FAT32 boot sector (sector 0) is not valid, but the backup boot sector (sector 6) is");
      Bytes b0 = v.get(6ull * 512, 512), b1 = v.get(7ull * 512, 512), b2 = v.get(8ull * 512, 512);
      if (b1.empty() || !fsinfo_valid(b1.data())) {
        b1 = make_fsinfo(512);
        c.add(Severity::Warning, "fat32-backup-fsinfo-invalid", "The backup FSInfo sector is not valid; it will be rebuilt with unknown free-space values");
      }
      std::vector<BootWrite> w;
      w.push_back(mk_write(0, b0, "fat32-boot-sector"));
      if (g.fsinfo == 1) w.push_back(mk_write(512, b1, "fat32-fsinfo"));
      if (!b2.empty() && g.rsvd > 8) w.push_back(mk_write(1024, b2, "fat32-boot-sector-2"));
      c.repair(BootRepairKind::FatRestorePrimaryFromBackup, "Copy the backup boot sector (sector 6) to sector 0, and the FSInfo / extra boot sectors with it", 0, std::move(w));
      c.detail("bytesPerSector", g.bps);
      c.detail("totalSectors", g.tot);
      return true;
    }
    if (probing) return false;
    o.fs = FsType::Fat16;  // 種別は不明。FAT12/16 として BPB の提案を試みる
    o.recognized = true;
    o.has_backup_structure = false;
    c.add(Severity::Error, "fat-boot-invalid", "The boot sector is not a valid FAT BPB");
    c.add(Severity::Info, "fat-no-backup-boot", "FAT12/16 has no backup boot sector; the BPB can only be rebuilt from the FAT layout (proposal only, nothing is written)");
    propose_fat_bpb(v, c);
    if (o.proposed_bpb.empty()) c.add(Severity::Warning, "fat-bpb-unreconstructable", "The BPB could not be reconstructed from the FAT layout");
    return true;
  }

  o.fs = f.type;
  o.recognized = true;
  o.boot_ok = true;
  c.detail("bytesPerSector", f.bps);
  c.detail("sectorsPerCluster", f.spc);
  c.detail("reservedSectors", f.rsvd);
  c.detail("numFats", f.nfats);
  c.detail("sectorsPerFat", f.fatsz);
  c.detail("totalSectors", f.tot);
  c.detail("media", hex(f.media));
  if (!f.fat32) c.detail("rootEntries", f.root_ent);
  uint64_t vol_bytes = f.tot * f.bps;
  if (vol_bytes > v.len) c.add(Severity::Warning, "fat-total-exceeds-length", "The BPB's total sector count is larger than the partition / device");
  uint8_t e[16];
  if (!v.rd(static_cast<uint64_t>(f.rsvd) * f.bps, e, 16) || !(e[0] == f.media && e[1] == 0xFF && e[2] == 0xFF))
    c.add(Severity::Warning, "fat-first-entry-mismatch", "The first FAT entry does not match the media descriptor (the FAT may be damaged)");
  if (f.nfats == 2) {
    Bytes a = v.get(static_cast<uint64_t>(f.rsvd) * f.bps, 512), b = v.get((f.rsvd + f.fatsz) * f.bps, 512);
    if (!a.empty() && !b.empty() && a != b) c.add(Severity::Warning, "fat-copies-differ", "The two FAT copies differ at the start");
  }
  if (!f.fat32) {
    o.has_backup_structure = false;
    c.add(Severity::Info, "fat-no-backup-boot", "FAT12/16 has no backup boot sector");
    return true;
  }
  c.detail("rootCluster", f.root_clus);
  c.detail("fsinfoSector", f.fsinfo);
  c.detail("backupBootSector", f.bk);
  uint64_t meta = f.rsvd + f.nfats * f.fatsz;
  uint64_t clusters = f.tot > meta ? (f.tot - meta) / f.spc : 0;
  // FSInfo
  bool fi_ok = false;
  Bytes fi;
  if (f.fsinfo >= 1 && f.fsinfo != 0xFFFF) {
    fi = v.get(static_cast<uint64_t>(f.fsinfo) * f.bps, f.bps);
    if (!fi.empty()) {
      fi_ok = fsinfo_valid(fi.data());
      if (!fi_ok) c.add(Severity::Warning, "fat32-fsinfo-invalid", "The FSInfo sector has a bad signature");
      else {
        uint32_t fr = le32(&fi[488]), nx = le32(&fi[492]);
        if ((fr != 0xFFFFFFFFu && fr > clusters) || (nx != 0xFFFFFFFFu && nx > clusters + 1)) {
          fi_ok = false;
          c.add(Severity::Warning, "fat32-fsinfo-out-of-range", "The FSInfo free-cluster count or next-free hint is larger than the volume");
        }
      }
    }
  } else {
    c.add(Severity::Info, "fat32-no-fsinfo", "The BPB has no FSInfo sector");
    fi_ok = true;
  }
  uint32_t bk = (f.bk >= 1 && f.bk != 0xFFFF) ? f.bk : 0;
  o.has_backup_structure = bk != 0;
  Bytes p0 = v.get(0, f.bps), p1 = f.fsinfo >= 1 && f.fsinfo != 0xFFFF ? fi : Bytes(), p2;
  if (f.rsvd > 2) p2 = v.get(2ull * f.bps, f.bps);
  if (bk) {
    Bytes b0 = v.get(static_cast<uint64_t>(bk) * f.bps, f.bps);
    Fat g;
    bool bok = !b0.empty() && parse_fat(b0.data(), &g) && g.fat32 && g.tot == f.tot && g.rsvd == f.rsvd && g.fatsz == f.fatsz && g.bps == f.bps;
    if (!bok) {
      c.add(Severity::Warning, "fat32-backup-boot-invalid", "The backup boot sector (sector " + std::to_string(bk) + ") is missing or does not match the boot sector");
      if (static_cast<uint64_t>(bk) + 3 <= f.rsvd) {
        std::vector<BootWrite> w;
        w.push_back(mk_write(static_cast<uint64_t>(bk) * f.bps, p0, "fat32-backup-boot-sector"));
        Bytes nfi = fi_ok && !p1.empty() ? p1 : make_fsinfo(f.bps);
        if (f.fsinfo == 1) w.push_back(mk_write(static_cast<uint64_t>(bk + 1) * f.bps, nfi, "fat32-backup-fsinfo"));
        if (!p2.empty()) w.push_back(mk_write(static_cast<uint64_t>(bk + 2) * f.bps, p2, "fat32-backup-boot-sector-2"));
        c.repair(BootRepairKind::FatRefreshBackupFromPrimary, "Copy the boot sector (sector 0) and FSInfo to the backup area (sector " + std::to_string(bk) + ")", 0, std::move(w));
      }
    } else if (!fat_same(p0.data(), b0.data())) {
      c.add(Severity::Warning, "fat32-backup-differs", "The backup boot sector differs from the boot sector");
      o.backup_ok = true;
    } else {
      o.backup_ok = true;
    }
  }
  if (!fi_ok && f.fsinfo == 1) {
    std::vector<BootWrite> w;
    w.push_back(mk_write(static_cast<uint64_t>(f.fsinfo) * f.bps, make_fsinfo(f.bps), "fat32-fsinfo"));
    c.repair(BootRepairKind::FatRebuildFsInfo, "Rewrite the FSInfo sector with valid signatures; free-space values become 'unknown' (0xFFFFFFFF)", 0, std::move(w));
  }
  return true;
}

// ------------------------------------------------------------------ NTFS

bool ntfs_ok_at(Vr& v, uint64_t rel, Ntfs* n, Bytes* sec) {
  uint8_t s[512];
  if (!v.rd(rel, s, 512) || !parse_ntfs(s, n)) return false;
  if (sec) {
    *sec = v.get(rel, n->bps);
    if (sec->empty()) return false;
  }
  return true;
}

bool mft_has_file(Vr& v, const Ntfs& n, uint64_t lcn) {
  uint64_t bytes = 0;
  if (!mul_ok(lcn, static_cast<uint64_t>(n.spc) * n.bps, &bytes)) return false;
  uint8_t t[4];
  return v.rd(bytes, t, 4) && std::memcmp(t, "FILE", 4) == 0;
}

bool inspect_ntfs(Ctx& c, bool probing) {
  Vr& v = *c.v;
  BootInspection& o = *c.out;
  Ntfs n;
  Bytes p;
  bool prim = ntfs_ok_at(v, 0, &n, &p);
  Ntfs bn;
  Bytes bsec;
  uint64_t brel = 0;
  bool back = false;
  if (prim) {
    brel = n.total * n.bps;
    back = ntfs_ok_at(v, brel, &bn, &bsec) && bn.total == n.total && bn.bps == n.bps;
  } else {
    // 先頭が壊れている: パーティションの終わりの手前からバックアップを探す (最終セクタ = total_sectors 番目)
    uint64_t span = std::min<uint64_t>(v.len, 4ull << 20) / 512 * 512;
    Bytes tail = v.get(v.len / 512 * 512 - span, span);
    uint64_t base = v.len / 512 * 512 - span;
    for (uint64_t k = tail.size(); k >= 512 && !back; k -= 512) {
      const uint8_t* s = &tail[k - 512];
      Ntfs t;
      if (!parse_ntfs(s, &t)) continue;
      uint64_t rel = base + k - 512;
      if (t.total * t.bps != rel) continue;
      bn = t;
      brel = rel;
      back = ntfs_ok_at(v, rel, &bn, &bsec);
      n = bn;
    }
    if (!back) {
      if (probing) return false;
      o.fs = FsType::Ntfs;
      o.recognized = true;
      c.add(Severity::Error, "ntfs-boot-invalid", "The NTFS boot sector is not valid and no backup boot sector was found at the end of the volume");
      return true;
    }
    if (probing && !mft_has_file(v, n, n.mft) && !mft_has_file(v, n, n.mirr)) return false;
  }
  o.fs = FsType::Ntfs;
  o.recognized = true;
  o.has_backup_structure = true;
  o.boot_ok = prim;
  o.backup_ok = back;
  c.detail("bytesPerSector", n.bps);
  c.detail("sectorsPerCluster", n.spc);
  c.detail("totalSectors", n.total);
  c.detail("mftCluster", n.mft);
  c.detail("mftMirrorCluster", n.mirr);
  uint64_t vol = (n.total + 1) * n.bps;
  if (vol > v.len) c.add(Severity::Warning, "ntfs-volume-exceeds-length", "The volume is larger than the partition / device (the backup boot sector is outside it)");
  bool mft_ok = mft_has_file(v, n, n.mft), mirr_ok = mft_has_file(v, n, n.mirr);
  if (!mft_ok) c.add(prim ? Severity::Error : Severity::Warning, "ntfs-mft-missing", "The $MFT record is not where the boot sector says it is");
  if (!mirr_ok) c.add(Severity::Warning, "ntfs-mftmirr-missing", "The $MFTMirr record is not where the boot sector says it is");
  if (!prim) {
    c.add(Severity::Error, "ntfs-boot-invalid", "The NTFS boot sector (sector 0) is not valid, but the backup boot sector at the end of the volume is");
    if (mft_ok || mirr_ok) {
      std::vector<BootWrite> w;
      w.push_back(mk_write(0, bsec, "ntfs-boot-sector"));
      c.repair(BootRepairKind::NtfsRestorePrimaryFromBackup, "Copy the backup boot sector (last sector of the volume) to sector 0", 0, std::move(w));
    } else {
      c.add(Severity::Error, "ntfs-no-mft", "Neither $MFT nor $MFTMirr was found at the location in the backup boot sector; restoring the boot sector would not make the volume usable");
    }
    return true;
  }
  if (!back) {
    if (vol <= v.len) {
      c.add(Severity::Warning, "ntfs-backup-boot-invalid", "The backup boot sector (last sector of the volume) is missing or does not match");
      if (mft_ok) {
        std::vector<BootWrite> w;
        w.push_back(mk_write(brel, p, "ntfs-backup-boot-sector"));
        c.repair(BootRepairKind::NtfsRefreshBackupFromPrimary, "Copy the boot sector (sector 0) to the last sector of the volume", 0, std::move(w));
      }
    }
  } else if (std::memcmp(p.data() + 11, bsec.data() + 11, 0x50 - 11) != 0) {
    c.add(Severity::Warning, "ntfs-backup-differs", "The backup boot sector differs from the boot sector");
  }
  return true;
}

// ------------------------------------------------------------------ exFAT

struct ExRegion {
  bool structure = false, checksum = false;
  Bytes data;  // 12 セクタ
  Exfat x;
};

ExRegion exfat_region(Vr& v, uint64_t rel, uint32_t bps) {
  ExRegion r;
  r.data = v.get(rel, 12ull * bps);
  if (r.data.empty()) return r;
  if (!parse_exfat(r.data.data(), &r.x) || r.x.bps != bps) return r;
  r.structure = true;
  uint32_t cs = exfat_boot_checksum(r.data.data(), bps);
  r.checksum = true;
  for (size_t i = 0; i < bps; i += 4)
    if (le32(&r.data[11ull * bps + i]) != cs) { r.checksum = false; break; }
  return r;
}

Bytes checksum_sector(const Bytes& region, uint32_t bps) {
  uint32_t cs = exfat_boot_checksum(region.data(), bps);
  Bytes s(bps);
  for (size_t i = 0; i < bps; i += 4)
    for (int k = 0; k < 4; k++) s[i + k] = static_cast<uint8_t>(cs >> (8 * k));
  return s;
}

bool inspect_exfat(Ctx& c, bool probing) {
  Vr& v = *c.v;
  BootInspection& o = *c.out;
  uint8_t s[512];
  uint32_t bps = 0;
  Exfat x;
  if (v.rd(0, s, 512) && parse_exfat(s, &x)) bps = x.bps;
  ExRegion m, b;
  if (bps) {
    m = exfat_region(v, 0, bps);
    b = exfat_region(v, 12ull * bps, bps);
  } else {
    for (uint32_t t = 512; t <= 4096 && !b.structure; t <<= 1) {
      b = exfat_region(v, 12ull * t, t);
      if (b.structure) bps = t;
    }
  }
  if (!m.structure && !b.structure) {
    if (probing) return false;
    // exFAT のシグネチャがあるなら認識済みとして報告する
    if (std::memcmp(s + 3, "EXFAT   ", 8) == 0) {
      o.fs = FsType::Exfat;
      o.recognized = true;
      c.add(Severity::Error, "exfat-boot-invalid", "The exFAT boot sector is damaged and the backup boot region is not valid either");
      return true;
    }
    return false;
  }
  o.fs = FsType::Exfat;
  o.recognized = true;
  o.has_backup_structure = true;
  o.boot_ok = m.structure && m.checksum;
  o.backup_ok = b.structure && b.checksum;
  const Exfat& xi = m.structure ? m.x : b.x;
  c.detail("bytesPerSector", bps);
  c.detail("volumeLengthSectors", xi.vol_len);
  if (xi.vol_len * bps > v.len) c.add(Severity::Warning, "exfat-volume-exceeds-length", "The volume is larger than the partition / device");
  if (!m.structure) c.add(Severity::Error, "exfat-main-boot-invalid", "The main boot sector (sector 0) is not valid");
  else if (!m.checksum) c.add(Severity::Error, "exfat-main-checksum-bad", "The main boot region checksum (sector 11) does not match sectors 0-10");
  if (!b.structure) c.add(Severity::Warning, "exfat-backup-boot-invalid", "The backup boot sector (sector 12) is not valid");
  else if (!b.checksum) c.add(Severity::Warning, "exfat-backup-checksum-bad", "The backup boot region checksum (sector 23) does not match sectors 12-22");
  if (m.structure && b.structure && m.checksum && b.checksum) {
    Bytes a = m.data, d = b.data;
    for (Bytes* r : {&a, &d})
      for (size_t i : {106u, 107u, 112u}) (*r)[i] = 0;
    if (a != d) c.add(Severity::Warning, "exfat-regions-differ", "The main and backup boot regions differ");
  }
  if (b.structure && b.checksum && !o.boot_ok) {
    std::vector<BootWrite> w;
    w.push_back(mk_write(0, b.data, "exfat-main-boot-region"));
    c.repair(BootRepairKind::ExfatRestoreMainFromBackup, "Copy the backup boot region (sectors 12-23) to the main boot region (sectors 0-11)", 0, std::move(w));
  }
  if (o.boot_ok && !o.backup_ok) {
    std::vector<BootWrite> w;
    w.push_back(mk_write(12ull * bps, m.data, "exfat-backup-boot-region"));
    c.repair(BootRepairKind::ExfatRefreshBackupFromMain, "Copy the main boot region (sectors 0-11) to the backup boot region (sectors 12-23)", 0, std::move(w));
  }
  // チェックサムのセクタだけ壊れている
  std::vector<BootWrite> cw;
  if (m.structure && !m.checksum) cw.push_back(mk_write(11ull * bps, checksum_sector(m.data, bps), "exfat-main-checksum"));
  if (b.structure && !b.checksum) cw.push_back(mk_write(23ull * bps, checksum_sector(b.data, bps), "exfat-backup-checksum"));
  if (!cw.empty())
    c.repair(BootRepairKind::ExfatRecomputeChecksums, "Recompute the checksum sector(s) from boot sectors 0-10 (only if those sectors are known to be good)", 0, std::move(cw));
  return true;
}

// ------------------------------------------------------------------ ext2/3/4

uint32_t ext_desc_size(const Bytes& sb, const ExtSb& e) {
  if (!(e.incompat & 0x80)) return 32;
  uint32_t d = le16(&sb[254]);
  return (d >= 64 && d <= 1024 && is_pow2(d)) ? d : 0;
}

// e2fsck -b の候補になるグループ番号 (スーパーブロックの予備を持つグループ)
std::vector<uint64_t> backup_groups(const Bytes& sb, const ExtSb& e) {
  std::vector<uint64_t> g;
  if (e.groups < 2) return g;
  if (e.compat & 0x200) {  // sparse_super2: s_backup_bgs[2]
    for (int i = 0; i < 2; i++) {
      uint32_t x = le32(&sb[0x24C + 4 * i]);
      if (x && x < e.groups) g.push_back(x);
    }
    return g;
  }
  if (!(e.ro & 0x1)) {  // sparse_super でない: すべてのグループ
    for (uint64_t i = 1; i < e.groups && i <= 8; i++) g.push_back(i);
    return g;
  }
  g.push_back(1);
  for (uint64_t base : {3ull, 5ull, 7ull})
    for (uint64_t p = base; p < e.groups && g.size() < 24; p *= base) g.push_back(p);
  std::sort(g.begin(), g.end());
  g.erase(std::unique(g.begin(), g.end()), g.end());
  return g;
}

Bytes make_ext_sb_primary(const Bytes& backup, const ExtSb& e) {
  Bytes sb = backup;
  sb[0x5A] = 0;
  sb[0x5B] = 0;  // s_block_group_nr = 0
  // 予備のコピーは書かれた時点の状態を持つ。空きの数は古いので、検査が必要なことを残す (EXT2_VALID_FS を落とす)
  uint16_t st = static_cast<uint16_t>(le16(&sb[58]) & ~1u);
  sb[58] = static_cast<uint8_t>(st);
  sb[59] = static_cast<uint8_t>(st >> 8);
  if (e.ro & 0x400) {
    uint32_t cs = crc32c_ext_sb(sb.data());
    for (int i = 0; i < 4; i++) sb[0x3FC + i] = static_cast<uint8_t>(cs >> (8 * i));
  }
  return sb;
}

bool inspect_ext(Ctx& c, bool probing) {
  Vr& v = *c.v;
  BootInspection& o = *c.out;
  Bytes psb = v.get(1024, 1024);
  ExtSb e;
  bool prim = !psb.empty() && parse_ext(psb.data(), &e) && e.group == 0;
  struct Backup { uint64_t group, block; Bytes sb; ExtSb e; };
  std::vector<Backup> backups;
  ExtSb base;  // 検査の基準にする (主か最初の予備)
  Bytes base_sb;
  auto probe_group = [&](uint64_t g, uint64_t bs, const ExtSb* ref) {
    Backup b;
    b.group = g;
    uint64_t bpg = ref ? ref->bpg : 8 * bs, fdb = ref ? ref->fdb : (bs == 1024 ? 1 : 0);
    uint64_t pos = (g * bpg + fdb) * bs;
    b.block = pos / bs;
    b.sb = v.get(pos, 1024);
    if (b.sb.empty() || !parse_ext(b.sb.data(), &b.e) || b.e.group != g || ext_sb_pos(g, b.e) != pos) return;
    if (ref && (b.e.blocks != ref->blocks || b.e.ipg != ref->ipg || std::memcmp(b.e.uuid, ref->uuid, 16) != 0)) return;
    backups.push_back(std::move(b));
  };
  if (prim) {
    base = e;
    base_sb = psb;
    for (uint64_t g : backup_groups(psb, e)) {
      if (backups.size() >= 6) break;
      probe_group(g, e.bs, &e);
    }
  } else {
    for (uint64_t bs = 1024; bs <= 4096; bs <<= 1)
      for (uint64_t g : {1ull, 3ull, 5ull, 7ull, 9ull, 25ull, 27ull, 49ull}) probe_group(g, bs, nullptr);
    if (backups.empty()) {
      if (probing) return false;
      o.recognized = true;
      o.fs = FsType::Ext4;
      c.add(Severity::Error, "ext-no-valid-superblock", "Neither the primary superblock nor a backup superblock (at the e2fsck -b locations) is valid");
      return true;
    }
    base = backups[0].e;
    base_sb = backups[0].sb;
  }
  o.fs = ext_type(base);
  o.recognized = true;
  o.has_backup_structure = base.groups >= 2;
  o.boot_ok = prim;
  o.backup_ok = !backups.empty();
  for (const auto& b : backups) o.ext_backup_blocks.push_back(b.block);
  c.detail("blockSize", base.bs);
  c.detail("blocksCount", base.blocks);
  c.detail("blockGroups", base.groups);
  c.detail("blocksPerGroup", base.bpg);
  if (base.blocks * base.bs > v.len) c.add(Severity::Warning, "ext-size-exceeds-length", "The file system is larger than the partition / device");
  bool csum_bad = false;
  if (prim && e.csum_present && !e.csum_ok) {
    csum_bad = true;
    prim = false;
    o.boot_ok = false;
    c.add(Severity::Error, "ext-sb-checksum-bad", "The primary superblock checksum does not match");
  }
  if (!prim && !csum_bad) {
    if (psb.empty() || le16(&psb[56]) != 0xEF53) c.add(Severity::Error, "ext-primary-sb-invalid", "The primary superblock (at offset 1024) is missing or has no ext magic");
    else c.add(Severity::Error, "ext-primary-sb-invalid", "The primary superblock fields are not valid");
  }
  if (prim && base.groups >= 2 && backups.empty()) c.add(Severity::Warning, "ext-backup-sb-invalid", "No valid backup superblock was found at the expected group locations");
  // グループ記述子 (主と予備の、位置を表すフィールドの比較)
  bool gdt_bad = false;
  uint32_t dsz = ext_desc_size(base_sb, base);
  uint64_t gdt_blocks = 0;
  bool gdt_checkable = !backups.empty() && dsz && !(base.incompat & 0x10);
  if (!backups.empty() && (base.incompat & 0x10)) c.add(Severity::Info, "ext-meta-bg-gdt-unchecked", "meta_bg is enabled: group descriptors were not compared with the backup");
  if (gdt_checkable) {
    gdt_blocks = (base.groups * dsz + base.bs - 1) / base.bs;
    uint64_t cmp_blocks = std::min<uint64_t>(gdt_blocks, 64);
    const Backup& bk = backups[0];
    uint64_t pr = (base.fdb + 1) * base.bs, br = (bk.group * base.bpg + base.fdb + 1) * base.bs;
    Bytes pg = v.get(pr, cmp_blocks * base.bs), bg = v.get(br, cmp_blocks * base.bs);
    if (!pg.empty() && !bg.empty()) {
      uint64_t nd = std::min<uint64_t>(base.groups, cmp_blocks * base.bs / dsz);
      for (uint64_t i = 0; i < nd && !gdt_bad; i++) {
        const uint8_t* a = &pg[i * dsz];
        const uint8_t* b = &bg[i * dsz];
        if (std::memcmp(a, b, 12) != 0) gdt_bad = true;                                   // block bitmap / inode bitmap / inode table (下位)
        if (dsz >= 64 && std::memcmp(a + 0x20, b + 0x20, 12) != 0) gdt_bad = true;        // 同 (上位)
      }
    }
    if (gdt_bad) c.add(Severity::Error, "ext-gdt-damaged", "The primary group descriptors do not match the backup (bitmap / inode-table locations differ)");
  }
  // 修復
  if (!backups.empty() && gdt_checkable && gdt_blocks > 256) {
    gdt_checkable = false;
    c.add(Severity::Info, "ext-gdt-too-large", "The group descriptor table is large; GDT repair is not offered");
  }
  for (const auto& bk : backups) {
    if (!(!prim || gdt_bad)) break;
    std::vector<BootWrite> w;
    std::string kinddesc;
    if (!prim) {
      w.push_back(mk_write(1024, make_ext_sb_primary(bk.sb, bk.e), "ext-superblock"));
      kinddesc = "Copy the backup superblock (block " + std::to_string(bk.block) + ", group " + std::to_string(bk.group) + ") to the primary superblock";
    }
    if (gdt_bad && gdt_checkable) {
      uint64_t br = (bk.group * base.bpg + base.fdb + 1) * base.bs;
      Bytes g = v.get(br, gdt_blocks * base.bs);
      if (!g.empty()) {
        w.push_back(mk_write((base.fdb + 1) * base.bs, g, "ext-group-descriptors"));
        kinddesc += prim ? "Copy the backup group descriptors (group " + std::to_string(bk.group) + ") to the primary group descriptors"
                         : " and the group descriptors with it";
      }
    }
    if (w.empty()) continue;
    c.repair(prim ? BootRepairKind::ExtRestoreGdt : BootRepairKind::ExtRestoreSuperblock, kinddesc + " (like e2fsck -b " + std::to_string(bk.block) + "; run a file system check afterwards because the backup counters may be stale)",
             bk.block, std::move(w));
  }
  return true;
}

// ------------------------------------------------------------------ 全体

bool run_family(FsType fs, Ctx& c, bool probing) {
  switch (fs) {
    case FsType::Fat12: case FsType::Fat16: case FsType::Fat32: return inspect_fat(c, probing);
    case FsType::Ntfs: return inspect_ntfs(c, probing);
    case FsType::Exfat: return inspect_exfat(c, probing);
    case FsType::Ext2: case FsType::Ext3: case FsType::Ext4: return inspect_ext(c, probing);
    default: return false;
  }
}

void finish_verdict(BootInspection* o) {
  if (!o->recognized) { o->verdict = "unrecognized"; return; }
  size_t err = o->error_count(), warn = 0;
  for (const auto& f : o->findings) if (f.severity == Severity::Warning) warn++;
  if (err) o->verdict = o->repairs.empty() ? "damaged" : "repairable";
  else if (warn) o->verdict = o->repairs.empty() ? "warnings" : "repairable";
  else o->verdict = "ok";
}

Status inspect_impl(BlockDevice& dev, uint64_t offset, uint64_t length, FsType fs, const BootInspectOptions& opt, BootInspection* out, Writes* w) {
  *out = BootInspection{};
  if (offset > dev.size()) return Status::Make(Errc::OutOfRange);
  uint64_t avail = dev.size() - offset;
  if (length == 0 || length > avail) length = avail;
  Vr v{dev, offset, length};
  if (opt.cancel && opt.cancel->cancelled()) return Status::Make(Errc::Cancelled);
  FsType guess = fs;
  if (guess == FsType::Unknown) {
    Status io;
    guess = probe_filesystem(dev, offset, length, &io);
    if (!io.ok()) return io;
  }
  bool done = false;
  if (guess != FsType::Unknown) {
    Ctx c{&v, out, w};
    done = run_family(guess, c, false);
    if (!done && fs == FsType::Unknown) *out = BootInspection{};
  }
  if (!done && fs == FsType::Unknown) {
    // 先頭が壊れていて判別できない: バックアップの構造から判別する
    for (FsType t : {FsType::Fat32, FsType::Exfat, FsType::Ntfs, FsType::Ext4}) {
      *out = BootInspection{};
      if (w) w->by_id.clear();
      Ctx c{&v, out, w};
      if (run_family(t, c, true)) { done = true; break; }
      if (opt.cancel && opt.cancel->cancelled()) return Status::Make(Errc::Cancelled);
    }
    if (!done) *out = BootInspection{};
  }
  if (!v.io.ok()) return v.io;
  if (!done) {
    out->findings.push_back({Severity::Info, "unsupported-or-unrecognized", "No supported file system (FAT / exFAT / NTFS / ext2-4) was recognized, even from backup structures"});
  }
  finish_verdict(out);
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
const char* sevname(Severity s) { return s == Severity::Error ? "error" : s == Severity::Warning ? "warning" : "info"; }

}  // namespace

Status inspect_boot(BlockDevice& dev, uint64_t offset, uint64_t length, FsType fs, const BootInspectOptions& opt, BootInspection* out) {
  if (!out) return Status::Make(Errc::InvalidArgument);
  return inspect_impl(dev, offset, length, fs, opt, out, nullptr);
}

std::string boot_inspection_to_json(const BootInspection& r) {
  std::string o = "{\"fs\":" + json_str(fs_type_name(r.fs)) + ",\"recognized\":" + (r.recognized ? "true" : "false") + ",\"verdict\":" + json_str(r.verdict) +
                  ",\"bootOk\":" + (r.boot_ok ? "true" : "false") + ",\"hasBackupStructure\":" + (r.has_backup_structure ? "true" : "false") +
                  ",\"backupOk\":" + (r.backup_ok ? "true" : "false") + ",\"findings\":[";
  for (size_t i = 0; i < r.findings.size(); i++) {
    if (i) o += ',';
    o += "{\"severity\":" + json_str(sevname(r.findings[i].severity)) + ",\"code\":" + json_str(r.findings[i].code) + ",\"message\":" + json_str(r.findings[i].message) + "}";
  }
  o += "],\"repairs\":[";
  for (size_t i = 0; i < r.repairs.size(); i++) {
    if (i) o += ',';
    o += "{\"id\":" + json_str(r.repairs[i].id) + ",\"description\":" + json_str(r.repairs[i].description) + ",\"source\":\"" + std::to_string(r.repairs[i].source) + "\"}";
  }
  o += "],\"details\":{";
  for (size_t i = 0; i < r.details.size(); i++) { if (i) o += ','; o += json_str(r.details[i].first) + ":" + json_str(r.details[i].second); }
  o += "},\"proposedBpb\":{";
  for (size_t i = 0; i < r.proposed_bpb.size(); i++) { if (i) o += ','; o += json_str(r.proposed_bpb[i].first) + ":" + json_str(r.proposed_bpb[i].second); }
  o += "},\"extBackupBlocks\":[";
  for (size_t i = 0; i < r.ext_backup_blocks.size(); i++) { if (i) o += ','; o += "\"" + std::to_string(r.ext_backup_blocks[i]) + "\""; }
  o += "]}";
  return o;
}

// ------------------------------------------------------------------ 計画

namespace {

std::string device_id_of(BlockDevice& d) { return d.describe() + "|" + std::to_string(d.size()); }

std::string compute_hash(const BootRepairPlan& p) {
  Sha256 h;
  auto u64 = [&](uint64_t v) { uint8_t b[8]; for (int i = 0; i < 8; i++) b[i] = static_cast<uint8_t>(v >> (8 * i)); h.update(b, 8); };
  h.update("usbmem-bootfix-plan-v1", 22);
  h.update(p.device_id.data(), p.device_id.size());
  u64(static_cast<uint64_t>(p.fs));
  u64(static_cast<uint64_t>(p.kind));
  u64(p.offset);
  u64(p.length);
  for (const auto& w : p.writes) {
    u64(w.offset);
    u64(w.data.size());
    h.update(w.data.data(), w.data.size());
    u64(w.before.size());
    h.update(w.before.data(), w.before.size());
  }
  return hex_encode(h.finish());
}

}  // namespace

std::string make_boot_confirm_token(const BootRepairPlan& plan, const std::string& typed) {
  Sha256 h;
  const char tag[] = "usbmem-bootfix-confirm-v1";
  h.update(tag, sizeof tag);  // NUL を含む
  h.update(plan.plan_hash.data(), plan.plan_hash.size());
  h.update("\0", 1);
  h.update(plan.device_id.data(), plan.device_id.size());
  h.update("\0", 1);
  h.update(typed.data(), typed.size());
  return hex_encode(h.finish());
}

Status plan_boot_repair(BlockDevice& dev, uint64_t offset, uint64_t length, FsType fs, const std::string& repair_id, uint64_t source, BootRepairPlan* out) {
  if (!out) return Status::Make(Errc::InvalidArgument);
  *out = BootRepairPlan{};
  BootInspection insp;
  Writes w;
  Status st = inspect_impl(dev, offset, length, fs, {}, &insp, &w);
  if (!st.ok()) return st;
  uint64_t avail = offset <= dev.size() ? dev.size() - offset : 0;
  if (length == 0 || length > avail) length = avail;
  out->fs = insp.fs;
  out->repair_id = repair_id;
  out->offset = offset;
  out->length = length;
  out->device_id = device_id_of(dev);
  auto bad = [&](const char* code, const std::string& msg) { out->problems.push_back({Severity::Error, code, msg}); out->ok = false; return Status::Ok(); };
  const BootRepairOption* opt = nullptr;
  for (const auto& r : insp.repairs)
    if (r.id == repair_id && (source == 0 || r.source == source)) { opt = &r; break; }
  if (!opt) return bad("repair-not-applicable", "This repair does not apply to the current state of the volume");
  out->kind = opt->kind;
  std::string key = repair_id + (opt->source ? "@" + std::to_string(opt->source) : "");
  auto it = w.by_id.find(key);
  if (it == w.by_id.end() || it->second.empty()) return bad("no-writes", "The repair has nothing to write");
  for (BootWrite bw : it->second) {
    uint64_t abs = 0;
    if (!add_ok(offset, bw.offset, &abs) || !range_ok(length, bw.offset, bw.data.size()) || !range_ok(dev.size(), abs, bw.data.size()))
      return bad("out-of-range", "A write falls outside the volume");
    bw.offset = abs;
    bw.before.resize(bw.data.size());
    Status rs = dev.read_at(abs, bw.before.data(), bw.before.size());
    if (!rs.ok()) return rs;
    out->bytes_to_write += bw.data.size();
    out->writes.push_back(std::move(bw));
  }
  if (out->bytes_to_write > (16u << 20)) return bad("too-large", "The repair would write more than 16 MiB");
  out->changes.push_back(opt->description);
  for (const auto& bw : out->writes) out->changes.push_back("write " + std::to_string(bw.data.size()) + " bytes at byte " + std::to_string(bw.offset) + " (" + bw.what + ")");
  out->ok = true;
  out->plan_hash = compute_hash(*out);
  out->confirm_phrase = "REPAIR " + out->plan_hash.substr(0, 6);
  return Status::Ok();
}

// ------------------------------------------------------------------ 適用

namespace {

std::mutex g_busy_mu;
std::set<const BlockDevice*> g_busy;

struct BusyGuard {
  const BlockDevice* d;
  bool held = false;
  explicit BusyGuard(const BlockDevice* dev) : d(dev) {
    std::lock_guard<std::mutex> l(g_busy_mu);
    held = g_busy.insert(dev).second;
  }
  ~BusyGuard() {
    if (held) { std::lock_guard<std::mutex> l(g_busy_mu); g_busy.erase(d); }
  }
};

constexpr char kBkMagic[8] = {'U', 'S', 'B', 'M', 'B', 'F', 'X', '1'};

void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = static_cast<uint8_t>(v >> (8 * i)); }
void put64(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = static_cast<uint8_t>(v >> (8 * i)); }

Bytes make_backup(const BootRepairPlan& plan, uint64_t ts) {
  Bytes f(80, 0);
  std::memcpy(f.data(), kBkMagic, 8);
  put32(&f[8], 1);
  put32(&f[12], static_cast<uint32_t>(plan.writes.size()));
  put64(&f[16], ts);
  put64(&f[24], plan.offset);
  put64(&f[32], plan.length);
  put32(&f[40], static_cast<uint32_t>(plan.fs));
  auto id = Sha256::hash(plan.device_id.data(), plan.device_id.size());
  std::memcpy(&f[48], id.data(), 32);
  for (const auto& w : plan.writes) {
    uint8_t rec[16] = {};
    put64(rec, w.offset);
    put32(rec + 8, static_cast<uint32_t>(w.before.size()));
    f.insert(f.end(), rec, rec + 16);
    f.insert(f.end(), w.before.begin(), w.before.end());
  }
  uint8_t crc[4];
  put32(crc, crc32(f.data(), f.size()));
  f.insert(f.end(), crc, crc + 4);
  auto h = Sha256::hash(f.data(), f.size());
  f.insert(f.end(), h.begin(), h.end());
  return f;
}

struct BkRec { uint64_t off; Bytes data; };

Status parse_backup_records(const Bytes& f, BootBackupInfo* info, std::vector<BkRec>* recs) {
  const Status bad = Status::Make(Errc::Corrupt);
  if (f.size() < 80 + 36 || f.size() > (64u << 20)) return bad;
  if (std::memcmp(f.data(), kBkMagic, 8) != 0) return bad;
  size_t body = f.size() - 36;
  if (le32(&f[body]) != crc32(f.data(), body)) return bad;
  auto h = Sha256::hash(f.data(), f.size() - 32);
  if (std::memcmp(h.data(), &f[f.size() - 32], 32) != 0) return bad;
  if (le32(&f[8]) != 1) return Status::Make(Errc::Unsupported);
  uint32_t n = le32(&f[12]);
  if (n > 4096) return bad;
  BootBackupInfo bi;
  bi.version = 1;
  bi.timestamp = le64(&f[16]);
  bi.offset = le64(&f[24]);
  bi.length = le64(&f[32]);
  bi.fs = le32(&f[40]);
  bi.record_count = n;
  size_t pos = 80;
  for (uint32_t i = 0; i < n; i++) {
    if (body - pos < 16) return bad;
    uint64_t off = le64(&f[pos]);
    uint32_t len = le32(&f[pos + 8]);
    if (body - pos - 16 < len) return bad;
    if (recs) recs->push_back({off, Bytes(f.begin() + static_cast<long>(pos) + 16, f.begin() + static_cast<long>(pos) + 16 + len)});
    bi.bytes += len;
    pos += 16 + len;
  }
  if (pos != body) return bad;
  if (info) *info = bi;
  return Status::Ok();
}

}  // namespace

Status parse_boot_backup(const std::vector<uint8_t>& file, BootBackupInfo* info) { return parse_backup_records(file, info, nullptr); }

Status restore_boot_backup(BlockDevice& dev, const std::vector<uint8_t>& file) {
  std::vector<BkRec> recs;
  Status st = parse_backup_records(file, nullptr, &recs);
  if (!st.ok()) return st;
  if (dev.is_read_only()) return Status::Make(Errc::ReadOnly);
  for (const auto& r : recs)
    if (!range_ok(dev.size(), r.off, r.data.size())) return Status::Make(Errc::OutOfRange);
  for (const auto& r : recs) {
    st = dev.write_at(r.off, r.data.data(), r.data.size());
    if (!st.ok()) return st;
  }
  return dev.flush();
}

BootApplyResult apply_boot_repair(BlockDevice& dev, const BootRepairPlan& plan, const std::string& token, const BootApplyOptions& opt) {
  BootApplyResult r;
  auto fail = [&](Errc e, const char* code, const std::string& msg) {
    r.status = Status::Make(e);
    r.code = code;
    r.message = msg;
    return r;
  };
  if (!plan.ok || plan.writes.empty()) return fail(Errc::InvalidArgument, "plan-not-ok", "The plan cannot be applied");
  if (dev.is_read_only()) return fail(Errc::ReadOnly, "device-read-only", "The device is read-only");
  if (opt.in_use) return fail(Errc::InvalidArgument, "in-use", "The volume is in use (mounted); unmount it first");
  BusyGuard guard(&dev);
  if (!guard.held) return fail(Errc::InvalidArgument, "busy", "Another repair is in progress on this device");
  if (compute_hash(plan) != plan.plan_hash) return fail(Errc::InvalidArgument, "plan-tampered", "The plan does not match its hash");
  if (plan.device_id != device_id_of(dev)) return fail(Errc::InvalidArgument, "wrong-device", "The plan was made for a different device");
  if (token != make_boot_confirm_token(plan, plan.confirm_phrase)) return fail(Errc::InvalidArgument, "bad-token", "The confirmation does not match");
  for (const auto& w : plan.writes)
    if (!range_ok(plan.length, w.offset - plan.offset, w.data.size()) || w.offset < plan.offset || !range_ok(dev.size(), w.offset, w.data.size()))
      return fail(Errc::OutOfRange, "out-of-range", "A write falls outside the volume");
  if (opt.cancel && opt.cancel->cancelled()) return fail(Errc::Cancelled, "cancelled", "Cancelled");
  // 計画後にデバイスが変わっていないか
  for (const auto& w : plan.writes) {
    Bytes cur(w.data.size());
    Status st = dev.read_at(w.offset, cur.data(), cur.size());
    if (!st.ok()) { r.status = st; r.code = "read-failed"; r.message = "Cannot read the volume"; return r; }
    if (cur != w.before) return fail(Errc::InvalidArgument, "device-changed", "The volume changed after the plan was made; make a new plan");
  }
  // バックアップ (保存できなければ書かない)
  if (opt.progress) opt.progress("backup", 0, 1);
  if (opt.backup_sink) {
    Status bs = opt.backup_sink(make_backup(plan, opt.timestamp));
    if (!bs.ok()) { r.status = bs; r.code = "backup-failed"; r.message = "The backup could not be saved; nothing was written"; return r; }
  } else if (opt.require_backup) {
    return fail(Errc::InvalidArgument, "backup-required", "A backup is required before writing");
  }
  // 書き込み
  size_t done = 0;
  Status werr;
  for (const auto& w : plan.writes) {
    if (opt.cancel && opt.cancel->cancelled()) { werr = Status::Make(Errc::Cancelled); break; }
    Status st = dev.write_at(w.offset, w.data.data(), w.data.size());
    if (!st.ok()) { werr = st; break; }
    done++;
    if (opt.progress) opt.progress("write", done, plan.writes.size());
  }
  if (werr.ok()) werr = dev.flush();
  r.writes_done = done;
  auto rollback = [&](size_t upto) {
    bool all = true;
    for (size_t i = upto + 1; i-- > 0;) {
      if (i >= plan.writes.size()) continue;
      const auto& w = plan.writes[i];
      if (!dev.write_at(w.offset, w.before.data(), w.before.size()).ok()) all = false;
    }
    if (!dev.flush().ok()) all = false;
    return all;
  };
  if (!werr.ok()) {
    r.status = werr;
    r.code = werr.code == Errc::Cancelled ? "cancelled" : "write-failed";
    r.message = "Writing failed";
    if (opt.rollback_on_error) r.rolled_back = rollback(done < plan.writes.size() ? done : plan.writes.size() - 1);
    return r;
  }
  // 読み戻し
  if (opt.progress) opt.progress("verify", 0, 1);
  bool same = true;
  for (const auto& w : plan.writes) {
    Bytes cur(w.data.size());
    if (!dev.read_at(w.offset, cur.data(), cur.size()).ok() || cur != w.data) { same = false; break; }
  }
  BootInspection post;
  Status is = inspect_boot(dev, plan.offset, plan.length, plan.fs, {}, &post);
  bool clean = is.ok() && post.boot_ok && post.error_count() == 0;
  for (const auto& f : post.findings)
    if (f.severity != Severity::Info) r.verify_findings.push_back({f.severity, f.code, f.message});
  r.verified = same && clean;
  if (!r.verified) {
    r.status = Status::Make(Errc::Corrupt);
    r.code = "verify-failed";
    r.message = same ? "The volume still reports boot-structure errors after the repair" : "Read-back does not match what was written";
    if (opt.rollback_on_error) r.rolled_back = rollback(plan.writes.size() - 1);
    return r;
  }
  r.status = Status::Ok();
  r.code = "ok";
  return r;
}

std::string boot_plan_to_json(const BootRepairPlan& p, bool include_data) {
  std::string o = "{\"ok\":" + std::string(p.ok ? "true" : "false") + ",\"fs\":" + json_str(fs_type_name(p.fs)) + ",\"repair\":" + json_str(p.repair_id) +
                  ",\"offset\":\"" + std::to_string(p.offset) + "\",\"length\":\"" + std::to_string(p.length) + "\",\"bytesToWrite\":\"" + std::to_string(p.bytes_to_write) +
                  "\",\"planHash\":" + json_str(p.plan_hash) + ",\"confirmPhrase\":" + json_str(p.confirm_phrase) + ",\"problems\":[";
  for (size_t i = 0; i < p.problems.size(); i++) {
    if (i) o += ',';
    o += "{\"severity\":" + json_str(sevname(p.problems[i].severity)) + ",\"code\":" + json_str(p.problems[i].code) + ",\"message\":" + json_str(p.problems[i].message) + "}";
  }
  o += "],\"changes\":[";
  for (size_t i = 0; i < p.changes.size(); i++) { if (i) o += ','; o += json_str(p.changes[i]); }
  o += "],\"writes\":[";
  for (size_t i = 0; i < p.writes.size(); i++) {
    if (i) o += ',';
    o += "{\"offset\":\"" + std::to_string(p.writes[i].offset) + "\",\"length\":" + std::to_string(p.writes[i].data.size()) + ",\"what\":" + json_str(p.writes[i].what);
    if (include_data) o += ",\"data\":\"" + hex_encode(p.writes[i].data.data(), p.writes[i].data.size()) + "\",\"before\":\"" + hex_encode(p.writes[i].before.data(), p.writes[i].before.size()) + "\"";
    o += "}";
  }
  return o + "]}";
}

}  // namespace usbmem
