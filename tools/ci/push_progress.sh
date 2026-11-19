#!/bin/bash
# CI のランナーごと止まる (The hosted runner lost communication) とアーティファクトもログも残らない。そこで、実行中のテスト名・メモリ・ディスク・dmesg を
# 一定間隔でリポジトリのブランチ (ci-progress) のファイルに書いて残す (xfstests の workflow で、ブランチ名の末尾が -progress のときだけ使う)。
#   使い方: push_progress.sh NAME OUTDIR [INTERVAL秒=30]    環境: GH_TOKEN (contents: write)、GITHUB_REPOSITORY
name=${1:?NAME}; out=${2:?OUTDIR}; iv=${3:-30}
path="progress/$name.txt"; sha=""
while :; do
  body=$({ date -u +%T; cat "$out/current.txt" 2>/dev/null; free -m | sed -n 2p; df -h / /mnt 2>/dev/null | tail -2; uptime; echo "--- dmesg"; sudo -n dmesg 2>/dev/null | tail -15; echo "--- fuse/dm"; pgrep -c usbmem_fuse; sudo -n dmsetup ls 2>/dev/null; } 2>&1)
  args=(-X PUT "repos/$GITHUB_REPOSITORY/contents/$path" -f "message=progress $name" -f "branch=ci-progress" -f "content=$(printf '%s\n' "$body" | base64 -w0)")
  [ -n "$sha" ] && args+=(-f "sha=$sha")
  if r=$(timeout 20 gh api "${args[@]}" --jq .content.sha 2>/dev/null); then sha=$r
  else sha=$(timeout 20 gh api "repos/$GITHUB_REPOSITORY/contents/$path?ref=ci-progress" --jq .sha 2>/dev/null || true); fi   # 失敗したら現在の sha を取り直す (次の更新が 422 で止まり続けないように)
  sleep "$iv"
done
