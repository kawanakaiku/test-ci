package org.usbmem.app

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotSame
import org.junit.Assert.assertNull
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test
import java.util.Collections
import java.util.concurrent.CountDownLatch
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger

/** 実機の USB の代わりに使う偽のセッション。close 後に触ると IllegalStateException (本物の UsbStorageSession と同じ)。 */
class FakeUsbSession(
    override val deviceName: String,
    override val writable: Boolean,
    override val vendorId: Int = 0x1234,
    override val productId: Int = 0x5678,
) : UsbSession {
    @Volatile var closed = false
    val closeCalls = AtomicInteger()
    val ioCount = AtomicInteger()
    @Volatile var idleMinutes = -1
    @Volatile var idleCallback: ((Boolean) -> Unit)? = null
    override val tuningIndex = -1

    private fun alive() { check(!closed) { "closed: $deviceName" } }

    /** 実際の I/O の代わり。デバイスごとに独立して数える。 */
    fun io(): Int { alive(); return ioCount.incrementAndGet() }

    override fun partitionTable() = PartitionTable("mbr", 512, 1L shl 30, "", false, false, emptyList(), emptyList()).also { alive() }
    override fun opStats(): String { alive(); return "28x${ioCount.get()}" }
    override fun mountNtfs(offsetBytes: Long, sizeBytes: Long, writable: Boolean, createOnly: Boolean, allowNoload: Boolean): NtfsVolume = throw UnsupportedOperationException("fake")
    override fun startStop(start: Boolean) { alive() }
    override fun isSpunDown() = false
    override fun setIdleSpinDown(idleMinutes: Int, onStopped: (Boolean) -> Unit) { alive(); this.idleMinutes = idleMinutes; idleCallback = onStopped }
    override fun tune(chunkKiB: Int, maxTransferKiB: Int) { alive() }
    override fun benchReadHash(offset: Long, totalBytes: Long, bufKiB: Int, hash: LongArray) = 1L
    override fun benchRead(offset: Long, totalBytes: Long, bufKiB: Int) = 1L
    override fun <T> withHandle(block: (Long) -> T): T { alive(); return block(deviceName.hashCode().toLong()) }
    override fun close() { closeCalls.incrementAndGet(); closed = true }
}

/** 偽のファクトリ: 開いたセッションを記録する。block を与えると、その間 open が止まる (遅い USB の再現)。 */
class FakeFactory : UsbSessionFactory {
    val opened = CopyOnWriteArrayList<FakeUsbSession>()
    val requests = CopyOnWriteArrayList<Pair<String, Boolean>>()
    val failFor: MutableSet<String> = Collections.synchronizedSet(HashSet())
    @Volatile var gate: ((UsbDeviceInfo) -> Unit)? = null

    override fun open(device: UsbDeviceInfo, writable: Boolean): UsbSession {
        requests += device.deviceName to writable
        gate?.invoke(device)
        if (device.deviceName in failFor) throw NativeException(2, "fake open failure")
        return FakeUsbSession(device.deviceName, writable, device.vendorId, device.productId).also { opened += it }
    }

    fun sessionsOf(name: String) = opened.filter { it.deviceName == name }
}

class UsbSessionManagerTest {
    private fun dev(n: Int, serial: String? = null) = UsbDeviceInfo("/dev/bus/usb/001/%03d".format(n), "Stick $n", 0x0781, 0x5581, serial)
    private val A = dev(2); private val B = dev(3); private val C = dev(4)

    private class Recorder : UsbSessionListener {
        val events = CopyOnWriteArrayList<String>()
        override fun onOpened(deviceName: String) { events += "open:$deviceName" }
        override fun onClosed(deviceName: String, reason: CloseReason) { events += "closed:$deviceName:$reason" }
        override fun onIdleChanged(deviceName: String, stopped: Boolean) { events += "idle:$deviceName:$stopped" }
    }

    private fun mgr(f: FakeFactory = FakeFactory(), writeSupported: Boolean = true, maxOpen: Int = Int.MAX_VALUE, closing: (String) -> Unit = {}) =
        UsbSessionManager(f, writeSupported, maxOpen, closing)

