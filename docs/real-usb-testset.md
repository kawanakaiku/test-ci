# 実機 USB メモリ 検証セットの作り方 (`tools/make_real_usb_testset.py`)

手元の USB メモリ 3 本に、決まったパーティション構成と**決定的なテストデータ**を作り、(1) PC で作成 → (2) Android 実機 (OTG) で読み書き試験 → (3) PC に戻して fsck とマニフェスト照合、という流れで使う**ユーザー実行用**スクリプト。

> このスクリプトは**デバイスの内容を全部消す**。実デバイスに対する書き込み部分は、開発側では実行していない (コマンド列の生成と安全機構は単体テスト、FS の作成と中身の照合はファイルのイメージで検証。「9. 検証したこと・できなかったこと」)。**最初に必ず dry-run** で内容を確認し、対象を取り違えないこと。

## 0. 方式: イメージ経由 (build-then-flash)

USB メモリの上で `mkfs` や `ntfs-3g` を直接走らせると、遅いデバイスへ小さなランダム書き込みと全デバイス対象の `sync` が大量に飛ぶ。3 本を同時に走らせたライブ環境の PC (RAM 15 GiB、スワップ満杯) が固まったのは、これと tmpfs 上の作業領域が重なったため。そこで、**デバイスには最後に一度だけ、順次書く**方式に変えた。

```
(a) デバイスに触らず、ローカルディスク上の疎イメージに全部作る   <- root 不要 (一部を除く)
        disk_table.img      デバイスと同じ大きさの疎ファイル。パーティション表だけを入れる (sgdisk / sfdisk はこのファイルにだけ触る)
        <名前>.img          パーティションごとの疎ファイル。mkfs・データ投入・検査・中身の照合は全部ここで行う
        imageset.json       メタデータ (プロファイル・デバイス容量・配置・各イメージの SHA-256・スクリプトの版)
(b) デバイスのパスを手で打って確認
(c) バックアップ (元の表・先頭 4 MiB・末尾 1 MiB)  と  先頭 4 MiB・末尾 1 MiB の消去
(d) 流し込み: パーティションのイメージ -> パーティション表の領域 (最後) の順に、nbdcopy (または dd) で一度だけ・順次書く     <- root 必須
(e) 読み戻し (O_DIRECT) して SHA-256 をイメージのものと比べる                                                         <- root 必須
```

- (a) の間、USB には**一切触らない**。作業は `--work-dir` / `--keep-images` のローカルディスクだけ (メモリ上は警告/中止)。
- (d) では、パーティション内の**ゼロの領域も含めて全部書く**(既定)。FS の未使用領域に古いデータが残らない。未割り当て領域には触らない。表は最後に書くので、途中で止まったデバイスは「有効なパーティション表が無い」状態になる。
- 同期は**そのデバイスだけ** (`conv=fsync` / nbdcopy `--flush` / `blockdev --flushbufs`)。全デバイス対象の `sync` は使わない (`sync -f <作業中のファイルシステム>` はイメージ側のみ)。パーティション表の再読み込み (`partprobe` など) もしない: 流し込みの後は**取り外して挿し直す**。
- 従来の「作成直後にデバイスから取り出してマニフェスト照合」は、イメージ上で済ませる (a)。デバイスについては (e) の SHA-256 読み戻しが「イメージと同一」を保証するので、その照合に代える。

### nbdkit / nbdcopy / nbdfuse をどう使ったか

| 道具 | 使いどころ | 結論 |
|---|---|---|
| `nbdcopy` + `nbdkit --filter=offset file` | (d) の流し込み。送り元はイメージ、書き先は `[ nbdkit --exit-with-parent --filter=offset file DEV cache=none offset=O range=L ]` で**デバイスの一部分だけ**を NBD として見せる。`--flush`・並列 1・要求 4 MiB・`cache=none` (O_DIRECT)。`--sparse=0 --no-extents` でゼロも書く | **採用**。`-s` を付けると nbdcopy と噛み合わず失敗する。`[` `]` は別々の argv 要素にしないと、nbdcopy がファイル名と解釈して黙ってファイルを作る |
| `dd` | nbdcopy / nbdkit が無いときの代替 (`bs=4M iflag=skip_bytes,count_bytes oflag=seek_bytes,direct conv=notrunc,fsync`) | `--writer dd` で強制できる。通常は auto |
| `nbdkit file` / `memory` プラグイン | イメージの置き場 | イメージは通常の疎ファイルなので不要 (メモリ上に置くのは固まる原因) |
| `nbdkit cow` フィルタ | イメージを汚さずに書き込み試験 | 作成には不要。検証用に使える (下) |
| `nbdkit partition` フィルタ | ディスクイメージの**1 つのパーティションだけ**を NBD として見せる | 作成には不要 (各ツールが通常ファイルを受け付ける)。`--image` で合成したディスクイメージから AVD/アプリへパーティション単位で見せるときに有用 |
| `nbdkit luks` フィルタ | LUKS を root なしで開く | **LUKS1 専用**で LUKS2 には使えない。qemu-img も LUKS2 を扱えない。代わりに下の XTS 方式を使う |
| `nbdfuse` | イメージを FUSE のファイルとして見せる | 作成には不要。root なしで NBD をブロックファイルとして使いたいときに使えるが、本スクリプトの経路には入れていない |

### root が要る段階 (それ以外は root 不要)

| 段階 | root | 理由・代替 |
|---|---|---|
| (d) 流し込み・(e) 読み戻し・(c) バックアップ・消去 | **必須** | 実デバイスの open |
| UDF (`mkudffs` + `mount -t udf`) | 要 | `--udf-method genisoimage` なら root 不要 (ISO9660+UDF ブリッジ) |
| `--f2fs-method mount` | 要 | 既定の `sload.f2fs` は不要 |
| exFAT `--exfat-method mount` | 要 | **`--exfat-method fuse`** (`mount.exfat-fuse` + `tools/exfat_fuse_shim.c` を実行時に cc でコンパイルして `LD_PRELOAD`) で不要。非 root では auto でこちら |
| LUKS2 `--luks-method open` | 要 | **`--luks-method xts`**: `cryptsetup luksFormat` を**ファイル**に対して実行し、`luksDump --dump-volume-key` でマスター鍵を取り、内側の ext4 を平文ファイルに `mke2fs -d` で作ってから、Python の `cryptography` で AES-256-XTS (plain64、512 バイトセクタ) で暗号化してペイロード位置へ書く。非 root では auto でこちら。暗号化後に数セクタを AES-ECB だけで手組みした独立実装と照合する |
| NTFS | 不要 | `ntfs-3g` は FUSE (`fusermount3`) |
| 上記以外の mkfs・投入 | 不要 | すべてイメージファイルに対して実行 |

したがって `--exfat-method fuse --luks-method xts --udf-method genisoimage` (非 root の auto 既定 + UDF だけ指定) なら、(a) は**全部 root なし**で作れて、`--build-only --keep-images DIR` で終わる。root が要るのは (b) 以降 (流し込み) だけ。

## 1. 機材と流れ

| プロファイル | 想定する USB | パーティション表 | 目的 |
|---|---|---|---|
| `A` | 7.8 GiB (8 GB 級) | GPT | 書き込み検証用 (NTFS / ext4 / FAT32 / exFAT) |
| `B` | 16 GiB | MBR + 拡張パーティション | 古い形式の構成 (論理パーティション、ext2/3/4、`needs_recovery`、4 GiB 超ファイル) |
| `C` | 31 GiB | GPT | 多種 FS (XFS / Btrfs / F2FS / LUKS2 / BitLocker 風 / squashfs / erofs / UDF / ISO9660) と、末尾 10 GiB 以上の未割り当て (パーティション編集の検証用) |

給電付き USB ハブ経由で PC に接続して作る。サイズの判定は ±8% (7.8 GiB なら 7.18〜8.42 GiB なので 8 GB 表記 = 7.45 GiB のメモリでも通る)。

## 2. 必要なコマンド

足りないものは dry-run の末尾と実行時の事前チェックで一覧され、1 つでも無ければ**何も書かずに中止**する。`sbin` が PATH に無くても自動で補う。

