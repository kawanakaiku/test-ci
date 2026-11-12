# XFS の読み取り専用リーダ

2026-11-12。XFS を**読み取り専用**で開く `usbmem::XfsVolume` (`core/compat/src/xfs_volume.cpp`、約 900 行) を追加した。

## 先に結論 — 他の FS 移植との違い (方針の逸脱)
- **Linux `fs/xfs` は取り込んでいない。ディスク形式を仕様として読み、独立に実装したリーダ**である (NTFS / exFAT / ext2 / ext4 / FAT / squashfs / erofs の「カーネルのコードを無改変で LinuxCompat に載せる」方針からの逸脱)。
  - 理由: `fs/xfs` は xfs_buf (バッファキャッシュ)・トランザクション・ログ・perag・xfs_mount の全体・workqueue・rmap/refcount の整合に深く依存し、読み取り専用に絞っても互換層の追加が過大になる (見積もり: 数万行規模の翻訳単位 + 互換層の大幅追加)。
    ディスク形式の読み取り (superblock・inode・extent・ディレクトリ 5 形式・シンボリックリンク) だけなら小さく書けて、境界検査と破損耐性を自前で徹底できる。
  - カーネルのソースは参照 (`fs/xfs/libxfs/xfs_format.h` などの形式定義) のみで、コードは取り込んでいない。**ライセンス**は将来取り込み版に差し替える可能性と形式の出典を考え GPL-2.0-or-later として扱う (`docs/licenses.md`)。
  - 検証は実際の `mkfs.xfs` / `xfs_repair -n` / `xfs_db` (xfsprogs 7.2.0) で作ったイメージで行う。したがって「実カーネルと同じ挙動」の保証はなく、実イメージとの突き合わせ (下記) が根拠になる。
- 読み取り専用。`fs_name()` は `"xfs"`、書き込み系はすべて `-EROFS`。rw を要求されても読み取り専用でマウントする (`mount_auto`)。
- 判定は既存の `fs_probe` (マジック `XFSB`)。`FsType::Xfs`。

## 対応範囲
| 項目 | 状態 |
|---|---|
| v5 (CRC 付き) | 対応。メタデータ CRC32C (superblock / inode / ディレクトリブロック / bmbt / リモートシンボリックリンク) を検証。不一致は EBADMSG(74) |
| v4 (CRC なし) | 対応 (ftype あり/なし、ブロック 512 B〜) |
| ブロックサイズ | 512 B〜64 KiB (v5 は 1 KiB〜)。ディレクトリブロックサイズ 4 KiB〜64 KiB (複数 fs ブロックにまたがる形も) |
| inode サイズ | 256 / 512 / 1024 / 2048 |
| ファイルの配置 | extent 形式・B+tree 形式 (bmbt)、疎ファイル (穴は 0)、未書き込み (unwritten) extent は 0 として読む、4 GiB 超 |
| ディレクトリ | short-form / block / leaf / node / btree の 5 形式 (すべて実イメージで確認)。ftype あり/なし |
| シンボリックリンク | inline (local) / remote (v5 はヘッダ付き)。`readlink` はリンク先の文字列を返すだけで辿らない |
| 特殊ファイル | キャラクタ / ブロック / FIFO / ソケットは一覧と stat に出る。read は失敗 |
| 機能 | sparse inode、finobt、rmapbt、reflink、inobtcount、bigtime (2038 年問題対応の ns 時刻)、nrext64 (巨大 extent 数)、exchange-range、parent pointer (読み飛ばし)、metadir (metadata ディレクトリ。通常ツリーの読みには影響しない)、ci (大文字小文字無視。名前の一致は**バイト一致のみ**) |
| xattr / ACL | 読まない (公開しない) |
| ファイル名 | バイト列をそのまま UTF-8 として扱う |

reflink で共有された extent は、通常の extent として読める (参照カウントは読み取りに不要)。**共有 extent の実イメージは作れていない** (mkfs.xfs だけでは共有 extent を作れず、マウントして `cp --reflink` する権限がない)。構造上は通常の extent と同一なので影響しない見込みだが、未検証である。

## 拒否するもの (理由つき)
| 条件 | 戻り値 | 理由 |
|---|---|---|
| マジックが XFSB でない / ジオメトリが不正 / 端末が小さい | EINVAL(22) | XFS でない、または壊れている |
| ログが**未再生** (クリーンでない) | EUCLEAN(117) | 再生しないので、見えるのは古い内容。`XfsMountOptions::allow_noload=true` なら**読み取り専用で**開ける (ext4 の `Ext4MountOptions` と同じ形。UI は既存の noload 確認ダイアログ) |
| `sb_inprogress` / `NEEDSREPAIR` | EUCLEAN | mkfs / repair が途中 |
| リアルタイム領域 (`rblocks != 0`) | EOPNOTSUPP(95) | 未対応 |
| 外部ログ (`logstart` が 0 で `logblocks` が別デバイス) | EOPNOTSUPP | 別デバイスが渡せない |
| 未知の incompat ビット / ZONED | EOPNOTSUPP | 解釈できない |
| v1 inode / v1 ディレクトリ (DIRV2 なし) | EOPNOTSUPP | 古すぎる形式 |
| CRC 不一致 (v5) | EBADMSG(74) | 破損 |
| 構造の矛盾 (範囲外のブロック・無限ループ・extent 数の過大など) | EUCLEAN | 破損 |

### ログの判定
ログは**再生しない**。クリーンかどうかだけを読んで決める: ログ領域の先頭・末尾の周期番号から head を求め (二分探索)、最後のレコードヘッダ (`0xFEEDbabe`) の `tail_lsn` が head と一致するか、直前がアンマウントレコード (op フラグ 0x20) ならクリーン。判定できない場合は未再生扱い (安全側)。
`allow_noload` で開いたときは `journal_ignored()` が true になり、UI は「ジャーナルを無視して開いている」ことを表示する (`NativeBridge.kt` の `journalIgnored`)。