    // ---- 同時オープンと、デバイス単位の閉じる/抜去

    @Test fun threeDevicesOpenSimultaneouslyAndIndependently() {
        val f = FakeFactory(); val m = mgr(f)
        val sa = m.open(A); val sb = m.open(B); val sc = m.open(C)
        assertEquals(listOf(A.deviceName, B.deviceName, C.deviceName), m.openDevices())
        assertEquals(3, m.openCount())
        assertNotSame(sa, sb); assertNotSame(sb, sc)
        assertSame(sb, m.session(B.deviceName))
        // 片方の I/O はもう片方に影響しない
        (sa as FakeUsbSession).io(); (sb as FakeUsbSession).io(); sb.io()
        assertEquals(1, sa.ioCount.get()); assertEquals(2, sb.ioCount.get()); assertEquals(0, (sc as FakeUsbSession).ioCount.get())
    }

    @Test fun closingOneLeavesTheOthersOpenAndUsable() {
        val closing = CopyOnWriteArrayList<String>()
        val f = FakeFactory(); val m = mgr(f, closing = { closing += it })
        m.open(A); m.open(B); m.open(C)
        assertTrue(m.close(B.deviceName))
        assertEquals(listOf(B.deviceName), closing.toList())              // 後始末 (ボリューム/ルート) は B の分だけ
        assertEquals(listOf(A.deviceName, C.deviceName), m.openDevices())
        assertTrue(f.sessionsOf(B.deviceName).single().closed)
        assertFalse(f.sessionsOf(A.deviceName).single().closed)
        assertFalse(f.sessionsOf(C.deviceName).single().closed)
        f.sessionsOf(A.deviceName).single().io(); f.sessionsOf(C.deviceName).single().io()   // 例外にならない
        assertNull(m.session(B.deviceName))
        assertFalse("閉じたものをもう一度閉じても何も起きない", m.close(B.deviceName))
        assertEquals(1, f.sessionsOf(B.deviceName).single().closeCalls.get())
    }

    @Test fun detachClosesOnlyThatDeviceAndNotifiesWhichOne() {
        val closing = CopyOnWriteArrayList<String>()
        val rec = Recorder()
        val f = FakeFactory(); val m = mgr(f, closing = { closing += it }); m.addListener(rec)
        m.open(A); m.open(B)
        assertTrue(m.onDetached(A.deviceName))
        assertEquals(listOf(A.deviceName), closing.toList())
        assertEquals(listOf(B.deviceName), m.openDevices())
        assertTrue(f.sessionsOf(A.deviceName).single().closed)
        assertFalse(f.sessionsOf(B.deviceName).single().closed)
        assertTrue("closed:${A.deviceName}:Detached" in rec.events)
        assertFalse(rec.events.any { it.startsWith("closed:${B.deviceName}") })
        // 抜かれた機器を挿し直せる
        m.open(A)
        assertEquals(listOf(A.deviceName, B.deviceName), m.openDevices())
    }

    @Test fun detachOfAnUnopenedDeviceIsHarmless() {
        val m = mgr()
        m.open(B)
        assertFalse(m.onDetached(A.deviceName))
        assertEquals(listOf(B.deviceName), m.openDevices())
    }

    @Test fun cleanupRunsBeforeSessionCloseAndEvenIfItThrows() {
        val order = CopyOnWriteArrayList<String>()
        val f = FakeFactory()
        val m = mgr(f, closing = { order += "cleanup"; throw IllegalStateException("boom") })
        val s = m.open(A) as FakeUsbSession
        m.close(A.deviceName)
        assertTrue("後始末が失敗してもセッションは閉じる", s.closed)
        assertFalse(m.isBusy(A.deviceName))
        assertEquals(listOf("cleanup"), order.toList())
    }

    @Test fun closeAllClosesEverythingEvenIfOneHookFails() {
        val f = FakeFactory()
        val m = mgr(f, closing = { if (it == B.deviceName) throw RuntimeException("x") })
        m.open(A); m.open(B); m.open(C)
        m.closeAll()
        assertEquals(0, m.openCount())
        assertTrue(f.opened.all { it.closed })
    }

