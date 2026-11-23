# SPDX-License-Identifier: Apache-2.0
"""カービングの検証用サンプル: 各形式の「構造として正しい」小さなファイルを Python の標準ライブラリだけで作る (JPEG・PNG・GIF・BMP・PDF・ZIP 系・
MP4/MOV・MP3・WAV・AVI・WebP・7z・gzip・SQLite・PE)。7z コマンド・PIL・ffmpeg があれば、本物のエンコーダが作ったものも足す (extra_samples)。
中身 (画素・音声・コード) は乱数でよい: 検査するのは構造 (マーカーの連なり・チャンクの CRC・原子の長さ・フレームの同期・ヘッダの長さ) だけなので。
"""
import gzip
import hashlib
import io
import os
import random
import shutil
import sqlite3
import struct
import subprocess
import tempfile
import zipfile
import zlib


def _rnd(seed, n):
    return random.Random(seed).randbytes(n)


def jpeg(seed, entropy=3000, progressive=False, exif_thumb=False):
    r = random.Random(seed)
    b = bytearray(b"\xff\xd8")
    b += b"\xff\xe0" + struct.pack(">H", 16) + b"JFIF\x00\x01\x01\x00\x00\x01\x00\x01\x00\x00"
    if exif_thumb:  # APP1 の中に別の JPEG (サムネイル) を持つ: 外側だけが 1 つのファイルとして出ること
        thumb = jpeg(seed + 1000, 200)
        app1 = b"Exif\x00\x00" + thumb
        b += b"\xff\xe1" + struct.pack(">H", len(app1) + 2) + app1
    b += b"\xff\xdb" + struct.pack(">H", 67) + b"\x00" + bytes(r.randrange(1, 100) for _ in range(64))
    b += b"\xff\xc2" if progressive else b"\xff\xc0"
    b += struct.pack(">HBHHB", 17, 8, 48, 64, 3) + b"\x01\x22\x00\x02\x11\x01\x03\x11\x01"
    b += b"\xff\xc4" + struct.pack(">H", 24) + b"\x00" + bytes(16) + bytes(r.randrange(256) for _ in range(5))
    for scan in range(2 if progressive else 1):
        b += b"\xff\xda" + struct.pack(">H", 12) + b"\x03\x01\x00\x02\x11\x03\x11\x00\x3f\x00"
        data = bytearray()
        while len(data) < entropy:
            c = r.randrange(256)
            data.append(c)
            if c == 0xFF:
                data.append(0)
            if r.random() < 0.002:
                data += b"\xff" + bytes([0xD0 + r.randrange(8)])  # RSTn
        b += data
    return bytes(b + b"\xff\xd9")


def png(seed, w=40, h=30):
    r = random.Random(seed)
    raw = b"".join(b"\x00" + bytes(r.randrange(256) for _ in range(w * 3)) for _ in range(h))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
    z = zlib.compress(raw)
    half = len(z) // 2
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"tEXt", b"Comment\x00sample") +
            chunk(b"IDAT", z[:half]) + chunk(b"IDAT", z[half:]) + chunk(b"IEND", b""))


def gif(seed, frames=3):
    r = random.Random(seed)
    b = bytearray(b"GIF89a" + struct.pack("<HHBBB", 32, 24, 0xF1, 0, 0) + bytes(r.randrange(256) for _ in range(12)))
    for f in range(frames):
        b += b"\x21\xf9\x04\x00\x0a\x00\x00\x00"
        b += b"\x2c" + struct.pack("<HHHHB", 0, 0, 32, 24, 0) + b"\x02"
        data = bytes(r.randrange(256) for _ in range(300 + 50 * f))
        for i in range(0, len(data), 255):
            c = data[i:i + 255]
            b += bytes([len(c)]) + c
        b += b"\x00"
    b += b"\x21\xfe\x05hello\x00"
    return bytes(b + b"\x3b")


