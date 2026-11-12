# ネスト (ループバック) マウント (読み取り専用)

マウント済みのボリューム (USB のパーティション、SAF で選んだイメージ、さらにその中のイメージ) の中にある**ディスクイメージ**
(`.img` / `.iso` / パーティション表付きのイメージなど) を、読み取り専用の**子ボリューム**としてマウントする。
要件書 2 章 (BlockDevice の抽象) の P2 項目。書き込みのネストは別段階 (末尾「次の段階」)。

## 1. 設計

```
USB / SAF の fd ──→ RawImageDevice ──→ [FS ドライバ] ──→ LockedFsVolume (親)
                                                              │  ファイルを 1 つ選ぶ (ino)
                                                              ▼
                                                      VolumeFileDevice (BlockDevice, 読み取り専用)
                                                              │  (パーティション表があれば SubRangeDevice)
                                                              ▼
                                                [FS ドライバ] ──→ LockedFsVolume (子)
```

- **SAF の fd は使わない。** ファイルアプリ (SAF プロバイダ) 経由で親のファイルを fd として開いて子に渡すと、
  プロバイダの専用 IO スレッドと、アプリ側のロック (`VolumeRegistry`) が互いを待つ行き詰まりが起こりうる。
  そこで、ネイティブ層で**親の `FsVolume` を直接読む**ブロックデバイス (`VolumeFileDevice`) を作る。
- **ボリューム単位のネイティブ mutex** (`LockedFsVolume`): すべての `FsVolume` を包み、JNI からの呼び出しと、子ボリュームからの読み出しを同じ mutex で直列化する。
  ロック順は常に「子 → 親」(子のドライバが親を読む向きだけ) なので循環しない。同じスレッドが同じボリュームに再入すると (= ネストの循環)、
  行き詰まらずに `-ELOOP` を返す。
- **子は常に読み取り専用。** `VolumeFileDevice::write_at` は `ReadOnly`、`is_read_only()` は真。
  子を書き込み可で開こうとしても、下のデバイスが読み取り専用なので読み取り専用でマウントされる (`SubRangeDevice::is_read_only()` が親の状態も見る + `mount_auto` が rw を落とす。wsan / wrel のテストで確認。以前は書き込み有効ビルドで「書き込み可」と名乗ってしまっていた)。
- **親が書き込み可のとき**: マウント中のイメージファイルの書き換えを防ぐ二重の仕組みがある。
  1. Kotlin 側 (`NtfsVolume.pin`): 子がマウント中のファイル (とそれを含むフォルダ) への write / truncate / remove / rename / move / removeTree は `EBUSY(16)` で拒否。
  2. ネイティブ側 (`VolumeFileDevice`): 親が `writable()` なら、読むたびに親ファイルのサイズと更新時刻を開いたときと比べ、違えば `Io(ESTALE)` を返す
     (別名のハードリンク経由で書き換えられても、子が壊れたデータを読まない)。
- **段数の上限は 3** (最上位 = 0 として、イメージの中のボリュームは 1〜3)。4 段目は `ELOOP(40)` で拒否。`open_nested_image(..., max_depth)` の引数で変えられる (テストでは 4 段も読めることを確認)。
- **循環の拒否**: 系譜 (`NestedLineage`: 祖先が裏打ちにしているファイルの列) に、いま開こうとしている (ボリューム, ino) が既にあれば `ELOOP`。
  加えて、実行時の再入検出 (上記) が最後の砦になる。
- **メモリ安全**: 子は親の `shared_ptr` を持つ。Kotlin が誤って親を先に閉じても、ネイティブのメモリは壊れない (読み出しが失敗するだけ)。

## 2. 閉じる順 (内 → 外)

`VolumeRegistry` が各ボリュームの親子関係 (`Entry.parentId`) を持つ。親を閉じる (`remove` / `removeByKey` / 同じキーでの置き換え / USB を閉じる) と、
**子 (孫を含む) を先に**アンマウントし、その子のイメージ用デバイスを閉じ、親のファイルのピンを外してから、最後に親を閉じる。
(書き込みのネストを入れるときは、この順番で「同期 → アンマウント」を行う。)

