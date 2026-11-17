// SPDX-License-Identifier: Apache-2.0
// NTFS の $LogFile の先頭 2 ページ (restart page) から、「クリーンに取り外されたか」を判定する (読むだけ。ログの再生はしない)。
// 自前の検査 (fscheck_ntfs.cpp) と、マウント時の状態の取得 (compat の NtfsVolume) が同じ判定を使う。docs/dirty-volumes.md
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace usbmem {

enum class NtfsLogState { Unknown, Clean, Unclean };

namespace ntfs_log_detail {
inline uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint64_t rd64(const uint8_t* p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | p[i]; return v; }
}  // namespace ntfs_log_detail

// restart page 1 枚の状態。0 = 不明 (RSTR でない・構造が不正)、1 = クリーン、2 = 未クリーン。
//  - 0xFF で埋まったページ (リセット済み = ログが空) と、chkdsk がリセットした "CHKD" はクリーン。
//  - RSTR: restart area の flags の RESTART_VOLUME_IS_CLEAN (0x0002) が立っているか、client_in_use_list が LOGFILE_NO_CLIENT (0xFFFF = ログが開かれていない) ならクリーン。
//    どちらでもなければ未クリーン (Windows が開いたまま取り外された)。
inline int ntfs_restart_page_state(const uint8_t* p, size_t size) {
  using namespace ntfs_log_detail;
  bool all_ff = true;
  for (size_t i = 0; i < size; i++) if (p[i] != 0xFF) { all_ff = false; break; }
  if (all_ff) return 1;
  if (!std::memcmp(p, "CHKD", 4)) return 1;
  if (std::memcmp(p, "RSTR", 4) != 0) return 0;
  uint16_t ra = rd16(p + 0x18);
  if (ra < 0x20 || ra + 0x30u > size) return 0;
  uint16_t in_use = rd16(p + ra + 0xC), flags = rd16(p + ra + 0xE);
  if (in_use == 0xFFFF) return 1;
  return (flags & 0x0002) ? 1 : 2;
}

// 2 枚の restart page (各 size バイト。通常 4096) から、ログの状態を返す。
//  - どちらも不明なら Unknown。どちらにも「未クリーン」がなければ Clean。
//  - どちらかが未クリーンのとき、新しい方 (current_lsn が大きい方) が確かにクリーンなら Clean (古い方が残っているだけ)。確認できなければ Unclean (保守的)。
inline NtfsLogState ntfs_logfile_state(const uint8_t* pg0, const uint8_t* pg1, size_t size = 4096) {
  using namespace ntfs_log_detail;
  int s0 = ntfs_restart_page_state(pg0, size), s1 = ntfs_restart_page_state(pg1, size);
  if (s0 == 0 && s1 == 0) return NtfsLogState::Unknown;
  if (s0 != 2 && s1 != 2) return NtfsLogState::Clean;
  if (s0 == 1 && s1 == 1) return NtfsLogState::Clean;
  if (s0 != 0 && s1 != 0 && !std::memcmp(pg0, "RSTR", 4) && !std::memcmp(pg1, "RSTR", 4)) {
    uint16_t a0 = rd16(pg0 + 0x18), a1 = rd16(pg1 + 0x18);
    uint64_t l0 = rd64(pg0 + a0), l1 = rd64(pg1 + a1);
    if (ntfs_restart_page_state(l1 > l0 ? pg1 : pg0, size) == 1) return NtfsLogState::Clean;
  }
  return NtfsLogState::Unclean;
}

}  // namespace usbmem