| 用途 | コマンド |
|---|---|
| パーティション表 | `sgdisk` (GPT)、`sfdisk` (MBR とバックアップ)。実デバイスへ流すときは `wipefs` `lsblk` `blockdev` `dd` も |
| 流し込み (d) | `nbdcopy` + `nbdkit` (offset フィルタ付き)。無ければ `dd` (自動で切り替え。`--writer` で指定)。`--sparse-write` の事前ゼロ化には `blkdiscard` (無ければ `dd`) |
| ext2/3/4・LUKS 内 | `mke2fs` `e2fsck` `debugfs` `dumpe2fs` |
| FAT32 | `mkfs.fat` `fsck.fat` `mcopy` (mtools) |
| exFAT | `mkfs.exfat` `fsck.exfat`。投入は `mount` (root) か、`--exfat-method fuse` なら `mount.exfat-fuse` `fusermount3` `cc` `tune.exfat` (root 不要) |
| NTFS | `mkntfs` `ntfs-3g` `ntfsfix` `fusermount3` (root でなければ)、`--dirty-ntfs` なら `ntfsinfo` |
| XFS / Btrfs | `mkfs.xfs` (`-p <dir>` が使える xfsprogs 6.x 以降) `xfs_repair` / `mkfs.btrfs` `btrfs` |
| F2FS | `mkfs.f2fs` `sload.f2fs` `fsck.f2fs` |
| LUKS2 | `cryptsetup` (`luksFormat` `luksDump`)。`--luks-method xts` なら python3 の `cryptography` も (root 不要) |
| BitLocker 風 | `python3` + `numpy` + `cryptography` (`tools/bitlocker_synth.py` を使う) |
| squashfs / erofs | `mksquashfs` `unsquashfs` / `mkfs.erofs` `fsck.erofs` |
| ISO9660 / UDF | `genisoimage` (無ければ `--iso-tool xorriso`) `isoinfo` / `mkudffs` `udfinfo` |

## 3. 使い方

```sh
# 1. まず dry-run (何も書かない)。プロファイルを省略すると 3 つとも出る。流し込み計画 (書く範囲・触らない範囲・時間の目安) と必要な空き容量が出る
python3 tools/make_real_usb_testset.py
python3 tools/make_real_usb_testset.py --profile B --device /dev/sdX     # 実デバイスの検証結果 (OK / NG と理由) も出る

# 2-a. 一本道 (root。イメージ作成 -> 確認 -> バックアップ -> 流し込み -> 読み戻し。自分では sudo を呼ばない)
sudo python3 tools/make_real_usb_testset.py --profile A --device /dev/sdX --execute \
     --work-dir /var/tmp/usbmem_work --keep-images /var/tmp/usbmem_img_A

# 2-b. 二段 (おすすめ): イメージ作成は一般ユーザーで (USB を挿さなくてよい)、流し込みだけ root
python3 tools/make_real_usb_testset.py --profile A --build-only --execute --keep-images /var/tmp/usbmem_img_A \
     --work-dir /var/tmp/usbmem_work --udf-method genisoimage            # C のときだけ UDF の指定が要る (root でなければ)
sudo python3 tools/make_real_usb_testset.py --flash-only /var/tmp/usbmem_img_A --device /dev/sdX --execute

# 3. 試運転 (1 GiB / 4.5 GiB のファイルを作らず短時間で終わる)
python3 tools/make_real_usb_testset.py --profile C --build-only --execute --no-big-files --keep-images /var/tmp/usbmem_img_C --udf-method genisoimage
```

`--execute` を付けて実デバイスへ進むと、**イメージができた後で**デバイスの情報 (パス・モデル・シリアル・容量・現在のパーティション) を表示し、**デバイスのパス (例 `/dev/sdb`) を手で打ち込んで一致しない限り進まない**。確認を飛ばすオプション (`--yes` など) は無い。

### イメージの再利用 (`--keep-images` / `--flash-only`)

- `--keep-images DIR` で作ったイメージ一式 (`disk_table.img`、`<名前>.img`、`imageset.json`、`manifests/`) を残す。指定しなければ `--work-dir/images_<プロファイル>` に作って、成功したら消す (失敗したら残す)。
- `--flash-only DIR` は**イメージを作り直さずに**流し込む。同じイメージを何本にも流せる。`imageset.json` のプロファイル・デバイス容量・論理セクタ・配置を照合し、**容量が違うデバイスは拒否**する (配置は容量で決まるため)。流し込み前に各イメージの SHA-256 を作成時の値と照合し、作成後に変わっていたら中止する (`--skip-image-check` で省略)。イメージ作成時の検査が NG のものがあれば流さない (`--ignore-image-checks` で承知の上の続行)。
- `--only p2_ext4,p8_udf --keep-images DIR` で、既存のイメージ一式の一部だけ作り直せる (root が必要な UDF だけ `sudo` で作り直す、など)。
- 同じイメージは AVD / ホストでの検証にも使える: `disk_table.img` と各 `<名前>.img` は通常ファイル。FS 単体なら `mount -o loop,ro` (root) や `debugfs` / `ntfs-3g -o ro` / `mdir -i` などで見られる。アプリ側は NBD クライアントを持つ (`docs/nbd.md`) ので、`nbdkit file <名前>.img` で配れば AVD から実機と同じバイト列を開ける。ディスク全体として見せたいときは `--flash-only DIR --image FILE --execute` (「8. イメージモード」) で 1 枚のディスクイメージに合成し、`nbdkit file FILE` で配る (パーティション単位なら `--filter=partition partition=N` を付ける)。

### オプション

| オプション | 意味 |
|---|---|
| `--profile {A,B,C}` | プロファイル。dry-run だけ省略可 |
| `--device /dev/sdX` | 対象 (ディスク全体)。実デバイスへ流すときに必須 (`--build-only` では任意: 付けるとそのデバイスの容量で配置する) |
| `--execute` | 実際に作る・書く。無ければ dry-run |
| `--keep-images DIR` | イメージ一式を DIR に残す (上の「イメージの再利用」) |
| `--flash-only DIR` | イメージを作り直さず、DIR のイメージ一式を流すだけ。プロファイルはイメージから決まる |
| `--build-only` | イメージ一式を作るだけで終わる (`--keep-images` が必要。デバイスには書かない。root 不要) |
| `--overwrite-images` | `--keep-images DIR` に既にあるイメージ一式を作り直す |
| `--writer {auto,nbdcopy,dd}` | 流し込みの方法 (既定 auto: nbdcopy + nbdkit が使えれば nbdcopy) |
| `--sparse-write` | ゼロの塊を書き飛ばす。**書き込み前に各パーティションの範囲を事前にゼロ化する** (`blkdiscard -z`、無ければ `dd`)。事前ゼロ化をしないと古いデータが FS の未使用領域に残る。ゼロ化が速い機器以外は、むしろ遅くなる。既定は off (ゼロも含めて全部書く) |
| `--progress` / `--no-progress` | 流し込みの進捗表示 (端末なら既定で表示) |
| `--skip-image-check` | `--flash-only` で、流す前のイメージの SHA-256 再確認を省く (読み戻し検証は行う) |
| `--ignore-image-checks` | イメージ作成時の検査 (fsck 相当) が NG でも流す |
| `--exfat-method {auto,mount,fuse}` / `--luks-method {auto,open,xts}` | 「0. 方式」の表。auto は root なら従来どおり (mount / open)、root でなければ root 不要の方式 |
| `--backup-dir DIR` | 元のパーティション表・先頭 4 MiB・末尾 1 MiB の保存先 (既定 `./usbmem_testset_backup/<プロファイル>_<日時>`) |
| `--full-backup` | デバイス全体のイメージも保存 (空き容量が必要) |
| `--manifest-dir DIR` | マニフェストの保存先。`DIR/<プロファイル>/<パーティション名>.sha256` (既定 `./usbmem_testset_manifests`) |
| `--work-dir DIR` | 一時ファイル置き場 (投入ツリーなど。既定 `./usbmem_testset_work`)。4.5 GiB のファイルを作るので **5 GiB 以上の空き**が要る。tmpfs (メモリ) 上には置かないこと。イメージ本体は `--keep-images` (無ければここの `images_<プロファイル>`) |
| `--log FILE` | 実行したコマンドと出力のログ (既定 `./usbmem_testset.log`) |
| `--dirty-ntfs` | プロファイル A の NTFS を**ダーティフラグ付き**にする (5 の最後の節) |
| `--no-big-files` | 1 GiB / 4.5 GiB のファイルを作らない |
| `--small-count N` | 小さいファイルの数 (既定 3000、256〜512 MiB の FS は 300) |
| `--luks-pbkdf {argon2id,pbkdf2}` | LUKS2 の KDF (既定 argon2id・メモリ 64 MiB・並列 1) |
| `--f2fs-method {sload,mount}` | F2FS への投入方法 (既定 `sload.f2fs`。root 不要) |
| `--udf-method {mkudffs,genisoimage}` | UDF の作り方 (既定 `mkudffs` + mount。`genisoimage` は mount 不要だが ISO9660+UDF ブリッジ) |
| `--bitlocker {synth,skip}` | C の BitLocker 風パーティション。`skip` なら作らない (中身は空) |
| `--iso-tool {genisoimage,xorriso}` | ISO を作るツール |
| `--skip-mount-fs` | root の `mount` / `cryptsetup open` が要る FS (exFAT・UDF・LUKS の中身・F2FS の mount 方式) の中身投入を省く。FS 自体は作る |
| `--verify-content` / `--no-verify-content` | イメージを作った直後に中身を取り出してマニフェストと照合する (既定で有効。デバイスではなくイメージに対して行うので速い) |
| `--only p2_ext4,...` | 指定したパーティションのイメージだけ作る。`--keep-images` の既存のイメージ一式があれば差し替える |
| `--image FILE` ほか | 「8. イメージモード」 |

