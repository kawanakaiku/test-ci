#!/usr/bin/env python3
"""疎ファイルの内容ハッシュ。データのある範囲 (SEEK_DATA/SEEK_HOLE) だけを読む。3 TiB の疎イメージでも一瞬。
穴は「その範囲は 0」として (オフセット, 長さ) をハッシュに混ぜるので、穴にデータが書かれても検出できる。"""
import hashlib, os, sys
def sparse_hash(path):
    h = hashlib.sha256()
    fd = os.open(path, os.O_RDONLY)
    try:
        size = os.fstat(fd).st_size; pos = 0
        while pos < size:
            try: d = os.lseek(fd, pos, os.SEEK_DATA)
            except OSError: d = size
            if d > pos: h.update(b"H%d:%d;" % (pos, d - pos))
            if d >= size: break
            e = os.lseek(fd, d, os.SEEK_HOLE)
            h.update(b"D%d:%d;" % (d, e - d)); off = d
            while off < e:
                b = os.pread(fd, min(1 << 22, e - off), off); h.update(b); off += len(b)
            pos = e
    finally: os.close(fd)
    return h.hexdigest()
if __name__ == "__main__": print(sparse_hash(sys.argv[1]))