    // ---- 二重オープンの拒否

    @Test fun doubleOpenIsRejected() {
        val f = FakeFactory(); val m = mgr(f)
        m.open(A)
        try { m.open(A); fail() } catch (e: UsbSessionException) { assertEquals(UsbSessionException.Kind.AlreadyOpen, e.kind) }
        assertEquals("ファクトリは 1 回しか呼ばれない", 1, f.requests.count { it.first == A.deviceName })
        assertEquals(1, m.openCount())
    }

    @Test fun concurrentOpenOfTheSameDeviceYieldsExactlyOneSession() {
        val f = FakeFactory()
        f.gate = { Thread.sleep(30) }   // 競合の窓を広げる
        val m = mgr(f)
        val n = 12
        val start = CountDownLatch(1)
        val ok = AtomicInteger(); val rejected = AtomicInteger(); val other = AtomicInteger()
        val pool = Executors.newFixedThreadPool(n)
        repeat(n) { pool.execute { start.await(); try { m.open(A); ok.incrementAndGet() } catch (e: UsbSessionException) { if (e.kind == UsbSessionException.Kind.AlreadyOpen) rejected.incrementAndGet() else other.incrementAndGet() } catch (_: Throwable) { other.incrementAndGet() } } }
        start.countDown(); pool.shutdown(); assertTrue(pool.awaitTermination(20, TimeUnit.SECONDS))
        assertEquals(1, ok.get()); assertEquals(n - 1, rejected.get()); assertEquals(0, other.get())
        assertEquals(1, f.opened.size)
    }

    @Test fun openAfterAFailedOpenCanRetryAndOthersAreUnaffected() {
        val f = FakeFactory(); val m = mgr(f)
        m.open(B)
        f.failFor += A.deviceName
        try { m.open(A); fail() } catch (e: NativeException) { /* ok */ }
        assertFalse("失敗した open は状態を残さない", m.isBusy(A.deviceName))
        assertEquals(listOf(B.deviceName), m.openDevices())
        f.failFor.clear()
        m.open(A)
        assertEquals(2, m.openCount())
    }

    @Test fun openLimitIsEnforcedWhenConfigured() {
        val m = mgr(maxOpen = 2)
        m.open(A); m.open(B)
        try { m.open(C); fail() } catch (e: UsbSessionException) { assertEquals(UsbSessionException.Kind.LimitReached, e.kind) }
        m.close(A.deviceName)
        m.open(C)
        assertEquals(listOf(B.deviceName, C.deviceName), m.openDevices())
    }

    // ---- 書き込みモードはデバイスごと

    @Test fun writeModeIsPerDeviceAndDefaultsToReadOnly() {
        val f = FakeFactory(); val m = mgr(f)
        assertEquals(WriteMode.ReadOnly, m.writeMode(A.deviceName))
        assertTrue(m.setWriteMode(A.deviceName, WriteMode.Full))
        assertTrue(m.setWriteMode(B.deviceName, WriteMode.CreateOnly))
        assertEquals(WriteMode.ReadOnly, m.writeMode(C.deviceName))
        val sa = m.open(A); val sb = m.open(B); val sc = m.open(C)
        assertTrue(sa.writable); assertTrue(sb.writable); assertFalse(sc.writable)
        assertEquals(listOf(true, true, false), listOf(A, B, C).map { d -> f.requests.single { it.first == d.deviceName }.second })
        assertEquals(WriteMode.Full, m.writeMode(A.deviceName))
        assertEquals(WriteMode.CreateOnly, m.writeMode(B.deviceName))
        assertEquals(WriteMode.ReadOnly, m.writeMode(C.deviceName))
    }

