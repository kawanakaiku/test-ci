package org.usbmem.app

import android.database.ContentObserver
import android.os.Handler
import android.os.Looper
import android.os.ParcelFileDescriptor
import android.provider.DocumentsContract
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Before
import org.junit.Test
import java.io.File
import java.io.FileInputStream
import java.security.MessageDigest
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger

/**
 * 複数の USB を同時に開いたときの、ボリューム登録簿 (VolumeRegistry) と DocumentsProvider の振る舞い。docs/multi-usb.md
 * USB の実機・AVD での USB 転送は無いので、デバイスは「偽のセッション + 実イメージのボリューム」で表す:
 * UsbSessionManager (本物) が偽の UsbSession を管理し、デバイスごとの公開ボリュームは p_avd.img (ext4) の実マウント。
 * 事前に tools/run_android_multi_usb_test.sh が /data/local/tmp に p_avd.img と nested.manifest を置く (nested テストと同じフィクスチャ)。
 */
class MultiUsbTest {
    private val inst = InstrumentationRegistry.getInstrumentation()
    private val ctx = inst.targetContext
    private val auth = VolumeRegistry.OWN_AUTHORITY
    private val files = ArrayList<File>()
    private val mgrs = ArrayList<UsbSessionManager>()
    private lateinit var image: File

    private fun shellCopy(src: String, dst: File): Boolean {
        val pfd = inst.uiAutomation.executeShellCommand("cat $src")
        FileInputStream(pfd.fileDescriptor).use { i -> dst.outputStream().use { i.copyTo(it) } }
        pfd.close()
        return dst.length() > 0
    }

    @Before fun setUp() {
        image = File(ctx.cacheDir, "multi_p_avd.img").also { files += it }
        assumeTrue("fixture not pushed", shellCopy("/data/local/tmp/p_avd.img", image))
    }

    @After fun tearDown() {
        for (m in mgrs) m.closeAll()
        for (e in VolumeRegistry.all(ctx).toList()) if (e.deviceName != null) VolumeRegistry.remove(ctx, e.id)
        for (f in files) f.delete()
    }

    /** 偽のセッション。close で closed になる。 */
    private class FakeSession(override val deviceName: String, override val writable: Boolean) : UsbSession {
        @Volatile var closed = false
        override val vendorId = 0x0781; override val productId = 0x5581; override val tuningIndex = -1
        override fun partitionTable() = PartitionTable("mbr", 512, 1L shl 30, "", false, false, emptyList(), emptyList())
        override fun opStats() = ""
        override fun mountNtfs(offsetBytes: Long, sizeBytes: Long, writable: Boolean, createOnly: Boolean, allowNoload: Boolean): NtfsVolume = throw UnsupportedOperationException()
        override fun startStop(start: Boolean) {}
        override fun isSpunDown() = false
        override fun setIdleSpinDown(idleMinutes: Int, onStopped: (Boolean) -> Unit) {}
        override fun tune(chunkKiB: Int, maxTransferKiB: Int) {}
        override fun benchReadHash(offset: Long, totalBytes: Long, bufKiB: Int, hash: LongArray) = 1L
        override fun benchRead(offset: Long, totalBytes: Long, bufKiB: Int) = 1L
        override fun <T> withHandle(block: (Long) -> T): T = block(0)
        override fun close() { closed = true }
    }

    private fun newManager(): UsbSessionManager = UsbSessionManager(
        { d, w -> FakeSession(d.deviceName, w) },
        onDeviceClosing = { name -> try { VolumeRegistry.removeByDevice(name) } finally { VolumeRegistry.notifyRoots(ctx) } },   // 本番 (UsbSessions) と同じ後始末
    ).also { mgrs += it }

    private fun dev(n: Int) = UsbDeviceInfo("/dev/bus/usb/001/%03d".format(n), "Stick $n", 0x0781, 0x5581)

    /** デバイス [d] の公開ボリュームを 1 つ登録する (USB のパーティションのマウントと同じ登録の仕方)。 */
    private fun publish(d: UsbDeviceInfo, startBytes: Long, label: String): VolumeRegistry.Entry {
        val pfd = ParcelFileDescriptor.open(image, ParcelFileDescriptor.MODE_READ_ONLY)
        val v = NtfsVolume.mountReadOnly(pfd.detachFd())
        return VolumeRegistry.addVolume(label, VolumeRegistry.usbKey(d.deviceName, startBytes), v, deviceName = d.deviceName)
    }

