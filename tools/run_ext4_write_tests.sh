#!/bin/sh
# ext4 書き込み (jbd2) の検証 (USBMEM_NTFS_WRITE=ON ビルド用。docs/ext4-write.md)。自前ドライバ (fs/ext4 + fs/jbd2 を LinuxCompat 上で動かしたもの) で書いたイメージを、
# 独立実装 (e2fsprogs の e2fsck / debugfs と、libext2fs ベースの fuse2fs) で検証する。
#   1. 決まった操作列 (実カーネル相当の mke2fs -d で作った各種 ext3/ext4 フィクスチャ): 自前ドライバで実行 → e2fsck -fn が clean →
#      同じ操作列を fuse2fs 上で POSIX 操作として実行した結果とツリー全体 (種別・サイズ・sha256・リンク先) が一致。
#   2. 乱数操作 + メモリ上のモデル (ブロックサイズ・機能の組み合わせ × seed。CreateOnly の操作だけの回も) → e2fsck clean、モデルのダンプが fuse2fs のツリーと一致。
#   3. 数千ファイルの作成・削除・rename (htree ディレクトリ・inode / ブロックの確保と解放の繰り返し)。
#   4. ジャーナルの再生: debugfs (e2fsprogs の独立実装) が書いたトランザクション (複数ブロック・revoke・未コミット) を自前ドライバが再生した結果が、
#      e2fsck の再生結果とブロック単位で一致し、e2fsck -fn が clean。再生の途中で電源が切れても、もう一度再生すれば同じ結果に収束する。
#   5. rw を要求しても何も書かない / 書き込み不可 (汚れ・エラー・ホワイトリスト外の機能・外部ジャーナル) は読み取り専用でイメージ不変 / 満杯・inode 枯渇。
# 使い方: run_ext4_write_tests.sh <usbmem_ext4_write_tests> [フィクスチャ dir (tools/gen_ext4_fixture.py)] [モデル試行数 (既定 2)]
set -eu
export PATH="$PATH:/sbin:/usr/sbin"
T=$1; D=${2:-/dev/shm/usbmem_ext4_${USER:-u}}; NM=${3:-2}
for c in mke2fs e2fsck debugfs dumpe2fs fuse2fs fusermount3 python3 cmp; do command -v $c >/dev/null || { echo "skip: $c missing" >&2; exit 77; }; done
[ -f "$D/e4_def4k.img" ] || { echo "skip: no fixture in $D (tools/gen_ext4_fixture.py)" >&2; exit 77; }
HERE=$(cd "$(dirname "$0")" && pwd)
W=$(mktemp -d "${USBMEM_WORK_DIR:-/dev/shm}"/usbmem_e4w.XXXXXX)
cleanup() { for m in "$W"/mnt*; do fusermount3 -u "$m" 2>/dev/null || true; done; rm -rf "$W"; }
trap cleanup EXIT
export ASAN_OPTIONS=${ASAN_OPTIONS:-verify_asan_link_order=0:detect_leaks=1}
rc=0; nfsck=0
fail() { echo "FAIL: $*"; rc=1; }
tree_of() {  # tree_of <img> <out>: fuse2fs (libext2fs) の ro マウントからツリーのダンプ
  mkdir -p "$W/mnt_t"; fuse2fs -o ro,fakeroot "$1" "$W/mnt_t" >/dev/null 2>&1 || { echo "fuse2fs ro mount failed: $1"; return 1; }
  python3 -I "$HERE/ops_ref.py" --tree "$W/mnt_t" > "$2"; fusermount3 -u "$W/mnt_t"
}
fsck_clean() {  # fsck_clean <img> <label>
  nfsck=$((nfsck + 1))
  out=$(e2fsck -fn "$1" 2>&1) || { printf "%s\n" "$out" | head -20; fail "$2: e2fsck not clean"; return 1; }
  # e2fsck -n は未再生のジャーナルを再生せずに 0 を返しうるので、needs_recovery (ジャーナル使用中の印) が消えていることも見る (= きれいにアンマウントされた)
  dumpe2fs -h "$1" 2>/dev/null | grep -q needs_recovery && { fail "$2: needs_recovery still set (not cleanly unmounted)"; return 1; }
  return 0
}
sum_of() { sha256sum "$1" | cut -d" " -f1; }
run_t() {  # run_t <label> <args...>
  label=$1; shift
  if out=$("$T" "$@" 2>&1); then echo "[$label] $(printf "%s\n" "$out" | grep -v '^WARN_ON\|^EXT4-fs\|super_set_uuid' | tail -1)"; else printf "%s\n" "$out" | tail -5; fail "$label"; fi
}