    @Test fun writeModeCanOnlyChangeBeforeOpening() {
        val m = mgr()
        m.open(A)
        assertFalse("開いている間は変えられない", m.setWriteMode(A.deviceName, WriteMode.Full))
        assertEquals(WriteMode.ReadOnly, m.writeMode(A.deviceName))
        assertTrue("他のデバイスは変えられる", m.setWriteMode(B.deviceName, WriteMode.Full))
        m.close(A.deviceName)
        assertTrue(m.setWriteMode(A.deviceName, WriteMode.CreateOnly))
        assertEquals(WriteMode.CreateOnly, m.writeMode(A.deviceName))
    }

    @Test fun writeModeIsForgottenWhenDeviceIsDetached() {
        val m = mgr()
        m.setWriteMode(A.deviceName, WriteMode.Full)
        m.open(A)
        m.onDetached(A.deviceName)
        assertEquals("挿し直したら読み取り専用から", WriteMode.ReadOnly, m.writeMode(A.deviceName))
        // ユーザーが閉じただけなら、同じ接続の間はモードを覚えている
        m.setWriteMode(B.deviceName, WriteMode.CreateOnly); m.open(B); m.close(B.deviceName)
        assertEquals(WriteMode.CreateOnly, m.writeMode(B.deviceName))
    }

    @Test fun writeDisabledBuildCannotSelectWriteModes() {
        val f = FakeFactory(); val m = mgr(f, writeSupported = false)
        assertFalse(m.setWriteMode(A.deviceName, WriteMode.CreateOnly))
        assertFalse(m.setWriteMode(A.deviceName, WriteMode.Full))
        assertTrue(m.setWriteMode(A.deviceName, WriteMode.ReadOnly))
        assertFalse(m.open(A).writable)
    }

    // ---- 1 本の遅い処理が他の本を止めない

    @Test fun slowOpenOfOneDeviceDoesNotBlockOthers() {
        val f = FakeFactory()
        val inOpen = CountDownLatch(1); val release = CountDownLatch(1)
        f.gate = { d -> if (d.deviceName == A.deviceName) { inOpen.countDown(); release.await(20, TimeUnit.SECONDS) } }
        val m = mgr(f)
        val t = Thread { m.open(A) }.also { it.start() }
        assertTrue(inOpen.await(10, TimeUnit.SECONDS))
        // A が開いている最中でも、B は開いて閉じられ、A の状態は「開く最中」
        m.open(B); assertTrue(m.isOpen(B.deviceName)); m.close(B.deviceName)
        assertTrue(m.isBusy(A.deviceName)); assertFalse(m.isOpen(A.deviceName)); assertNull(m.session(A.deviceName))
        try { m.open(A); fail() } catch (e: UsbSessionException) { assertEquals(UsbSessionException.Kind.AlreadyOpen, e.kind) }
        assertFalse("開く最中は書き込みモードを変えられない", m.setWriteMode(A.deviceName, WriteMode.Full))
        release.countDown(); t.join(10_000)
        assertTrue(m.isOpen(A.deviceName))
    }

    @Test fun slowCloseOfOneDeviceDoesNotBlockOthers() {
        val inClose = CountDownLatch(1); val release = CountDownLatch(1)
        val f = FakeFactory()
        val m = mgr(f, closing = { if (it == A.deviceName) { inClose.countDown(); release.await(20, TimeUnit.SECONDS) } })
        m.open(A); m.open(B)
        val t = Thread { m.close(A.deviceName) }.also { it.start() }
        assertTrue(inClose.await(10, TimeUnit.SECONDS))
        // A の書き込み確定が終わらない間も、B は使える/閉じられる/開き直せる
        f.sessionsOf(B.deviceName).single().io()
        assertEquals(listOf(B.deviceName), m.openDevices())
        m.close(B.deviceName); m.open(B)
        try { m.open(A); fail() } catch (e: UsbSessionException) { assertEquals(UsbSessionException.Kind.Closing, e.kind) }
        assertFalse("閉じている最中にもう一度閉じても二重にならない", m.close(A.deviceName))
        release.countDown(); t.join(10_000)
        assertFalse(m.isBusy(A.deviceName))
        assertEquals(1, f.sessionsOf(A.deviceName).single().closeCalls.get())
    }

