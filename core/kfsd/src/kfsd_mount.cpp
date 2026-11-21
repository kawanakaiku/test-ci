// SPDX-License-Identifier: GPL-2.0-or-later
// kfsd の「FS の種類の判別 + マウント」。android/app/src/main/cpp/jni_ntfs.cpp の mount_auto と同じ振り分けの規則 (docs/kfsd.md: 将来は JNI もこれを使う)。
#include "kfsd_mount.h"

#include <cerrno>

#include "usbmem/fs_probe.h"
#ifdef USBMEM_FS_EXT2
#include "usbmem/ext2_volume.h"
#endif
#ifdef USBMEM_FS_EXT4
#include "usbmem/ext4_volume.h"
#endif
#ifdef USBMEM_FS_EXFAT
#include "usbmem/exfat_volume.h"
#endif
#ifdef USBMEM_FS_FAT
#include "usbmem/fat_c.h"
#include "usbmem/fat_volume.h"
#endif
#ifdef USBMEM_FS_ISOFS
#include "usbmem/isofs_volume.h"
#endif
#ifdef USBMEM_FS_UDF
#include "usbmem/udf_volume.h"
#endif
#ifdef USBMEM_FS_NTFS
#include "usbmem/ntfs_volume.h"
#endif
#ifdef USBMEM_FS_SQUASHFS
#include "usbmem/squashfs_volume.h"
#endif
#ifdef USBMEM_FS_EROFS
#include "usbmem/erofs_volume.h"
#endif
#ifdef USBMEM_FS_BTRFS
#include "usbmem/btrfs_volume.h"
#endif
#ifdef USBMEM_FS_F2FS
#include "usbmem/f2fs_volume.h"
#endif
#ifdef USBMEM_FS_XFS
#include "usbmem/xfs_volume.h"
#endif

