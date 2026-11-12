#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""ディスクイメージ形式 (QCOW2/VHD/VHDX/VMDK) のテスト用フィクスチャを作る。使用: gen_blockdev_fixtures.py <outdir>

C++ 実装とは独立に作る:
  - 標準形式は qemu-img (と qemu-io) が書く。出来たイメージは `qemu-img compare` で元の RAW と一致することを毎回確認する。
  - qemu-img で作れない VHD 差分 / VHDX 差分 / 複数エクステント VMDK / 壊れたイメージは、この script が仕様から直接書く。
出力: <outdir>/manifest.tsv  (タブ区切り)
   image  format  expected_raw  expect  note
   expect = ok | unsupported | corrupt
   expected_raw = 展開した結果として期待される RAW ファイル名 (expect != ok なら "-")
パーティション + ext2 を含む main ディスク (MBR / GPT) を元データにするので、そのままパーティション解析・FS 検出の試験にも使う。
"""
import os, random, shutil, struct, subprocess, sys, uuid, zlib

os.environ["PATH"] = os.path.expanduser("~/.local/bin") + ":" + os.environ.get("PATH", "") + ":/sbin:/usr/sbin"
OUT = os.path.abspath(sys.argv[1])
if shutil.which("qemu-img") is None or shutil.which("mke2fs") is None or shutil.which("sfdisk") is None:
    print("SKIP: qemu-img / mke2fs / sfdisk not found (blockdev fixtures not generated)")
    sys.exit(77)   # ctest の SKIP_RETURN_CODE
MiB = 1 << 20
rows = []
HAVE_QEMU_IO = shutil.which("qemu-io") is not None


def run(*a, check=True, **kw):
    r = subprocess.run(a, stdout=subprocess.PIPE, stderr=subprocess.PIPE, **kw)
    if check and r.returncode != 0:
        sys.stderr.write("FAILED: %s\n%s\n" % (" ".join(a), r.stderr.decode(errors="replace")))
        raise SystemExit(1)
    return r


def pattern(seed, off, n):
    return bytes((((off + i) * 31 + seed * 7 + ((off + i) >> 8)) & 0xFF) for i in range(n))


def put(path, off, data):
    with open(path, "r+b") as f:
        f.seek(off)
        f.write(data)


def new_sparse(path, size):
    with open(path, "wb") as f:
        f.truncate(size)


def add(image, fmt, raw, expect="ok", note=""):
    rows.append((image, fmt, raw, expect, note))


def qemu_compare(fmt, image, raw):
    r = run("qemu-img", "compare", "-f", fmt, "-F", "raw", os.path.join(OUT, image), os.path.join(OUT, raw), check=False)
    if r.returncode != 0:
        sys.stderr.write("qemu-img compare mismatch for %s (%s): %s%s\n" % (image, fmt, r.stdout.decode(), r.stderr.decode()))
        raise SystemExit(1)


def convert(src, fmt, dst, *opts, qfmt=None, extra=()):
    a = ["qemu-img", "convert", "-f", "raw", "-O", fmt] + list(extra)
    if opts:
        a += ["-o", ",".join(opts)]
    run(*a, os.path.join(OUT, src), os.path.join(OUT, dst))
    qemu_compare(qfmt or fmt, dst, src)


# ------------------------------------------------------------------ 元ディスク
def make_ext2(path, size, seedtag):
    tree = os.path.join(OUT, "_tree_" + seedtag)
    shutil.rmtree(tree, ignore_errors=True)
    os.makedirs(tree + "/dir/sub")
    with open(tree + "/hello.txt", "wb") as f:
        f.write(b"hello from %s\n" % seedtag.encode())
    with open(tree + "/dir/big.bin", "wb") as f:
        f.write(pattern(3, 0, 1500000))
    with open(tree + "/dir/sub/zeros.bin", "wb") as f:
        f.write(b"\0" * 300000)
    with open(path, "wb") as f:
        f.truncate(size)
    run("mke2fs", "-q", "-t", "ext2", "-b", "1024", "-d", tree, "-L", "BDTEST", "-F", path)
    shutil.rmtree(tree)


def make_main(name, label):
    """40 MiB: [1 MiB 空き][16 MiB ext2][8 MiB 模様][空き + 境界をまたぐデータ]"""
    path = os.path.join(OUT, name)
    size = 40 * MiB
    new_sparse(path, size)
    ext = os.path.join(OUT, "_ext2.img")
    make_ext2(ext, 16 * MiB, "main")
    with open(ext, "rb") as f:
        put(path, 1 * MiB, f.read())
    os.unlink(ext)
    put(path, 17 * MiB, pattern(5, 0, 8 * MiB))                        # p2: 模様
    rng = random.Random(1234)
    put(path, 31 * MiB + 900 * 1024, rng.randbytes(300 * 1024))          # 32 MiB 境界をまたぐ非圧縮データ
    put(path, 36 * MiB + 777, b"\xaa" * (700 * 1024))                    # 圧縮しやすい非整列データ
    put(path, size - 512, b"TAIL" * 128)                                 # 末尾セクタ
    if label == "mbr":
        script = "label: dos\nlabel-id: 0x1234abcd\nstart=2048, size=32768, type=83\nstart=34816, size=16384, type=c\n"
    else:
        script = ("label: gpt\nlabel-id: 11111111-2222-3333-4444-555555555555\n"
                  "start=2048, size=32768, type=0FC63DAF-8483-4772-8E79-3D69D8477DE4, name=\"rootfs\"\n"
                  "start=34816, size=16384, type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7, name=\"data\"\n")
    # GPT は末尾 (バックアップ) を書くので、sfdisk の書く領域を先に確保したうえで走らせる
    r = subprocess.run(["sfdisk", "-q", path], input=script.encode(), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if r.returncode != 0:
        sys.stderr.write(r.stderr.decode())
        raise SystemExit(1)
    return name


def make_plain(name, size):
    path = os.path.join(OUT, name)
    new_sparse(path, size)
    rng = random.Random(99)
    put(path, 0, pattern(1, 0, 1 * MiB))
    put(path, 3 * MiB + 123, pattern(2, 0, 200 * 1024))
    put(path, 2 * MiB - 700, rng.randbytes(1500))                       # 2 MiB 境界をまたぐ
    put(path, size // 2, b"\x55" * 4096)
    put(path, size - 512, pattern(7, 0, 512))
    return name


# ------------------------------------------------------------------ 自前の VHD 差分ライター
def vhd_checksum(b, off):
    return (~sum(b[:off] + b[off + 4:])) & 0xFFFFFFFF


def vhd_footer(size, dtype, data_off, uid):
    f = bytearray(512)
    f[0:8] = b"conectix"
    struct.pack_into(">IIQ", f, 8, 2, 0x00010000, data_off)
    struct.pack_into(">I", f, 24, 0)
    f[28:32] = b"tst "
    struct.pack_into(">I", f, 32, 0x00010000)
    f[36:40] = b"Wi2k"
    struct.pack_into(">QQ", f, 40, size, size)
    struct.pack_into(">I", f, 56, 0x00100f3f)
    struct.pack_into(">I", f, 60, dtype)
    f[68:84] = uid
    struct.pack_into(">I", f, 64, vhd_checksum(bytes(f), 64))
    return bytes(f)


def read_vhd_uuid(path):
    with open(path, "rb") as f:
        f.seek(-512, 2)
        ft = f.read(512)
    assert ft[:8] == b"conectix"
    return ft[68:84]


def write_vhd_diff(path, parent_name, parent_uuid, raw_parent, size, overrides, block_size=2 * MiB, shuffle_seed=5):
    """overrides: {sector: 512 B}。raw_parent は親の期待内容。戻り値 = 差分の期待 RAW。"""
    expected = bytearray(raw_parent)
    nblocks = (size + block_size - 1) // block_size
    bm_bytes = ((block_size // 512 + 7) // 8 + 511) // 512 * 512
    by_block = {}
    for s, data in overrides.items():
        by_block.setdefault(s * 512 // block_size, {})[s] = data
        expected[s * 512:s * 512 + 512] = data
    uid = uuid.uuid4().bytes
    bat_off = 512 + 1024
    bat_size = (nblocks * 4 + 511) // 512 * 512
    loc_off = bat_off + bat_size
    loc_w2ru = parent_name.encode("utf-16-le")
    loc_w2ku = ("/abs/" + parent_name).encode("utf-16-le")
    loc_space = 512
    blocks_off = loc_off + 2 * loc_space
    bat = [0xFFFFFFFF] * nblocks
    body = bytearray()
    rng = random.Random(shuffle_seed)
    pos = blocks_off
    for bi in sorted(by_block):
        bm = bytearray(bm_bytes)
        data = bytearray(rng.randbytes(block_size))          # 立っていないセクタの中身はゴミ (読まれてはいけない)
        for s, d in by_block[bi].items():
            sec = s - bi * block_size // 512
            bm[sec // 8] |= 0x80 >> (sec % 8)
            data[sec * 512:sec * 512 + 512] = d
        bat[bi] = pos // 512
        body += bm + data
        pos += bm_bytes + block_size
    dh = bytearray(1024)
    dh[0:8] = b"cxsparse"
    struct.pack_into(">QQ", dh, 8, 0xFFFFFFFFFFFFFFFF, bat_off)
    struct.pack_into(">III", dh, 24, 0x00010000, nblocks, block_size)
    dh[40:56] = parent_uuid
    struct.pack_into(">I", dh, 56, 0)
    dh[64:64 + 512] = parent_name.encode("utf-16-be")[:512].ljust(512, b"\0")
    for i, (code, blob, off) in enumerate([(b"W2ru", loc_w2ru, loc_off), (b"W2ku", loc_w2ku, loc_off + loc_space)]):
        struct.pack_into(">4sIIIQ", dh, 576 + 24 * i, code, loc_space // 512, len(blob), 0, off)
    struct.pack_into(">I", dh, 36, vhd_checksum(bytes(dh), 36))
    ft = vhd_footer(size, 4, 512, uid)
    with open(path, "wb") as f:
        f.write(ft)
        f.write(dh)
        f.write(b"".join(struct.pack(">I", e) for e in bat).ljust(bat_size, b"\0"))
        f.write(loc_w2ru.ljust(loc_space, b"\0"))
        f.write(loc_w2ku.ljust(loc_space, b"\0"))
        f.write(body)
        f.write(ft)
    return bytes(expected), uid


# ------------------------------------------------------------------ 自前の VHDX 差分ライター
def crc32c(data):
    crc = 0xFFFFFFFF
    for b in data:
        crc = CRC32C_TABLE[(crc ^ b) & 0xFF] ^ (crc >> 8)
    return crc ^ 0xFFFFFFFF


CRC32C_TABLE = []
for _i in range(256):
    _c = _i
    for _ in range(8):
        _c = (_c >> 1) ^ 0x82F63B78 if _c & 1 else _c >> 1
    CRC32C_TABLE.append(_c)


def guid_bytes(s):
    return uuid.UUID(s).bytes_le


def write_vhdx_diff(path, parent_name, raw_parent, size, overrides, block_size=1 * MiB, lss=512, corrupt=None):
    """VHDX 差分。overrides: {sector: lss バイト}。戻り値 = 期待 RAW。"""
    expected = bytearray(raw_parent)
    nblocks = (size + block_size - 1) // block_size
    ratio = (1 << 23) * lss // block_size
    nchunks = (nblocks + ratio - 1) // ratio
    total_entries = nchunks * (ratio + 1)
    by_block = {}
    for s, data in overrides.items():
        assert len(data) == lss
        by_block.setdefault(s * lss // block_size, {})[s] = data
        expected[s * lss:s * lss + lss] = data
    bat_off = 3 * MiB
    bat_len = ((total_entries * 8) + MiB - 1) // MiB * MiB
    meta_off = bat_off + bat_len
    meta_len = 1 * MiB
    data_off = meta_off + meta_len
    # ブロックの配置: 各チャンクのビットマップ (1 MiB) + 部分的に存在するブロック
    bat = [0] * total_entries
    layout = []   # (offset, bytes)
    pos = data_off
    rng = random.Random(77)
    chunk_bitmaps = {}
    for bi in sorted(by_block):
        ch = bi // ratio
        if ch not in chunk_bitmaps:
            chunk_bitmaps[ch] = (pos, bytearray(MiB))
            bat[(ch + 1) * ratio + ch] = (pos // MiB << 20) | 6
            pos += MiB
        bm_off, bm = chunk_bitmaps[ch]
        data = bytearray(rng.randbytes(block_size))
        for s, d in by_block[bi].items():
            sec_in_chunk = s - ch * ratio * block_size // lss
            bm[sec_in_chunk // 8] |= 1 << (sec_in_chunk % 8)
            o = (s * lss) - bi * block_size
            data[o:o + lss] = d
        bat[bi + bi // ratio] = (pos // MiB << 20) | 7
        layout.append((pos, bytes(data)))
        pos += block_size
    for bm_off, bm in chunk_bitmaps.values():
        layout.append((bm_off, bytes(bm)))
    # 親ロケータ
    ents = [("parent_linkage", str(uuid.uuid4())), ("relative_path", parent_name), ("absolute_win32_path", "C:\\vm\\" + parent_name)]
    keys = b""
    entry_tbl = b""
    kvdata = b""
    base = 20 + 12 * len(ents)
    off = base
    for k, v in ents:
        kb, vb = k.encode("utf-16-le"), v.encode("utf-16-le")
        entry_tbl += struct.pack("<IIHH", off, off + len(kb), len(kb), len(vb))
        kvdata += kb + vb
        off += len(kb) + len(vb)
    locator = guid_bytes("B04AEFB7-D19E-4A81-B789-25B8E9445913") + struct.pack("<HH", 0, len(ents)) + entry_tbl + kvdata
    items = [
        ("CAA16737-FA36-4D43-B3B6-33F0AA44E76B", struct.pack("<II", block_size, 2), 0x4),                    # file parameters: HasParent
        ("2FA54224-CD1B-4876-B211-5DBED83BF4B8", struct.pack("<Q", size), 0x6),                              # virtual disk size
        ("BECA12AB-B2E6-4523-93EF-C309E000C746", uuid.uuid4().bytes_le, 0x6),                                 # virtual disk id
        ("8141BF1D-A96F-4709-BA47-F233A8FAAB5F", struct.pack("<I", lss), 0x6),                                # logical sector size
        ("CDA348C7-445D-4471-9CC9-E9885251C556", struct.pack("<I", lss), 0x6),                                # physical sector size
        ("A8D35F2D-B30B-454D-ABF7-D3D84834AB0C", locator, 0x4),                                              # parent locator
    ]
    md = bytearray(meta_len)
    md[0:8] = b"metadata"
    struct.pack_into("<HH", md, 8, 0, len(items))
    doff = 64 * 1024
    for i, (g, d, fl) in enumerate(items):
        e = 32 + 32 * i
        md[e:e + 16] = guid_bytes(g)
        struct.pack_into("<III", md, e + 16, doff, len(d), fl)
        md[doff:doff + len(d)] = d
        doff += (len(d) + 7) // 8 * 8
    # ヘッダ 2 つ + リージョンテーブル 2 つ
    def header(seq):
        h = bytearray(4096)
        h[0:4] = b"head"
        struct.pack_into("<Q", h, 8, seq)
        h[16:32] = uuid.uuid4().bytes_le
        h[32:48] = uuid.uuid4().bytes_le
        struct.pack_into("<HH", h, 64, 0, 1)           # LogVersion 0, Version 1
        struct.pack_into("<I", h, 68, 1 * MiB)
        struct.pack_into("<Q", h, 72, 2 * MiB)         # ログ領域 (ログ GUID = 0 なので使わない)
        struct.pack_into("<I", h, 4, crc32c(bytes(h)))
        return bytes(h)
    rt = bytearray(64 * 1024)
    rt[0:4] = b"regi"
    struct.pack_into("<II", rt, 8, 2, 0)
    for i, (g, o, l) in enumerate([("2DC27766-F623-4200-9D64-115E9BFD4A08", bat_off, bat_len), ("8B7CA206-4790-4B9A-B8FE-575F050F886E", meta_off, meta_len)]):
        e = 16 + 32 * i
        rt[e:e + 16] = guid_bytes(g)
        struct.pack_into("<QII", rt, e + 16, o, l, 1)
    struct.pack_into("<I", rt, 4, crc32c(bytes(rt)))
    with open(path, "wb") as f:
        f.truncate(pos)
        f.write(b"vhdxfile" + "tst".encode("utf-16-le"))
        f.seek(64 * 1024); f.write(header(1))
        f.seek(128 * 1024); f.write(header(2))
        f.seek(192 * 1024); f.write(rt)
        f.seek(256 * 1024); f.write(rt)
        f.seek(bat_off); f.write(b"".join(struct.pack("<Q", e) for e in bat))
        f.seek(meta_off); f.write(md)
        for o, b in layout:
            f.seek(o); f.write(b)
    if corrupt == "header-crc":
        put(path, 64 * 1024 + 4, b"\xde\xad\xbe\xef")
        put(path, 128 * 1024 + 4, b"\xde\xad\xbe\xef")
    return bytes(expected)


# ------------------------------------------------------------------ main
def main():
    shutil.rmtree(OUT, ignore_errors=True)
    os.makedirs(OUT)
    mbr = make_main("main_mbr.raw", "mbr")
    gpt = make_main("main_gpt.raw", "gpt")
    plain = make_plain("plain.raw", 24 * MiB)
    small = make_plain("small.raw", 8 * MiB)

    # ---- QCOW2
    for cs in (512, 4096, 65536, 2097152):
        convert(mbr, "qcow2", "q_v3_cs%d.qcow2" % cs, "cluster_size=%d" % cs)
        add("q_v3_cs%d.qcow2" % cs, "qcow2", mbr)
    convert(mbr, "qcow2", "q_v2.qcow2", "compat=0.10")
    add("q_v2.qcow2", "qcow2", mbr, note="v2")
    convert(mbr, "qcow2", "q_comp64k.qcow2", extra=["-c"])
    add("q_comp64k.qcow2", "qcow2", mbr, note="compressed")
    convert(mbr, "qcow2", "q_comp4k.qcow2", "cluster_size=4096", extra=["-c"])
    add("q_comp4k.qcow2", "qcow2", mbr, note="compressed 4k")
    convert(mbr, "qcow2", "q_comp512.qcow2", "cluster_size=512", extra=["-c"])
    add("q_comp512.qcow2", "qcow2", mbr, note="compressed 512")
    convert(gpt, "qcow2", "q_gpt.qcow2")
    add("q_gpt.qcow2", "qcow2", gpt)
    # ゼロクラスタ / 破棄 (qemu-io)
    if HAVE_QEMU_IO:
        z = os.path.join(OUT, "q_zero.qcow2")
        shutil.copy(os.path.join(OUT, "q_v3_cs4096.qcow2"), z)
        zraw = os.path.join(OUT, "q_zero.raw")
        shutil.copy(os.path.join(OUT, mbr), zraw)
        run("qemu-io", "-f", "qcow2", "-c", "write -z 17M 1M", "-c", "write -z 31M 512k", "-c", "discard 2M 64k", z)
        put(zraw, 17 * MiB, b"\0" * MiB)
        put(zraw, 31 * MiB, b"\0" * 512 * 1024)
        put(zraw, 2 * MiB, b"\0" * 64 * 1024)
        qemu_compare("qcow2", "q_zero.qcow2", "q_zero.raw")
        add("q_zero.qcow2", "qcow2", "q_zero.raw", note="zero clusters + discard")
    # 内部スナップショット入り
    sn = "q_snap.qcow2"
    shutil.copy(os.path.join(OUT, "q_v3_cs65536.qcow2"), os.path.join(OUT, sn))
    run("qemu-img", "snapshot", "-c", "s1", os.path.join(OUT, sn))
    add(sn, "qcow2", mbr, note="internal snapshot")
    # バッキングチェーン: base <- mid <- top
    mod1 = os.path.join(OUT, "chain_mid.raw")
    mod2 = os.path.join(OUT, "chain_top.raw")
    shutil.copy(os.path.join(OUT, plain), mod1)
    put(mod1, 5 * MiB + 100, pattern(11, 0, 300 * 1024))
    put(mod1, 20 * MiB, b"\x77" * 100000)
    shutil.copy(mod1, mod2)
    put(mod2, 5 * MiB + 150 * 1024, b"\x99" * 4096)          # mid の上を一部上書き
    put(mod2, 22 * MiB + 1, pattern(12, 0, 50000))
    convert(plain, "qcow2", "chain_base.qcow2", "cluster_size=65536")
    run("qemu-img", "convert", "-f", "raw", "-O", "qcow2", "-B", "chain_base.qcow2", "-F", "qcow2", "-o", "cluster_size=65536", mod1, os.path.join(OUT, "chain_mid.qcow2"), cwd=OUT)
    run("qemu-img", "convert", "-f", "raw", "-O", "qcow2", "-B", "chain_mid.qcow2", "-F", "qcow2", "-o", "cluster_size=65536", mod2, os.path.join(OUT, "chain_top.qcow2"), cwd=OUT)
    qemu_compare("qcow2", "chain_mid.qcow2", "chain_mid.raw")
    qemu_compare("qcow2", "chain_top.qcow2", "chain_top.raw")
    add("chain_base.qcow2", "qcow2", plain)
    add("chain_mid.qcow2", "qcow2", "chain_mid.raw", note="backing x1 (qcow2)")
    add("chain_top.qcow2", "qcow2", "chain_top.raw", note="backing x2 (qcow2)")
    # RAW のバッキング (-F raw)
    run("qemu-img", "convert", "-f", "raw", "-O", "qcow2", "-B", plain, "-F", "raw", mod1, os.path.join(OUT, "chain_rawbase.qcow2"), cwd=OUT)
    qemu_compare("qcow2", "chain_rawbase.qcow2", "chain_mid.raw")
    add("chain_rawbase.qcow2", "qcow2", "chain_mid.raw", note="backing raw")
    # 仮想サイズがバッキングより大きい
    big = os.path.join(OUT, "chain_big.raw")
    shutil.copy(mod1, big)
    with open(big, "r+b") as f:
        f.truncate(28 * MiB)
    put(big, 26 * MiB, b"\x42" * 70000)
    run("qemu-img", "create", "-f", "qcow2", "-b", "chain_base.qcow2", "-F", "qcow2", "-o", "cluster_size=65536", "chain_grow.qcow2", "28M", cwd=OUT)
    run("qemu-io", "-f", "qcow2", "-c", "write -P 0x42 26M 70000", os.path.join(OUT, "chain_grow.qcow2"), check=False) if HAVE_QEMU_IO else None
    grow_raw = os.path.join(OUT, "chain_grow.raw")
    shutil.copy(os.path.join(OUT, plain), grow_raw)
    with open(grow_raw, "r+b") as f:
        f.truncate(28 * MiB)
    if HAVE_QEMU_IO:
        put(grow_raw, 26 * MiB, b"\x42" * 70000)
        qemu_compare("qcow2", "chain_grow.qcow2", "chain_grow.raw")
        add("chain_grow.qcow2", "qcow2", "chain_grow.raw", note="virtual size > backing")
    # 拒否されるべきもの
    run("qemu-img", "create", "-f", "qcow2", "-o", "compression_type=zstd", os.path.join(OUT, "q_zstd.qcow2"), "8M", check=False)
    if os.path.exists(os.path.join(OUT, "q_zstd.qcow2")):
        add("q_zstd.qcow2", "qcow2", "-", "unsupported", "zstd")
    r = run("qemu-img", "create", "-f", "qcow2", "--object", "secret,id=sec0,data=pw", "-o", "encrypt.format=luks,encrypt.key-secret=sec0",
            os.path.join(OUT, "q_luks.qcow2"), "8M", check=False)
    if r.returncode == 0:
        add("q_luks.qcow2", "qcow2", "-", "unsupported", "luks encrypted")
    r = run("qemu-img", "create", "-f", "qcow2", "-o", "extended_l2=on,cluster_size=65536", os.path.join(OUT, "q_extl2.qcow2"), "8M", check=False)
    if r.returncode == 0:
        add("q_extl2.qcow2", "qcow2", "-", "unsupported", "extended L2")
    r = run("qemu-img", "create", "-f", "qcow2", "-o", "data_file=q_data.bin", "q_extdata.qcow2", "8M", check=False, cwd=OUT)
    if r.returncode == 0:
        add("q_extdata.qcow2", "qcow2", "-", "unsupported", "external data file")

    # ---- VHD
    convert(mbr, "vpc", "v_dyn.vhd", "force_size=on", qfmt="vpc")
    add("v_dyn.vhd", "vhd", mbr, note="dynamic (qemu)")
    convert(small, "vpc", "v_fixed.vhd", "subformat=fixed", "force_size=on", qfmt="vpc")
    add("v_fixed.vhd", "vhd", small, note="fixed (qemu)")
    r = run("qemu-img", "convert", "-f", "raw", "-O", "vpc", "-o", "block_size=524288,force_size=on", os.path.join(OUT, plain), os.path.join(OUT, "v_dyn512k.vhd"), check=False)
    if r.returncode == 0:
        qemu_compare("vpc", "v_dyn512k.vhd", plain)
        add("v_dyn512k.vhd", "vhd", plain, note="dynamic block 512K")
    convert(gpt, "vpc", "v_gpt.vhd", "force_size=on", qfmt="vpc")
    add("v_gpt.vhd", "vhd", gpt)
    # 差分 (自前): 親 v_dyn512k または v_dyn
    parent_name, parent_raw = "v_dyn.vhd", mbr
    with open(os.path.join(OUT, parent_raw), "rb") as f:
        praw = f.read()
    rng = random.Random(31)
    ov = {}
    for s in (0, 1, 7, 4095, 4096, 4097, 8191, 40000, 40001, 81919):
        ov[s] = rng.randbytes(512)
    for s in range(2 * MiB // 512 - 3, 2 * MiB // 512 + 3):          # ブロック境界の前後
        ov[s] = rng.randbytes(512)
    exp, puid = write_vhd_diff(os.path.join(OUT, "v_diff.vhd"), parent_name, read_vhd_uuid(os.path.join(OUT, parent_name)), praw, 40 * MiB, ov)
    with open(os.path.join(OUT, "v_diff.raw"), "wb") as f:
        f.write(exp)
    add("v_diff.vhd", "vhd", "v_diff.raw", note="differencing (hand-written), parent v_dyn.vhd")
    # 差分の孫 (親 = v_diff.vhd)
    ov2 = {s: rng.randbytes(512) for s in (3, 4096, 100, 50000)}
    exp2, _ = write_vhd_diff(os.path.join(OUT, "v_diff2.vhd"), "v_diff.vhd", puid and read_vhd_uuid(os.path.join(OUT, "v_diff.vhd")), exp, 40 * MiB, ov2, shuffle_seed=8)
    with open(os.path.join(OUT, "v_diff2.raw"), "wb") as f:
        f.write(exp2)
    add("v_diff2.vhd", "vhd", "v_diff2.raw", note="differencing x2")
    # 独立した第三者の実装 (7-Zip) で差分 VHD を展開して期待値と一致することを確認する。
    # (qemu-img は差分 VHD のセクタビットマップを見ない = 部分的に割り当てられたブロックを正しく読めないので照合には使えない)
    if shutil.which("7z"):
        for n, rawn in (("v_diff.vhd", "v_diff.raw"), ("v_diff2.vhd", "v_diff2.raw")):
            tmpd = os.path.join(OUT, "_7z_" + n)
            run("7z", "x", "-y", "-o" + tmpd, os.path.join(OUT, n))
            with open(os.path.join(tmpd, n.replace(".vhd", ".img")), "rb") as f1, open(os.path.join(OUT, rawn), "rb") as f2:
                if f1.read() != f2.read():
                    sys.stderr.write("7z extraction of %s differs from the expected content\n" % n)
                    raise SystemExit(1)
            shutil.rmtree(tmpd)
        print("7z cross-check of differencing VHD (x2): identical")
    # 親 UUID 不一致
    write_vhd_diff(os.path.join(OUT, "v_diff_badparent.vhd"), parent_name, b"\x11" * 16, praw, 40 * MiB, ov)
    add("v_diff_badparent.vhd", "vhd", "-", "corrupt", "parent uuid mismatch")

    # ---- VHDX
    convert(mbr, "vhdx", "x_dyn.vhdx", qfmt="vhdx")
    add("x_dyn.vhdx", "vhdx", mbr, note="dynamic block 32M (qemu)")
    convert(plain, "vhdx", "x_dyn1m.vhdx", "block_size=1048576", qfmt="vhdx")
    add("x_dyn1m.vhdx", "vhdx", plain, note="dynamic block 1M")
    convert(small, "vhdx", "x_fixed.vhdx", "subformat=fixed", qfmt="vhdx")
    add("x_fixed.vhdx", "vhdx", small, note="fixed")
    convert(gpt, "vhdx", "x_gpt.vhdx", qfmt="vhdx")
    add("x_gpt.vhdx", "vhdx", gpt)
    with open(os.path.join(OUT, plain), "rb") as f:
        plain_raw = f.read()
    ovx = {s: rng.randbytes(512) for s in (0, 1, 2047, 2048, 2049, 9000, 20000, 40000, 47999)}
    expx = write_vhdx_diff(os.path.join(OUT, "x_diff.vhdx"), "x_dyn1m.vhdx", plain_raw, 24 * MiB, ovx)
    with open(os.path.join(OUT, "x_diff.raw"), "wb") as f:
        f.write(expx)
    add("x_diff.vhdx", "vhdx", "x_diff.raw", note="differencing (hand-written), parent x_dyn1m.vhdx")
    ovx4 = {s: rng.randbytes(4096) for s in (0, 5, 255, 256, 3000)}
    expx4 = write_vhdx_diff(os.path.join(OUT, "x_diff4k.vhdx"), "x_dyn1m.vhdx", plain_raw, 24 * MiB, ovx4, lss=4096)
    with open(os.path.join(OUT, "x_diff4k.raw"), "wb") as f:
        f.write(expx4)
    add("x_diff4k.vhdx", "vhdx", "x_diff4k.raw", note="differencing, 4K logical sectors")
    write_vhdx_diff(os.path.join(OUT, "x_badcrc.vhdx"), "x_dyn1m.vhdx", plain_raw, 24 * MiB, ovx, corrupt="header-crc")
    add("x_badcrc.vhdx", "vhdx", "-", "corrupt", "both headers have a bad checksum")

    # ---- VMDK
    convert(mbr, "vmdk", "m_sparse.vmdk", "subformat=monolithicSparse", qfmt="vmdk")
    add("m_sparse.vmdk", "vmdk", mbr, note="monolithicSparse")
    convert(mbr, "vmdk", "m_stream.vmdk", "subformat=streamOptimized", qfmt="vmdk")
    add("m_stream.vmdk", "vmdk", mbr, note="streamOptimized")
    convert(gpt, "vmdk", "m_gpt.vmdk", "subformat=monolithicSparse", qfmt="vmdk")
    add("m_gpt.vmdk", "vmdk", gpt)
    convert(small, "vmdk", "m_flat.vmdk", "subformat=monolithicFlat", qfmt="vmdk")
    add("m_flat.vmdk", "vmdk", small, note="monolithicFlat (descriptor + -flat.vmdk)")
    convert(plain, "vmdk", "m_2gs.vmdk", "subformat=twoGbMaxExtentSparse", qfmt="vmdk")
    add("m_2gs.vmdk", "vmdk", plain, note="twoGbMaxExtentSparse")
    r = run("qemu-img", "convert", "-f", "raw", "-O", "vmdk", "-o", "subformat=monolithicSparse,adapter_type=lsilogic,hwversion=6", os.path.join(OUT, plain),
            os.path.join(OUT, "m_hw6.vmdk"), check=False)
    if r.returncode == 0:
        add("m_hw6.vmdk", "vmdk", plain, note="hwversion 6")
    # 複数エクステント (自前): FLAT 3 つ + ZERO + 先頭に SPARSE 風のオフセットつき FLAT
    parts = [(os.path.join(OUT, "multi-f001.bin"), 3 * MiB + 512), (os.path.join(OUT, "multi-f002.bin"), 2 * MiB), (os.path.join(OUT, "multi-f003.bin"), 1 * MiB)]
    with open(os.path.join(OUT, small), "rb") as f:
        sm = f.read()
    exp_multi = bytearray()
    desc_ext = []
    pos = 0
    for i, (p, n) in enumerate(parts):
        chunk = sm[pos:pos + n] if i != 1 else sm[pos:pos + n]
        pad = 4096 if i == 2 else 0                        # 3 つ目はファイル内オフセットつき
        with open(p, "wb") as f:
            f.write(b"\xee" * pad + chunk)
        desc_ext.append('RW %d FLAT "%s" %d' % (n // 512, os.path.basename(p), pad // 512))
        exp_multi += chunk
        pos += n
    desc_ext.append("RW %d ZERO" % ((len(sm) - pos) // 512))
    exp_multi += b"\0" * (len(sm) - pos)
    descr = "# Disk DescriptorFile\nversion=1\nCID=fffffffe\nparentCID=ffffffff\ncreateType=\"twoGbMaxExtentFlat\"\n\n# Extent description\n" + "\n".join(desc_ext) + "\n\n# The Disk Data Base\nddb.virtualHWVersion = \"4\"\n"
    with open(os.path.join(OUT, "multi.vmdk"), "w") as f:
        f.write(descr)
    with open(os.path.join(OUT, "multi.raw"), "wb") as f:
        f.write(exp_multi)
    qemu_compare("vmdk", "multi.vmdk", "multi.raw")
    add("multi.vmdk", "vmdk", "multi.raw", note="3 flat extents + ZERO extent (hand-written descriptor)")
    # 差分 VMDK (qemu が作れれば)
    r = run("qemu-img", "create", "-f", "vmdk", "-b", "m_sparse.vmdk", "-F", "vmdk", "m_diff.vmdk", cwd=OUT, check=False)
    if r.returncode == 0:
        r2 = run("qemu-io", "-f", "vmdk", "-c", "write -P 0x31 4M 100k", "-c", "write -P 0x32 33M 3000", os.path.join(OUT, "m_diff.vmdk"), check=False) if HAVE_QEMU_IO else None
        if r2 is not None and r2.returncode == 0:
            shutil.copy(os.path.join(OUT, mbr), os.path.join(OUT, "m_diff.raw"))
            put(os.path.join(OUT, "m_diff.raw"), 4 * MiB, b"\x31" * 102400)
            put(os.path.join(OUT, "m_diff.raw"), 33 * MiB, b"\x32" * 3000)
            qemu_compare("vmdk", "m_diff.vmdk", "m_diff.raw")
            add("m_diff.vmdk", "vmdk", "m_diff.raw", note="differencing (qemu)")

    # ---- 壊れたイメージ / RAW
    with open(os.path.join(OUT, "junk.bin"), "wb") as f:
        f.write(random.Random(5).randbytes(300000))
    add("junk.bin", "raw", "junk.bin", note="random bytes are RAW (no format magic)")
    add(mbr, "raw", mbr, note="RAW disk itself")

    # 後始末 (使わない中間ファイル)
    for n in ("_tree_main", "_tree_gpt"):
        shutil.rmtree(os.path.join(OUT, n), ignore_errors=True)
    with open(os.path.join(OUT, "manifest.tsv"), "w") as f:
        for r in rows:
            f.write("\t".join(r) + "\n")
    print("generated %d fixtures in %s" % (len(rows), OUT))


main()