## 3. API

- core (`core/compat/public/usbmem/nested_mount.h`): `LockedFsVolume` / `make_locked`、`VolumeFileDevice`、`open_nested_image(parent, parent_lineage, path, max_depth)`。
  エラーは errno: `ELOOP` (段数・循環)、`EISDIR`、`EINVAL` (通常ファイルでない・512 バイト未満)、親の errno (`ENOENT` など)。
- JNI (`android/app/src/main/cpp/jni_nested.cpp`): `nativeNestedOpen(volHandle, path)` → デバイスハンドル (既存の `nativeListPartitions` / `nativeNtfsMountDevice` / `nativeClose` にそのまま渡せる)、`nativeNestedDepth`。
  デバイスの系譜は `usbmem_device::lineage` に持たせ、その上にマウントした `Vol` に引き継ぐ (`jni_vol.h`)。
- Kotlin: `NtfsVolume.openImage(path)` → `NestedImage` (`partitionTable()` / `mount(offset, size)`)、
  `VolumeRegistry.inspectNested` (パーティション表を読むだけ) / `mountNested` (登録) / `childrenOf` / `ownDocument`。
- 画面: 「イメージ」タブの「ボリューム内のイメージをマウント」(`NestedMountSection`)。ボリュームを選び、フォルダをたどってファイルを選ぶ →
  パーティション表があれば一覧 (なければ全体) → 「マウント (読み取り専用)」。ext3/ext4 のジャーナル未再生は、USB と同じ確認ダイアログで読み取り専用に開ける。

## 4. このアプリ自身のプロバイダのファイル

ファイルアプリのピッカーで、このアプリが公開しているファイル (`org.usbmem.app.documents`) を「イメージ」として選んだ場合、
SAF の fd を開くと行き詰まりうる (上記)。`VolumeRegistry.addUri` / `loadImage` は、その URI を (ボリューム id, パス) に直して**ネストマウントに振り替える** (読み取り専用)。
起動時の復元でも、自分のプロバイダの URI は読まない。

## 5. 検証

### ホスト (`core/compat/tests/test_nested.cpp`、`tools/gen_nested_fixture.py`)
フィクスチャは実ツール (mke2fs `-d` / mkfs.fat + mcopy / mkntfs + ntfscp / mkfs.exfat + 自前の書き込み / sgdisk / sfdisk) で作る。Linux の FS ドライバや FUSE は使わない。
親: ext4 (ジャーナル付き)・ext2・FAT32・exFAT・NTFS。子: ext2・ext4・FAT・NTFS、GPT 付き (ext2 + FAT)、MBR 付き (主 2 + 拡張内の論理 1)、全ゼロ、100 バイトの小さなファイル。
入れ子の鎖: ext4 ⊃ ext2 ⊃ FAT ⊃ ext2。

| テスト | 内容 |
|---|---|
| `nested_read_*` | 親 5 種 × 子ごとに、ネストのデバイスが元のファイルと**全バイト一致** (64 KiB 刻み・4093 バイト刻み・ランダム範囲)、ネストでマウントした木が、元を直接マウントした木と**全ファイル・属性・内容まで一致** |
| `nested_partitions_*` | GPT / MBR(+論理) のパーティション表が元と一致し、各パーティションの木も一致 |
| `nested_depth_ext4` | 1〜3 段は開けて内容が一致、4 段目は ELOOP、`max_depth=4` なら 4 段も読める、`max_depth=0` は拒否 |
| `nested_lifetime_*` | 親の持ち主を全部手放しても子が読める (ASan で確認)、デバイスを close すると以降は Closed / マウント失敗、子を手放すと親も解放される |
| `nested_cycle` | 巡り巡って自分を読む FS: デッドロックせず `Io` で失敗 (別スレッドでも)、系譜に同じファイルがあれば ELOOP、別のファイルなら通る |
| `nested_readonly_*` | 書き込みは `ReadOnly`、`mount_rw` でも子は書き込み可にならない (create / unlink も失敗) |
| `nested_misc_*` | 存在しない (ENOENT)・ディレクトリ (EISDIR)・100 バイト (EINVAL)・null の親、全ゼロのイメージはマウント失敗でクラッシュしない、範囲外・オーバーフロー・キャンセル |
| `nested_ioerr_*` | 親の下位デバイスの読み出しエラー (持続 / 1 回だけ) を、約 100 地点に注入。**誤ったデータは 1 つも返さない** (エラーまたは正しいデータだけ)。短い読み (エラー手前までの正しいデータ) も検査 |
| `nested_concurrent_*` | 子の木の比較 ×2、親のイメージファイルの読み出し ×2、ネストの開き直し ×1 を同時に実行 (行き詰まらない・不一致なし) |
| `nested_stale_ext2` | (書き込み有効ビルドのみ) 親が書き込み可のとき、マウント中のイメージを親経由で書き換えると `Io(ESTALE)`。開き直せば新しい内容が見える |