    @Test fun detachWhileOpeningClosesTheNewSession() {
        val f = FakeFactory()
        val inOpen = CountDownLatch(1); val release = CountDownLatch(1)
        f.gate = { d -> if (d.deviceName == A.deviceName) { inOpen.countDown(); release.await(20, TimeUnit.SECONDS) } }
        val closing = CopyOnWriteArrayList<String>()
        val m = mgr(f, closing = { closing += it })
        m.setWriteMode(A.deviceName, WriteMode.Full)
        var result: Throwable? = null
        val t = Thread { try { m.open(A) } catch (e: Throwable) { result = e } }.also { it.start() }
        assertTrue(inOpen.await(10, TimeUnit.SECONDS))
        assertFalse(m.onDetached(A.deviceName))   // まだ開き終わっていない: 開き終えた直後に閉じる
        release.countDown(); t.join(10_000)
        assertEquals(UsbSessionException.Kind.ClosedWhileOpening, (result as UsbSessionException).kind)
        assertTrue(f.sessionsOf(A.deviceName).single().closed)
        assertEquals(listOf(A.deviceName), closing.toList())
        assertFalse(m.isBusy(A.deviceName))
        assertEquals("抜かれたので設定も捨てる", WriteMode.ReadOnly, m.writeMode(A.deviceName))
    }

    // ---- アイドル停止 (スピンダウン) はデバイスごと

    @Test fun idleSpinDownIsPerDevice() {
        val rec = Recorder()
        val f = FakeFactory(); val m = mgr(f); m.addListener(rec)
        m.setIdleSpinDownMinutes(A.deviceName, 5)          // 開く前の設定は、開くときに適用される
        val sa = m.open(A) as FakeUsbSession; val sb = m.open(B) as FakeUsbSession
        assertEquals(5, sa.idleMinutes); assertEquals(-1, sb.idleMinutes)   // B には何も設定されない
        m.setIdleSpinDownMinutes(B.deviceName, 30)
        assertEquals(30, sb.idleMinutes); assertEquals(5, sa.idleMinutes)
        m.setIdleSpinDownMinutes(A.deviceName, 0)
        assertEquals(0, sa.idleMinutes); assertEquals(30, sb.idleMinutes)
        assertEquals(30, m.idleSpinDownMinutes(B.deviceName)); assertEquals(0, m.idleSpinDownMinutes(C.deviceName))
        // 止まった/再開した通知には、どのデバイスかが付く
        sb.idleCallback!!.invoke(true); sa.idleCallback!!.invoke(false)
        assertEquals(listOf("idle:${B.deviceName}:true", "idle:${A.deviceName}:false"), rec.events.filter { it.startsWith("idle:") })
    }

    // ---- 同時 I/O・開閉・抜去の競合

    @Test fun concurrentIoOpenCloseAndDetachAcrossDevicesStaysConsistent() {
        val f = FakeFactory(); val closing = CopyOnWriteArrayList<String>()
        val m = mgr(f, closing = { closing += it })
        val devices = (10..15).map { dev(it) }
        val stop = java.util.concurrent.atomic.AtomicBoolean(false)
        val unexpected = CopyOnWriteArrayList<Throwable>()
        val pool = Executors.newFixedThreadPool(devices.size + 2)
        val workersDone = CountDownLatch(devices.size)
        // デバイスごとのワーカー: 開く -> I/O -> 閉じる/抜去 を繰り返す (抜去された場合の I/O 失敗は想定内)
        devices.forEach { d ->
            pool.execute {
                try {
                    repeat(150) { i ->
                        try {
                            val s = m.open(d) as FakeUsbSession
                            repeat(20) { try { s.io() } catch (_: IllegalStateException) { /* 抜去/閉じ済み */ } }
                            if (i % 2 == 0) m.close(d.deviceName) else m.onDetached(d.deviceName)
                        } catch (e: UsbSessionException) {
                            if (e.kind != UsbSessionException.Kind.Closing && e.kind != UsbSessionException.Kind.AlreadyOpen && e.kind != UsbSessionException.Kind.ClosedWhileOpening) unexpected += e
                        }
                    }
                } catch (t: Throwable) { unexpected += t } finally { workersDone.countDown() }
            }
        }
        // 抜去役: 勝手にデバイスを抜く。読み取り役: 状態の参照
        pool.execute { try { var k = 0; while (!stop.get()) { m.onDetached(devices[k++ % devices.size].deviceName); Thread.yield() } } catch (t: Throwable) { unexpected += t } }
        pool.execute { try { while (!stop.get()) { m.openDevices(); m.openCount(); devices.forEach { m.writeMode(it.deviceName); m.isOpen(it.deviceName) } } } catch (t: Throwable) { unexpected += t } }
        assertTrue("ワーカーが終わらない (デッドロック?)", workersDone.await(90, TimeUnit.SECONDS))
        stop.set(true)
        pool.shutdown(); assertTrue(pool.awaitTermination(60, TimeUnit.SECONDS))
        assertEquals(emptyList<Throwable>(), unexpected.toList())
        m.closeAll()
        assertEquals(0, m.openCount())
        // 開いたセッションは 1 つ残らず、ちょうど 1 回だけ閉じられている (二重 close もリークも無い)
        assertTrue(f.opened.isNotEmpty())
        f.opened.forEach { assertEquals("${it.deviceName} close calls", 1, it.closeCalls.get()) }
        // 後始末は、閉じたセッションと同じ数だけ呼ばれる
        assertEquals(f.opened.size, closing.size)
    }