def bmp(seed, w=37, h=21, bpp=24):
    r = random.Random(seed)
    stride = ((w * bpp + 31) // 32) * 4
    pix = bytes(r.randrange(256) for _ in range(stride * h))
    return b"BM" + struct.pack("<IHHI", 54 + len(pix), 0, 0, 54) + struct.pack("<IiiHHIIiiII", 40, w, h, 1, bpp, 0, len(pix), 2835, 2835, 0, 0) + pix


def pdf(seed, update=False):
    r = random.Random(seed)
    objs = [b"<< /Type /Catalog /Pages 2 0 R >>", b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
            b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] /Contents 4 0 R >>"]
    stream = bytes(r.randrange(32, 127) for _ in range(500))
    objs.append(b"<< /Length %d >>\nstream\n" % len(stream) + stream + b"\nendstream")
    out = bytearray(b"%PDF-1.4\n")
    offs = []
    for i, o in enumerate(objs):
        offs.append(len(out))
        out += b"%d 0 obj\n" % (i + 1) + o + b"\nendobj\n"
    xref = len(out)
    out += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objs) + 1)
    for o in offs:
        out += b"%010d 00000 n \n" % o
    out += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (len(objs) + 1, xref)
    if update:  # 増分更新 (%%EOF が 2 つ)
        o5 = len(out)
        out += b"5 0 obj\n<< /Note (update) >>\nendobj\n"
        x2 = len(out)
        out += b"xref\n5 1\n%010d 00000 n \ntrailer\n<< /Size 6 /Root 1 0 R /Prev %d >>\nstartxref\n%d\n%%%%EOF\n" % (o5, xref, x2)
    return bytes(out)


def zipfile_bytes(files, stream=False, mimetype_first=None):
    """files: [(名前, 内容)]。stream=True ならデータ記述子 (bit 3) 付きで書く (シーク不能な出力と同じ)"""
    bio = io.BytesIO()
    if stream:
        class NoSeek(io.RawIOBase):
            def __init__(s): s.pos = 0
            def writable(s): return True
            def write(s, b): bio.write(b); s.pos += len(b); return len(b)
            def tell(s): return s.pos
        zf = zipfile.ZipFile(NoSeek(), "w", zipfile.ZIP_DEFLATED)
    else:
        zf = zipfile.ZipFile(bio, "w", zipfile.ZIP_DEFLATED)
    if mimetype_first:
        zf.writestr(zipfile.ZipInfo("mimetype"), mimetype_first, compress_type=zipfile.ZIP_STORED)
    for n, d in files:
        zf.writestr(n, d)
    zf.close()
    return bio.getvalue()


def zips(seed):
    r = random.Random(seed)
    big = bytes(r.randrange(256) for _ in range(5000))
    txt = b"hello zip " * 300
    out = {}
    out["zip"] = zipfile_bytes([("a.txt", txt), ("dir/b.bin", big)])
    out["zip_stream"] = zipfile_bytes([("s1.txt", txt), ("s2.bin", big)], stream=True)
    out["docx"] = zipfile_bytes([("[Content_Types].xml", b"<Types/>"), ("_rels/.rels", b"<R/>"), ("word/document.xml", txt)])
    out["xlsx"] = zipfile_bytes([("[Content_Types].xml", b"<Types/>"), ("xl/workbook.xml", txt)])
    out["pptx"] = zipfile_bytes([("[Content_Types].xml", b"<Types/>"), ("ppt/presentation.xml", txt)])
    out["jar"] = zipfile_bytes([("META-INF/MANIFEST.MF", b"Manifest-Version: 1.0\n"), ("A.class", big)])
    out["apk"] = zipfile_bytes([("AndroidManifest.xml", b"\x03\x00\x08\x00" + big[:100]), ("classes.dex", b"dex\n035\x00" + big), ("META-INF/CERT.SF", txt)])
    out["odt"] = zipfile_bytes([("content.xml", txt)], mimetype_first=b"application/vnd.oasis.opendocument.text")
    out["epub"] = zipfile_bytes([("OEBPS/c.xhtml", txt)], mimetype_first=b"application/epub+zip")
    return out


def _atom(t, d):
    return struct.pack(">I", 8 + len(d)) + t + d


