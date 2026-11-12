package org.usbmem.app

import android.os.ParcelFileDescriptor
import android.provider.DocumentsContract
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Assume.assumeTrue
import org.junit.Before
import org.junit.Test
import java.io.File
import java.io.FileInputStream
import java.security.MessageDigest

/**
 * ネスト (ループバック) マウント: ボリューム内のディスクイメージを、読み取り専用の子ボリュームとしてマウントする。docs/nested-mount.md
 * ホストが tools/gen_nested_fixture.py の p_avd.img (ext4 48 MiB。/imgs/ に c_ext2 / c_fat / c_ntfs / c_gpt / c_mbr / c_empty / tiny / l1)、
 * p_avd_rw.img (ext2。書き込み可で開く親)、nested.manifest (各イメージの中のファイルとその sha256) を /data/local/tmp に置く。
 */
class NestedMountTest {
    private val inst = InstrumentationRegistry.getInstrumentation()
    private val ctx = inst.targetContext
    private val auth = VolumeRegistry.OWN_AUTHORITY
    private val files = ArrayList<File>()
    private val entries = ArrayList<String>()
    /** image 名 → (パス → (サイズ, sha256)) */
    private val manifest = HashMap<String, HashMap<String, Pair<Long, String>>>()

    private fun shellCopy(src: String, dst: File): Boolean {
        val pfd = inst.uiAutomation.executeShellCommand("cat $src")
        FileInputStream(pfd.fileDescriptor).use { i -> dst.outputStream().use { i.copyTo(it) } }
        pfd.close()
        return dst.length() > 0
    }

    private fun sha(b: ByteArray) = MessageDigest.getInstance("SHA-256").digest(b).joinToString("") { "%02x".format(it) }

    @Before fun loadManifest() {
        val f = File(ctx.cacheDir, "nested.manifest").also { files += it }
        assumeTrue("fixture not pushed", shellCopy("/data/local/tmp/nested.manifest", f))
        for (l in f.readLines()) {
            val p = l.split('\t')
            if (p.size == 5 && p[0] == "F") manifest.getOrPut(p[1]) { HashMap() }[p[2]] = p[3].toLong() to p[4]
        }
    }

    @After fun cleanup() {
        for (id in entries.reversed()) VolumeRegistry.remove(ctx, id)
        for (f in files) f.delete()
    }

    /** 親 (p_avd.img。ext4 は読み取り専用) を登録する。 */
    private fun parent(name: String = "p_avd.img", key: String = "nest-parent", writable: Boolean = false): VolumeRegistry.Entry {
        val img = File(ctx.cacheDir, "nested_$name").also { files += it }
        assumeTrue("fixture not pushed", shellCopy("/data/local/tmp/$name", img))
        val pfd = ParcelFileDescriptor.open(img, if (writable) ParcelFileDescriptor.MODE_READ_WRITE else ParcelFileDescriptor.MODE_READ_ONLY)
        val e = try { VolumeRegistry.addFd("Parent", key, pfd.detachFd(), writable = writable, createOnly = false) }
                catch (ex: NativeException) { assumeTrue("write support disabled (errno ${ex.code})", ex.code != 30); throw ex }
        entries += e.id
        return e
    }

    private fun mountWhole(parentId: String, path: String): VolumeRegistry.Entry {
        val size = VolumeRegistry.get(ctx, parentId)!!.volume.stat(path).size
        return VolumeRegistry.mountNested(ctx, parentId, path, 0, size, path.substringAfterLast('/')).also { entries += it.id }
    }

    private fun readAll(v: NtfsVolume, path: String): ByteArray {
        val size = v.stat(path).size
        val out = java.io.ByteArrayOutputStream()
        val buf = ByteArray(65536)
        var off = 0L
        while (off < size) {
            val n = v.read(path, off, buf)
            if (n <= 0) throw AssertionError("$path @$off: short read")
            out.write(buf, 0, n); off += n
        }
        return out.toByteArray()
    }

    private fun allFiles(v: NtfsVolume, dir: String = "/", out: MutableList<String> = ArrayList()): List<String> {
        for (e in v.list(dir)) {
            val p = if (dir == "/") "/${e.name}" else "$dir/${e.name}"
            if (e.isDir) allFiles(v, p, out) else out += p
        }
        return out
    }

