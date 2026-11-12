# ISO 9660 (fs/isofs) の移植

Linux の `fs/isofs` (ISO 9660 / Joliet / Rock Ridge) を**無改変**のまま LinuxCompat 上で読み取り専用に動かし、アプリから「マウント」できるようにした。
UDF は別文書 (docs/udf-port.md)。光学メディア 2 種の共通部分 (ブリッジ盤の優先順位など) はここに書く。

## 構成

| 場所 | 内容 |
|---|---|
| `third_party/isofs/fs-isofs/` | 上流 `fs/isofs` (コミット 067c5c24。他の FS と同じ)。ビルドするのは namei inode dir util rock export joliet。`compress.c` (zisofs) は入れない |
| `core/compat/optical.cmake` | `usbmem_isofs` / `usbmem_udf` とテストのターゲット (共有の `CMakeLists.txt` は末尾の `include` 1 行だけ) |
| `core/compat/include/compat/optical.h` | 両 FS のコンパイルに `-include` で強制的に入れる補助定義 (共有ヘッダを書き換えない) |
| `core/compat/include/linux/cdrom.h`, `user_namespace.h` | 空に近いシム (光学ドライブの ioctl は使えないので常に -EINVAL) |
| `core/compat/src/rt_vfs_isofs.c` | ホスト入口 (iget → lookup / iterate_shared / read_mapping_folio を直接呼ぶ) |
| `core/compat/src/isofs_volume.cpp` | `IsofsVolume : FsVolume` (`fs_name()` = `"iso9660"`、`readlink` あり) |
| `tools/gen_iso_fixture.py` | テスト用イメージの生成 |
| `core/compat/tests/test_isofs.cpp` | ホストテスト |

共有ファイルへの変更は追加だけ: `fs.h` (dentry の `d_op` を関数ポインタ入りのビューにした) と `CMakeLists.txt` の末尾 2 行。

## 対応範囲

- ISO 9660 レベル 1〜3 (レベル 1 は 8.3 の大文字、レベル 2/3 は長い名前)、High Sierra の識別も上流どおり。
- Joliet (UTF-16BE の名前)。マウントオプションは `utf8` (iocharset を使わない)。上流の `utf16s_to_utf8s` は互換層ではエンディアンを無視するので、`joliet.c` だけコンパイル定義で専用実装 (`usbmem_utf16s_to_utf8s_be`: サロゲートペア対応、対にならないものは U+FFFD) に差し替えた。
- Rock Ridge (長い名前、権限、所有者、nlink、時刻、シンボリックリンク、**8 階層を超える relocation** = `rr_moved` を透過的に元の場所へ)。
- 名前: Rock Ridge も Joliet も無いときは、上流の既定どおり**小文字化してバージョン `;1` を落とす**。
- 全ファイル 2048 バイトブロック。マルチエクステントは上流が処理する。4 GiB を超えるファイルは検証していない (フィクスチャを作っていない)。
- **zisofs は未対応**: 圧縮ファイル (Rock Ridge ZF) は、先頭が zisofs のマジック (`37 E4 53 96 C9 DB D6 07`、16 バイト以上) のとき `read` が `-EOPNOTSUPP(95)` を返す。圧縮データを解かずに生で返さない。
- 書き込みは全面的に未対応 (`mount(read_only=0)` は `-EROFS`)。

## inode 番号

isofs の `i_ino` は `(ブロック << (bufbits-5)) | (レコード位置 >> 5)`。位置の下位 5 ビットが落ちるので `ino` だけでは inode (ブロック, レコード位置) が引けない。
`ino_to_loc()` が**そのブロックを読み、ディレクトリレコードを先頭から数えて**スロットに入るレコードの正確な位置を求める (レコードは最小 34 バイトなのでスロットに 2 つ始まることはない)。ディレクトリは常に位置 0 の `.` レコード。ブロック 0 や `s_nzones` 以上は `-ESTALE`。
上流の `readdir` はディレクトリ内の**論理**ブロックから作った番号を `d_ino` に出すので、`rd_actor` で `i_ino` と同じ値に作り直す (ホストは readdir の ino をそのまま stat に使うため)。`.` は dir 自身、`..` は呼び出し側が `file->f_pos` に入れた親の ino。

## 互換層の穴埋め

- `put_bh`: 共有の定義は何もしない (ext4 が `brelse` と併用するため)。isofs の `rock.c` は `put_bh` だけで bh を手放す (参照カウントなしの互換 bh では解放が要る) ので、`optical.h` で `brelse` に再定義した。これを入れる前は ASan のリークになっていた。
- `generic_ro_fops` / `page_symlink_inode_operations` / `dir_emit_dot(dotdot)` / `get_unaligned_be*` / `QSTR_LEN` などは `optical.h` と `rt_vfs_isofs.c`。

## マウント前の検査とブリッジ盤の優先順位