def mp4(seed, brand=b"isom", faststart=False, big_mdat=False):
    r = random.Random(seed)
    ftyp = _atom(b"ftyp", brand + b"\x00\x00\x02\x00" + brand + b"mp42")
    mvhd = _atom(b"mvhd", b"\x00" * 100 + b"\x00\x00\x00\x02")
    moov = _atom(b"moov", mvhd + _atom(b"trak", b"\x00" * 60))
    payload = bytes(r.randrange(256) for _ in range(20000))
    if big_mdat:  # 64 ビットの大きさ
        mdat = struct.pack(">I4sQ", 1, b"mdat", 16 + len(payload)) + payload
    else:
        mdat = _atom(b"mdat", payload)
    free = _atom(b"free", b"\x00" * 24)
    return ftyp + (moov + free + mdat if faststart else free + mdat + moov)


def mp3(seed, frames=60, id3=True, tag=True):
    r = random.Random(seed)
    out = bytearray()
    if id3:
        body = b"TIT2" + struct.pack(">I", 11) + b"\x00\x00\x00" + b"sample song" + b"\x00" * 100
        n = len(body)
        out += b"ID3\x03\x00\x00" + bytes([(n >> 21) & 0x7F, (n >> 14) & 0x7F, (n >> 7) & 0x7F, n & 0x7F]) + body
    pad_acc = 0.0
    for i in range(frames):
        # MPEG1 Layer III 128 kbps 44.1 kHz: 417.96 バイト / フレーム (パディングで 417 / 418)
        pad_acc += 0.9642857
        pad = 1 if pad_acc >= 1 else 0
        if pad:
            pad_acc -= 1
        ln = 417 + pad
        out += bytes([0xFF, 0xFB, 0x90 | (pad << 1), 0x00]) + bytes(r.randrange(256) for _ in range(ln - 4))
    if tag:
        out += b"TAG" + b"title".ljust(30, b"\x00") + b"\x00" * 95
    return bytes(out)


def riff(seed):
    r = random.Random(seed)
    out = {}
    pcm = bytes(r.randrange(256) for _ in range(8000))
    fmt = struct.pack("<HHIIHH", 1, 1, 8000, 8000, 1, 8)
    body = b"WAVE" + b"fmt " + struct.pack("<I", 16) + fmt + b"LIST" + struct.pack("<I", 4) + b"INFO" + b"data" + struct.pack("<I", len(pcm)) + pcm
    out["wav"] = b"RIFF" + struct.pack("<I", len(body)) + body
    odd = pcm[:7001]
    body = b"WAVE" + b"fmt " + struct.pack("<I", 16) + fmt + b"data" + struct.pack("<I", len(odd)) + odd + b"\x00"
    out["wav_odd"] = b"RIFF" + struct.pack("<I", len(body)) + body
    avih = b"avih" + struct.pack("<I", 56) + bytes(56)
    hdrl = b"LIST" + struct.pack("<I", 4 + len(avih)) + b"hdrl" + avih
    movi_data = b"00dc" + struct.pack("<I", 1000) + bytes(r.randrange(256) for _ in range(1000))
    movi = b"LIST" + struct.pack("<I", 4 + len(movi_data)) + b"movi" + movi_data
    idx1 = b"idx1" + struct.pack("<I", 16) + struct.pack("<4sIII", b"00dc", 0x10, 4, 1000)
    body = b"AVI " + hdrl + movi + idx1
    out["avi"] = b"RIFF" + struct.pack("<I", len(body)) + body
    vp8l = b"VP8L" + struct.pack("<I", 300) + b"\x2f" + bytes(r.randrange(256) for _ in range(299))
    body = b"WEBP" + vp8l
    out["webp"] = b"RIFF" + struct.pack("<I", len(body)) + body
    return out


def gz(seed):
    r = random.Random(seed)
    data = (b"gzip sample line %d\n" * 200) % tuple(range(200)) + bytes(r.randrange(256) for _ in range(2000))
    bio = io.BytesIO()
    with gzip.GzipFile(filename="sample.txt", mode="wb", fileobj=bio, mtime=1700000000) as f:
        f.write(data)
    return bio.getvalue()


def sevenz(seed):
    if not shutil.which("7z"):
        return None
    d = tempfile.mkdtemp(prefix="c7z_")
    try:
        with open(os.path.join(d, "a.txt"), "wb") as f:
            f.write(_rnd(seed, 3000) + b"text " * 400)
        with open(os.path.join(d, "b.bin"), "wb") as f:
            f.write(_rnd(seed + 1, 9000))
        out = os.path.join(d, "x.7z")
        subprocess.run(["7z", "a", "-bd", "-y", out, os.path.join(d, "a.txt"), os.path.join(d, "b.bin")], capture_output=True, timeout=60, cwd=d)
        with open(out, "rb") as f:
            return f.read()
    finally:
        shutil.rmtree(d, ignore_errors=True)