    /** 子ボリュームの中身が、manifest の [image] の全ファイルと (サイズ・sha256 まで) 一致し、余計なファイルもないこと。 */
    private fun verify(v: NtfsVolume, image: String, skip: Set<String> = emptySet()) {
        val want = manifest[image] ?: throw AssertionError("no manifest for $image")
        val got = allFiles(v)
        assertEquals("$image: file set", want.keys.sorted(), got.sorted())
        for ((p, sz) in want) {
            if (p in skip) continue
            assertEquals("$image $p size", sz.first, v.stat(p).size)
            assertEquals("$image $p sha256", sz.second, sha(readAll(v, p)))
        }
    }

    // ---------------------------------------------------------------- 読み取り
    @Test fun ext2ImageInsideExt4() {
        val p = parent()
        val c = mountWhole(p.id, "/imgs/c_ext2.img")
        assertEquals(p.id, c.parentId)
        assertEquals("ext2", c.volume.fsName)
        assertFalse(c.volume.writable)
        assertEquals(1, c.volume.nestDepth())
        verify(c.volume, "c_ext2.img")
    }

    @Test fun fatAndNtfsImages() {
        val p = parent()
        verify(mountWhole(p.id, "/imgs/c_fat.img").also { assertEquals("fat", it.volume.fsName) }.volume, "c_fat.img")
        verify(mountWhole(p.id, "/imgs/c_ntfs.img").also { assertEquals("ntfs", it.volume.fsName) }.volume, "c_ntfs.img")
    }

    @Test fun gptAndMbrPartitions() {
        val p = parent()
        for ((img, parts) in listOf("c_gpt.img" to mapOf(1 to "c_ext2.img", 2 to "c_fat.img"), "c_mbr.img" to mapOf(1 to "c_ext2.img", 2 to "c_fat.img", 5 to "c_ext2.img"))) {
            val t = VolumeRegistry.inspectNested(ctx, p.id, "/imgs/$img")
            assertEquals(img, if (img == "c_gpt.img") "gpt" else "mbr", t.kind)
            assertFalse(t.wholeDeviceVolume)
            val mountable = t.partitions.filter { !it.extendedContainer }
            assertEquals(img, parts.keys, mountable.map { it.index }.toSet())
            for (part in mountable) {
                val c = VolumeRegistry.mountNested(ctx, p.id, "/imgs/$img", part.startBytes, part.sizeBytes, "$img #${part.index}").also { entries += it.id }
                assertFalse(c.volume.writable)
                verify(c.volume, parts[part.index]!!)
            }
        }
    }

    @Test fun wholeDeviceImageHasNoTable() {
        val p = parent()
        val t = VolumeRegistry.inspectNested(ctx, p.id, "/imgs/c_ext2.img")
        assertTrue(t.wholeDeviceVolume)
        assertEquals(3L * 1024 * 1024, t.diskBytes)
    }

    // ---------------------------------------------------------------- 段数・循環・拒否
    @Test fun depthLimitAndChain() {
        val p = parent()
        val l1 = mountWhole(p.id, "/imgs/l1.img")
        assertEquals("ext4", l1.volume.fsName); assertEquals(1, l1.volume.nestDepth())
        val l2 = mountWhole(l1.id, "/l2.img")
        assertEquals("ext2", l2.volume.fsName); assertEquals(2, l2.volume.nestDepth())
        val l3 = mountWhole(l2.id, "/l3.img")
        assertEquals("fat", l3.volume.fsName); assertEquals(3, l3.volume.nestDepth())
        verify(l1.volume, "l1.img", skip = setOf("/l2.img"))      // l2.img は 10 MiB: 内側で確かめる
        verify(l2.volume, "l2.img", skip = setOf("/l3.img"))
        verify(l3.volume, "l3.img")                                // 3 段の奥から、l4.img (3 MiB) を丸ごと読んで sha256 が一致
        // 4 段目は拒否 (ELOOP=40)。何も登録されない
        try { mountWhole(l3.id, "/l4.img"); fail("4th level must be refused") } catch (e: NativeException) { assertEquals(40, e.code) }
        assertTrue(VolumeRegistry.childrenOf(l3.id).isEmpty())
        try { VolumeRegistry.inspectNested(ctx, l3.id, "/l4.img"); fail("inspect at depth 3 must be refused") } catch (e: NativeException) { assertEquals(40, e.code) }
    }