必要な空きの目安 (dry-run に表示。余裕を見た値): イメージ A 約 4 GiB / B 約 7.2 GiB / C 約 16 GiB + 投入ツリー (A 約 1.3 GiB / B・C 約 4.8 GiB)。`--no-big-files` ならイメージは A 約 0.7 GiB / B 約 1.1 GiB / C 約 5 GiB、投入ツリーは 0.3 GiB。実測 (疎ファイルの実使用量) は A 3.1 GiB / B 5.7 GiB / C (`--no-big-files`) 3.3 GiB。LUKS と BitLocker 風は全面が非ゼロなのでパーティションの大きさ分かかる。旧方式の「作業ディレクトリに 5 GiB」に比べ、置き場が増える代わりに、デバイスへの書き込みは一度きりになる。

## 4. 構成表

先頭 1 MiB を空け、全パーティションの開始・サイズは 1 MiB の倍数。末尾 1 MiB は残す (GPT のバックアップ領域に十分)。サイズは `tools/make_real_usb_testset.py` の `PROFILES` の定数で、容量に合わせて変えてよい。

### A (7.8 GiB, GPT)

| # | 名前 | FS | サイズ | 内容 |
|---|---|---|---|---|
| 1 | `p1_ntfs` | NTFS | 3 GiB | 1 GiB ファイル入り。`--dirty-ntfs` でダーティ状態にできる |
| 2 | `p2_ext4` | ext4 (ジャーナル付き、クリーン) | 2 GiB | 1 GiB ファイル入り |
| 3 | `p3_fat32` | FAT32 | 1 GiB | 4 GiB 超・1 GiB なし |
| 4 | `p4_exfat` | exFAT | 残り (約 1.4〜1.8 GiB) | 1 GiB ファイル入り (残りが 1.4 GiB 未満のメモリでは省略) |

### B (16 GiB, MBR)

| # | 名前 | FS | サイズ | 内容 |
|---|---|---|---|---|
| 1 | `p1_fat32` | FAT32 (プライマリ) | 1 GiB | |
| 2 | `p2_ntfs` | NTFS (プライマリ) | 5 GiB | **4.5 GiB のファイル**入り (1 GiB は入らないので作らない) |
| 3 | `p3_exfat` | exFAT (プライマリ) | 3 GiB | 1 GiB ファイル入り |
| 4 | (拡張) | 拡張パーティション | 3 GiB + 論理ごとの EBR 1 MiB | |
| 5 | `p5_ext2` | ext2 (論理) | 1 GiB | |
| 6 | `p6_ext3` | ext3 (論理) | 1 GiB | |
| 7 | `p7_ext4` | ext4 (論理、ジャーナル付き) | 1 GiB | **`needs_recovery` が立った状態** (空のジャーナルだが、マウント時にリカバリが要る) |
| | 未割り当て | | 約 4 GiB | |

### C (31 GiB, GPT)

| # | 名前 | FS | サイズ | 内容 |
|---|---|---|---|---|
| 1 | `p1_xfs` | XFS (v5) | 5 GiB | **4.5 GiB のファイル**入り |
| 2 | `p2_btrfs` | Btrfs | 6 GiB | **4.5 GiB のファイル**入り (メタデータ DUP の分を見て大きめ) |
| 3 | `p3_f2fs` | F2FS | 3 GiB | 1 GiB ファイル入り |
| 4 | `p4_luks` | LUKS2 (中に ext4) | 2 GiB | パスフレーズ `usbmem-test-pass`。1 GiB ファイル入り |
| 5 | `p5_bitlocker` | BitLocker 風 (中に NTFS) | 1 GiB | 合成イメージ (6 の「BitLocker 風パーティションについて」)。パスワード `synth-Password-1` |
| 6 | `p6_squashfs` | squashfs | 256 MiB | |
| 7 | `p7_erofs` | erofs | 256 MiB | |
| 8 | `p8_udf` | UDF | 512 MiB | |
| 9 | `p9_iso` | ISO9660 (Joliet + Rock Ridge) | 512 MiB | |
| | 未割り当て | | **約 12.5 GiB** (31 GiB の場合。許容下限 28.5 GiB でも 10 GiB 以上) | パーティション編集機能の検証用 |

> 4.5 GiB のファイルを XFS / Btrfs に限ったのは、要件の「FAT32 不可、B の NTFS と C の XFS / Btrfs だけ」による。要件書の例どおりの 3 GiB では 4.5 GiB が入らないため、XFS を 5 GiB、Btrfs を 6 GiB にした。

### GPT のタイプと GUID

NTFS・FAT32・exFAT・BitLocker 風・UDF・ISO は `0700` (Microsoft basic data)、LUKS は `8309`、それ以外は `8300`。ディスク GUID・パーティション GUID・FS の UUID・ボリューム ID・LUKS UUID はすべてプロファイルとパーティション名から決まる固定値 (何度作っても同じ)。MBR のディスク署名も固定。

## 5. テストデータ (全 FS 共通、固定シードで決定的)

各 FS のルートに `README.txt` と `MANIFEST.sha256` を置き、同じマニフェストを PC 側の `--manifest-dir` にも保存する。内容はファイル名と ID だけで決まる (毎回同じバイト列)。

| ディレクトリ | 内容 |
|---|---|
| `00_sizes/` | 0 B、1 B、4095、4096、4097、1 MiB−1、1 MiB、1 MiB+1 バイトのファイル |
| `01_japanese/` | 濁点 (NFC と、清音+結合濁点の NFD)、半角カナ (半角濁点・半濁点)、全角英数と全角空白、空白と記号入りの名前、`.隠しファイル`、拡張子なし、BMP 外の文字 `𠮷` (FAT32 と ISO を除く)、255 バイトに近い長い名前、深い日本語ディレクトリ、空のディレクトリ |
| `02_big/` | `gib_1GiB.bin` (1 GiB。容量に余裕のある FS のみ)、`big_4p5GiB.bin` (4.5 GiB。B の NTFS と C の XFS / Btrfs のみ) |
| `03_small/` | 小さいファイル 3000 個 (0〜2048 バイト、250 個ずつのサブディレクトリ。256〜512 MiB の FS は 300 個) |
| `04_deep/` | 深さ 20 階層 (`d01/.../d20/leaf.txt`) |
| `05_case/` | `Case.txt` / `case.txt` / `CASE.txt` (大文字小文字だけ違う同名。区別できる FS のみ) |
| `06_links/` | `target.txt`、symlink 4 つ (相対・ディレクトリ・ぶら下がり・日本語名)、ハードリンク (`hard_a.dat` = `hard_b.dat`)。対応する FS のみ |
| `07_sparse/` | 疎ファイル (64 MiB、256〜512 MiB の FS は 8 MiB)。先頭 / 1 MiB+17 / 末尾付近だけデータで、あとは穴。対応する FS のみ |

