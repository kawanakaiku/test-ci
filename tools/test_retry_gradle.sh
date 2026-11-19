#!/bin/bash
# tools/ci/retry_gradle.sh の単体試験 (fake の gradle を差す。Gradle / ネットワーク不要。待ちは 0 秒にする)。
#   使い方: bash tools/test_retry_gradle.sh
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
RG="$HERE/ci/retry_gradle.sh"
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
export RETRY_GRADLE_DELAYS="0 0" TMPDIR=$T
fail=0
# fake: 呼び出し回数を数え、$FAKE_SCRIPT[n] (n 回目の出力ファイル) を出力して、$FAKE_RC[n] で終わる
cat > "$T/fake" <<'EOS'
#!/bin/bash
n=$(( $(cat "$T/count" 2>/dev/null || echo 0) + 1 )); echo $n > "$T/count"
f="$T/out.$n"; [ -f "$f" ] || f="$T/out.last"
cat "$f"; exit "$(cat "$f.rc" 2>/dev/null || cat "$T/out.last.rc")"
EOS
chmod +x "$T/fake"; export T
setup() { rm -f "$T"/count "$T"/out.*; }
out() { printf '%s\n' "$2" > "$T/out.$1"; echo "$3" > "$T/out.$1.rc"; }
run() { "$RG" "$T/fake" > "$T/stdout" 2>&1; rc=$?; calls=$(cat "$T/count" 2>/dev/null || echo 0); }
check() { # 名前 期待rc 期待呼び出し回数
  if [ "$rc" = "$2" ] && [ "$calls" = "$3" ]; then echo "ok   $1 (rc=$rc calls=$calls)"; else echo "FAIL $1: rc=$rc (期待 $2) calls=$calls (期待 $3)"; sed 's/^/    | /' "$T/stdout"; fail=1; fi
}
S429='> Could not resolve org.jetbrains.kotlin:kotlin-stdlib:2.2.0.
   > Could not GET https://dl.google.com/x.pom. Received status code 429 from server: Too Many Requests'
NF='Could not find org.jetbrains.kotlin:kotlin-stdlib:2.2.0.
Searched in the following locations:'

setup; out 1 "$S429" 1; out 2 "BUILD SUCCESSFUL" 0; run; check "429 の次に成功 (2 回で成功)" 0 2
setup; out 1 "$NF" 1; out 2 "$S429" 1; out 3 "BUILD SUCCESSFUL" 0; run; check "Could not find と 429 のあと 3 回目で成功" 0 3
setup; out last "$S429" 1; run; check "ずっと 429 (3 回でやめる)" 1 3
setup; out last "BUILD SUCCESSFUL" 0; run; check "最初から成功 (再試行なし)" 0 1
setup; out last "e: file:///a/Foo.kt:3:5 Unresolved reference: bar
FAILURE: Build failed with an exception.
Execution failed for task ':app:compileDebugKotlin'." 1; run; check "Kotlin コンパイルエラー (再試行しない)" 1 1
setup; out last "/a/b.c:10:3: error: expected ';'
Execution failed for task ':app:buildCMakeDebug[arm64-v8a]'." 1; run; check "C++ コンパイルエラー (再試行しない)" 1 1
setup; out last "FAILED
3 tests completed, 1 failed
There were failing tests. Read request timeout handling test: Read timed out" 1; run; check "テスト失敗 (文中に timed out があっても再試行しない)" 1 1
setup; out last "$S429
e: file:///a/Foo.kt:3:5 Syntax error" 1; run; check "429 とコンパイルエラーが同居 (再試行しない)" 1 1
setup; out 1 "Execution failed for task ':app:compileDebugKotlin'.
> Could not resolve all files for configuration ':app:debugCompileClasspath'.
   > Could not resolve org.jetbrains.kotlin:kotlin-stdlib:2.2.0." 1; out 2 "BUILD SUCCESSFUL" 0; run; check "compile タスク内の依存解決失敗は再試行する" 0 2
setup; out last "A problem occurred evaluating project ':app'. Could not find method foo() for arguments" 1; run; check "Could not find method (スクリプトの誤り。再試行しない)" 1 1
setup; out last "Something else went wrong" 3; run; check "しるしの無い失敗 (終了コードそのまま)" 3 1
setup; out 1 "Caused by: java.net.SocketException: Connection reset" 1; out 2 "BUILD SUCCESSFUL" 0; run; check "Connection reset のあと成功" 0 2
setup; out 1 "Read timed out" 1; out 2 "OK" 0; run; check "Read timed out のあと成功" 0 2
# 待ち時間の指定が効くこと (DELAYS の要素数 = やり直しの回数)
setup; out last "$S429" 1; RETRY_GRADLE_DELAYS="0" run; check "DELAYS が 1 要素なら 2 回でやめる" 1 2
exit $fail