namespace usbmem {
namespace kfsd {

std::unique_ptr<FsVolume> mount_auto(const BlockDevicePtr& dev, bool rw, int* err, bool allow_noload, bool allow_dirty, std::string* fs_type) {
  if (dev->is_read_only()) rw = false;  // 下のデバイスが読み取り専用 (ネストの子など) なら、書き込み可を要求されても読み取り専用でマウントする
  FsType t = probe_filesystem(*dev, 0, dev->size());
  if (fs_type) *fs_type = fs_type_name(t);
#ifdef USBMEM_FS_EXFAT
  if (t == FsType::Exfat) {
    // rw を要求されたときだけ mount_rw (ビルドで USBMEM_NTFS_WRITE が無効なら -EROFS で失敗)。汚れた (VolumeDirty) / MediaFailure のボリュームは
    // mount_rw の中で読み取り専用に落ちる (writable() == false で分かる)
    return rw ? ExfatVolume::mount_rw(dev, err) : ExfatVolume::mount(dev, err);
  }
#endif
#ifdef USBMEM_FS_EXT2
  if (t == FsType::Ext2) {
    // ext2 (ジャーナルなし・extent なし) は fs/ext2。rw を要求されたときだけ mount_rw (ビルドで USBMEM_NTFS_WRITE が無効なら -EROFS で失敗)。
    // 汚れた / エラー状態 / 未知の ro_compat のボリュームは mount_rw の中で読み取り専用に落ちる (writable() == false で分かる)
    return rw ? Ext2Volume::mount_rw(dev, err) : Ext2Volume::mount(dev, err);
  }
#endif
#ifdef USBMEM_FS_EXT4
  if (t == FsType::Ext3 || t == FsType::Ext4) {
    // ext3 / ext4 は fs/ext4 (+ fs/jbd2)。NTFS として試さない。docs/ext4-write.md
    //  - rw を要求されたときだけ mount_rw (ビルドで USBMEM_NTFS_WRITE が無効なら -EROFS で失敗)。未再生のジャーナル (needs_recovery) はここで再生する。
    //    書き込みのホワイトリスト外の機能・エラー状態・外部ジャーナルなどは mount_rw の中で読み取り専用に落ちる (writable() == false)。
    //  - 読み取り専用: ジャーナルは再生しない。未再生 (needs_recovery) のボリュームは EUCLEAN(117) で拒否、暗号化 / casefold / fs-verity などは EOPNOTSUPP(95)。
    //    allow_noload は、ユーザーが「ジャーナルが未再生で内容が古い/欠けている可能性がある」ことを確認したうえで、読み取り専用で開く場合だけ true
    if (rw && !allow_noload) return Ext4Volume::mount_rw(dev, err);
    Ext4MountOptions o4; o4.allow_noload = allow_noload;
    return Ext4Volume::mount(dev, err, o4);
  }
#endif
#ifdef USBMEM_FS_FAT
  if (t == FsType::Fat12 || t == FsType::Fat16 || t == FsType::Fat32) {
    // FAT12/16/32 (vfat の長い名前を含む) は fs/fat。NTFS として試さない。docs/fat-write.md
    //  - rw を要求されたときだけ mount_rw (ビルドで USBMEM_NTFS_WRITE が無効なら -EROFS で失敗)。ダーティ印 (ブートセクタ / FAT[1]) が付いたボリュームなど
    //    書き込みに適さないものは mount_rw の中で読み取り専用に落ちる (writable() == false)
    return rw ? FatVolume::mount_rw(dev, err) : FatVolume::mount(dev, err);
  }
#endif
#if defined(USBMEM_FS_ISOFS) && defined(USBMEM_FS_UDF)
  if (t == FsType::Udf || t == FsType::Iso9660) {
    // 光学メディア (読み取り専用)。ISO 9660 + UDF のブリッジ盤は UDF を優先し、UDF として開けなければ (対応外のパーティションなど) ISO 9660 にフォールバックする。docs/isofs-port.md / docs/udf-port.md
    if (t == FsType::Udf) {
      int uerr = 0;
      if (auto u = UdfVolume::mount(dev, &uerr)) return u;
      if (err) *err = uerr;
    }
    int ierr = 0;
    if (auto i = IsofsVolume::mount(dev, &ierr)) return i;
    if (err && t == FsType::Iso9660) *err = ierr;
    return nullptr;
  }
#endif
#ifdef USBMEM_FS_SQUASHFS
  if (t == FsType::Squashfs) {
    // squashfs は読み取り専用の形式 (fs/squashfs)。rw を要求されても読み取り専用でマウントする (writable() == false)。非対応の圧縮方式 (LZMA1) は EOPNOTSUPP(95)
    return SquashfsVolume::mount(dev, err);
  }
#endif
#ifdef USBMEM_FS_EROFS
  if (t == FsType::Erofs) {
    // erofs も読み取り専用の形式 (fs/erofs)。rw を要求されても読み取り専用でマウントする。圧縮 (z_erofs) は未対応で EOPNOTSUPP(95) (docs/erofs-port.md)
    return ErofsVolume::mount(dev, err);
  }
#endif
#ifdef USBMEM_FS_BTRFS
  if (t == FsType::Btrfs) {
    // Btrfs は自前のリーダー (docs/btrfs-port.md) と、実験的な書き込み (docs/btrfs-write.md)。tree-log が未再生なら EUCLEAN(117)。
    // 複数デバイス / RAID0・5・6・10 / 未知の機能は EOPNOTSUPP(95)。allow_noload は、ユーザーが確認したうえで tree-log を無視して開く場合だけ true (ext4 と同じ)
    // rw を要求されたときだけ mount_rw (ビルドで USBMEM_NTFS_WRITE が無効なら -EROFS で失敗)。書き込みのホワイトリスト (single / DUP・free space tree・サブボリュームなし・
    // ミラー一致など: docs/btrfs-write.md) を満たさないボリュームは mount_rw の中で読み取り専用に落ちる (writable() == false で分かる)
    if (rw && !allow_noload) return BtrfsVolume::mount_rw(dev, err);
    BtrfsMountOptions ob; ob.allow_noload = allow_noload;
    return BtrfsVolume::mount(dev, err, ob);
  }
#endif
#ifdef USBMEM_FS_F2FS
  if (t == FsType::F2fs) {
    // f2fs は自前実装 (docs/f2fs-port.md)。暗号化 / casefold / fs-verity などは EOPNOTSUPP(95)、fsync 後のロールフォワードが必要な状態は EUCLEAN(117)。
    // allow_noload は、確認のうえ「直近の fsync の分を無視して」開く場合だけ true。
    // 書き込み (docs/f2fs-write.md): rw を要求されたときだけ mount_rw (ビルドで USBMEM_NTFS_WRITE が無効なら読み取り専用)。変更はすべてメモリに溜め、sync で完全なチェックポイントを 1 回書く。
    // 書き込みのホワイトリスト外 (圧縮・quota・未知の機能など) は mount_rw の中で読み取り専用に落ちる (writable() == false)
    if (rw && !allow_noload) return F2fsVolume::mount_rw(dev, err);
    F2fsMountOptions of; of.allow_noload = allow_noload;
    return F2fsVolume::mount(dev, err, of);
  }
#endif
#ifdef USBMEM_FS_XFS
  if (t == FsType::Xfs) {
    // XFS は自前のリーダー (docs/xfs-port.md) と、実験的な書き込み (CreateOnly の範囲だけ: docs/xfs-write.md)。ログが未再生 (要リカバリ) のボリュームは EUCLEAN(117) で拒否、
    // リアルタイム領域・外部ログ・未知の incompat 機能は EOPNOTSUPP(95)。allow_noload は、ユーザーが確認したうえで読み取り専用で開く場合だけ true
    // rw を要求されたときだけ mount_rw (ビルドで USBMEM_NTFS_WRITE が無効なら -EROFS で失敗)。V5 以外・rmapbt・ストライプなどは mount_rw の中で読み取り専用に落ちる (writable() == false で分かる)
    if (rw && !allow_noload) return XfsVolume::mount_rw(dev, err);
    XfsMountOptions ox; ox.allow_noload = allow_noload;
    return XfsVolume::mount(dev, err, ox);
  }
#endif
#ifdef USBMEM_FS_NTFS
  // NTFS: allow_dirty = false なら、ダーティフラグだけのボリュームは書き込みマウントせず、読み取り専用のまま返す (state に USBMEM_VOL_CONFIRM_WRITE。呼び出し側が確認する)
  return rw ? NtfsVolume::mount_rw(dev, err, allow_dirty) : NtfsVolume::mount(dev, err);
#else
  // 公開版 (NTFS を含まない): 判別できなかったものは EINVAL (ファイルシステムではない)。判別できたが、この版に入っていない FS (上の分岐がビルドされていないもの) は
  // EPROTONOSUPPORT(93) = 「この版では未対応」。UI は Kotlin の Edition (nativeEditionInfo) と合わせて案内する (docs/editions.md)。
  (void)allow_dirty; (void)allow_noload; (void)rw;
  if (err) *err = (t == FsType::Unknown) ? EINVAL : EPROTONOSUPPORT;
  return nullptr;
#endif
}

MountFn mount_fn() {
  return [](BlockDevicePtr dev, const MountParams& p) {
    MountResult r;
#ifdef USBMEM_FS_FAT
    // FAT の更新時刻は書いた端末のローカル時刻 (タイムゾーン情報なし)。本体が送ってきた「UTC より何分西か」で UTC に直す
    usbmem_fat_set_timezone(p.tz_minutes_west);
#endif
    int err = 0;
    r.vol = mount_auto(dev, p.rw(), &err, p.allow_noload(), p.allow_dirty(), &r.fs_type);
    r.err = r.vol ? 0 : (err > 0 ? err : EIO);
    return r;
  };
}

}  // namespace kfsd
}  // namespace usbmem