FS ごとの有無: 大文字小文字の区別は ext*・XFS・Btrfs・F2FS・LUKS(ext4)・squashfs・erofs・UDF が対象 (NTFS・FAT32・exFAT・BitLocker 風・ISO は除く)。symlink は NTFS・FAT32・exFAT・BitLocker 風を除く全て。ハードリンクは FAT32・exFAT・ISO を除く全て。疎ファイルは NTFS・ext*・XFS・Btrfs・F2FS・LUKS・squashfs。全ファイル・ディレクトリの mtime は 2024-01-01T00:00:00Z に固定。

### マニフェストの形式 (`MANIFEST.sha256`)

タブ区切り。`#` で始まる行はコメント。

```
F <サイズ> <sha256> <パス>            通常ファイル (疎ファイルは末尾に sparse。sha256 は穴を 0 として計算した論理内容)
D 0 - <パス>                           ディレクトリ
L 0 - <パス> <リンク先>                symlink
H <サイズ> <sha256> <パス> <同じ inode のパス>   ハードリンク
```

`MANIFEST.sha256` 自身はマニフェストに入れない。`README.txt` は入る。

### 照合: `verify` サブコマンド

```sh
python3 tools/make_real_usb_testset.py verify MANIFEST.sha256 /media/usb/ntfs --allow-extra
```

サイズと sha256 をすべて確認し、symlink のリンク先とハードリンク (同一 inode) も確認する (取り出し方によってリンクが失われる場合は `--no-links`)。マニフェストに無いファイルは「余分」として報告する (書き込み試験で作ったファイルは `--allow-extra` で許容)。`lost+found`、`System Volume Information` などの OS が作るものは無視する。

### `--dirty-ntfs` の仕組み

NTFS のボリュームを ntfs-3g で作って中身を書き、アンマウントして `ntfsfix -n` と中身の照合を済ませた**後**で、`$Volume` の `$VOLUME_INFORMATION` のフラグの bit 0 (`VOLUME_IS_DIRTY`) を、MFT レコードの更新シーケンス (fixup) を壊さずに直接立てる (`$MFTMirr` の写しも同じにする。片方だけだと「`$MFTMirr` が `$MFT` と一致しない」という別の異常になる)。作成直後に `ntfsinfo -m` が `Volume is scheduled for check` と答える (= 独立実装がダーティと認識する) ことを確認する。Linux の ntfs3 や Windows はこの状態を「前回きちんと取り外されていない」と見なす。**Android アプリの「ダーティな NTFS は書き込ませない」挙動の検証用**。

注意: PC に戻した後で **`ntfs-3g` を書き込みでマウントすると、ntfs-3g はこのフラグを無視してマウントし、アンマウント時にフラグを消してしまう**。ダーティ状態を保ちたいときは PC では読み取り専用で扱う (`ntfsinfo` / `ntfsls` などの libntfs-3g のツールは `Volume is scheduled for check` と答えて開かない)。PC で意図的に直すには `ntfsfix -d <パーティション>` か Windows の `chkdsk`。このフラグを立てた NTFS は書き込み試験には使えないので、既定はクリーン。

## 6. 各 FS の作り方と制約

| FS | 作り方 | 補足 |
|---|---|---|
| ext2/3/4 | `mke2fs -t <fs> -d <ステージ>` (UUID・hash_seed 固定、`lazy_itable_init=0`) | B の ext4 は fsck 後に `debugfs -w -R "feature needs_recovery"`。直後に `dumpe2fs -h` で確認 |
| FAT32 | `mkfs.fat -F 32` + `mcopy -s` | mtools は BMP 外の文字を落とすので `𠮷` は入れない。symlink / hardlink / 疎ファイル / 大文字小文字違いなし |
| exFAT | `mkfs.exfat` + 投入 | `mount` 方式: カーネル exFAT で mount (root。`--skip-mount-fs` で省略可)。`fuse` 方式: `mount.exfat-fuse` (root 不要)。exfat-fuse 1.4.0 はボリューム GUID 付きの exFAT を拒否するので、fuse 方式では GUID なしで作って投入・照合を済ませた後に `tune.exfat -U` で GUID を付け、`fsck.exfat -n` で確認する。fuse の `user=` オプションは `LD_PRELOAD` の小さな shim (`tools/exfat_fuse_shim.c`) で除く |
| NTFS | `mkntfs -F -Q` + `ntfs-3g` (FUSE) で mount して書く (`windows_names`) | root 不要 (FUSE が使えれば)。ハードリンク可・疎ファイル可・symlink なし |
| XFS | `mkfs.xfs -p <ステージ>` | v5。`-p` は疎ファイルを穴のまま作らない (全部書く) |
| Btrfs | `mkfs.btrfs --rootdir <ステージ>` | 単一デバイス、メタデータ DUP |
| F2FS | `mkfs.f2fs` + `sload.f2fs -f <ステージ>` | `--f2fs-method mount` なら mount して書く (root) |
| LUKS2 | `open` 方式: `cryptsetup luksFormat --type luks2` + `open` + `mke2fs -t ext4 -d` + `close` (root)。`xts` 方式: `luksFormat` (ファイルに) → マスター鍵を `luksDump --dump-volume-key` で取得 → 内側 ext4 を平文ファイルに作る → Python で AES-256-XTS 暗号化して書く (root 不要) | argon2id・メモリ 64 MiB。xts 方式は 2 GiB で約 1 分。暗号化の向き・IV (plain64)・鍵の分割は、AES-ECB だけで手組みした XTS と数セクタ照合して確かめる。**本物の `cryptsetup open` で開けるかは root でないため未確認** (「9」) |
| BitLocker 風 | NTFS を作って書く → `tools/bitlocker_synth.py` (XTS-AES-128、パスワード保護) | **実 Windows 製ではない合成**。下記 |
| squashfs | `mksquashfs -all-root -no-xattrs -b 131072` (gzip) | 作者の時刻を固定 |
| erofs | `mkfs.erofs -T <固定> -U <UUID> --all-root` (無圧縮) | 圧縮版が必要なら別途 |
| UDF | `mkudffs --utf8 --blocksize=<論理セクタ> --media-type=hd` + `mount -t udf` で書く | root が必要。`--udf-method genisoimage` なら root 不要だが ISO9660+UDF 1.02 ブリッジ (純粋な UDF ではない) |
| ISO9660 | `genisoimage -J -R -joliet-long -input-charset utf-8 -iso-level 3` | Joliet は 64 文字・BMP のみなので、名前を 60 文字以内にし `𠮷` は入れない。大文字小文字違い・ハードリンク・疎ファイルなし |

### BitLocker 風パーティションについて

`tools/bitlocker_synth.py` は合成ライター。以前は libbde に拒否されて解錠できなかったが、2026-11-13 に原因 (暗号化済みサイズに退避領域を含めていなかった) を直し、libbde・アプリ (実機を含む) とも開けることを確認した。詳細は `docs/crypto-volumes.md` 6.1。

**Windows で作る場合** (実物。こちらを推奨): `--bitlocker skip` で作ると 1 GiB のパーティション (タイプ `0700`、中身は空) が残る。それを Windows の「ディスクの管理」で NTFS にフォーマット (ボリュームラベル `TS_BITLOCKER`) し、いくつかのファイルを置いてから「BitLocker を有効にする」(パスワード保護) を実行する。Windows 側で `Get-FileHash -Algorithm SHA256` した一覧を manifest 代わりにする (`tools/winvhd/make_bitlocker_matrix.sh` の手順が参考になる)。

## 7. 安全機構

1. **既定は dry-run**。`--execute` + (`--profile` と `--device` / `--image` / `--build-only`) か `--flash-only` がそろわなければ何も書かない。**実デバイスに触るのは (b) の確認より後だけ**: (a) のイメージ作成中は USB に一切触らない (デバイスの検証は読み取りのみ)。(a) が失敗または検査 NG なら確認にも進まない。
2. **デバイスの検証** (`validate_device`)。1 つでも満たさなければ理由を表示して中止する (この時点では何も書かれていない)。
   - ブロックデバイスであり、`lsblk` の種別が `disk` (パーティション・loop は不可)
   - `lsblk` の TRAN が `usb` (`sata` `ata` `nvme` `sas` `mmc` などの内蔵バス、空・不明は拒否)
   - 読み取り専用でない。論理セクタが 512 または 4096
   - サイズがプロファイルの期待値 (7.8 / 16 / 31 GiB) の ±8% 以内
   - どのパーティション (LUKS などの子デバイスを含む) も**マウントされていない**、スワップでもない (`lsblk` と `/proc/mounts` の両方で確認)
   - **システムのあるディスクでない**: `/` `/home` `/boot` `/boot/efi` `/efi` `/usr` `/var` `/etc` `/opt` `/root` `/srv` `/nix` `/var/log` `/tmp` のマウント元 (LVM / dm-crypt を辿って親ディスクまで) が対象ディスクを含んでいたら拒否。`/` のマウント元がディスクに辿れない (overlay、ZFS など) ときは安全側で拒否
   - root で実行している
