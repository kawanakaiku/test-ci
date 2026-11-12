#!/usr/bin/env python3
"""fs/ntfs / fs/exfat / fs/ext2 / fs/ext4 / fs/fat の `static const struct XXX_operations name = { .a = f, ... };` を走査し、
互換レイヤー用の ops 構造体定義(メンバは void*)を生成する。メンバ名は上流ソースが実際に使うものだけ。"""
import re, sys, glob, collections
srcs = sys.argv[1:]   # 複数の FS ソースディレクトリ (fs/ntfs, fs/exfat, ...) のメンバの和集合を出す
fields = collections.defaultdict(set)
WANT = re.compile(r'(\w+_operations|file_system_type|xattr_handler|vm_operations_struct)$')
pat = re.compile(r'^(?:static\s+)?const\s+struct\s+(\w+)\s+\w+\s*=\s*\{(.*?)\n\s*\};', re.S | re.M)
for f in [g for d in srcs for g in glob.glob(d + '/*.c')]:
    for m in pat.finditer(open(f).read()):
        if not WANT.match(m.group(1)): continue
        for fld in re.findall(r'^\s*\.(\w+)\s*=', m.group(2), re.M):
            fields[m.group(1)].add(fld)
# 上流のコードが関数ポインタとして**呼び出す**メンバ (fs/fat の fat_cont_expand が aops->write_begin() を呼ぶ)。
# 引数は FS ごとに違いうるので、プロトタイプなしの関数ポインタ (任意の関数を代入でき、任意の引数で呼べる) にする。
CALLED = {('address_space_operations', 'write_begin'), ('address_space_operations', 'write_end')}
skip = {'fatent_operations', 'fscrypt_operations', 'fsverity_operations', 'seq_operations', 'iomap_ops', 'iomap_write_ops', 'iomap_dio_ops', 'iomap_read_ops', 'iomap_writeback_ops'}
print("/* 自動生成: tools/gen_ops_structs.py (手編集しない) */")
print("#ifndef USBMEM_COMPAT_OPS_H\n#define USBMEM_COMPAT_OPS_H\n")
for name in sorted(fields):
    if name in skip or name == 'file_system_type' and False: continue
    print("struct %s {" % name)
    for fld in sorted(fields[name]):
        print("  int (*%s)();" % fld if (name, fld) in CALLED else "  void *%s;" % fld)
    print("};")
print("\n#endif")