def sqlite_db(seed):
    d = tempfile.mkdtemp(prefix="csq_")
    try:
        p = os.path.join(d, "t.db")
        c = sqlite3.connect(p)
        c.execute("create table t(id integer primary key, v text, b blob)")
        r = random.Random(seed)
        c.executemany("insert into t(v,b) values(?,?)", [("row %d" % i, bytes(r.randrange(256) for _ in range(100))) for i in range(300)])
        c.commit()
        c.close()
        with open(p, "rb") as f:
            return f.read()
    finally:
        shutil.rmtree(d, ignore_errors=True)


def pe(seed, dll=False):
    r = random.Random(seed)
    text = bytes(r.randrange(256) for _ in range(1024))
    data = bytes(r.randrange(256) for _ in range(512))
    hdr_size = 0x200
    dos = bytearray(0x80)
    dos[0:2] = b"MZ"
    struct.pack_into("<I", dos, 60, 0x80)
    coff = struct.pack("<HHIIIHH", 0x8664, 2, 0x65000000, 0, 0, 240, 0x2022 if dll else 0x0022)
    opt = bytearray(240)
    struct.pack_into("<HBB", opt, 0, 0x20B, 14, 0)
    struct.pack_into("<I", opt, 16, 0x1000)       # entry point
    struct.pack_into("<Q", opt, 24, 0x140000000)  # image base
    struct.pack_into("<II", opt, 32, 0x1000, 0x200)  # section / file alignment
    struct.pack_into("<I", opt, 56, 0x3000)       # size of image
    struct.pack_into("<I", opt, 60, hdr_size)
    struct.pack_into("<H", opt, 68, 3)            # subsystem console
    struct.pack_into("<I", opt, 108, 16)          # number of data dirs
    sec = b""
    sec += b".text\0\0\0" + struct.pack("<IIIIIIHHI", len(text), 0x1000, len(text), hdr_size, 0, 0, 0, 0, 0x60000020)
    sec += b".data\0\0\0" + struct.pack("<IIIIIIHHI", len(data), 0x2000, len(data), hdr_size + len(text), 0, 0, 0, 0, 0xC0000040)
    hdr = bytes(dos) + b"PE\0\0" + coff + bytes(opt) + sec
    hdr = hdr.ljust(hdr_size, b"\x00")
    return hdr + text + data


def basic_samples(seed=1):
    """名前 → (形式の id, 期待する拡張子, バイト列)"""
    s = {}
    s["jpeg_plain"] = ("jpeg", "jpg", jpeg(seed, 4000))
    s["jpeg_prog"] = ("jpeg", "jpg", jpeg(seed + 1, 6000, progressive=True))
    s["jpeg_exif"] = ("jpeg", "jpg", jpeg(seed + 2, 5000, exif_thumb=True))
    s["png"] = ("png", "png", png(seed))
    s["gif"] = ("gif", "gif", gif(seed))
    s["bmp"] = ("bmp", "bmp", bmp(seed))
    s["pdf"] = ("pdf", "pdf", pdf(seed))
    s["pdf_update"] = ("pdf", "pdf", pdf(seed + 1, update=True))
    for k, v in zips(seed).items():
        s[k] = ("zip", "zip" if k == "zip_stream" else k, v)
    s["mp4"] = ("mp4", "mp4", mp4(seed))
    s["mp4_faststart"] = ("mp4", "mp4", mp4(seed + 1, faststart=True))
    s["mp4_big"] = ("mp4", "mp4", mp4(seed + 2, big_mdat=True))
    s["mov"] = ("mp4", "mov", mp4(seed + 3, brand=b"qt  "))
    s["m4a"] = ("mp4", "m4a", mp4(seed + 4, brand=b"M4A "))
    s["heic"] = ("mp4", "heic", mp4(seed + 5, brand=b"heic"))
    s["mp3_id3"] = ("mp3", "mp3", mp3(seed))
    s["mp3_bare"] = ("mp3", "mp3", mp3(seed + 1, frames=80, id3=False, tag=False))
    for k, v in riff(seed).items():
        s[k] = ("riff", k.split("_")[0], v)
    s["gzip"] = ("gzip", "gz", gz(seed))
    z = sevenz(seed)
    if z:
        s["7z"] = ("7z", "7z", z)
    s["sqlite"] = ("sqlite", "sqlite", sqlite_db(seed))
    s["exe"] = ("pe", "exe", pe(seed))
    s["dll"] = ("pe", "dll", pe(seed + 1, dll=True))
    return s