3. **デバイスパスの手入力確認**。デバイス名・モデル・シリアル・サイズ・現在のパーティション一覧 (`lsblk`) を表示し、`--device` と同じパスを入力しなければ中止。入力待ちの後で検証をもう一度行う。`--yes` のような回避オプションは無い。
4. **バックアップ** (破壊的操作の前、`--backup-dir`): `sgdisk_backup.bin` (`sgdisk --backup`)、`sfdisk_dump.txt` (`sfdisk -d`)、`head_4MiB.bin`、`tail_1MiB.bin`、`lsblk.json`、`--full-backup` なら `full.img`。バックアップに失敗したら中止。
5. **途中失敗**: 失敗した時点で即中止し、完了した手順とバックアップの場所、中途半端な状態であることを表示する。イメージは残るので、`--flash-only DIR` で**同じイメージをそのまま流し直せる** (流し込みは表が最後なので、途中で止まったデバイスは「有効な表が無い」状態。再実行は既存の表を消して最初から書く)。
6. **読み戻し検証**: 流した範囲を O_DIRECT で読み戻し (`blockdev --flushbufs` でキャッシュを捨ててから)、SHA-256 をイメージのものと比べる。1 つでも不一致なら失敗扱い。
7. **同時実行の禁止**: 実デバイスへの流し込みは排他ロックで 1 本ずつ (ハブの帯域を取り合って固まるのを避ける)。2 本目は即中止。イメージ作成だけ (`--build-only`) は同時に動かしてよい。
8. **メモリ**: 大きなバッファを持たない (1 回 4 MiB の `mmap` バッファだけ)。作業ディレクトリ・イメージ置き場がメモリ上 (tmpfs / ライブ環境の overlay) なら、必要量が空きメモリの半分を超える場合に中止 (`--allow-ram-workdir` で承知の上の続行)、そうでなくても警告する。
9. **ログ**: 実行した全コマンドと出力を `--log` に追記。

## 8. イメージモード (開発側の検証用)

`--image FILE --profile X --execute` は、実デバイスの代わりに**通常ファイル** (疎ファイル) へ、(a) で作ったイメージを (d) と同じ経路 (nbdcopy + nbdkit offset フィルタ、または dd) で流し込み、(e) で読み戻して SHA-256 を照合する。デバイスの検証・手入力確認・バックアップは行わない (実デバイスではないため)。`/dev` 配下 (`/dev/shm` 以外) やデバイスファイルは拒否する。(d)(e) も root は不要。新しいファイルはゼロなので、ゼロの塊は書き飛ばす (`--destination-is-zero`)。`--flash-only DIR --image FILE` なら、作ってあるイメージ一式を 1 枚のディスクイメージに合成できる (AVD・ホスト検証用)。

```sh
python3 tools/make_real_usb_testset.py --profile B --image /var/tmp/B.img --execute --work-dir /var/tmp/usbmem_work --keep-images /var/tmp/usbmem_img_B
```

## 9. 検証したこと・できなかったこと

開発側 (一般ユーザー、`sudo -n` はパスワードが必要で使えない = root が要る操作はできない) で行ったこと。

**できたこと**

- dry-run が 3 プロファイルで期待どおり: 整列 (開始・サイズが 1 MiB の倍数)、合計が容量に収まる、未割り当て (A: 1.2 MiB / B: 4 GiB / C: 12.5 GiB。容量を ±8% 動かしても C は 10 GiB 以上)。
- 単体テスト `tools/test_make_real_usb_testset.py` (108 件 + 結合テスト 2 件): 配置、`sgdisk` / `sfdisk` 入力、バックアップのコマンド列、**実行順序 (イメージ作成 -> 確認 -> バックアップ -> 消去 -> 流し込み -> 読み戻し。作成が失敗・検査 NG なら確認にも進まない)**、**流し込み範囲の計算** (パーティションの範囲、MBR の EBR 領域、GPT のバックアップ位置、重ならない・デバイス内、未割り当ての補集合、表が最後)、**ゼロ領域の扱い** (既定はゼロも書く、`--sparse-write` は事前ゼロ化とセット、dd/nbdcopy のオプション)、**`--flash-only` のメタデータ検査** (別容量・別プロファイル・不完全・配置違いを拒否、SHA-256 の改変検出)、nbdcopy 引数の組み立て、root 不要方式のコマンド列、LUKS XTS と手組みの独立実装の一致、ファイルをデバイス代わりにした dd 流し込みと読み戻し (壊すと不一致を検出)、**デバイス拒否** (内蔵 NVMe・SATA、マウント済み、サイズ不一致、TRAN 不明、システム / ホームが対象ディスク上、ルート不明、パーティション指定、読み取り専用)、手入力確認、`--yes` 系オプションが存在しないこと、確認が通るまで破壊的コマンドが 1 つも走らないこと、バックアップが `wipefs` / `sgdisk --zap-all` より先であること、触るデバイスが対象だけであること、データジェネレータ (決定性、各 FS の有無、マニフェスト照合が改ざん・欠落・余分を検出)、NTFS ダーティフラグの書き込み (実 `mkntfs` イメージ、元に戻ること)。
- **イメージモード** (`--skip-mount-fs`) で A / B / C を実際に作成し、パーティション表 (`sgdisk -p` / `sfdisk -l`) と各 FS を確認:
  - 検査 (`fsck` 相当): FAT32 `fsck.fat -n`、ext2/3/4 `e2fsck -fn`、NTFS `ntfsfix -n`、exFAT `fsck.exfat -n`、XFS `xfs_repair -n`、Btrfs `btrfs check`、F2FS `fsck.f2fs`、erofs `fsck.erofs`、squashfs `unsquashfs -s`、ISO `isoinfo -d`、UDF `udfinfo`、BitLocker 風 は署名 `-FVE-FS-` を確認。
  - **中身をマニフェストと全ファイル照合 (サイズ + sha256)**: FAT32 (`mcopy`)、ext2/3/4 (`debugfs rdump`)、NTFS (`ntfs-3g` を FUSE で読み取りマウント。ハードリンクも確認)、Btrfs (`btrfs restore`)、squashfs (`unsquashfs`。symlink・ハードリンクも確認)、erofs (`fsck.erofs --extract`。同上)、ISO (`xorriso -osirrox`)。4.5 GiB のファイルも Btrfs と B の NTFS で照合した。
  - B の ext4 に `needs_recovery` が立っていること、`--dirty-ntfs` で NTFS のダーティフラグが立つこと (`0x0000 → 0x0001`)。
  - 疎ファイルのイメージを実デバイスに見立てた位置へ穴を飛ばしてコピーした後の FS が壊れていないこと (別手順で `e2fsck` / `mdir` / NTFS フラグを再確認)。
- 結合テスト (`USBMEM_TESTSET_INTEGRATION=1 python3 tools/test_make_real_usb_testset.py`。イメージモードで A (`--dirty-ntfs`) と B を実際に作る。`USBMEM_TESTSET_TMP` で置き場を指定) も通した。
- **イメージ経由の全体** (一般ユーザー、すべて root なし):
  - A / B / C を `--build-only` で作成。パーティション表 (`sgdisk -p` / `sfdisk -l`) は配置・GUID・ディスク署名とも期待どおり (配置の計算 `compute_layout`、表のコマンド `table_cmds`、テストデータの生成 `build_dataset` は旧方式のコードのまま変えていないので、旧方式と同じ表・同じデータになる)。FS ごとの検査と、中身のマニフェスト全ファイル照合 (サイズ + sha256) は旧方式と同じ項目が通った。exFAT は **fuse 方式で投入と照合まで**、LUKS2 は **xts 方式で内側 ext4 の中身の照合まで**、UDF は genisoimage 方式、を含む (旧方式では root が必要で未確認だった項目)。
  - `--image` (nbdcopy + nbdkit offset フィルタ) への流し込みと読み戻し: A / B / C の全範囲で SHA-256 一致。`--flash-only` も同様。容量違い・プロファイル違いのデバイスは何も書かずに拒否されること。
  - **通常ファイルをデバイスに見立てた**流し込み (O_DIRECT 経路、ゼロも含めて全部書く既定、`--sparse-write` の事前ゼロ化 + `--destination-is-zero`、`--writer dd`): 古いデータ (乱数) を入れた 7.8 GiB のファイルへ書き、全範囲が一致し、未割り当て領域が無傷であること。
