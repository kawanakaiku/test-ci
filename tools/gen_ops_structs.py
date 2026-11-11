#!/usr/bin/env python3
"""fs/ntfs の `static const struct XXX_operations name = { .a = f, ... };` を走査し、
互換レイヤー用の ops 構造体定義(メンバは void*)を生成する。メンバ名は上流ソースが実際に使うものだけ。"""
import re, sys, glob, collections
src = sys.argv[1]
fields = collections.defaultdict(set)
WANT = re.compile(r'(\w+_operations|file_system_type|xattr_handler|vm_operations_struct)$')
pat = re.compile(r'^(?:static\s+)?const\s+struct\s+(\w+)\s+\w+\s*=\s*\{(.*?)\n\s*\};', re.S | re.M)
for f in glob.glob(src + '/*.c'):
    for m in pat.finditer(open(f).read()):
        if not WANT.match(m.group(1)): continue
        for fld in re.findall(r'^\s*\.(\w+)\s*=', m.group(2), re.M):
            fields[m.group(1)].add(fld)
skip = {'iomap_ops', 'iomap_write_ops', 'iomap_dio_ops', 'iomap_read_ops', 'iomap_writeback_ops'}
print("/* 自動生成: tools/gen_ops_structs.py (手編集しない) */")
print("#ifndef USBMEM_COMPAT_OPS_H\n#define USBMEM_COMPAT_OPS_H\n")
for name in sorted(fields):
    if name in skip or name == 'file_system_type' and False: continue
    print("struct %s {" % name)
    for fld in sorted(fields[name]):
        print("  void *%s;" % fld)
    print("};")
print("\n#endif")