def extra_samples():
    """本物のエンコーダの出力 (PIL / ffmpeg / cjpeg があれば)。名前 → (形式の id, 拡張子, バイト列)"""
    s = {}
    try:
        from PIL import Image
        r = random.Random(7)
        im = Image.frombytes("RGB", (64, 48), bytes(r.randrange(256) for _ in range(64 * 48 * 3)))
        for name, kw, ext, fmt in [("pil_jpeg", dict(quality=85), "jpg", "JPEG"), ("pil_jpeg_prog", dict(quality=70, progressive=True), "jpg", "JPEG"),
                                   ("pil_jpeg_exif", dict(quality=90, exif=b"Exif\x00\x00MM\x00*\x00\x00\x00\x08\x00\x00"), "jpg", "JPEG"),
                                   ("pil_png", dict(), "png", "PNG"), ("pil_png_opt", dict(optimize=True), "png", "PNG"),
                                   ("pil_gif", dict(), "gif", "GIF"), ("pil_bmp", dict(), "bmp", "BMP"), ("pil_webp", dict(), "webp", "WEBP")]:
            bio = io.BytesIO()
            try:
                (im.convert("P") if fmt == "GIF" else im).save(bio, fmt, **kw)
            except Exception:
                continue
            s[name] = ({"jpg": "jpeg", "png": "png", "gif": "gif", "bmp": "bmp", "webp": "riff"}[ext], ext, bio.getvalue())
        frames = [Image.frombytes("RGB", (32, 32), bytes(r.randrange(256) for _ in range(32 * 32 * 3))) for _ in range(4)]
        bio = io.BytesIO()
        frames[0].save(bio, "GIF", save_all=True, append_images=frames[1:], duration=100, loop=0)
        s["pil_gif_anim"] = ("gif", "gif", bio.getvalue())
        bio = io.BytesIO()
        im.save(bio, "TIFF")
    except Exception:
        pass
    if shutil.which("ffmpeg"):
        d = tempfile.mkdtemp(prefix="cff_")
        try:
            def ff(name, ext, fid, args):
                out = os.path.join(d, name + "." + ext)
                p = subprocess.run(["ffmpeg", "-y", "-loglevel", "error"] + args + [out], capture_output=True, timeout=120)
                if p.returncode == 0 and os.path.getsize(out) > 0:
                    with open(out, "rb") as f:
                        s["ff_" + name] = (fid, ext, f.read())
            vid = ["-f", "lavfi", "-i", "testsrc=duration=1:size=160x120:rate=10"]
            aud = ["-f", "lavfi", "-i", "sine=frequency=440:duration=1"]
            ff("mp4", "mp4", "mp4", vid + ["-c:v", "mpeg4"])
            ff("mp4_faststart", "mp4", "mp4", vid + ["-c:v", "mpeg4", "-movflags", "+faststart"])
            ff("mp4_frag", "mp4", "mp4", vid + ["-c:v", "mpeg4", "-movflags", "frag_keyframe+empty_moov"])
            ff("mov", "mov", "mp4", vid + ["-c:v", "mpeg4"])
            ff("m4a", "m4a", "mp4", aud + ["-c:a", "aac"])
            ff("mp3", "mp3", "mp3", aud + ["-c:a", "libmp3lame"])
            ff("wav", "wav", "riff", aud)
            ff("avi", "avi", "riff", vid + ["-c:v", "mpeg4"])
            ff("jpg", "jpg", "jpeg", vid + ["-frames:v", "1"])
        finally:
            shutil.rmtree(d, ignore_errors=True)
    return s


def sha(b):
    return hashlib.sha256(b).hexdigest()