- 時間 (ローカル NVMe 上): イメージ作成は A 44 秒 (1 GiB ファイル込み)・B 73 秒 (4.5 GiB ファイル込み)・C 136 秒 (`--no-big-files`。うち LUKS の XTS 暗号化 67 秒)。ファイルへの擬似流し込みは 7.8 GiB で 8 秒 (約 1 GiB/s)、読み戻しは約 600 MiB/s (A 13 秒・B 21 秒・C 31 秒)。実 USB は書き込み速度で決まる (dry-run に 15 / 30 / 60 MiB/s での目安が出る。A 7.8 GiB で 2〜9 分、C 18.5 GiB で 5〜21 分、読み戻しは別にほぼ同じ量)。

**できなかったこと (root が必要、または実デバイスが必要)** — ユーザーが実機で行う。いずれも「**未検証**」。

- **実デバイスへの流し込みと読み戻し** (root): `nbdcopy` の実デバイスに対する O_DIRECT 書き込み、`blockdev --flushbufs`、`blkdiscard -z`、`wipefs`、バックアップ、デバイスの検証 (`lsblk` の実出力)。ファイルで代用して検証したのは、nbdkit/dd の引数とデータの流れまで。
- **LUKS2 の本物の `cryptsetup open`**: xts 方式で作ったイメージを実際に開けるかは未確認 (root が必要)。ヘッダは `cryptsetup luksFormat` 製で、暗号化は独立実装との照合まで。初回は実機に流す前に `sudo cryptsetup open --test-passphrase` と、開いて `mount -o ro` して `verify` で確認すること。
- UDF の `mkudffs` 方式 (`mount -t udf`)、exFAT の `mount` 方式、`--f2fs-method mount` (既定の `sload.f2fs`、exFAT fuse、UDF genisoimage は確認)。
- 実デバイスに対する一連の流れ全体 (udev、GUI の自動マウントの影響、USB での所要時間)。新方式は流し込み後にパーティション表を再読み込みしないので、取り外して挿し直してから `lsblk` で確認すること。
- XFS と F2FS と BitLocker 風 の**中身の取り出しによる照合** (取り出せる非 root ツールが無いため `xfs_repair -n` / `fsck.f2fs` / 署名確認まで)。実機で PC に戻したときに `verify` で照合すること。
- BitLocker 風を Android アプリで開けるか (6 の「BitLocker 風パーティションについて」を参照)。

所要時間の目安 (イメージ作成・ローカル SSD): A 約 45 秒、B 約 1.2 分、C 約 2.3 分 (`--no-big-files`、UDF は genisoimage)。実 USB へ流す量は、**ゼロも含めたパーティションの合計**: A 7.8 GiB・B 12 GiB・C 18.5 GiB (読み戻しで同じ量を読む)。従来の mkfs 直書きより総量は増えるが、書き込みが順次の 1 回で、PC 側のキャッシュ負荷 (ダーティページ) も `O_DIRECT` で抑えられる。実 USB での速度比較は未計測。

## 10. Android 実機での検証手順

アプリは開発中のため、以下は**確認項目**。画面の名称はアプリの現状に合わせて読み替える。

1. 作成した USB メモリを OTG ケーブル (できれば給電付きハブ) で Android 実機に接続し、アプリで USB 権限を許可する。
2. **パーティション一覧**: A は GPT (`p1_ntfs` …)、B は MBR + 拡張 (5〜7 が論理)、C は GPT 9 個 + 末尾の未割り当て (約 12.5 GiB)。名前 (GPT のパーティション名)、種別、FS ラベル (`TS_NTFS` など)・UUID が表示と合うか。
3. **読み取り**: 各パーティションをマウントし、`README.txt` と `MANIFEST.sha256` を開く。可能なら全ファイルをコピーして `verify` で照合する (「11. 戻した後」)。確認する点:
   - 日本語名 (濁点の NFC / NFD、半角カナ、長い名前) が文字化けせず、両方見える
   - 0 B、1 B、4095〜4097 B、1 MiB±1 B の中身
   - 3000 個のファイルの一覧と、20 階層の深いパス
   - 大きいファイル (1 GiB、**4.5 GiB**: B の NTFS、C の XFS / Btrfs) の末尾まで読めて sha256 が一致
   - 疎ファイルの穴が 0 として読める
   - symlink / ハードリンク (対応 FS)、大文字小文字違いの同名 (区別できる FS)
4. **書き込み (主に A)**: NTFS / ext4 / exFAT / FAT32 に新規ファイル・ディレクトリ・上書き・削除・リネームを行う。取り外す前にアプリでアンマウント/同期を実行する。
5. **ダーティな NTFS** (`--dirty-ntfs` で作ったもの): 書き込みマウントが拒否される (または読み取り専用になる) こと。
6. **`needs_recovery` の ext4** (B の `p7_ext4`): ジャーナル未回復の ext4 を書き込みで開こうとしたときの挙動 (拒否、または読み取り専用で `noload`)。
7. **LUKS2** (C の `p4_luks`): パスフレーズ `usbmem-test-pass` で開けるか、間違えたときに認証エラーになるか。開いた中の ext4 を読めるか。
8. **BitLocker 風** (C の `p5_bitlocker`): パスワード `synth-Password-1` で開けるか (6 の「BitLocker 風パーティションについて」を参照)。
9. **パーティション編集** (C の末尾 12.5 GiB の未割り当て): 新規パーティションの作成、サイズ変更、削除、バックアップと復元。**C の USB は使い捨てなので壊してよい**が、編集前に `--backup-dir` のバックアップがあることを確認する。
10. 取り外すときは必ずアプリでアンマウントしてから抜く。

### 10.1 自動検証: 計装テスト `RealUsbVerifyTest` / `RealUsbWriteTest`

10 の「読み取り」の確認項目 (全ファイルの照合) を、人手を介さず**アプリのプロセス内**で行う計装テスト。DocumentsProvider は adb シェルから読めないので、`UsbSessionManager` で USB を開き、ネイティブの `NtfsVolume` API で直接読む。通常の AVD の試験では `assumeTrue` で skip される (`-e usbmem.real true` が無いと動かない)。

```sh
# 読み取り専用の照合 (書き込みは一切しない)。adb のシリアルは必須で、既定は無い (明示した端末にしか触れない)
tools/run_android_real_usb_verify.sh --serial <adb serial> [--devices 0930:6544[,VID:PID..]] [--mode sequential|parallel] \
    [--only 1,2] [--skip-hash 02_big/big_4p5GiB.bin] [--perm 120] [--tag NAME] [--no-build] [--no-install]
```

- 対象: 接続中の USB マスストレージ (`--devices` で VID:PID を絞る)。USB の許可が無ければ `--perm 秒` でダイアログを出して待つ (OK は手でタップ)。OS が自動マウントした USB は先に `adb shell sm unmount public:X,Y` で外す (アプリは OS のマウント中は開くのを拒否する)。
- 動作: `UsbSessionManager.open` (読み取り専用) → `partitionTable()` → 各パーティションを FS 自動判定で読み取り専用マウント (`mountNtfs(.., writable=false)`。ジャーナル未再生の ext は EUCLEAN(117) の後に `allowNoload=true` で再試行 = UI の許可フロー相当) → ルートの `MANIFEST.sha256` を読み、全エントリのサイズ・SHA-256・symlink のリンク先・ハードリンクの同一 inode を照合し、余分なファイルを列挙 (失敗にはしない。`lost+found` など `verify_tree` の `IGNORED_EXTRA` はルート直下だけ無視)。LUKS2 / BitLocker は `CryptoVolume` でテスト用の固定パスワード (`usbmem-test-pass` / `synth-Password-1`) を試す (`nativeDeviceSubRange` でパーティションを切り出して渡す。`-e usbmem.luksPass` で変更可)。マウントに失敗したら errno と理由を記録して続行する。
- `--mode sequential` は 1 本ずつ開いて読んで閉じる。`parallel` は全部開いてから、USB ごとのスレッドで同時に読む。
- 結果: logcat のタグ `RealUsbVerify` (失敗と要約のみ詳しく、4.5 GiB のファイルは 512 MiB ごとに進捗) と、アプリの `files/real_usb_report_<tag>.json` (スクリプトが `run-as` で `real_usb_reports/<tag>.json` に回収し、要約表を表示する)。

