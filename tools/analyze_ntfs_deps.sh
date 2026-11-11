#!/bin/sh
# Linux mainline の fs/ntfs (NTFS Resurrection) について、ファイルごとのカーネル API 依存度を集計する。
# 使い方: tools/analyze_ntfs_deps.sh <linux ソースのルート> > report.txt
# 推測ではなく実ソースの grep 結果を根拠にするためのスクリプト。
set -eu
SRC=${1:?usage: $0 <linux-src-root>}
D="$SRC/fs/ntfs"
echo "# fs/ntfs dependency report"
[ -d "$SRC/.git" ] && echo "commit: $(git -C "$SRC" rev-parse HEAD)"
echo "files: $(ls "$D"/*.c "$D"/*.h "$D"/lib/* | wc -l)  lines: $(cat "$D"/*.c "$D"/*.h "$D"/lib/* | wc -l)"
echo
# 代表的な依存ファミリ。左が表示名、右が grep -E パターン。
FAMILIES='vfs-inode:\bstruct inode\b|\bi_size_read|\bi_size_write|\biget5_locked|\bnew_inode|\bunlock_new_inode|\bd_instantiate|\bd_splice_alias
dentry:\bstruct dentry\b|\bd_add\b|\bd_find_alias
folio-pagecache:\bfolio_|\bfilemap_|\bstruct page\b|\bkmap_|\bmapping_|\bread_mapping_folio|\bwrite_begin|\bwrite_end
iomap:\biomap_|\bstruct iomap\b
bio-blkdev:\bbio_|\bsubmit_bio|\bblkdev_|\bbdev_|\bsb_set_blocksize|\bsb_min_blocksize
mm-alloc:\bkmalloc|\bkzalloc|\bkvmalloc|\bkvzalloc|\bvmalloc|\bvzalloc|\bkfree|\bkvfree|\bvfree|\bkmem_cache_|\balloc_pages|\bmemalloc_
locking:\bmutex_|\bdown_(read|write)|\bup_(read|write)|\bspin_lock|\brwsem|\bwait_event|\bwake_up|\bcompletion
writeback:\bwriteback_|\bmark_inode_dirty|\bfolio_mark_dirty|\bfolio_start_writeback|\bwbc\b|\bsync_|\bfsync
module-sysfs:\bMODULE_|\bmodule_init|\bmodule_exit|\bregister_filesystem|\bproc_|\bregister_sysctl|\bseq_
fs-context:\bfs_context|\bfs_parameter|\bfs_parse|\bget_tree_bdev|\bfill_super|\bsuper_operations|\bsb_
posix-acl-xattr-sec:\bposix_acl|\bxattr|\bsecurity_|\bsimple_xattr|\bfileattr
nls-unicode:\bnls_|\bload_nls|\butf16s_to_utf8s|\butf8s_to_utf16s|\bunload_nls
byteorder-bits:\bcpu_to_le|\ble[0-9]+_to_cpu|\bget_unaligned|\bput_unaligned|\bset_bit|\btest_bit|\bfind_(first|next)_
time-cred:\bcurrent_time|\bktime_|\btimespec64|\bcurrent_fsuid|\bcurrent_fsgid|\bcurrent\b|\bcapable\(
errors-print:\bERR_PTR|\bIS_ERR|\bPTR_ERR|\bpr_(err|warn|info)|\bprintk|\bWARN_ON|\bBUG_ON|\bunlikely|\blikely'
echo "## family hits per file (count of matching lines)"
printf '%-24s' file; printf '%s\n' "$FAMILIES" | cut -d: -f1 | tr '\n' ' '; echo "lines"
for f in "$D"/*.c "$D"/*.h "$D"/lib/*.c "$D"/lib/*.h; do
  printf '%-24s' "$(basename "$f")"
  printf '%s\n' "$FAMILIES" | while IFS= read -r line; do
    pat=${line#*:}
    printf '%d ' "$(grep -Ec "$pat" "$f" || true)"
  done
  wc -l < "$f"
done
echo
echo "## #include <linux/...> usage across fs/ntfs (file count)"
grep -h '#include <' "$D"/*.c "$D"/*.h "$D"/lib/* | sort | uniq -c | sort -rn
echo
echo "## exported to VFS: operation tables"
grep -n -E 'const struct (file_operations|inode_operations|address_space_operations|super_operations|dentry_operations|export_operations|iomap_ops|iomap_write_ops|iomap_dio_ops|fs_context_operations|xattr_handler) ' "$D"/*.c | sed "s|$D/||"