    @Test fun notAnImage() {
        val p = parent()
        fun code(path: String): Int = try { mountWhole(p.id, path); -1 } catch (e: NativeException) { e.code }
        assertEquals(2, code("/imgs/no_such.img"))      // ENOENT
        assertEquals(21, code("/imgs"))                  // EISDIR
        assertEquals(22, code("/imgs/tiny.img"))         // EINVAL: 512 バイト未満
        assertTrue("an all-zero image must not mount", code("/imgs/c_empty.img") > 0)
        assertTrue(VolumeRegistry.childrenOf(p.id).isEmpty())
    }

    @Test fun mountingTwiceReturnsTheSameEntry() {
        val p = parent()
        val a = mountWhole(p.id, "/imgs/c_ext2.img")
        val b = mountWhole(p.id, "/imgs/c_ext2.img")
        assertEquals(a.id, b.id)
        assertEquals(1, VolumeRegistry.childrenOf(p.id).size)
    }

    // ---------------------------------------------------------------- 閉じる順 (内 → 外)
    @Test fun closingTheParentClosesTheChildrenFirst() {
        val p = parent()
        val l1 = mountWhole(p.id, "/imgs/l1.img")
        val l2 = mountWhole(l1.id, "/l2.img")
        val c = mountWhole(p.id, "/imgs/c_ext2.img")
        val vols = listOf(l2.volume, l1.volume, c.volume)
        VolumeRegistry.remove(ctx, p.id)
        assertNull(VolumeRegistry.get(ctx, p.id)); assertNull(VolumeRegistry.get(ctx, l1.id)); assertNull(VolumeRegistry.get(ctx, l2.id)); assertNull(VolumeRegistry.get(ctx, c.id))
        for (v in vols) try { v.list("/"); fail("child must be closed") } catch (e: IllegalStateException) { /* closed */ }
        try { p.volume.list("/"); fail("parent must be closed") } catch (e: IllegalStateException) { /* closed */ }
        // 同じ親をもう一度登録し直して、子もマウントし直せる
        val p2 = parent()
        verify(mountWhole(p2.id, "/imgs/c_ext2.img").volume, "c_ext2.img")
    }

    @Test fun closingOneChildKeepsTheParentAndSiblings() {
        val p = parent()
        val a = mountWhole(p.id, "/imgs/c_ext2.img")
        val b = mountWhole(p.id, "/imgs/c_fat.img")
        VolumeRegistry.remove(ctx, a.id)
        assertEquals(listOf(b.id), VolumeRegistry.childrenOf(p.id).map { it.id })
        verify(b.volume, "c_fat.img")
        assertTrue(p.volume.list("/imgs").isNotEmpty())
        // 閉じたあとは、同じイメージをまたマウントできる
        verify(mountWhole(p.id, "/imgs/c_ext2.img").volume, "c_ext2.img")
    }

    // ---------------------------------------------------------------- 親が書き込み可のとき: マウント中のイメージは書き換えられない
    @Test fun mountedImageIsProtectedInAWritableParent() {
        val p = parent("p_avd_rw.img", "nest-rw", writable = true)
        assumeTrue("parent not writable", p.volume.writable)
        val c = mountWhole(p.id, "/imgs/c_ext2.img")
        fun busy(block: () -> Unit) { try { block(); fail("must be refused while the image is mounted") } catch (e: NativeException) { assertEquals(16, e.code) } }
        busy { p.volume.write("/imgs/c_ext2.img", 100, byteArrayOf(1, 2, 3)) }
        busy { p.volume.truncate("/imgs/c_ext2.img", 1000) }
        busy { p.volume.remove("/imgs/c_ext2.img") }
        busy { p.volume.rename("/imgs/c_ext2.img", "x.img") }
        busy { p.volume.move("/imgs/c_ext2.img", "/", "x.img") }
        busy { p.volume.removeTree("/imgs") }
        busy { p.volume.move("/imgs", "/", "imgs2") }
        verify(c.volume, "c_ext2.img")        // 何も壊れていない
        // 他のファイルは普通に作れる / 書ける
        p.volume.create("/imgs", "other.txt", false); p.volume.write("/imgs/other.txt", 0, "x".toByteArray())
        // 子を閉じれば、書き換えられる
        VolumeRegistry.remove(ctx, c.id)
        p.volume.write("/imgs/c_ext2.img", 100, byteArrayOf(9))
    }