書き込み試験 `RealUsbWriteTest` (`--write --devices <VID:PID 1 つ> --part <番号> [--stage createonly|full|both]`):
書ける FS は NTFS / exFAT / ext2 / ext3 / ext4 だけ (FAT32・読み取り専用 FS・MBR の ext4 = `needs_recovery` の固定イメージは、指定されても拒否)。1 回 1 パーティション。
(1) 読み取り専用で全照合 + 自前の fsck (`DiskCheck`、読み取り専用) → (2) `CreateOnly` で専用フォルダ `usbmem_cotest_<時刻>` を作って書き、既存ファイルへの書き込み・縮小・名前変更・削除が拒否されることを確認 (CreateOnly は削除できないので、このフォルダは次の段階まで残る) → 閉じて読み取り専用で再オープン + 照合 + fsck → (3) `Full` で、そのフォルダを消し、`WriteSelfTest.run` (専用フォルダの中だけで作成・書き込み・上書き・縮小・名前変更・移動・削除 + 毎回読み戻し + 後片付け) と 32 MiB の速度計測 → 閉じて読み取り専用で再オープン + 全照合 + fsck + 残骸なしの確認。ダーティ / `needs_recovery` が立っていれば不合格。失敗したらその場で止まる (残骸は読み取り専用で一覧するだけで、手では消さない)。**この節の実測では、書き込み試験は実行していない** (下の「実測」)。

#### 実測 (A80 Pro, Android 9, USB ハブ経由、読み取り専用)

3 本の対応: **A** = TransMemory `0930:6544` (GPT, 7391 MiB)、**B** = USB Storage `05e3:0751` (MBR, 14924 MiB)、**C** = USB DISK `090c:2000` (GPT, 30000 MiB)。USB 2.0 級の速度 (1 本ずつで 15〜29 MB/s)。1 本ずつ開いて読んだ結果 (全て照合 OK、問題 0):

| USB | # | FS | マウント | 照合したファイル / ディレクトリ / symlink / ハードリンク | SHA-256 した量 | 読み取り速度 | 所要 | 余分 |
|---|---|---|---|---|---|---|---|---|
| A | 1 | NTFS | OK | 3027 / 42 / - / 1 | 1093 MiB | 18.0 MB/s | 74 s | 0 |
| A | 2 | ext4 | OK | 3030 / 43 / 4 / 1 | 1093 MiB | 18.8 MB/s | 68 s | 0 |
| A | 3 | FAT32 | OK | 3022 / 40 / - / - | 5 MiB | 3.0 MB/s | 10 s | 6 (OS が作った `LOST.DIR`・`Android/data/...`) |
| A | 4 | exFAT | OK | 3023 / 40 / - / - | 5 MiB | 1.1 MB/s | 14 s | 0 |
| B | 1 | FAT32 | OK | 3022 / 40 / - / - | 5 MiB | 2.0 MB/s | 12 s | 6 (同上) |
| B | 2 | NTFS | OK | 3027 / 42 / - / 1 (4.5 GiB 含む) | 4677 MiB | 17.4 MB/s | 294 s | 0 |
| B | 3 | exFAT | OK | 3024 / 41 / - / - | 1029 MiB | 15.6 MB/s | 80 s | 0 |
| B | 5 | ext2 | OK | 3029 / 42 / 4 / 1 | 69 MiB | 63.5 MB/s | 7 s | 0 |
| B | 6 | ext3 (ext4 ドライバ) | OK | 3029 / 42 / 4 / 1 | 69 MiB | 66.4 MB/s | 7 s | 0 |
| B | 7 | ext4 (`needs_recovery`) | OK (`noload` の読み取り専用のみ) | 3029 / 42 / 4 / 1 | 69 MiB | 63.1 MB/s | 8 s | 0 |
| C | 1 | XFS | OK | 3030 / 43 / 4 / 1 (4.5 GiB 含む) | 4677 MiB | 28.8 MB/s | 179 s | 0 |
| C | 2 | Btrfs | OK | 3030 / 43 / 4 / 1 (4.5 GiB 含む) | 4677 MiB | **9.0 MB/s** | 550 s | 0 |
| C | 3 | F2FS | OK | 3030 / 43 / 4 / 1 | 1093 MiB | 26.8 MB/s | 50 s | 0 |
| C | 4 | LUKS2 (中は ext4) | OK (解錠 20.6 s) | 3030 / 43 / 4 / 1 | 1093 MiB | **4.5 MB/s** | 281 s | 0 |
| C | 5 | BitLocker 風 | **解錠できた** (2026-11-13、ライター修正後に作り直した p5。マウント OK、ファイル 3025・ディレクトリ 40 全件一致、0.5 MB/s) | - | - | - | - | - |
| C | 6 | squashfs | OK | 329 / 32 / 4 / 1 | 11 MiB | 59.9 MB/s | 1 s | 0 |
| C | 7 | erofs | OK | 328 / 31 / 4 / 1 | 3 MiB | 29.5 MB/s | 1 s | 0 |
| C | 8 | UDF | OK | 328 / 31 / 4 / 1 | 3 MiB | 26.2 MB/s | 1 s | 0 |
| C | 9 | ISO9660 | OK | 322 / 30 / 4 / 0 | 3 MiB | 26.4 MB/s | 1 s | 1 (`rr_moved` = Rock Ridge の再配置用ディレクトリ。ISO 作成ツール由来) |

1 本ごとの所要 (開く〜閉じる): A 167 s、B 410 s、C 1083 s。USB を開くのは 40〜130 ms、閉じるのは数 ms。

- 3 本の合計で、**失敗した FS は無い** (BitLocker 風を除く)。XFS / Btrfs / F2FS / squashfs / erofs / UDF / ISO の symlink 4 つ (相対・ディレクトリ・ぶら下がり・日本語名) のリンク先も一致した (`readlink` 未対応で確認できなかったものは 0)。BMP 外の文字 `𠮷`、NFC / NFD の濁点、半角カナ、255 バイト近い名前、20 階層、疎ファイル、大文字小文字違いの同名も全て一致 (FS が持つもの)。
- A / B の FAT32 の「余分」6 つは、USB を挿したとき OS (vold) が FAT32 だけ自動マウントして作ったもの (**テスト実行前にあったもの**)。アプリは OS のマウント中は USB を開かないので、テスト中に増えたものではない。なお、アプリが USB を閉じるたびに OS が FAT32 を再マウントすることがあり (実機の端末の挙動)、次の実行の前に `sm unmount public:X,Y` が必要だった。
- **BitLocker 風 (C の p5)**: 当初は `corrupt: ボリュームヘッダの退避位置が不正です` で解錠できなかった。原因は合成ライターが「暗号化済みボリュームのサイズ」に退避領域を含めていなかったこと (docs/crypto-volumes.md 6.1)。修正して p5 だけ作り直して書き込み (PC 側では libbde の `bdemount` で復号 → NTFS を全ツリー照合して一致)、J9260 の実機で `synth-Password-1` により解錠・全件一致を確認した (J9260、読み取り専用)。LUKS2 は argon2id の鍵導出込みで 20.6 s、解錠してから中の ext4 を全ファイル照合できた。
- **遅い読み取り**: Btrfs は 9 MB/s (同じ USB の XFS は 28.8 MB/s、4.5 GiB のファイルを 1 本読むのに 9 分)、LUKS2 は 4.5 MB/s、BitLocker は 0.5 MB/s。原因は、実機試験の APK が `assembleDebug` でネイティブコードが最適化なし (-O0) だったこと (5〜8 倍遅い)、暗号ボリュームの 32 KiB キャッシュが小さい読みを 8 倍に増幅していたこと、AES / SHA-256 / Btrfs の CRC32C がハードウェア命令を使っていなかったこと。Debug でも `-O2` を付け、キャッシュ・先読みを作り直し、AES / SHA-256 / CRC32C に命令版を足した (`docs/crypto-volumes.md` 6.4)。ホストの模擬デバイスでの測定のみで、**実機での再測定は未実施** (次回の実機試験で BitLocker / LUKS2 / Btrfs の読み取り速度を測り直すこと)。正しさには影響しない。