    private fun mountChild(parent: VolumeRegistry.Entry, path: String): VolumeRegistry.Entry {
        val size = parent.volume.stat(path).size
        return VolumeRegistry.mountNested(ctx, parent.id, path, 0, size, path.substringAfterLast('/'))
    }

    private fun roots(): Map<String, Pair<String, String>> {
        val out = LinkedHashMap<String, Pair<String, String>>()   // root_id -> (title, summary)
        ctx.contentResolver.query(DocumentsContract.buildRootsUri(auth), null, null, null, null)!!.use { c ->
            while (c.moveToNext()) out[c.getString(c.getColumnIndexOrThrow("root_id"))] = c.getString(c.getColumnIndexOrThrow("title")) to c.getString(c.getColumnIndexOrThrow("summary"))
        }
        return out
    }

    private fun readAll(v: NtfsVolume, path: String): String {
        val md = MessageDigest.getInstance("SHA-256")
        val buf = ByteArray(65536)
        var off = 0L
        while (true) {
            val n = v.read(path, off, buf)
            if (n <= 0) break
            md.update(buf, 0, n); off += n
        }
        return md.digest().joinToString("") { "%02x".format(it) }
    }

    @Test fun rootsAreSeparatePerDeviceAndPerVolume() {
        val a = dev(2); val b = dev(3)
        val ids = UsbDeviceNaming.shortIds(listOf(a, b))
        val a1 = publish(a, 0, UsbDeviceNaming.volumeLabel("Ultra Fit", ids.getValue(a.deviceName), 1))
        val a2 = publish(a, 1L shl 20, UsbDeviceNaming.volumeLabel("Ultra Fit", ids.getValue(a.deviceName), 2))
        val b1 = publish(b, 0, UsbDeviceNaming.volumeLabel("Ultra Fit", ids.getValue(b.deviceName), 1))
        assertEquals(3, setOf(a1.id, a2.id, b1.id).size)
        val r = roots()
        for (e in listOf(a1, a2, b1)) assertTrue("root ${e.label}", r.containsKey(e.id))
        // 同じ機種名でも、公開名 (ルートのタイトル) は全部違う
        assertEquals(3, listOf(a1, a2, b1).map { r.getValue(it.id).first }.toSet().size)
        assertTrue(r.getValue(a1.id).first.contains("[1-2]") && r.getValue(b1.id).first.contains("[1-3]"))
        assertTrue("どのデバイスか分かる説明", r.getValue(a1.id).second.contains("USB 1-2") && r.getValue(b1.id).second.contains("USB 1-3"))
    }

    @Test fun closingOneDeviceRemovesOnlyItsRootsAndNestedChildrenAndNotifies() {
        val m = newManager()
        val a = dev(2); val b = dev(3); val c = dev(4)
        for (d in listOf(a, b, c)) m.open(d)
        val a1 = publish(a, 0, "A #1"); val a2 = publish(a, 1L shl 20, "A #2")
        val b1 = publish(b, 0, "B #1"); val c1 = publish(c, 0, "C #1")
        val aChild = mountChild(a1, "/imgs/c_fat.img"); val bChild = mountChild(b1, "/imgs/c_fat.img")
        val before = roots()
        for (e in listOf(a1, a2, b1, c1, aChild, bChild)) assertTrue(before.containsKey(e.id))

        // ルート変更の通知 (notifyChange) が来ること
        val latch = CountDownLatch(1)
        val obs = object : ContentObserver(Handler(Looper.getMainLooper())) { override fun onChange(selfChange: Boolean) { latch.countDown() } }
        ctx.contentResolver.registerContentObserver(DocumentsContract.buildRootsUri(auth), true, obs)
        try {
            assertTrue(m.close(a.deviceName))
            assertTrue("roots notifyChange", latch.await(10, TimeUnit.SECONDS))
        } finally { ctx.contentResolver.unregisterContentObserver(obs) }

        val after = roots()
        for (e in listOf(a1, a2, aChild)) assertFalse("${e.label} の root は消える", after.containsKey(e.id))
        for (e in listOf(b1, c1, bChild)) assertTrue("${e.label} の root は残る", after.containsKey(e.id))
        assertNull(VolumeRegistry.get(ctx, a1.id)); assertNull(VolumeRegistry.get(ctx, aChild.id))
        assertEquals(listOf(b.deviceName, c.deviceName), m.openDevices())
        // 残ったデバイスのボリューム (ネストした子も) は、SAF 経由でそのまま読める
        val kids = ctx.contentResolver.query(DocumentsContract.buildChildDocumentsUri(auth, "${b1.id}:/imgs"), arrayOf("_display_name"), null, null, null)!!.use { c0 -> generateSequence { if (c0.moveToNext()) c0.getString(0) else null }.toList() }
        assertTrue(kids.contains("c_fat.img"))
        assertTrue(bChild.volume.list("/").isNotEmpty())
        // 次のデバイスを抜く
        assertTrue(m.onDetached(b.deviceName))
        val last = roots()
        assertFalse(last.containsKey(b1.id)); assertFalse(last.containsKey(bChild.id)); assertTrue(last.containsKey(c1.id))
    }

