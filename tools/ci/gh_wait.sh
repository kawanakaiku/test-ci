#!/bin/bash
# GitHub Actions の run の完了を待つ (多重起動・暴走を防ぐ版)。
#   使い方: tools/ci/gh_wait.sh RUN_ID [REPO=kawanakaiku/test-ci] [INTERVAL秒=120] [MAX秒=14400]
#   必ず run_in_background で 1 本だけ起動する (前景で待つと、ツールのタイムアウトで背景に回され、再実行のたびに待機ループが増える)。
#   - 同じ RUN_ID の二重起動は、ロックで即終了 (終了コード 3)。
#   - gh は 1 回ごとに `timeout 30` と `nice -n 19` で包む (固まった gh が CPU を食い続けるのを防ぐ)。
#   - 間隔は 60 秒以上、全体に上限 (既定 4 時間)。上限に達したら終了コード 4。
id=${1:?RUN_ID}; repo=${2:-kawanakaiku/test-ci}; iv=${3:-120}; max=${4:-14400}
[ "$iv" -ge 60 ] || iv=60
exec 9>"/tmp/gh_wait.$id.lock"
flock -n 9 || { echo "既に待機中: $id (二重に待たない)"; exit 3; }
end=$(( $(date +%s) + max ))
g() { timeout 30 nice -n 19 gh "$@" 2>/dev/null; }
while :; do
  st=$(g run view "$id" -R "$repo" --json status -q .status)
  [ "$st" = completed ] && break
  [ "$(date +%s)" -lt "$end" ] || { echo "上限 ${max}s に達した (status=$st)"; exit 4; }
  sleep "$iv"
done
g run view "$id" -R "$repo" --json conclusion,createdAt,updatedAt,jobs -q '.conclusion, "所要 " + ((((.updatedAt|fromdate)-(.createdAt|fromdate))/60|floor|tostring)) + " 分", (.jobs[]|[.name,.conclusion]|@tsv)'
