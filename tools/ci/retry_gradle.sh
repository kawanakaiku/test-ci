#!/bin/bash
# Gradle の依存解決の一時失敗 (HTTP 429 / 接続リセット / タイムアウト / どのリポジトリからも取れない) のときだけ、待ってやり直す (docs/ci.md)。
#   使い方: tools/ci/retry_gradle.sh ./gradlew --no-daemon :app:assembleDebug ...   (コマンドは何でもよい。終了コードはそのまま返す)
#   - 終了コード 0 ならそのまま 0。
#   - 失敗したとき、ログに「一時失敗のしるし」があり、かつ「本当の失敗のしるし」(コンパイルエラー・テスト失敗) が無いときだけやり直す。
#   - 既定は最大 3 回 (待ち 30 秒 → 90 秒)。--refresh-dependencies は使わない (キャッシュを捨てるので、かえって取得が増える)。
# 環境変数: RETRY_GRADLE_DELAYS (待ち秒数の並び。既定 "30 90"。やり直しの回数 = 要素数)、RETRY_GRADLE_LOG (ログの置き場。既定は mktemp)。
set -u
[ $# -ge 1 ] || { echo "usage: retry_gradle.sh COMMAND [ARGS...]" >&2; exit 2; }
read -r -a DELAYS <<< "${RETRY_GRADLE_DELAYS:-30 90}"

# 一時失敗のしるし (拡張正規表現)。"Could not find" は group:artifact:version の形のときだけ (ビルドスクリプトの "Could not find method" などを除く)
TRANSIENT='Could not resolve |Could not find [A-Za-z0-9_.-]+:[A-Za-z0-9_.-]+:|Could not (GET|HEAD|download) |status code (429|5[0-9][0-9])|HTTP/[0-9.]+ (429|5[0-9][0-9])|Too Many Requests|Connection reset|Connection timed out|Connect timed out|Read timed out|SocketTimeoutException|Remote host terminated|Premature end of Content-Length|Software caused connection abort|Temporary failure in name resolution|UnknownHostException|SSLHandshakeException|Received fatal alert|Broken pipe'
# 本当の失敗のしるし。これがあれば再試行しない (Kotlin / Java / C++ のコンパイルエラー、テスト失敗)。
# 注意: "Execution failed for task ':app:compileDebugKotlin'" は依存解決の失敗でも出るので、しるしにしない
REAL='^e: |: error: |^error: |Compilation error|There were failing tests|tests completed, [0-9]+ failed|Unresolved reference'

LOG=${RETRY_GRADLE_LOG:-$(mktemp "${TMPDIR:-/tmp}/retry_gradle.XXXXXX")}
attempt=1
total=$(( ${#DELAYS[@]} + 1 ))
while :; do
  set -o pipefail
  "$@" 2>&1 | tee "$LOG"
  rc=${PIPESTATUS[0]}
  set +o pipefail
  [ "$rc" -eq 0 ] && exit 0
  if grep -Eq -- "$REAL" "$LOG"; then
    echo "retry_gradle: 失敗 (終了コード $rc)。ビルド / テストの本当の失敗なので再試行しない"; exit "$rc"
  fi
  if ! grep -Eq -- "$TRANSIENT" "$LOG"; then
    echo "retry_gradle: 失敗 (終了コード $rc)。依存解決の一時失敗のしるしが無いので再試行しない"; exit "$rc"
  fi
  if [ "$attempt" -ge "$total" ]; then
    echo "::error::retry_gradle: 依存解決の一時失敗が $total 回続いた。あきらめる"; exit "$rc"
  fi
  d=${DELAYS[$((attempt-1))]}
  hit=$(grep -Eo -- "$TRANSIENT" "$LOG" | head -n1)
  echo "::warning::retry_gradle: 依存解決の一時失敗 ($hit)。${d} 秒待って再試行 ($((attempt+1))/$total)"
  sleep "$d"
  attempt=$((attempt+1))
done
