#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""ライセンスの棚卸し: 追跡ファイルごとの SPDX・出所・公開版へ入れてよいか (docs/editions.md)。標準ライブラリのみ。ネットワーク不要。

入力: git の追跡ファイル (git ls-files) と tools/public_manifest.txt (公開版の定義。上から順に最初に当たった規則が決める)。
出力 (いずれも作業ツリーの内容を読む。法的な断定はしない: 「SPDX の表記」「manifest の規則 (= 出所の判断と理由)」を並べるだけ):

  license_inventory.py                      コンポーネントごとの集計 (人が読む表)
  license_inventory.py --markdown           docs/editions.md に貼る Markdown の表 (コンポーネント別の集計 + 除外 / 要確認の一覧)
  license_inventory.py --tsv                ファイルごとの機械可読な表 (パス / SPDX / SPDX の種別 / 規則 / 出所 / 判断 / 備考)
  license_inventory.py --json               同じ内容の JSON
  license_inventory.py --check              公開版の定義の矛盾を検査する (終了コード 1 = 問題あり):
                                              ・公開版に入る (include / overlay) ファイルが GPL / LGPL / AGPL 系の SPDX を持つ (relicense していない)
                                              ・relicense の対象が GPL 系の SPDX を持たない (古くなった規則)
                                              ・どの規則にも当たらない追跡ファイル (未分類 = 要確認)。公開版からは除外されるが、規則に書くことを求める
                                              ・規則が 1 つも当たらない (古くなった規則)
  license_inventory.py --update-doc FILE    FILE の <!-- inventory:table --> ... <!-- /inventory:table --> と <!-- inventory:lists --> ... の間を、--markdown / --lists の出力で置き換える (docs/editions.md)
  license_inventory.py --plan               公開版の書き出し計画 (export_public.sh が読む。1 行 1 ファイル: <動作>\\t<パス>[\\t<元ファイル>])

