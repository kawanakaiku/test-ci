package org.usbmem.app

/**
 * シンボリックリンクを**ボリュームの中だけで**辿る (ファイルアプリへ見せるため)。
 *  - 絶対パスのリンク ("/a/b") は、そのボリュームのルートからのパスとして解釈する (元のシステムのルートではない)。
 *  - ".." はボリュームのルートより上へは行かない。
 *  - 辿る回数は 1 つのパスにつき [MAX_HOPS] まで (Linux と同じ 40)。超えたら ELOOP。a→b, b→a や自分自身への循環はここで止まる。
 *  - ネイティブの stat / list はリンクを辿らない。ここが唯一の「辿る」場所。
 */
object LinkResolver {
    const val MAX_HOPS = 40
    const val ELOOP = 40
    const val ENOENT = 2

    /** リンクを持てるファイルシステム (これ以外は辿る処理を一切しない = NTFS / exFAT の既存の動作は変わらない)。 */
    private val LINK_FS = setOf("ext2", "ext3", "ext4", "squashfs", "erofs", "iso9660", "isofs", "udf")
    fun supported(v: NtfsVolume): Boolean = v.fsName in LINK_FS

    private fun parts(p: String) = p.split('/').filter { it.isNotEmpty() }

    /** [path] (ボリューム内の絶対パス) を、リンクを辿った実体のパスにする。[followFinal] = false なら最後の要素がリンクでもそのまま残す (削除・名前変更用)。 */
    fun physical(v: NtfsVolume, path: String, followFinal: Boolean): String {
        if (!supported(v)) return path
        val cur = ArrayList<String>()
        val queue = ArrayDeque(parts(path))
        var hops = 0
        while (queue.isNotEmpty()) {
            val c = queue.removeFirst()
            if (c == ".") continue
            if (c == "..") { if (cur.isNotEmpty()) cur.removeAt(cur.size - 1); continue }
            val last = queue.isEmpty()
            if (last && !followFinal) { cur.add(c); break }
            val p = "/" + (cur + c).joinToString("/")
            if (v.stat(p).isLink) {
                if (++hops > MAX_HOPS) throw NativeException(ELOOP, "too many levels of symbolic links: '$path'")
                val t = v.readlink(p)
                if (t.startsWith("/")) cur.clear()
                queue.addAll(0, parts(t))   // 相対リンクは、リンクのあるディレクトリ (= cur) からの相対
            } else cur.add(c)
        }
        return "/" + cur.joinToString("/")
    }

    /** ファイルアプリに見せる 1 件。[entry] は表示用 (リンクなら実体の種別・サイズ)、[broken] = 辿れない/循環するリンク (リンク先の文字列を中身とする小さなファイルとして見せる)。 */
    data class View(val entry: NtfsEntry, val broken: Boolean, val target: String? = null)

    /**
     * 実体のディレクトリ [dirPhys] (physical で辿った後のパス) の下の [e] を見せ方に直す。リンクでなければそのまま。
     * リンク先がディレクトリのとき、リンク自身を含む親(祖先)ディレクトリ以上を指すなら循環するので、ファイル(リンク先の文字列)として見せる。
     */
    fun view(v: NtfsVolume, dirPhys: String, e: NtfsEntry): View {
        if (!e.isLink || !supported(v)) return View(e, false)
        val target = try { v.readlink(joinPath(dirPhys, e.name)) } catch (_: Exception) { "" }
        val textEntry = e.copy(isDir = false, size = target.toByteArray(Charsets.UTF_8).size.toLong())
        return try {
            val phys = physical(v, joinPath(dirPhys, e.name), true)
            val st = v.stat(phys)
            if (st.isDir && (dirPhys == phys || dirPhys.startsWith(if (phys == "/") "/" else "$phys/"))) View(textEntry, true, target)
            else View(e.copy(ino = st.ino, isDir = st.isDir, size = st.size, mtimeSec = st.mtimeSec), false, target)
        } catch (_: NativeException) { View(textEntry, true, target) }
    }

    /** [fromDir] (絶対パス) から見た [to] (絶対パス) の相対パス。例: from=/a/b, to=/a/c/d → ../c/d。同じ場所なら ".". */
    fun relative(fromDir: String, to: String): String {
        val f = parts(fromDir); val t = parts(to)
        var i = 0
        while (i < f.size && i < t.size && f[i] == t[i]) i++
        val up = List(f.size - i) { ".." }
        val r = (up + t.drop(i)).joinToString("/")
        return r.ifEmpty { "." }
    }

    fun joinPath(dir: String, name: String) = if (dir.endsWith("/")) dir + name else "$dir/$name"
}