ミューテーション: `VolumeFileDevice` の読み出しオフセットを 1 ずらすと、`nested_read` / `nested_partitions` / `nested_ioerr` が落ちることを確認した (元に戻して全件パス)。

FAT ドライバは、ディレクトリの読み出しエラーを「そこで終わり」として扱う (Linux 本体と同じ。ネストに限らず、直接のデバイスでも同じ)。
エラー注入のテストでは、これを「欠けた」= エラーとして数える (誤ったデータが見えるわけではないため)。

### AVD (`NestedMountTest`、`tools/run_android_nested_test.sh`)
親は ext4 の `p_avd.img` (48 MiB)、書き込み可の親は ext2 の `p_avd_rw.img`。13 件:
ext2 / FAT / NTFS のイメージ、GPT / MBR(論理を含む) の各パーティション (中身は `nested.manifest` の sha256 と突き合わせ、余分なファイルもないこと)、
表のない全体、3 段 (ext4 ⊃ ext2 ⊃ FAT) の奥のファイルまで読み 4 段目は ELOOP、イメージでないもの (ENOENT / EISDIR / EINVAL / 全ゼロ)、
同じ場所の二重マウントは同じエントリ、親を閉じると子が先に閉じる、1 つの子だけ閉じても親と兄弟は無事、
**書き込み可の親でマウント中のイメージを write / truncate / remove / rename / move / removeTree / 親フォルダの移動しようとすると EBUSY** (子を閉じれば書ける)、
自分のプロバイダの URI がネストに振り替わる (行き詰まらない)、**ファイルアプリ (SAF プロバイダ) 経由で子の中身を読みながら親のイメージも同時に読む**、複数スレッドの同時読み。

## 6. 制限・未解決

- 書き込みのネストは未実装 (次の段階)。
- ネストしたマウントは、再起動後に復元しない (USB と同じ)。
- 子のブロックサイズは 512 バイト固定 (パーティション表の検出は 512 / 4096 を自動で試す)。4Kn セクタのイメージを入れ子にした実測は未実施。
- 大きなイメージ (数 GiB) の入れ子での速度は未測定 (子の読み出しは、親のファイルシステムのページキャッシュと、子自身のページキャッシュの二重になる)。
- 実機での確認は未実施 (AVD とホストのみ)。
- 親の FS の `read` が短い値を返す場合は継続して読み、0 が返ったら I/O エラーとして扱う。

## 7. 次の段階 (書き込みのネスト)

外側が「フル」書き込みで、内側を書き込み可にする場合の方針 (未実装):
内側の書き込みは外側のファイルへの書き込みになる。内側を先に同期 (ジャーナル / 順序書き込み) → 外側のファイルを同期、
閉じる順は 内 → 外 (今の `closeEntry` の順序をそのまま使う)。外側のクラッシュ整合性が内側の整合性の前提になるため、
内側の書き込みは外側の `write_inplace` (割り当てを変えない上書き) だけに限り、外側が拡張しない・断片化を増やさないようにする。
AVD で電源断の模擬 (外側 → 内側の順に切る) を通してから実機に進む。