    @Test fun readingOneDeviceIsUnaffectedByClosingAnotherConcurrently() {
        val m = newManager()
        val a = dev(2); val b = dev(3)
        m.open(a); m.open(b)
        val a1 = publish(a, 0, "A #1"); val b1 = publish(b, 0, "B #1")
        val want = readAll(b1.volume, "/imgs/c_fat.img")
        assertEquals(want, readAll(a1.volume, "/imgs/c_fat.img"))
        val stop = java.util.concurrent.atomic.AtomicBoolean(false)
        val bReads = AtomicInteger(); val bErrors = CopyOnWriteArrayList<Throwable>(); val aOther = CopyOnWriteArrayList<Throwable>()
        val pool = Executors.newFixedThreadPool(4)
        // B を何度も読み続ける (ハッシュは毎回同じでなければならない)
        repeat(2) { pool.execute { try { while (!stop.get()) { assertEquals(want, readAll(b1.volume, "/imgs/c_fat.img")); bReads.incrementAndGet() } } catch (t: Throwable) { bErrors += t } } }
        // A も読み続ける。閉じられた後は NativeException / IllegalStateException で失敗してよい (クラッシュ・別の例外は不可)
        pool.execute { try { while (!stop.get()) readAll(a1.volume, "/imgs/c_fat.img") } catch (e: NativeException) { } catch (e: IllegalStateException) { } catch (t: Throwable) { aOther += t } }
        Thread.sleep(300)
        val reads0 = bReads.get()
        assertTrue(m.onDetached(a.deviceName))   // A を抜く (後始末 = A のボリュームを閉じる) 間も B は読み続けている
        val deadline = System.currentTimeMillis() + 20_000
        while (bReads.get() < reads0 + 2 && System.currentTimeMillis() < deadline) Thread.sleep(50)
        stop.set(true); pool.shutdown(); assertTrue(pool.awaitTermination(30, TimeUnit.SECONDS))
        assertEquals(emptyList<Throwable>(), bErrors.toList())
        assertEquals(emptyList<Throwable>(), aOther.toList())
        assertTrue("A を閉じている間も B は読めていた", bReads.get() >= reads0 + 2)
        assertNull(VolumeRegistry.get(ctx, a1.id))
        assertEquals(want, readAll(b1.volume, "/imgs/c_fat.img"))
    }

    @Test fun twoDevicesMountedFromThreadsConcurrentlyBothWork() {
        // 2 本のデバイスのボリュームを別スレッドで同時にマウント・読み取りして、互いに壊さない (LinuxCompat のプロセス共通の状態の確認)
        val d = listOf(dev(2), dev(3), dev(4))
        val pool = Executors.newFixedThreadPool(d.size)
        val errors = CopyOnWriteArrayList<Throwable>()
        val sums = CopyOnWriteArrayList<String>()
        val done = CountDownLatch(d.size)
        d.forEachIndexed { i, x -> pool.execute { try { repeat(3) { r -> val e = publish(x, r * 4096L, "T$i-$r"); sums += readAll(e.volume, "/imgs/c_fat.img") } } catch (t: Throwable) { errors += t } finally { done.countDown() } } }
        assertTrue(done.await(120, TimeUnit.SECONDS)); pool.shutdown()
        assertEquals(emptyList<Throwable>(), errors.toList())
        assertEquals(1, sums.toSet().size); assertEquals(9, sums.size)
        assertEquals(9, VolumeRegistry.all(ctx).count { it.deviceName != null })
        for (x in d) VolumeRegistry.removeByDevice(x.deviceName)
        assertEquals(0, VolumeRegistry.all(ctx).count { it.deviceName != null })
    }
}