## 構成
- `core/compat/public/usbmem/xfs_volume.h`、`core/compat/src/xfs_volume.cpp`、`core/compat/xfs.cmake` (`usbmem_xfs` 静的ライブラリとテスト)。互換層 (rt_*.c) には**依存しない**ので `BlockDevice` だけでビルドできる。
- メタデータは 16 KiB ページの簡易キャッシュ (最大 128 ページ)、ディレクトリの名前表 (直近 4 ディレクトリ、最大 2^19 項目) を持つ。すべての公開メソッドは 1 つの mutex の下で動く。
- JNI の `mount_auto` が `FsType::Xfs` で `XfsVolume::mount(dev, err, {allow_noload})` を呼ぶ (`jni_ntfs.cpp`)。UI (`UsbScreen.kt`) では `p.fs == "xfs"` を読み取り専用ボタン (「マウント」) の対象にした。`LinkResolver.kt` の `LINK_FS` にも xfs を追加。
- `tools/gen_xfs_fixture.py` がフィクスチャを作る。`mkfs.xfs -p <dir>` (ルート権限不要) でツリーを入れ、`xfs_repair -n` で検査し、`xfs_db` でディレクトリ形式・extent 形式を確認して `.formats` に書く。manifest は他の FS と同形式 (D / F / L / S / R)。キャラクタ / ブロックデバイスはルート権限なしでは作れないため、protofile 経由の `special_proto` だけに入れる (mtime は作成時刻なので manifest は -1 = 未検査)。
- 最小の v5 イメージは約 300 MiB (mkfs.xfs の制約)。疎ファイル + `fallocate -d` で /dev/shm の実使用量は約 0.5 GB (全 18 イメージ)。`tools/run_android_xfs_test.sh` は 1 イメージずつ端末に push して消す (端末の空き 1.5 GiB 以上)。

## フィクスチャ (18 イメージ。すべて `xfs_repair -n` クリーン)
v5_default / v5_bs1k / v5_bs2k / v5_bs64k / v5_dir8k / v5_dir64k / v5_isize1k / v5_isize2k / v5_old_features (sparse・finobt・bigtime・rmapbt・reflink・inobtcount なし) / v5_nrext64 / v5_agcount2 / v5_agcount16 / v5_metadir / v5_ci / v4_ftype1 / v4_ftype0 / v4_bs1k_dir4k / v4_bs512 / special_proto。
各イメージのツリー: 日本語名・長い名前 (255 バイト)・1000 ファイル超のディレクトリ (node/btree 形式)・断片化ファイル (btree 形式の extent)・疎な巨大ファイル (> 4 GiB)・ハードリンク・シンボリックリンク (inline / remote、長さ境界)・FIFO・ソケット。
作れなかったもの: v5 ブロック 512 B (mkfs.xfs は CRC 付きで 1 KiB 未満を拒否)、`agcount=1` (mkfs.xfs が拒否)、reflink で共有した extent、実際にマウントして書いた直後のダーティなログ (ログの未再生は、アンマウントレコードのフラグ破壊と周期番号の改変で模擬)。

## テスト (`core/compat/tests/test_xfs.cpp`、ctest: `xfs_gen_fixture` / `xfs_read` / `xfs_features` / `xfs_corrupt` / `xfs_ioerr` / `xfs_fuzz`)
- `xfs_read`: 全イメージで manifest と全項目突き合わせ (ディレクトリの項目集合・mtime・サイズ・nlink・全バイトの内容・リンク先・特殊ファイル)、書き込み系が EROFS、範囲外・不正 inode 番号が安全に失敗すること。
- `xfs_features`: 機能ビットごとの許可・拒否 (RT・外部ログ・未知の incompat・NEEDSREPAIR・ZONED・v1 など) とログ未再生 (EUCLEAN / allow_noload で開ける)。
- `xfs_corrupt`: メタデータの 1 箇所破壊 (CRC・サイズ・extent 数・ループ・範囲外)。クラッシュ/ハングせず、v5 は CRC で検出。
- `xfs_ioerr`: I/O エラー注入 (n 回目の読み取りが失敗) — エラーを返し、クラッシュしない。
- `xfs_fuzz`: ランダムバイト破壊 + 全ツリー走査 (SIGALRM でハング検出、読み取り量の上限)。
- ASan / UBSan (`san` / `wsan` 構成) で実行する。
- ファジングで見つかって直したもの: (1) 壊れた superblock の `blocklog` 等が 32 以上のときのシフト (UBSan)、(2) extent の論理開始位置が 2^52 以上のとき「穴の終わり = 開始 × ブロックサイズ」が 64 ビットで 0 に回り込み、read が無限ループ (ハング)。後者は `xfs_fuzz_seed2` が回帰テスト。extent の論理終端が 2^62 バイト以上は破損として拒否する。

## 既知の制限
- 独立実装であり、実カーネルの `fs/xfs` ではない (上記)。正しさの根拠は mkfs.xfs / xfs_repair で作った実イメージとの突き合わせと破損耐性テスト。
- ログの再生はしない。再生が要るボリュームは `allow_noload` で開くと古い内容 (または一部欠けた内容) になりうる。
- リアルタイム領域・外部ログ・ZONED・v1 は未対応。xattr / ACL は読まない。ci は名前のバイト一致のみ。
- reflink 共有 extent と実機 USB は未検証。
- 書き込みは対応しない (`-EROFS`)。