    // ---- 命名 (公開名の衝突回避)

    @Test fun threeIdenticalModelsGetDistinctNamesFromSerialTail() {
        val devs = listOf(dev(2, "AA00112233"), dev(3, "AA00998877"), dev(4, "AA00445566")).map { it.copy(productName = "Ultra Fit") }
        val ids = UsbDeviceNaming.shortIds(devs)
        assertEquals(listOf("2233", "8877", "5566"), devs.map { ids.getValue(it.deviceName) })
        val labels = devs.map { UsbDeviceNaming.volumeLabel(it.productName, ids.getValue(it.deviceName), 1) }
        assertEquals(3, labels.toSet().size)
        assertEquals("Ultra Fit [2233] #1", labels[0])
        assertEquals("Ultra Fit [2233]", UsbDeviceNaming.displayName("Ultra Fit", "2233"))
        assertEquals("Ultra Fit [2233]", UsbDeviceNaming.volumeLabel("Ultra Fit", "2233", null))
    }

    @Test fun duplicateOrMissingSerialsFallBackToBusNumbers() {
        // 安価な USB メモリは同じ (または全部 0 の) シリアルを返す。重なったものはバス番号-デバイス番号に切り替える
        val devs = listOf(dev(5, "0000000000"), dev(6, "0000000000"), dev(7, null), dev(8, "ABCD1234"), dev(9, "XXXX1234"))
        val ids = UsbDeviceNaming.shortIds(devs)
        assertEquals("1-5", ids[devs[0].deviceName]); assertEquals("1-6", ids[devs[1].deviceName]); assertEquals("1-7", ids[devs[2].deviceName])
        // 下 4 桁が同じ ("1234") 2 本は、両方ともバス番号にする (片方だけ短い識別子だと紛らわしい)
        assertEquals("1-8", ids[devs[3].deviceName]); assertEquals("1-9", ids[devs[4].deviceName])
        assertEquals(devs.size, ids.values.toSet().size)
    }

    @Test fun busIdParsesDeviceNamesAndHandlesOddOnes() {
        assertEquals("1-5", UsbDeviceNaming.busId("/dev/bus/usb/001/005"))
        assertEquals("12-103", UsbDeviceNaming.busId("/dev/bus/usb/012/103"))
        val odd = UsbDeviceNaming.busId("weird-name")
        assertEquals(4, odd.length)
        assertEquals(odd, UsbDeviceNaming.busId("weird-name"))   // 決定的
    }

    @Test fun blankProductNameUsesDefault() {
        assertEquals("USB [1-5]", UsbDeviceNaming.displayName("  ", "1-5"))
        assertEquals("USB [1-5]", UsbDeviceNaming.displayName(null, "1-5"))
    }
}