3 本を**同時に開いて並列に読んだ**結果 (3 回試みた。うち 1 回は電池残量の都合で途中で止めた):

1. 1 回目 (4.5 GiB のファイルも含む、20 分): A と C は全部 OK (A 10 MB/s、C の XFS 17 MB/s: 3 本で帯域を分け合う)。**B は 2.5 GiB 読んだところで `bdev read failed errno=71 (EPROTO)` が出て USB が切れ** (端末の USB 一覧から消え、再列挙された。その後の B の読み取りは `errno 5` / `22` で失敗)。アプリは落ちず、他の 2 本は影響を受けずに完走した。B の USB メモリまたはハブ側の一過性の通信障害 (EPROTO は従来から実機の USB で起きることがある) で、単独で読んだときは同じ B が最後まで OK だった。
2. 2 回目 (4.5 GiB のファイルは SHA-256 を省略、7 分): A と B は全部 OK。**C の最後の p9 (ISO9660) だけ、マウントが `errno 19 (ENODEV)` で失敗** (直前の UDF は OK、読み取りは通常の件数)。1 本ずつ読んだときの ISO は OK だったので、同時読み取り時に一度だけ起きた一過性の失敗 (原因は未調査。マウント時の最初の読み取りが期待と違うデータを返した可能性もあるので、再現するなら調べる価値がある)。その後、マウント失敗時に 1 度だけ再試行して `retriedAfterErrno` を記録するよう計装を足した。
3. 3 回目: 電池の都合で開始直後に止めた (結果なし)。

つまり、同時読み取りは「正しいデータが読めたものは全て SHA-256 一致」だが、3 本同時だとハブ・給電の都合で一過性の USB エラーが出ることがある。

#### 書き込み試験の状況

`RealUsbWriteTest` は実装・ビルド済みだが、**実機 USB では未実行**。実行する場合は、`tools/run_android_real_usb_verify.sh --serial <serial> --write --devices 0930:6544 --part 1` のように 1 パーティションずつ。終わったら USB を PC に戻し、書き込み前後で壊れていないことを 11 の手順で確認する:

```sh
sudo ntfsfix -n /dev/sdX1        # NTFS (-n は修復しない)
sudo e2fsck -fn /dev/sdX2        # ext2/3/4 (B の p7_ext4 は needs_recovery のため「リカバリが必要」が正常)
sudo fsck.exfat -n /dev/sdX4     # exFAT
python3 tools/make_real_usb_testset.py verify usbmem_testset_manifests/A/p1_ntfs.sha256 /mnt/t --allow-extra   # 中身 (ro マウント)
```

`usbmem_cotest_*` / `usbmem_selftest_*` / `usbmem_speed_*` のフォルダが残っていないこと (残っていたら、このテストが作ったものなので消してよい)、`verify` が元のファイルの不一致 0 を返すことが合格条件。

## 11. PC に戻した後 (fsck とマニフェスト照合)

USB を PC に戻す (自動マウントさせず、`lsblk` で `/dev/sdX` を確認)。

```sh
# パーティション表
sudo sgdisk -p /dev/sdX          # GPT (A, C)    ※ 検査: sudo sgdisk -v /dev/sdX
sudo sfdisk -l /dev/sdX          # MBR (B)

# FS ごとの検査 (読み取りのみ)
sudo e2fsck -fn /dev/sdX2                 # ext2/3/4  (B の ext4 は needs_recovery のため「リカバリが必要」と出るのが正常。書き込み試験をしたなら -fn の結果で判断)
sudo fsck.fat -n /dev/sdX3                # FAT32
sudo fsck.exfat -n /dev/sdX4              # exFAT
sudo ntfsfix -n /dev/sdX1                 # NTFS (-n は修復しない。-d は dirty フラグを消す = 修復になるので試験が済んでから)
sudo xfs_repair -n /dev/sdX1              # XFS
sudo btrfs check --readonly /dev/sdX2     # Btrfs
sudo fsck.f2fs /dev/sdX3                  # F2FS
sudo cryptsetup luksDump /dev/sdX4        # LUKS2

# マウントして中身を照合 (読み取り専用)
sudo mount -o ro /dev/sdX2 /mnt/t
python3 tools/make_real_usb_testset.py verify usbmem_testset_manifests/C/p2_btrfs.sha256 /mnt/t --allow-extra
```

書き込み試験をした FS は、追加・変更したファイルが「余分」「sha256 不一致」として出る。**元のファイルが壊れていないこと**と、**アプリが書いたファイルを PC が読めて fsck がクリーン**であることが確認項目。

## 12. バックアップと復元

バックアップは `--backup-dir` (例 `./backup_A`) にある。

```sh
# 元が GPT だった場合のパーティション表
sudo sgdisk --load-backup=./backup_A/sgdisk_backup.bin /dev/sdX
# 元が MBR だった場合
sudo sfdisk /dev/sdX < ./backup_A/sfdisk_dump.txt

# 先頭 4 MiB (PMBR / GPT / ブートセクタ) と末尾 1 MiB (GPT のバックアップ領域) をそのまま戻す
sudo dd if=./backup_A/head_4MiB.bin of=/dev/sdX bs=1M conv=fsync
sudo dd if=./backup_A/tail_1MiB.bin of=/dev/sdX bs=1M seek=<デバイスのバイト数-1048576> oflag=seek_bytes conv=fsync

# --full-backup を付けていた場合の完全復元
sudo dd if=./backup_A/full.img of=/dev/sdX bs=4M conv=fsync status=progress
```

先頭 4 MiB / 末尾 1 MiB とパーティション表のバックアップで戻るのは**表とブートセクタだけ**で、元のパーティションの中身は、このスクリプトが新しいファイルシステムを作った時点で失われている。**中身まで戻したいときは `--full-backup`**。デバイスのバイト数は `lsblk -b -dno SIZE /dev/sdX`。

## 13. 既知の制約

- 実デバイス上の一連の流れは、開発側で実行していない (9 参照)。初回は `--no-big-files` の試運転が安全。
- 流し込みはゼロも含めて全パーティションを書く。USB 2.0 だと非常に遅い (C は 18.5 GiB を書き、同じ量を読み戻す)。`--sparse-write` は速くなるとは限らない (事前ゼロ化が遅い機器では逆効果)。
- FAT32 はファイル名の BMP 外の文字、symlink、ハードリンク、疎ファイル、大文字小文字違いを持たない。ISO は Joliet の制約 (64 文字、BMP のみ)。
- XFS の `-p` と ext の `-d` は疎ファイルの穴を保たない場合がある (内容の sha256 には影響しない)。穴の保持は mount して書く NTFS / ext (一部) に限る。
- BitLocker 風は合成。UDF (mkudffs)・F2FS (mount)・exFAT (mount)・LUKS (open) の方式は root が要る (root 不要の代替は「0. 方式」)。LUKS の xts 方式は `cryptography` が要り、2 GiB で約 1 分かかる。
- 論理セクタが 4096 のデバイスは、パーティション表は 4096 単位で作るが、開発側では確認していない。
- 「余白」: **未割り当て領域**には以前のデータが残る (触らない。先頭 4 MiB と末尾 1 MiB だけ消す)。パーティションの中は、既定ではゼロも含めて全部書くので古いデータは残らない (`--sparse-write` を使うと、事前ゼロ化できなかった場合に残りうる)。機微なデータの入っていたメモリには使わないこと。
- 流し込みの途中で電源が落ちたり抜けたりしたデバイスは、パーティション表が無い状態 (表が最後) か、先頭 4 MiB が消えた状態になる。`--flash-only` で流し直すか、バックアップから戻す (12)。
- LUKS のパスフレーズ・BitLocker のパスワードは**テスト用の固定値**で、ソースにもドキュメントにも書いてある。実データを入れないこと。