# ---------- 1. 決まった操作列 ----------
for fx in e4_def4k:4096 e4_def1k:1024 e3_4k:4096; do
  name=${fx%%:*}; bs=${fx##*:}; IMG=$D/$name.img
  python3 -I "$HERE/gen_ext2_ops.py" $bs "$W/ops_$name.txt"
  # ext4 固有: 未初期化 (unwritten) エクステントへの書き込み (初期化への変換)、深い extent 木の途中の書き換え・縮小
  if grep -q "unwritten.bin" "$D/$name.manifest" 2>/dev/null; then
    printf 'write /unwritten.bin 4096 5000 31\nwrite /frag_deep.bin 300000 9000 32\ntruncate /frag_deep.bin 200001\nwrite /holes.bin 0 100 33\n' >> "$W/ops_$name.txt"
  fi
  cp "$IMG" "$W/a.img"; cp "$IMG" "$W/r.img"
  out=$("$T" --ops "$W/a.img" "$W/ops_$name.txt" 2>&1) || { printf "%s\n" "$out" | tail -5; fail "ops $name: driver"; continue; }
  echo "[$name] $(printf "%s\n" "$out" | grep -a '^ops:')"
  fsck_clean "$W/a.img" "ops $name" || continue
  mkdir -p "$W/mnt_r"
  fuse2fs -o rw,fakeroot "$W/r.img" "$W/mnt_r" >/dev/null 2>&1 || { fail "fuse2fs rw mount ($name)"; continue; }
  python3 -I "$HERE/ops_ref.py" "$W/mnt_r" "$W/ops_$name.txt" || { fusermount3 -u "$W/mnt_r"; fail "reference ops ($name)"; continue; }
  fusermount3 -u "$W/mnt_r"
  tree_of "$W/a.img" "$W/ta" && tree_of "$W/r.img" "$W/tr" || { fail "tree dump ($name)"; continue; }
  if cmp -s "$W/ta" "$W/tr"; then echo "[$name] tree identical to reference ($(wc -l < "$W/ta") entries), e2fsck clean"; else diff "$W/ta" "$W/tr" | head -10; fail "ops $name: tree differs from fuse2fs reference"; fi
done

# ---------- 2. 乱数操作 + モデル ----------
i=1
for cfg in "4096 ext4 -" "1024 ext4 -" "2048 ext4 ^metadata_csum,^metadata_csum_seed,uninit_bg" "4096 ext3 -" "1024 ext4 ^orphan_file" "4096 ext4 -:createonly"; do
  set -- $cfg; bs=$1; ty=$2; feat=${3%%:*}; mode=${3#*:}; [ "$mode" = "$3" ] && mode=full
  for k in $(seq 1 "$NM"); do
    seed=$((i * 1000 + k)); img=$W/m.img
    rm -f "$img"; truncate -s 64M "$img"
    if [ "$feat" = "-" ]; then mke2fs -q -t $ty -b $bs "$img"; else mke2fs -q -t $ty -b $bs -O "$feat" "$img"; fi
    if [ $mode = createonly ]; then export USBMEM_E4_CREATEONLY=1; else unset USBMEM_E4_CREATEONLY || true; fi
    out=$("$T" --model "$img" $seed 1200 "$W/m.dump" 2>&1) || { printf "%s\n" "$out" | grep -v '^WARN_ON' | tail -8; fail "model $cfg seed=$seed"; continue; }
    unset USBMEM_E4_CREATEONLY || true
    fsck_clean "$img" "model $cfg seed=$seed" || continue
    tree_of "$img" "$W/m.tree" || { fail "tree dump (model)"; continue; }
    cmp -s "$W/m.dump" "$W/m.tree" || { diff "$W/m.dump" "$W/m.tree" | head -5; fail "model $cfg seed=$seed: fuse2fs tree differs from model"; continue; }
    echo "[model bs=$bs $ty $feat $mode seed=$seed] $(printf "%s\n" "$out" | grep -a 'model:' | cut -c1-80); e2fsck clean; tree matches fuse2fs"
  done
  i=$((i + 1))
done

# ---------- 3. 数千ファイル ----------
for bs in 1024 4096; do
  img=$W/big.img; rm -f "$img"; truncate -s 64M "$img"; mke2fs -q -t ext4 -b $bs "$img"
  python3 -I - "$W/many.ops" <<'PY'
import sys
L = ["mkdir /m"]
for i in range(3000):
    L += ["create /m/f%04d" % i, "write /m/f%04d 0 %d %d" % (i, (i * 37) % 6000, i % 251)]
for i in range(0, 3000, 2): L.append("unlink /m/f%04d" % i)
for i in range(1, 3000, 4): L += ["rename /m/f%04d /m/g%04d" % (i, i)]
for i in range(3000, 4500):
    L += ["create /m/h%04d" % i, "write /m/h%04d 0 %d %d" % (i, (i * 13) % 3000, i % 199)]
for i in range(3, 3000, 4): L.append("unlink /m/f%04d" % i)
open(sys.argv[1], "w").write("\n".join(L) + "\n")
PY
  cp "$img" "$W/big0.img"
  out=$("$T" --ops "$img" "$W/many.ops" 2>&1) || { printf "%s\n" "$out" | tail -5; fail "many files bs=$bs: driver"; continue; }
  fsck_clean "$img" "many files bs=$bs" || continue
  cp "$W/big0.img" "$W/bigr.img"; mkdir -p "$W/mnt_r"
  fuse2fs -o rw,fakeroot "$W/bigr.img" "$W/mnt_r" >/dev/null 2>&1 && python3 -I "$HERE/ops_ref.py" "$W/mnt_r" "$W/many.ops"; fusermount3 -u "$W/mnt_r"
  tree_of "$img" "$W/ta" && tree_of "$W/bigr.img" "$W/tr" && cmp -s "$W/ta" "$W/tr" && echo "[many files bs=$bs] $(printf "%s\n" "$out" | grep -a 'ops:' | tail -1); $(wc -l < "$W/ta") entries identical to reference; e2fsck clean" || fail "many files bs=$bs: tree differs"
done

# ---------- 4. ジャーナルの再生 (debugfs が書いたトランザクション) ----------
# 空きブロックに、2 つのコミット済みトランザクション (後のものが一部を上書き・revoke) と 1 つの未コミットのトランザクションを書く。
# 正しい再生: T1 → T2 の順に反映、T2 で revoke されたブロックは T1 の内容を書かない、未コミット (T3) は反映しない。
mkj() {  # mkj <img> <bs> <extra mke2fs opts...>
  img=$1; bs=$2; shift 2
  rm -f "$img"; truncate -s 32M "$img"; mke2fs -q -t ext4 -b $bs "$@" "$img"
  python3 -I - "$W" $bs <<'PY'
import sys, os
w, bs = sys.argv[1], int(sys.argv[2])
for name, seed in (("j1", 1), ("j2", 2), ("j3", 3)):
    open(os.path.join(w, name), "wb").write(bytes(((i * 13 + seed * 101) & 255) for i in range(bs * 3)))
PY
  # 置き場所: 末尾近くの空きブロック (どのブロックサイズでも空きのはず)
  nb=$(( 32 * 1024 * 1024 / bs )); b1=$((nb - 40)); b2=$((nb - 39)); b3=$((nb - 38)); b4=$((nb - 37)); b5=$((nb - 36))
  printf 'jo -c\njw -b %d,%d,%d %s/j1\njw -b %d,%d -r %d %s/j2\njw -b %d,%d,%d -c %s/j3\njc\n' $b1 $b2 $b3 "$W" $b2 $b4 $b3 "$W" $b1 $b4 $b5 "$W" > "$W/jcmd"
  debugfs -w -f "$W/jcmd" "$img" >/dev/null 2>&1
  echo "$b1 $b2 $b3 $b4 $b5"
}
blocks_of() {  # blocks_of <img> <bs> <b...>: 各ブロックの sha256
  img=$1; bs=$2; shift 2
  for b in "$@"; do dd if="$img" bs=$bs skip=$b count=1 2>/dev/null | sha256sum | cut -c1-16; done | tr '\n' ' '
}
for jc in "4096 -" "1024 -" "4096 ^metadata_csum" ; do
  set -- $jc; bs=$1; o=$2
  if [ "$o" = "-" ]; then bl=$(mkj "$W/j.img" $bs); else bl=$(mkj "$W/j.img" $bs -O "$o"); fi
  dumpe2fs -h "$W/j.img" 2>/dev/null | grep -q needs_recovery || { fail "journal fixture ($jc): needs_recovery not set by debugfs"; continue; }
  cp "$W/j.img" "$W/je.img"; e2fsck -fy "$W/je.img" >/dev/null 2>&1 || true   # 参照: e2fsprogs の再生
  cp "$W/j.img" "$W/jo.img"
  out=$("$T" --replay "$W/jo.img" 2>&1) || { printf "%s\n" "$out" | tail -5; fail "replay ($jc): driver"; continue; }
  fsck_clean "$W/jo.img" "replay ($jc)" || continue
  dumpe2fs -h "$W/jo.img" 2>/dev/null | grep -q needs_recovery && { fail "replay ($jc): needs_recovery still set"; continue; }
  ours=$(blocks_of "$W/jo.img" $bs $bl); ref=$(blocks_of "$W/je.img" $bs $bl)
  [ "$ours" = "$ref" ] || { echo "ours: $ours"; echo "ref:  $ref"; fail "replay ($jc): replayed blocks differ from e2fsck's replay"; continue; }
  # 再生の途中で電源断 → もう一度再生すると同じ結果に収束する
  cp "$W/j.img" "$W/jx.img"; total=$(USBMEM_CRASH_AT=-1 "$T" --replay "$W/jx.img" 2>&1 | grep -a '^writes:' | sed 's/writes: //')
  conv=0; tried=0; tornsb=0; tornsb_nobk=0
  for at in $(seq 0 $total); do
    for mode in prefix cache; do
      cp "$W/j.img" "$W/jx.img"
      USBMEM_CRASH_AT=$at USBMEM_CRASH_MODE=$mode USBMEM_CRASH_TORN=1 USBMEM_CRASH_SEED=$at "$T" --replay "$W/jx.img" >/dev/null 2>&1 || true
      sha_before=$(sha256sum "$W/jx.img" | cut -d' ' -f1)
      if ! "$T" --replay "$W/jx.img" >"$W/r2.log" 2>&1; then
        # スーパーブロック自身がセクタ途中で切れた (1024 バイトの超ブロックの前半だけ書けてチェックサムが合わない = EFSBADCRC, errno 74):
        # カーネルの ext4 と同じくマウントを拒否するのが正しい。条件: (1) 拒否は 1 バイトも書かない (2) e2fsck がバックアップ超ブロックで直せるなら、直せて clean になる
        # (グループが 1 つだけの小さなボリュームにはバックアップが無く、e2fsck でも直せない = 実カーネルでも同じ)。
        if grep -aq 'mount_rw failed: errno 74' "$W/r2.log"; then
          [ "$(sha256sum "$W/jx.img" | cut -d' ' -f1)" = "$sha_before" ] || { fail "replay ($jc): torn superblock at $at/$mode: the refused mount changed the image"; continue; }
          frc=0; e2fsck -fy "$W/jx.img" >/dev/null 2>&1 || frc=$?
          if [ $frc -le 1 ]; then
            e2fsck -fn "$W/jx.img" >/dev/null 2>&1 || { fail "replay ($jc): torn superblock at $at/$mode: not clean after e2fsck"; continue; }
            tornsb=$((tornsb + 1)); nfsck=$((nfsck + 1))
          else tornsb_nobk=$((tornsb_nobk + 1)); fi
          continue
        fi
        fail "replay ($jc): second replay failed after cut at $at/$mode"; continue
      fi
      tried=$((tried + 1))
      e2fsck -fn "$W/jx.img" >/dev/null 2>&1 || { fail "replay ($jc): not clean after cut at $at/$mode + re-replay"; continue; }
      nfsck=$((nfsck + 1))
      [ "$(blocks_of "$W/jx.img" $bs $bl)" = "$ref" ] && conv=$((conv + 1)) || fail "replay ($jc): cut at $at/$mode did not converge"
    done
  done
  echo "[replay bs=$bs $o] blocks match e2fsck's replay ($ref); e2fsck clean; interrupted replay: $conv/$tried cut points converged, $tornsb torn-superblock cuts refused without writing + repaired by e2fsck, $tornsb_nobk refused without writing (no backup superblock) (journal writes $total)"
done

# ---------- 4b. 壊したイメージを書き込み可でマウントして操作する (ファジング: クラッシュ・ハング・ASan エラーが無いこと) ----------
rm -rf "$W/fzt"; mkdir -p "$W/fzt/d"
python3 -I - "$W/fzt" <<'PY'
import os, sys
r = sys.argv[1]
for i in range(30): open(os.path.join(r, "f%d" % i), "wb").write(bytes((j * 31 + i) & 255 for j in range(100 * i + 5)))
for i in range(10): open(os.path.join(r, "d/g%d" % i), "wb").write(bytes((j * 7 + i) & 255 for j in range(70000)))
PY
for fz in "1024 -" "4096 ^metadata_csum"; do
  set -- $fz; rm -f "$W/fz.img"
  if [ "$2" = "-" ]; then mke2fs -q -t ext4 -b $1 -d "$W/fzt" "$W/fz.img" 8M; else mke2fs -q -t ext4 -b $1 -O "$2" -d "$W/fzt" "$W/fz.img" 8M; fi
  run_t "fuzz (rw, bs=$1 $2)" --fuzzw "$W/fz.img" 200 $1
done

# ---------- 5. 何も書かない / 書き込み拒否 / 満杯 ----------
cp "$D/e4_def4k.img" "$W/n.img"; run_t "noop (rw mount, no writes: image unchanged)" --noop "$W/n.img"
mk_small() { rm -f "$W/s.img"; truncate -s 16M "$W/s.img"; mke2fs -q -t ext4 -b 1024 "$@" "$W/s.img"; }
mk_small; debugfs -w -R "ssv state 0" "$W/s.img" >/dev/null 2>&1; run_t "refuse unclean (state=0)" --refuse "$W/s.img"
mk_small; debugfs -w -R "ssv state 3" "$W/s.img" >/dev/null 2>&1; run_t "refuse error state" --refuse "$W/s.img"
mk_small -O quota; run_t "refuse quota (not in whitelist)" --refuse "$W/s.img"
mk_small -O ^has_journal; run_t "refuse no journal" --refuse "$W/s.img"
for fx in e4_metabg4k e4_inline1k e4_bigalloc4k e4_nocsum2k e4_gdtcsum4k e2_4k; do cp "$D/$fx.img" "$W/x.img"; run_t "refuse $fx (not in whitelist / no journal)" --refuse "$W/x.img"; done
mk_small -N 200; run_t "enospc" --enospc "$W/s.img"
fsck_clean "$W/s.img" "enospc" || true

echo "e2fsck -fn checks: $nfsck"
[ $rc -eq 0 ] && echo "ext4 write: all passed" || echo "ext4 write: FAILED"
exit $rc