    // ---------------------------------------------------------------- このアプリ自身のプロバイダのファイル (SAF の fd を経由しない)
    @Test fun ownProviderUriIsMountedNatively() {
        val p = parent()
        val uri = DocumentsContract.buildDocumentUri(auth, "${p.id}:/imgs/c_ext2.img")
        assertEquals(p.id to "/imgs/c_ext2.img", VolumeRegistry.ownDocument(uri))
        val done = java.util.concurrent.atomic.AtomicReference<VolumeRegistry.Entry?>()
        val t = Thread { done.set(VolumeRegistry.addUri(ctx, uri)) }
        t.start(); t.join(60_000)
        assertFalse("addUri on an own-provider URI must not hang", t.isAlive)
        val e = done.get()!!.also { entries += it.id }
        assertEquals(p.id, e.parentId)
        verify(e.volume, "c_ext2.img")
        assertNull(VolumeRegistry.ownDocument(DocumentsContract.buildDocumentUri(auth, "nope:/x")))
        assertNull(VolumeRegistry.ownDocument(DocumentsContract.buildDocumentUri("com.android.externalstorage.documents", "primary:x")))
    }

    // ---------------------------------------------------------------- ファイルアプリ (SAF プロバイダ) 経由で子の中身を読む / 親と同時に読む
    @Test fun readThroughTheProviderTwoLevelsDeepWhileReadingTheParent() {
        val p = parent()
        val c = mountWhole(p.id, "/imgs/c_ext2.img")
        val cr = ctx.contentResolver
        fun readDoc(id: String): ByteArray = cr.openInputStream(DocumentsContract.buildDocumentUri(auth, id))!!.use { it.readBytes() }
        val want = manifest["c_ext2.img"]!!
        val errors = java.util.concurrent.atomic.AtomicInteger()
        val threads = ArrayList<Thread>()
        // 子のファイルを読む (プロバイダ → 子 → 親)
        for (i in 0 until 2) threads += Thread {
            try { for ((path, w) in want) if (sha(readDoc("${c.id}:$path")) != w.second) errors.incrementAndGet() } catch (_: Throwable) { errors.incrementAndGet() }
        }
        // 同時に、親のイメージファイルそのものを読む (プロバイダ → 親)
        threads += Thread {
            try { val b = readDoc("${p.id}:/imgs/c_ext2.img"); if (b.size != 3 * 1024 * 1024) errors.incrementAndGet() } catch (_: Throwable) { errors.incrementAndGet() }
        }
        threads.forEach { it.start() }
        threads.forEach { it.join(120_000) }
        assertTrue("provider reads must finish (no deadlock)", threads.none { it.isAlive })
        assertEquals(0, errors.get())
        // 子の document は root としても出る
        cr.query(DocumentsContract.buildRootsUri(auth), null, null, null, null)!!.use { cur ->
            val ids = ArrayList<String>()
            val col = cur.getColumnIndex(DocumentsContract.Root.COLUMN_ROOT_ID)
            while (cur.moveToNext()) ids += cur.getString(col)
            assertTrue(ids.contains(c.id) && ids.contains(p.id))
        }
    }

    // ---------------------------------------------------------------- 並行 (複数スレッドで子と親を同時に読む)
    @Test fun concurrentReadsOfParentAndChild() {
        val p = parent()
        val c = mountWhole(p.id, "/imgs/c_fat.img")
        val want = manifest["c_fat.img"]!!
        val errors = java.util.concurrent.atomic.AtomicInteger()
        val threads = ArrayList<Thread>()
        for (i in 0 until 3) threads += Thread { try { repeat(3) { for ((path, w) in want) if (sha(readAll(c.volume, path)) != w.second) errors.incrementAndGet() } } catch (_: Throwable) { errors.incrementAndGet() } }
        for (i in 0 until 2) threads += Thread { try { repeat(3) { if (p.volume.list("/imgs").size < 7) errors.incrementAndGet(); readAll(p.volume, "/imgs/c_fat.img") } } catch (_: Throwable) { errors.incrementAndGet() } }
        threads.forEach { it.start() }
        threads.forEach { it.join(180_000) }
        assertTrue("no deadlock", threads.none { it.isAlive })
        assertEquals(0, errors.get())
    }
}
