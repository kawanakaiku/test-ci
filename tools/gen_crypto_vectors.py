#!/usr/bin/env python3
"""core/tests/crypto_vectors.inc を生成する (暗号プリミティブの既知値テスト用)。

期待値は C++ 実装とは無関係な実装から作る: hashlib / hmac / pbkdf2_hmac (OpenSSL), cryptography (AES-ECB/CBC/XTS/CCM),
argon2-cffi (参照実装 libargon2)。使い方: tools/gen_crypto_vectors.py > core/tests/crypto_vectors.inc
"""
import hashlib
import hmac
import struct
import sys

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives.ciphers.aead import AESCCM
import argon2.low_level as a2


def pat(n, seed=1):
    return bytes(((i * 7 + seed) & 255) for i in range(n))


def h(b):
    return b.hex()


out = []
emit = out.append

# ---- ハッシュ (ブロック境界の長さを含む)
for alg in ("sha1", "sha256", "sha512"):
    for n in (0, 1, 3, 55, 56, 57, 63, 64, 65, 111, 112, 113, 127, 128, 129, 200, 1000):
        m = pat(n, n + 3)
        emit('HASHV("%s", "%s", "%s")' % (alg, h(m), h(hashlib.new(alg, m).digest())))

# ---- HMAC
for alg in ("sha1", "sha256", "sha512"):
    for kl, dl in ((0, 0), (1, 5), (20, 50), (64, 64), (65, 10), (128, 3), (131, 200), (200, 300)):
        k, d = pat(kl, 9), pat(dl, 4)
        emit('HMACV("%s", "%s", "%s", "%s")' % (alg, h(k), h(d), h(hmac.new(k, d, alg).digest())))

# ---- PBKDF2
for alg, pw, salt, it, ln in (
    ("sha1", b"password", b"salt", 1, 20), ("sha1", b"password", b"salt", 4096, 20), ("sha1", b"passwordPASSWORDpassword", b"saltSALTsaltSALTsaltSALTsaltSALTsalt", 100, 25),
    ("sha256", b"password", b"salt", 1000, 32), ("sha256", b"pw", b"", 7, 100), ("sha512", b"password", b"NaCl", 1000, 64),
    ("sha512", b"p" * 200, b"s" * 33, 50, 130), ("sha256", b"", b"salt", 3, 16),
):
    emit('PBKDFV("%s", "%s", "%s", %d, "%s")' % (alg, h(pw), h(salt), it, h(hashlib.pbkdf2_hmac(alg, pw, salt, it, ln))))

# ---- AES-ECB (鍵長 128/192/256)
for kl in (16, 24, 32):
    for seed in (1, 2, 3):
        k, p = pat(kl, seed), pat(16, seed + 10)
        enc = Cipher(algorithms.AES(k), modes.ECB()).encryptor().update(p)
        emit('AESV("%s", "%s", "%s")' % (h(k), h(p), h(enc)))

# ---- CBC 復号
for kl in (16, 32):
    for n in (16, 32, 512):
        k, iv, p = pat(kl, 5), pat(16, 6), pat(n, 7)
        enc = Cipher(algorithms.AES(k), modes.CBC(iv)).encryptor().update(p)
        emit('CBCV("%s", "%s", "%s", "%s")' % (h(k), h(iv), h(enc), h(p)))

# ---- XTS 復号 (128/256 ビット鍵、セクタ 512 / 4096、大きなセクタ番号)
for kl in (32, 64):
    for n in (512, 4096, 16, 48):
        for sec in (0, 1, 12345, 0x123456789ABC):
            k, p = pat(kl, 21 + n % 7), pat(n, 31 + sec % 5)
            tw = struct.pack("<QQ", sec, 0)
            enc = Cipher(algorithms.AES(k), modes.XTS(tw)).encryptor().update(p)
            emit('XTSV("%s", %d, "%s", "%s")' % (h(k), sec, h(enc), h(p)))

# ---- CCM (ノンス 12、タグ 16)。出力は暗号文 + タグ
for kl in (16, 32):
    for n in (0, 1, 15, 16, 17, 44, 100, 513):
        k, nonce, p = pat(kl, 41), pat(12, 42), pat(n, 43)
        ct = AESCCM(k, tag_length=16).encrypt(nonce, p, None)
        emit('CCMV("%s", "%s", "%s", "%s", "%s")' % (h(k), h(nonce), h(ct[:-16]), h(ct[-16:]), h(p)))

# ---- BLAKE2b
for ln in (1, 20, 32, 64):
    for n in (0, 1, 127, 128, 129, 300):
        m = pat(n, 50 + n)
        emit('B2V(%d, "%s", "%s")' % (ln, h(m), h(hashlib.blake2b(m, digest_size=ln).digest())))

# ---- Argon2 (d / i / id)。参照実装 libargon2 の出力
types = {"d": a2.Type.D, "i": a2.Type.I, "id": a2.Type.ID}
for tname in ("d", "i", "id"):
    for t, m, p, ln in ((1, 8, 1, 32), (2, 16, 1, 32), (3, 32, 2, 32), (2, 64, 4, 64), (4, 40, 3, 16), (1, 256, 1, 32), (3, 65, 1, 130), (2, 24, 2, 4)):
        pw, salt = pat(11, 60), pat(16, 61)
        r = a2.hash_secret_raw(pw, salt, time_cost=t, memory_cost=m, parallelism=p, hash_len=ln, type=types[tname], version=19)
        emit('ARGONV("%s", "%s", "%s", %d, %d, %d, "%s")' % (tname, h(pw), h(salt), t, m, p, h(r)))

sys.stdout.write("// 自動生成: tools/gen_crypto_vectors.py (hashlib / cryptography / argon2-cffi の出力)。手で編集しない。\n")
sys.stdout.write("\n".join(out) + "\n")