`probe_filesystem` (`core/src/fs_probe.cpp`、既存) は 0x8000 から 2048 バイトずつ記述子列を走査し、`CD001` があれば ISO 9660、`NSR02`/`NSR03` があれば UDF。**両方あれば UDF を返す**。
アプリの `mount_auto` (`jni_ntfs.cpp`) は次の方針:

1. 判別結果が UDF なら UDF として開く。**開けなければ ISO 9660 にフォールバック**する (ブリッジ盤で UDF 側が対応外のパーティション型・壊れているなど)。
2. 判別結果が ISO 9660 なら ISO 9660 として開く。

理由: ブリッジ盤 (ISO 9660 + UDF 1.02) は UDF 側のほうが名前が長く正確 (255 バイト、UTF-8 に変換可能) で、Rock Ridge の無い盤でも大文字小文字・長い名前が保たれる。ISO 側は常に残るので、UDF が失敗したときの退路になる。
ISO 側の `usbmem_isofs_mount` は、セクタ 16 以降 100 セクタ以内に `CD001` (または High Sierra の `CDROM`) が無ければ `-EINVAL` を返す。

パーティション表が無い盤: `core/src/partition.cpp` は先頭が 0x55AA で終わらないとき `probe_filesystem` をデバイス全体にかけ、見つかれば「スーパーフロッピー」として 1 つのボリューム (`wholeDeviceVolume`) にする。isohybrid (先頭に MBR を持つ ISO) は MBR のパーティション 1 が LBA 0 から始まるのでパーティション内の ISO として判別される (`core/tests/test_partition.cpp` の `udf_whole_device_and_bridge` で確認)。アプリ側 (`PartitionModel.kt`) の変更は不要。

## アプリ側の変更 (最小)

- `jni_ntfs.cpp`: `mount_auto` に UDF / ISO 9660 の分岐 (上記) と 2 つの include。
- `UsbScreen.kt`: マウントボタンを出す FS に `iso9660` / `udf` を追加 (読み取り専用の文言を使う)。
- `LinkResolver.kt`: `LINK_FS` に `"udf"` を追加 (`"iso9660"` は既にあった。`fs_name()` と一致)。
- `android/app/src/main/cpp/CMakeLists.txt`: `usbmem_isofs usbmem_udf` をリンク。

## 検証

テスト用イメージ (`tools/gen_iso_fixture.py`、作成直後に**独立の isoinfo / 7z**でも名前・サイズ・種別・リンク先を照合してから manifest にする):
`rr` (genisoimage -R、レベル 3、深さ 12 = relocation あり) / `rr_xorriso` (xorrisofs -R) / `both` (-R -J) / `joliet` / `joliet_xorriso` / `plain_l1` / `plain_l2`。
内容: サイズ 0/11/2047/2048/2049/4095/4096/4097/100000/3145851、mode 0755/0600/0444、日本語名、200 文字の名前、空白と引用符、絵文字 (RR のみ)、1000 ファイルのディレクトリ、深い階層、空ディレクトリ、シンボリックリンク 7 種 (相対・上方・絶対・壊れた・長い・サブ内・ディレクトリ宛て)、zisofs 風ファイル 1 つ。

ホストテスト (`usbmem_isofs_tests`): 全イメージで**全ツリー比較** (readdir の名前集合・種別・ino での stat 一致・サイズ・mtime・RR は mode と nlink・内容をバイト比較、不揃いな位置/長さの部分読み)、`--corrupt` (PVD・ブロックサイズ・ルート・切り詰めなど 20 ケース)、`--ioerr` (読み取りエラー注入)、`--fuzz`、`--coexist` (ISO + ext2 + UDF を同じプロセスで)。読み取りに書き込み・flush が 1 度も出ないことも確認。
ASan/UBSan 構成でリークなし (`rock.c` の `put_bh` 漏れを見つけて直した)。`precheck` の 8 バイトバッファ読み出し超過も san の fuzz で見つけて直した。

AVD (emulator-5556): `IsofsInstrumentedTest` 5 件通過 (rr / both / joliet / plain_l2 の全ファイル内容、zisofs 風の拒否、ゼロ埋めイメージの拒否)。

## 限界・未解決

- zisofs の展開は未対応 (明示的に拒否)。4 GiB 超のファイルは未検証。マルチセッション (最後のセッションの先頭を選ぶ) は未対応で、常にセッション 0。
- Joliet のイメージで `;1` 付きの名前を `lookup` に渡すと見つからない (`d_op->d_compare` を互換層が適用しないため。readdir が返す名前でそのまま引けるので UI 上は問題ない)。
- Joliet 付きの盤は PVD が壊れても補助ボリューム記述子 (Joliet) のルートで開ける (上流の仕様)。
- 実機の光学ドライブ (USB の CD/DVD) は未確認。USB メモリに焼いた ISO (isohybrid) も実機未確認。