SPDX の種別: permissive (Apache-2.0 / MIT / BSD / 0BSD / Zlib ...) / dual (A OR B のどちらかが permissive: 公開版では permissive の側を選ぶ) /
copyleft (GPL / LGPL / AGPL 系。WITH の例外つきも含む) / other (上のどれでもない) / none (SPDX なし)。
SPDX が無いファイルは、冒頭の文章から GPL 系・BSD 系の文面を拾う (text-*)。拾えなければ none。
"""
import argparse
import collections
import fnmatch
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DEFAULT_MANIFEST = os.path.join(HERE, "public_manifest.txt")

PERMISSIVE = {"apache-2.0", "mit", "bsd-2-clause", "bsd-3-clause", "0bsd", "isc", "zlib", "unlicense", "cc0-1.0", "bsl-1.0"}
ACTIONS = ("include", "relicense", "exclude", "reserve", "overlay", "scrub")   # reserve = exclude と同じ。まだ無いファイルに先回りして書く (当たらなくても「古い規則」にしない)
ORIGINS = ("own", "own-gpl", "own-doc", "kernel", "kernel-shim", "kernel-patch", "external-gpl", "external-permissive", "dev", "generated")
CODE_EXT = {".c", ".h", ".cpp", ".cc", ".hpp", ".inc", ".kt", ".kts", ".java", ".py", ".sh", ".cmake", ".gradle"}


# ---------------------------------------------------------------- manifest
class Rule:
    def __init__(self, lineno, action, origin, glob, dest=None, text=""):
        self.reserved = action == "reserve"
        if self.reserved:
            action = "exclude"
        self.lineno, self.action, self.origin, self.glob, self.dest, self.text = lineno, action, origin, glob, dest, text
        self.rx = glob_to_regex(glob) if action != "scrub" else None
        self.hits = 0

    def __repr__(self):
        return "%s:%s %s %s %s" % ("manifest", self.lineno, self.action, self.origin, self.glob)


def glob_to_regex(g):
    out, i = [], 0
    while i < len(g):
        c = g[i]
        if c == "*":
            if g[i:i + 2] == "**":
                out.append(".*")
                i += 2
                continue
            out.append("[^/]*")
        elif c == "?":
            out.append("[^/]")
        else:
            out.append(re.escape(c))
        i += 1
    return re.compile("^" + "".join(out) + "$")


def parse_manifest(path):
    rules, scrubs = [], []
    with open(path, encoding="utf-8") as f:
        for n, raw in enumerate(f, 1):
            line = raw.rstrip("\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            body = line
            if line.split()[0] == "scrub":
                # scrub <origin> s!re!repl!  (# 以降のコメントは付けない)
                parts = line.split(None, 2)
                if len(parts) != 3 or not parts[2].startswith("s!"):
                    raise SystemExit("%s:%d: scrub の書式は 'scrub <出所> s!<正規表現>!<置換>!' です" % (path, n))
                spec = parts[2].split("!")
                if len(spec) < 4:
                    raise SystemExit("%s:%d: scrub の書式が不正です" % (path, n))
                scrubs.append((re.compile(spec[1]), spec[2]))
                continue
            if "#" in body:
                body, _, text = body.partition("#")
            else:
                text = ""
            toks = body.split()
            if not toks:
                continue
            act = toks[0]
            if act not in ACTIONS:
                raise SystemExit("%s:%d: 動作 '%s' は不明です (%s)" % (path, n, act, "/".join(ACTIONS)))
            if act == "overlay":
                if len(toks) != 4:
                    raise SystemExit("%s:%d: overlay は 'overlay <出所> <元ファイル> <置き場所>'" % (path, n))
                rules.append(Rule(n, act, toks[1], toks[2], dest=toks[3], text=text.strip()))
                continue
            if len(toks) != 3:
                raise SystemExit("%s:%d: '%s <出所> <glob>' の形ではありません: %s" % (path, n, act, line))
            if toks[1] not in ORIGINS:
                raise SystemExit("%s:%d: 出所 '%s' は不明です (%s)" % (path, n, toks[1], ", ".join(ORIGINS)))
            rules.append(Rule(n, act, toks[1], toks[2], text=text.strip()))
    return rules, scrubs


def decide(rules, path):
    """上から順に見て最初に当たった規則。overlay は「元ファイル」(glob) が path のときにも当たるが、それは書き出し側で扱うので、ここでは無視する。"""
    for r in rules:
        if r.action in ("overlay", "scrub"):
            continue
        if r.rx.match(path):
            r.hits += 1
            return r
    return None


# ---------------------------------------------------------------- SPDX
SPDX_RX = re.compile(r"SPDX-License-Identifier:[ \t]*([^\n]*)")
TOKEN = r"[A-Za-z0-9.+\-]+"
EXPR_RX = re.compile(r"^\(?\s*(" + TOKEN + r"(?:\s+(?:WITH|OR|AND)\s+" + TOKEN + r")*)")


def extract_spdx(text):
    m = SPDX_RX.search(text)
    if not m:
        return None
    rest = m.group(1).strip()
    rest = re.sub(r"\s*(\*/|-->)\s*$", "", rest)
    e = EXPR_RX.match(rest)
    return e.group(1) if e else rest.split()[0] if rest.split() else None


def is_copyleft_id(i):
    i = i.lower()
    return i.startswith(("gpl", "lgpl", "agpl"))


def classify_spdx(expr):
    """SPDX 式の種別: permissive / dual / copyleft / other / none"""
    if not expr:
        return "none"
    parts = re.split(r"\s+OR\s+", expr)
    classes = []
    for p in parts:
        ids = re.split(r"\s+AND\s+", p)
        cl = []
        for i in ids:
            base = re.split(r"\s+WITH\s+", i)[0].strip("()").strip()
            low = base.lower().rstrip("+")
            if is_copyleft_id(low):
                cl.append("copyleft")
            elif low in PERMISSIVE:
                cl.append("permissive")
            else:
                cl.append("other")
        classes.append("copyleft" if "copyleft" in cl else "other" if "other" in cl else "permissive")
    if len(parts) == 1:
        return classes[0]
    if "permissive" in classes:
        return "dual" if "copyleft" in classes or "other" in classes else "permissive"
    return "copyleft" if "copyleft" in classes else "other"


GPL_TEXT = re.compile(r"GNU (Lesser |Library )?General Public License|under the terms of the GNU|Free Software Foundation", re.I)
BSD_TEXT = re.compile(r"Redistribution and use in source and binary forms", re.I)
ZLIB_TEXT = re.compile(r"provided 'as-is'", re.I)


def read_head(path, n=8192):
    try:
        with open(path, "rb") as f:
            b = f.read(n)
    except OSError:
        return None
    if b"\0" in b[:4096]:
        return ""  # バイナリ
    return b.decode("utf-8", errors="replace")


def scan_file(path):
    """(spdx 式 or None, 種別, 出どころ spdx|text|none|binary)"""
    head = read_head(os.path.join(ROOT, path))
    if head is None:
        return None, "none", "unreadable"
    if head == "":
        return None, "none", "binary"
    e = extract_spdx(head)
    if e:
        return e, classify_spdx(e), "spdx"
    base = os.path.basename(path)
    ext = os.path.splitext(base)[1].lower()
    if ext not in CODE_EXT and not base.startswith(("COPYING", "LICENSE")) and base != "CMakeLists.txt":
        return None, "none", "none"   # ドキュメント・設定: 文面の検出は誤検出が多い (GPL を話題にしているだけ)。SPDX があれば上で拾っている
    top = "\n".join(head.splitlines()[:80])
    if GPL_TEXT.search(top):
        return None, "text-copyleft", "text"
    if BSD_TEXT.search(top):
        return None, "text-permissive", "text"
    if ZLIB_TEXT.search(top):
        return None, "text-permissive", "text"
    return None, "none", "none"


# 出所の注記 (要レビュー): 自前と書かれていても、Linux カーネルのコードを写した可能性を示す言及。誤検出を含む (人が読んで判断する)。
KREF = re.compile(r"(\bfs/[a-z0-9_]+/|Linux の fs/|Linux カーネル|カーネルのソース|カーネルのコード|kernel source|from the kernel|ported from|移植元)")


def kernel_refs(path):
    head = read_head(os.path.join(ROOT, path), 200000)
    if not head:
        return []
    out = []
    for i, l in enumerate(head.splitlines(), 1):
        if "SPDX" in l or l.lstrip().startswith(("#include", "# include")):
            continue
        if KREF.search(l):
            out.append((i, l.strip()[:160]))
    return out


# ---------------------------------------------------------------- inventory
COMPONENTS = [
    # (パスの glob, 表示名, 説明)
    ("core/src/**", "core/src", "自前: パーティション表・ディスク編集・USB MSC / SCSI・暗号 (AES / SHA / HMAC / PBKDF2 / XTS / Argon2)・LUKS1/2・BitLocker・QCOW2 / VHD / VHDX / VMDK / NBD・DiskTools (fsck / mkfs)・救出 / バックアップ"),
    ("core/include/**", "core/include", "自前: 公開 API ヘッダ"),
    ("core/tests/**", "core/tests", "自前: 上のテスト (暗号ベクタは公開値)"),
    ("core/*.cmake", "core/*.cmake, core/CMakeLists.txt", "自前: ビルド定義"),
    ("core/CMakeLists.txt", "core/*.cmake, core/CMakeLists.txt", "自前: ビルド定義"),
    ("core/kfsd/include/**", "core/kfsd: プロトコル・クライアント・サーバ共通部 (Apache-2.0)", "自前: 本体と kfsd の RPC (kfsd_proto.h) と RpcVolume・serve。ドライバを含まない"),
    ("core/kfsd/src/kfsd_client.cpp", "core/kfsd: プロトコル・クライアント・サーバ共通部 (Apache-2.0)", "自前: 本体と kfsd の RPC (kfsd_proto.h) と RpcVolume・serve。ドライバを含まない"),
    ("core/kfsd/src/kfsd_server.cpp", "core/kfsd: プロトコル・クライアント・サーバ共通部 (Apache-2.0)", "自前: 本体と kfsd の RPC (kfsd_proto.h) と RpcVolume・serve。ドライバを含まない"),
    ("core/kfsd/src/kfsd_io.h", "core/kfsd: プロトコル・クライアント・サーバ共通部 (Apache-2.0)", "自前: 本体と kfsd の RPC (kfsd_proto.h) と RpcVolume・serve。ドライバを含まない"),
    ("core/kfsd/tests/test_kfsd_proto.cpp", "core/kfsd: プロトコル・クライアント・サーバ共通部 (Apache-2.0)", "自前: 本体と kfsd の RPC (kfsd_proto.h) と RpcVolume・serve。ドライバを含まない"),
    ("core/kfsd/CMakeLists.txt", "core/kfsd: プロトコル・クライアント・サーバ共通部 (Apache-2.0)", "自前: 本体と kfsd の RPC (kfsd_proto.h) と RpcVolume・serve。ドライバを含まない"),
    ("core/kfsd/**", "core/kfsd: kfsd の実行ファイル・ドライバ呼び出し (GPL-2.0-or-later)", "カーネル FS ドライバを呼ぶ別プロセスの実行ファイル。個人版と別配布のプラグイン (docs/kfsd.md)"),
    ("core/fuseshim/**", "core/fuseshim (ntfs-3g / wimlib)", "外部 GPL / LGPL を動かす試作層 (GPL-2.0-or-later)"),
    ("core/fuse*", "core/fuseshim (ntfs-3g / wimlib)", "外部 GPL / LGPL を動かす試作層 (GPL-2.0-or-later)"),
    ("core/compat/include*/**", "core/compat: LinuxCompat ヘッダ", "カーネルの内部 API の再実装 (<linux/*.h>)"),
    ("core/compat/src/rt_*", "core/compat: LinuxCompat 実行時", "inode / dentry / ページキャッシュ / bio / buffer_head / jbd2 / VFS の再現"),
    ("core/compat/src/bdev_bridge.cpp", "core/compat: LinuxCompat 実行時", "inode / dentry / ページキャッシュ / bio / buffer_head / jbd2 / VFS の再現"),
    ("core/compat/src/f2fs*", "core/compat: F2FS (自前)", "自前: オンディスク形式から書き起こした読み書き。圧縮の展開だけ個人版はカーネル lib"),
    ("core/compat/public/usbmem/f2fs*", "core/compat: F2FS (自前)", "自前: オンディスク形式から書き起こした読み書き。圧縮の展開だけ個人版はカーネル lib"),
    ("core/compat/f2fs.cmake", "core/compat: F2FS (自前)", "自前: オンディスク形式から書き起こした読み書き。圧縮の展開だけ個人版はカーネル lib"),
    ("core/compat/tests/*f2fs*", "core/compat: F2FS (自前)", "自前: オンディスク形式から書き起こした読み書き。圧縮の展開だけ個人版はカーネル lib"),
    ("core/compat/src/btrfs*", "core/compat: Btrfs / XFS (自前だが GPL 表記)", "自前の読み書き。SPDX が GPL-2.0-or-later (カーネルの lib・形式定義への依存) のため公開版から除外"),
    ("core/compat/src/xfs*", "core/compat: Btrfs / XFS (自前だが GPL 表記)", "自前の読み書き。SPDX が GPL-2.0-or-later (カーネルの lib・形式定義への依存) のため公開版から除外"),
    ("core/compat/public/usbmem/btrfs*", "core/compat: Btrfs / XFS (自前だが GPL 表記)", "自前の読み書き。SPDX が GPL-2.0-or-later (カーネルの lib・形式定義への依存) のため公開版から除外"),
    ("core/compat/public/usbmem/xfs*", "core/compat: Btrfs / XFS (自前だが GPL 表記)", "自前の読み書き。SPDX が GPL-2.0-or-later (カーネルの lib・形式定義への依存) のため公開版から除外"),
    ("core/compat/public/usbmem/fs_volume.h", "core/compat: FsVolume 共通 IF・ネストマウント", "自前: FS 共通のインタフェースと、その上のネストマウント (GPL 表記は安全側。公開版では Apache-2.0 へ)"),
    ("core/compat/public/usbmem/vol_state.h", "core/compat: FsVolume 共通 IF・ネストマウント", "自前: FS 共通のインタフェースと、その上のネストマウント (GPL 表記は安全側。公開版では Apache-2.0 へ)"),
    ("core/compat/public/usbmem/nested_mount.h", "core/compat: FsVolume 共通 IF・ネストマウント", "自前: FS 共通のインタフェースと、その上のネストマウント (GPL 表記は安全側。公開版では Apache-2.0 へ)"),
    ("core/compat/src/fs_volume.cpp", "core/compat: FsVolume 共通 IF・ネストマウント", "自前: FS 共通のインタフェースと、その上のネストマウント (GPL 表記は安全側。公開版では Apache-2.0 へ)"),
    ("core/compat/src/nested_mount.cpp", "core/compat: FsVolume 共通 IF・ネストマウント", "自前: FS 共通のインタフェースと、その上のネストマウント (GPL 表記は安全側。公開版では Apache-2.0 へ)"),
    ("core/compat/src/*_volume.cpp", "core/compat: カーネル FS ドライバのラッパ・公開ヘッダ", "NTFS / exFAT / ext2 / ext4 / FAT / ISO 9660 / UDF / squashfs / erofs を呼ぶ薄い層 (GPL-2.0-or-later)"),
    ("core/compat/public/**", "core/compat: カーネル FS ドライバのラッパ・公開ヘッダ", "NTFS / exFAT / ext2 / ext4 / FAT / ISO 9660 / UDF / squashfs / erofs を呼ぶ薄い層 (GPL-2.0-or-later)"),
    ("core/compat/src/**", "core/compat: カーネル FS ドライバのラッパ・公開ヘッダ", "NTFS / exFAT / ext2 / ext4 / FAT / ISO 9660 / UDF / squashfs / erofs を呼ぶ薄い層 (GPL-2.0-or-later)"),
    ("core/compat/tests/**", "core/compat: カーネル FS / LinuxCompat のテスト", "上のドライバのテスト (GPL-2.0-or-later 表記が大半)"),
    ("core/compat/**", "core/compat: ビルド定義", "各カーネル FS のビルド定義"),
    ("third_party/ntfs-resurrection/**", "third_party/ntfs-resurrection", "Linux fs/ntfs (NTFS Resurrection)。GPL-2.0-or-later"),
    ("third_party/exfat/**", "third_party/exfat", "Linux fs/exfat。GPL-2.0-or-later"),
    ("third_party/ext2/**", "third_party/ext2", "Linux fs/ext2。GPL-2.0"),
    ("third_party/ext4/**", "third_party/ext4 (+jbd2)", "Linux fs/ext4 / fs/jbd2。GPL-2.0 (一部 LGPL-2.1)"),
    ("third_party/fat/**", "third_party/fat (+nls)", "Linux fs/fat・nls。GPL-2.0"),
    ("third_party/isofs/**", "third_party/isofs", "Linux fs/isofs。GPL-2.0"),
    ("third_party/udf/**", "third_party/udf", "Linux fs/udf。GPL-2.0 (一部 LGPL-2.0+)"),
    ("third_party/squashfs/**", "third_party/squashfs", "Linux fs/squashfs。GPL-2.0"),
    ("third_party/erofs/**", "third_party/erofs", "Linux fs/erofs。GPL-2.0"),
    ("third_party/kernel-lib/**", "third_party/kernel-lib", "Linux lib/ の zlib・lz4・lzo・xz・zstd・xxhash (GPL-2.0 / BSD / 0BSD / zlib の混在)"),
    ("third_party/**", "third_party/その他", "Linux カーネル由来"),
    ("android/app/src/main/java/**", "android: Kotlin アプリ", "自前の Kotlin。AndroidX / Compose (Apache-2.0) に依存"),
    ("android/app/src/main/cpp/**", "android: JNI", "自前の JNI (FS の分岐は USBMEM_FS_* の条件コンパイル)"),
    ("android/app/src/androidTest/**", "android: 計装テスト", "実機・エミュレータ用。カーネル FS のフィクスチャ・個人の機材を前提にする"),
    ("android/app/src/test/**", "android: JVM 単体テスト", "JUnit"),
    ("android/**", "android: ビルド設定・リソース・ラッパー", "Gradle / マニフェスト / リソース / Gradle ラッパー (Apache-2.0)"),
    ("tools/fuse/**", "tools: FUSE アダプター・fuseshim・xfstests", "GPL 表記の自前アダプターと、ntfs-3g / wimlib の取得・試験"),
    ("tools/fuseshim/**", "tools: FUSE アダプター・fuseshim・xfstests", "GPL 表記の自前アダプターと、ntfs-3g / wimlib の取得・試験"),
    ("tools/**", "tools", "テスト・フィクスチャ生成・CI の補助 (公開版に入るのは公開版のテストが使うものだけ)"),
    ("docs/**", "docs", "ドキュメント (公開版に入るのは公開版の機能に関するものだけ)"),
    (".github/**", ".github (CI)", "個人版の CI"),
    ("**", "ルート", "README など"),
]
_COMP = [(glob_to_regex(g), n, d) for g, n, d in COMPONENTS]


def component_of(path):
    for rx, n, d in _COMP:
        if rx.match(path):
            return n, d
    return "その他", ""


def tracked_files():
    out = subprocess.run(["git", "-C", ROOT, "ls-files", "-z"], check=True, capture_output=True).stdout
    return [p for p in out.decode("utf-8").split("\0") if p]


def build(manifest=DEFAULT_MANIFEST):
    rules, scrubs = parse_manifest(manifest)
    rows = []
    overlays = [r for r in rules if r.action == "overlay"]
    for p in tracked_files():
        if not os.path.isfile(os.path.join(ROOT, p)):
            continue  # 追跡されているが作業ツリーに無い (削除中)。数えない
        expr, cls, src = scan_file(p)
        r = decide(rules, p)
        if r is None:
            action, origin, why = "unclassified", "?", "どの規則にも当たらない (要確認 = 公開版から除外)"
        else:
            action, origin, why = r.action, r.origin, r.text
        comp, _ = component_of(p)
        publish = action in ("include", "relicense")
        eff_cls = "permissive" if action == "relicense" else cls
        row = {"path": p, "spdx": expr or "-", "spdx_class": cls, "spdx_source": src, "action": action, "origin": origin,
               "rule": ("%d" % r.lineno) if r else "-", "publish": publish, "component": comp, "reason": why, "note": []}
        if publish and eff_cls in ("copyleft", "text-copyleft", "other"):
            row["note"].append("公開版に入る (%s) が SPDX / 文面が %s (%s)" % (action, cls, expr or "文面"))
        if action == "relicense" and cls not in ("copyleft", "text-copyleft"):
            row["note"].append("relicense の対象だが GPL 系の SPDX が無い (古い規則)")
        if action == "include" and cls == "dual":
            row["note"].append("dual: 公開版では permissive の側 (%s) で出す" % expr)
        if action == "unclassified":
            row["note"].append("未分類")
        if publish and origin == "own":
            refs = kernel_refs(p)
            if refs:
                row["note"].append("出所の注記 %d 行 (要レビュー。例: L%d %s)" % (len(refs), refs[0][0], refs[0][1][:80]))
                row["kernel_refs"] = len(refs)
        rows.append(row)
    for o in overlays:
        # overlay 先は追跡ファイルではないので、行を足す (公開版に入る新規ファイル)
        src_ok = os.path.isfile(os.path.join(ROOT, o.glob))
        row = {"path": o.dest, "spdx": "-", "spdx_class": "none", "spdx_source": "overlay", "action": "overlay", "origin": o.origin,
               "rule": "%d" % o.lineno, "publish": True, "component": component_of(o.dest)[0], "reason": o.text + " (雛形: %s)" % o.glob, "note": [] if src_ok else ["overlay の元ファイルが無い: " + o.glob]}
        if src_ok:
            expr, cls, _ = scan_file(o.glob)
            row["spdx"], row["spdx_class"] = expr or "-", cls
            if cls in ("copyleft", "text-copyleft"):
                row["note"].append("overlay の雛形が GPL 系")
        rows.append(row)
    return rules, scrubs, rows


def problems(rules, rows):
    msgs = []
    for r in rows:
        for n in r["note"]:
            if n.startswith(("公開版に入る", "relicense", "未分類", "overlay")):
                msgs.append("%s: %s" % (r["path"], n))
    for r in rules:
        if r.action in ("overlay",):
            if not os.path.isfile(os.path.join(ROOT, r.glob)):
                msgs.append("manifest:%d: overlay の元ファイルが追跡されていない: %s" % (r.lineno, r.glob))
        elif r.hits == 0 and not r.reserved:
            msgs.append("manifest:%d: どのファイルにも当たらない規則 (古い規則?): %s %s" % (r.lineno, r.action, r.glob))
    return msgs


# ---------------------------------------------------------------- reports
def summarize(rows):
    comps = collections.OrderedDict()
    for r in rows:
        c = comps.setdefault(r["component"], {"files": 0, "spdx": collections.Counter(), "inc": 0, "rel": 0, "exc": 0, "unc": 0, "origins": collections.Counter(), "desc": component_of(r["path"])[1]})
        c["files"] += 1
        c["spdx"][r["spdx"] if r["spdx"] != "-" else ("(文面: GPL 系)" if r["spdx_class"] == "text-copyleft" else "(なし)")] += 1
        c["origins"][r["origin"]] += 1
        if r["action"] in ("include", "overlay"):
            c["inc"] += 1
        elif r["action"] == "relicense":
            c["rel"] += 1
        elif r["action"] == "unclassified":
            c["unc"] += 1
        else:
            c["exc"] += 1
    return comps


def spdx_brief(counter, limit=4):
    items = counter.most_common()
    s = ", ".join("%s %d" % (k, v) for k, v in items[:limit])
    if len(items) > limit:
        s += ", ほか %d 種" % (len(items) - limit)
    return s


def decision_of(c):
    if c["unc"]:
        return "要確認 (一部未分類 = 除外)"
    if c["exc"] == 0 and (c["inc"] or c["rel"]):
        return "公開版に入れる" + (" (SPDX を Apache-2.0 へ書き換え %d)" % c["rel"] if c["rel"] else "")
    if c["inc"] + c["rel"] == 0:
        return "公開版から除外"
    return "一部だけ入れる (入る %d / 除外 %d)" % (c["inc"] + c["rel"], c["exc"])


def report_text(rows):
    comps = summarize(rows)
    w = max(len(k) for k in comps)
    lines = ["%-*s  %5s  %5s  %s" % (w, "コンポーネント", "ファイル", "公開版", "SPDX の内訳 / 判断")]
    for k, c in comps.items():
        lines.append("%-*s  %8d  %6d  %s | %s" % (w, k, c["files"], c["inc"] + c["rel"], spdx_brief(c["spdx"]), decision_of(c)))
    tot = len(rows)
    pub = sum(1 for r in rows if r["publish"])
    lines.append("")
    lines.append("追跡ファイル %d (うち overlay の新規 %d)。公開版に入る %d / 入らない %d (未分類 %d)" % (
        tot - sum(1 for r in rows if r["action"] == "overlay"), sum(1 for r in rows if r["action"] == "overlay"), pub, tot - pub,
        sum(1 for r in rows if r["action"] == "unclassified")))
    return "\n".join(lines)


def report_markdown(rows):
    comps = summarize(rows)
    o = ["| コンポーネント | ファイル | SPDX の内訳 | 出所 | 判断 | 公開版 | 説明 |", "|---|--:|---|---|---|--:|---|"]
    for k, c in comps.items():
        org = ", ".join("%s %d" % (a, b) for a, b in c["origins"].most_common())
        o.append("| `%s` | %d | %s | %s | %s | %d | %s |" % (k, c["files"], spdx_brief(c["spdx"], 5), org, decision_of(c), c["inc"] + c["rel"], c["desc"]))
    return "\n".join(o)


def report_lists(rows):
    out = []
    rel = [r for r in rows if r["action"] == "relicense"]
    out.append("### SPDX を Apache-2.0 へ書き換えて出すもの (relicense: %d)\n" % len(rel))
    out.append("| ファイル | 元の SPDX | 理由 |\n|---|---|---|")
    for r in rel:
        out.append("| `%s` | %s | %s |" % (r["path"], r["spdx"], r["reason"]))
    ex = [r for r in rows if r["action"] == "exclude" and r["origin"] in ("own-gpl", "own") and r["spdx_class"] != "none" or r["action"] == "exclude" and r["origin"] == "own-gpl"]
    out.append("\n### 自前だが公開版から外すもの (%d)\n" % len(ex))
    out.append("| ファイル | SPDX | 理由 |\n|---|---|---|")
    for r in ex:
        out.append("| `%s` | %s | %s |" % (r["path"], r["spdx"], r["reason"]))
    refs = [r for r in rows if r.get("kernel_refs")]
    out.append("\n### 公開版に入る「自前」のうち、カーネルへの言及があるもの (要レビュー: %d)\n" % len(refs))
    out.append("自前と書かれたファイルの中に、`fs/<名前>/`・「Linux カーネル」・「カーネルのソース」などの言及がある行数。読んだだけ (仕様の確認) か、写したかを人が判断する。\n")
    out.append("| ファイル | 行数 | 例 |\n|---|--:|---|")
    for r in refs:
        ex_ = [n for n in r["note"] if n.startswith("出所の注記")]
        out.append("| `%s` | %d | %s |" % (r["path"], r["kernel_refs"], ex_[0].split("例: ", 1)[-1].rstrip(")") if ex_ else ""))
    unc = [r for r in rows if r["action"] == "unclassified"]
    out.append("\n### 未分類 (どの規則にも当たらない = 要確認 = 除外: %d)\n" % len(unc))
    for r in unc:
        out.append("- `%s`" % r["path"])
    if not unc:
        out.append("(なし)")
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--manifest", default=DEFAULT_MANIFEST)
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--markdown", action="store_true")
    g.add_argument("--lists", action="store_true", help="relicense / 自前だが除外 / 出所の注記 / 未分類の一覧 (Markdown)")
    g.add_argument("--tsv", action="store_true")
    g.add_argument("--json", action="store_true")
    g.add_argument("--check", action="store_true")
    g.add_argument("--plan", action="store_true")
    g.add_argument("--update-doc", metavar="FILE")
    a = ap.parse_args()
    rules, scrubs, rows = build(a.manifest)
    if a.tsv:
        print("\t".join(["path", "spdx", "spdx_class", "origin", "action", "rule", "note"]))
        for r in rows:
            print("\t".join([r["path"], r["spdx"], r["spdx_class"], r["origin"], r["action"], r["rule"], "; ".join(r["note"])]))
    elif a.json:
        print(json.dumps(rows, ensure_ascii=False, indent=1))
    elif a.markdown:
        print(report_markdown(rows))
    elif a.lists:
        print(report_lists(rows))
    elif a.update_doc:
        with open(a.update_doc, encoding="utf-8") as f:
            doc = f.read()
        for name, body in (("table", report_markdown(rows)), ("lists", report_lists(rows))):
            rx = re.compile(r"(<!-- inventory:%s -->\n).*?(<!-- /inventory:%s -->)" % (name, name), re.S)
            if not rx.search(doc):
                raise SystemExit("%s に <!-- inventory:%s --> の組が無い" % (a.update_doc, name))
            doc = rx.sub(lambda m: m.group(1) + body.rstrip("\n") + "\n" + m.group(2), doc)
        with open(a.update_doc, "w", encoding="utf-8") as f:
            f.write(doc)
        print("更新: " + a.update_doc)
    elif a.plan:
        for r in rows:
            if r["action"] == "overlay":
                rule = [x for x in rules if x.action == "overlay" and x.dest == r["path"]][0]
                print("overlay\t%s\t%s" % (r["path"], rule.glob))
            elif r["publish"]:
                print("%s\t%s" % (r["action"], r["path"]))
    elif a.check:
        ms = problems(rules, rows)
        for m in ms:
            print("NG:", m)
        print("%d 件の問題" % len(ms) if ms else "OK: 公開版の定義に矛盾はない (公開版に入る %d ファイル)" % sum(1 for r in rows if r["publish"]))
        return 1 if ms else 0
    else:
        print(report_text(rows))
    return 0


if __name__ == "__main__":
    sys.exit(main())
