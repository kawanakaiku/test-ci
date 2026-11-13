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

/** テスト用の種別。既定の偽セッションは回転ディスク (従来のテストが想定していた、アイドル停止が効くデバイス)。 */
object FakeKinds {
    val HDD = MediaKindInfo(MediaKind.Rotational, 7200, MediaBasis.VpdBlockDeviceChars, probed = true)
    val SSD = MediaKindInfo(MediaKind.NonRotational, 0, MediaBasis.VpdBlockDeviceChars, probed = true)
    val UNKNOWN_STICK = MediaKindInfo(MediaKind.Unknown, 0, MediaBasis.None, MediaHint.RemovableNoVpd, removable = true, probed = true)
}

/** 実機の USB の代わりに使う偽のセッション。close 後に触ると IllegalStateException (本物の UsbStorageSession と同じ)。 */
class FakeUsbSession(
    override val deviceName: String,
    override val writable: Boolean,
    override val vendorId: Int = 0x1234,
    override val productId: Int = 0x5678,
    override val mediaKind: MediaKindInfo = FakeKinds.HDD,
    override val lun: Int = 0,
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
    /** デバイスごとの種別 (既定は回転ディスク) */
    val kinds: MutableMap<String, MediaKindInfo> = java.util.concurrent.ConcurrentHashMap()

    /** 複数 LUN: デバイス名 → 調査結果 (無ければ LUN 1 つ)。scanLuns / releaseDevice の呼び出しも記録する */
    val scans: MutableMap<String, UsbLunScan> = java.util.concurrent.ConcurrentHashMap()
    val scanCalls = CopyOnWriteArrayList<Pair<String, Int>>()   // (deviceName, skipMask)
    val released = CopyOnWriteArrayList<String>()
    val lunRequests = CopyOnWriteArrayList<Pair<String, Int>>()   // (deviceName, lun)

    override fun open(device: UsbDeviceInfo, writable: Boolean): UsbSession = open(device, 0, writable)

    override fun open(device: UsbDeviceInfo, lun: Int, writable: Boolean): UsbSession {
        requests += device.deviceName to writable
        lunRequests += device.deviceName to lun
        gate?.invoke(device)
        if (device.deviceName in failFor || UsbSlotKey.of(device.deviceName, lun) in failFor) throw NativeException(2, "fake open failure")
        return FakeUsbSession(device.deviceName, writable, device.vendorId, device.productId, kinds[device.deviceName] ?: FakeKinds.HDD, lun).also { opened += it }
    }

    override fun scanLuns(device: UsbDeviceInfo, skipMask: Int): UsbLunScan {
        scanCalls += device.deviceName to skipMask
        return scans[device.deviceName] ?: UsbLunScan(1)
    }

    override fun releaseDevice(deviceName: String) { released += deviceName }

    fun sessionsOf(name: String) = opened.filter { it.deviceName == name }
    fun sessionOf(name: String, lun: Int) = opened.filter { it.deviceName == name && it.lun == lun }
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

    // ---- 回転ディスク以外 (フラッシュ/SSD・種別不明) では、ユーザーが明示的に有効にしない限り自動停止しない

    @Test fun idleSpinDownIsIgnoredForNonRotationalAndUnknownDevicesUnlessConfirmed() {
        val f = FakeFactory(); val m = mgr(f)
        f.kinds[A.deviceName] = FakeKinds.HDD; f.kinds[B.deviceName] = FakeKinds.SSD; f.kinds[C.deviceName] = FakeKinds.UNKNOWN_STICK
        for (d in listOf(A, B, C)) m.setIdleSpinDownMinutes(d.deviceName, 10)   // 保存値 (VID:PID 単位) が 10 分だったとする
        val sa = m.open(A) as FakeUsbSession; val sb = m.open(B) as FakeUsbSession; val sc = m.open(C) as FakeUsbSession
        assertEquals(10, sa.idleMinutes)              // 回転ディスクは従来どおり
        assertEquals(-1, sb.idleMinutes)              // SSD/フラッシュ: 監視を始めない (setIdleSpinDown を呼ばない)
        assertEquals(-1, sc.idleMinutes)              // 種別不明: 同じ
        assertTrue(m.spinDownAllowed(A.deviceName)); assertFalse(m.spinDownAllowed(B.deviceName)); assertFalse(m.spinDownAllowed(C.deviceName))
        assertEquals(10, m.effectiveIdleSpinDownMinutes(A.deviceName)); assertEquals(0, m.effectiveIdleSpinDownMinutes(B.deviceName)); assertEquals(0, m.effectiveIdleSpinDownMinutes(C.deviceName))
        assertEquals(10, m.idleSpinDownMinutes(B.deviceName))   // 希望値そのものは残る (表示は effective を使う)
    }

    @Test fun explicitConfirmationEnablesIdleSpinDownOnlyForTheConfirmedKind() {
        val f = FakeFactory(); val m = mgr(f)
        f.kinds[B.deviceName] = FakeKinds.SSD; f.kinds[C.deviceName] = FakeKinds.UNKNOWN_STICK
        m.setSpinDownConfirmed(B.deviceName, MediaKind.NonRotational)
        m.setSpinDownConfirmed(C.deviceName, MediaKind.NonRotational)    // C は種別不明なのに「SSD として」確認されていた (中身が変わった)
        m.setIdleSpinDownMinutes(B.deviceName, 5); m.setIdleSpinDownMinutes(C.deviceName, 5)
        val sb = m.open(B) as FakeUsbSession; val sc = m.open(C) as FakeUsbSession
        assertEquals(5, sb.idleMinutes)               // 確認した種別と一致 -> 有効
        assertEquals(-1, sc.idleMinutes)              // 種別が違う -> 無効に倒す
        assertTrue(m.spinDownAllowed(B.deviceName)); assertFalse(m.spinDownAllowed(C.deviceName))
        // 開いたまま確認すると、直ちに反映される
        m.setSpinDownConfirmed(C.deviceName, MediaKind.Unknown)
        assertEquals(5, sc.idleMinutes); assertTrue(m.spinDownAllowed(C.deviceName))
        // 確認を取り消すと止まる (0 = 監視しない)
        m.setSpinDownConfirmed(B.deviceName, null)
        assertEquals(0, sb.idleMinutes); assertFalse(m.spinDownAllowed(B.deviceName)); assertEquals(0, m.effectiveIdleSpinDownMinutes(B.deviceName))
    }

    @Test fun unlockingNeverTurnsOnAutoStopByItself() {
        // 確認 (unlock) しても、分数を選ぶまでは「しない」のまま
        val f = FakeFactory(); val m = mgr(f)
        f.kinds[B.deviceName] = FakeKinds.SSD
        val sb = m.open(B) as FakeUsbSession
        assertEquals(-1, sb.idleMinutes)
        m.setSpinDownConfirmed(B.deviceName, MediaKind.NonRotational)
        assertEquals(0, sb.idleMinutes)               // 監視は始まらない (0 分)
        assertEquals(0, m.effectiveIdleSpinDownMinutes(B.deviceName))
        m.setIdleSpinDownMinutes(B.deviceName, 30)    // 明示的に選んで初めて有効
        assertEquals(30, sb.idleMinutes)
    }

    @Test fun confirmationAndMinutesAreForgottenOnDetachAndDoNotLeakAcrossDevices() {
        val f = FakeFactory(); val m = mgr(f)
        f.kinds[B.deviceName] = FakeKinds.SSD
        m.setSpinDownConfirmed(B.deviceName, MediaKind.NonRotational); m.setIdleSpinDownMinutes(B.deviceName, 10)
        m.open(B)
        assertEquals(MediaKind.NonRotational, m.spinDownConfirmed(B.deviceName)); assertNull(m.spinDownConfirmed(A.deviceName))
        m.onDetached(B.deviceName)
        assertNull(m.spinDownConfirmed(B.deviceName)); assertEquals(0, m.idleSpinDownMinutes(B.deviceName))
        val again = m.open(B) as FakeUsbSession       // 挿し直すと、確認も分数も引き継がない
        assertEquals(-1, again.idleMinutes)
    }

    @Test fun mediaKindInfoParsesNativeValuesAndFallsBackSafely() {
        val hdd = MediaKindInfo.fromNative(intArrayOf(1, 7200, 1, 0, 0, 1), "rmb=0 ...")
        assertEquals(MediaKind.Rotational, hdd.kind); assertEquals(7200, hdd.rpm); assertEquals(MediaBasis.VpdBlockDeviceChars, hdd.basis); assertTrue(hdd.isRotational); assertTrue(hdd.probed)
        val ssd = MediaKindInfo.fromNative(intArrayOf(2, 123, 3, 0, 0, 1), null)
        assertEquals(MediaKind.NonRotational, ssd.kind); assertEquals(0, ssd.rpm)   // 回転数は回転ディスクだけ
        assertEquals(MediaBasis.AtaPassThrough12, ssd.basis); assertEquals("", ssd.detail); assertFalse(ssd.isRotational)
        val stick = MediaKindInfo.fromNative(intArrayOf(0, 0, 0, 1, 1, 1), "x")
        assertEquals(MediaKind.Unknown, stick.kind); assertEquals(MediaHint.RemovableNoVpd, stick.hint); assertTrue(stick.removable)
        // 範囲外・短い配列は Unknown に倒す (スピンダウンを隠す側)
        val bad = MediaKindInfo.fromNative(intArrayOf(9, 5, 99, 77, 0, 0), "")
        assertEquals(MediaKind.Unknown, bad.kind); assertEquals(MediaBasis.None, bad.basis); assertEquals(MediaHint.None, bad.hint)
        assertEquals(MediaKindInfo.UNKNOWN, MediaKindInfo.fromNative(intArrayOf(1, 7200), "x"))
        assertFalse(MediaKindInfo.UNKNOWN.isRotational)
    }

    @Test fun sessionWithoutMediaKindDefaultsToUnknownSoSpinDownStaysClosed() {
        // mediaKind を実装していないセッション (既定の Unknown) は、確認なしでは自動停止しない
        val m = UsbSessionManager({ d, _ -> object : UsbSession {
            override val deviceName = d.deviceName; override val writable = false; override val vendorId = 1; override val productId = 2; override val tuningIndex = -1
            val inner = FakeUsbSession(d.deviceName, false)
            override fun partitionTable() = inner.partitionTable(); override fun opStats() = ""
            override fun mountNtfs(offsetBytes: Long, sizeBytes: Long, writable: Boolean, createOnly: Boolean, allowNoload: Boolean): NtfsVolume = throw UnsupportedOperationException()
            override fun startStop(start: Boolean) {}; override fun isSpunDown() = false
            override fun setIdleSpinDown(idleMinutes: Int, onStopped: (Boolean) -> Unit) { lastIdle = idleMinutes }
            override fun tune(chunkKiB: Int, maxTransferKiB: Int) {}
            override fun benchReadHash(offset: Long, totalBytes: Long, bufKiB: Int, hash: LongArray) = 1L
            override fun benchRead(offset: Long, totalBytes: Long, bufKiB: Int) = 1L
            override fun <T> withHandle(block: (Long) -> T): T = block(0)
            override fun close() {}
            var lastIdle = -1
        } })
        m.setIdleSpinDownMinutes(A.deviceName, 10)
        val s = m.open(A)
        assertEquals(MediaKind.Unknown, s.mediaKind.kind)             // インタフェースの既定
        assertFalse(m.spinDownAllowed(A.deviceName))
        assertEquals(0, m.effectiveIdleSpinDownMinutes(A.deviceName))
    }

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

    // ---- 複数 LUN (カードリーダーの SD スロット / microSD スロット): スロット = (デバイス名, LUN)

    private fun reader() = UsbDeviceInfo("/dev/bus/usb/001/007", "USB3.0 Card Reader", 0x05e3, 0x0749, "000000001")
    private fun lunInfo(lun: Int, state: UsbLunState, size: Long = 0) = UsbLunInfo(lun, state, "Generic", "STORAGE DEVICE", true, size, if (size > 0) 512 else 0)

    @Test fun slotKeysAreCompatibleWithOldKeysForLun0() {
        val n = "/dev/bus/usb/001/005"
        assertEquals(n, UsbSlotKey.of(n, 0))
        assertEquals("$n/l1", UsbSlotKey.of(n, 1))
        assertEquals(n, UsbSlotKey.deviceOf(n)); assertEquals(0, UsbSlotKey.lunOf(n))
        assertEquals(n, UsbSlotKey.deviceOf("$n/l1")); assertEquals(1, UsbSlotKey.lunOf("$n/l1"))
        assertEquals(n, UsbSlotKey.deviceOf("$n/l15")); assertEquals(15, UsbSlotKey.lunOf("$n/l15"))
        assertEquals("usb:$n@0", VolumeRegistry.usbKey(UsbSlotKey.of(n, 0), 0))          // 従来のボリュームキーのまま
        assertEquals("usb:$n/l1@1048576", VolumeRegistry.usbKey(UsbSlotKey.of(n, 1), 1048576))
        // 表示用
        assertEquals("1-5", UsbDeviceNaming.busId("$n/l1"))
        assertEquals("1-5 LUN 1", UsbDeviceNaming.busLabel("$n/l1")); assertEquals("1-5", UsbDeviceNaming.busLabel(n))
        assertEquals("USB3.0 Card Reader [1-5] · LUN 1 · ${formatBytes(16_000_000_000L)}", UsbDeviceNaming.lunTitle("USB3.0 Card Reader", "1-5", 1, 16_000_000_000L))
        assertEquals("USB3.0 Card Reader [1-5] · LUN 0", UsbDeviceNaming.lunTitle("USB3.0 Card Reader", "1-5", 0))
    }

    @Test fun lunLabelsAreDistinctPerLun() {
        val a = UsbDeviceNaming.volumeLabel("Reader", "1-7", 1, 0); val b = UsbDeviceNaming.volumeLabel("Reader", "1-7", 1, 1)
        assertEquals("Reader [1-7] · LUN 0 #1", a); assertEquals("Reader [1-7] · LUN 1 #1", b)
        assertEquals("Reader [1-7] #1", UsbDeviceNaming.volumeLabel("Reader", "1-7", 1))   // LUN 1 つの機器は従来のまま
    }

    @Test fun parsesLunListJsonFromNative() {
        val json = """[{"lun":0,"state":"no-media","ptype":0,"removable":true,"vendor":"Generic","product":"STORAGE DEVICE","blocks":"0","blockSize":0,"note":"02/3a"},
            {"lun":1,"state":"ready","ptype":0,"removable":true,"vendor":"Generic","product":"STORAGE DEVICE","blocks":"30535680","blockSize":512,"note":""},
            {"lun":2,"state":"skipped","ptype":0,"removable":false,"vendor":"","product":"","blocks":"0","blockSize":0,"note":""},
            {"lun":3,"state":"weird","ptype":0,"removable":false,"vendor":"","product":"","blocks":"x","blockSize":512,"note":"e"}]"""
        val l = UsbLunInfo.parseList(json)
        assertEquals(4, l.size)
        assertEquals(UsbLunState.NoMedia, l[0].state); assertFalse(l[0].openable)
        assertEquals(UsbLunState.Ready, l[1].state); assertTrue(l[1].openable); assertEquals(30535680L * 512, l[1].sizeBytes)
        assertEquals(UsbLunState.Skipped, l[2].state)
        assertEquals(UsbLunState.Error, l[3].state); assertEquals(0L, l[3].sizeBytes)
        val scan = UsbLunScan(4, l)
        assertTrue(scan.multi); assertEquals(listOf(1), scan.ready.map { it.lun })
        assertFalse(UsbLunScan(1).multi)
    }

    @Test fun lunsOfOneDeviceOpenAndCloseIndependently() {
        val f = FakeFactory(); val closing = CopyOnWriteArrayList<String>()
        val m = mgr(f, closing = { closing += it })
        val r = reader()
        val s0 = m.open(r, 0) as FakeUsbSession; val s1 = m.open(r, 1) as FakeUsbSession
        assertEquals(0, s0.lun); assertEquals(1, s1.lun)
        assertEquals(listOf(r.deviceName, r.deviceName + "/l1"), m.openDevices())
        assertEquals(listOf(r.deviceName, r.deviceName + "/l1"), m.slotsOf(r.deviceName))
        assertSame(s1, m.session(r.deviceName + "/l1")); assertSame(s0, m.session(r.deviceName))
        assertEquals(listOf(r.deviceName to 0, r.deviceName to 1), f.lunRequests.toList())
        // LUN 1 を閉じても LUN 0 は使える (後始末も LUN 1 の分だけ)
        assertTrue(m.close(r.deviceName + "/l1"))
        assertEquals(listOf(r.deviceName + "/l1"), closing.toList())
        assertTrue(s1.closed); assertFalse(s0.closed); s0.io()
        assertNull(m.session(r.deviceName + "/l1"))
        assertTrue(m.isDeviceBusy(r.deviceName))
        // 閉じた LUN をもう一度開ける
        val s1b = m.open(r, 1); assertNotSame(s1, s1b)
        assertTrue(m.isOpen(r.deviceName + "/l1"))
    }

    @Test fun sameLunCannotBeOpenedTwiceButOtherLunCan() {
        val m = mgr(); val r = reader()
        m.open(r, 1)
        try { m.open(r, 1); fail() } catch (e: UsbSessionException) { assertEquals(UsbSessionException.Kind.AlreadyOpen, e.kind) }
        m.open(r, 0)   // 別の LUN は開ける
        assertEquals(2, m.openCount())
    }

    @Test fun lunSessionsAreIndependentInIoAndFailure() {
        val f = FakeFactory(); val m = mgr(f); val r = reader()
        f.failFor += r.deviceName    // LUN 0 だけ開けない (スロット名で指定)
        try { m.open(r, 0); fail() } catch (e: NativeException) { }
        assertFalse(m.isBusy(r.deviceName))              // 失敗したスロットは残らない
        f.failFor.clear(); f.failFor += r.deviceName + "/l1"
        val s0 = m.open(r, 0) as FakeUsbSession
        try { m.open(r, 1); fail() } catch (e: NativeException) { }
        s0.io(); assertEquals(1, s0.ioCount.get())
        assertEquals(listOf(r.deviceName), m.openDevices())
    }

    @Test fun writeModeIsPerLun() {
        val f = FakeFactory(); val m = mgr(f); val r = reader()
        assertTrue(m.setWriteMode(r.deviceName + "/l1", WriteMode.Full))
        assertEquals(WriteMode.ReadOnly, m.writeMode(r.deviceName))       // LUN 0 は読み取り専用のまま
        val s0 = m.open(r, 0); val s1 = m.open(r, 1)
        assertFalse(s0.writable); assertTrue(s1.writable)
        assertEquals(WriteMode.Full, m.writeMode(r.deviceName + "/l1"))
        assertFalse("開いている LUN のモードは変えられない", m.setWriteMode(r.deviceName + "/l1", WriteMode.ReadOnly))
        assertTrue("開いていない別の LUN は変えられる", m.setWriteMode(r.deviceName + "/l2", WriteMode.CreateOnly))
        assertEquals(WriteMode.ReadOnly, m.writeMode(r.deviceName + "/l3"))
    }

    @Test fun idleSpinDownAndSpinConfirmationArePerLun() {
        val f = FakeFactory(); val m = mgr(f); val r = reader()
        m.setIdleSpinDownMinutes(r.deviceName + "/l1", 10)
        val s0 = m.open(r, 0) as FakeUsbSession; val s1 = m.open(r, 1) as FakeUsbSession
        assertEquals(-1, s0.idleMinutes)      // LUN 0 には設定していない
        assertEquals(10, s1.idleMinutes)
        assertEquals(0, m.idleSpinDownMinutes(r.deviceName))
    }

    @Test fun detachClosesEveryLunOfThatDeviceOnlyAndReleasesTheLink() {
        val f = FakeFactory(); val closing = CopyOnWriteArrayList<String>(); val rec = Recorder()
        val m = mgr(f, closing = { closing += it }); m.addListener(rec)
        val r = reader(); val other = A
        m.open(r, 0); m.open(r, 1); m.open(other)
        m.setWriteMode(r.deviceName + "/l1", WriteMode.Full)
        assertTrue(m.onDetached(r.deviceName))
        assertEquals(listOf(other.deviceName), m.openDevices())
        assertEquals(setOf(r.deviceName, r.deviceName + "/l1"), closing.toSet())
        assertTrue(f.sessionsOf(r.deviceName).all { it.closed }); assertFalse(f.sessionsOf(other.deviceName).single().closed)
        assertEquals(listOf(r.deviceName), f.released)                  // LUN 一覧のために保っていた接続も手放す
        assertTrue(rec.events.contains("closed:${r.deviceName}/l1:Detached"))
        assertEquals("抜かれた機器の LUN の設定は引き継がない", WriteMode.ReadOnly, m.writeMode(r.deviceName + "/l1"))
    }

    @Test fun detachWithoutOpenSlotsStillReleasesTheScanHold() {
        val f = FakeFactory(); val m = mgr(f); val r = reader()
        f.scans[r.deviceName] = UsbLunScan(2, listOf(lunInfo(0, UsbLunState.NoMedia), lunInfo(1, UsbLunState.Ready, 1L shl 30)))
        m.setWriteMode(r.deviceName + "/l1", WriteMode.CreateOnly)   // 開いていないスロットの設定
        assertFalse(m.onDetached(r.deviceName))
        assertEquals(listOf(r.deviceName), f.released)
        assertEquals(WriteMode.ReadOnly, m.writeMode(r.deviceName + "/l1"))
    }

    @Test fun scanLunsSkipsLunsThatAreAlreadyOpen() {
        val f = FakeFactory(); val m = mgr(f); val r = reader()
        f.scans[r.deviceName] = UsbLunScan(3, listOf(lunInfo(0, UsbLunState.Ready, 1L shl 30), lunInfo(1, UsbLunState.NoMedia), lunInfo(2, UsbLunState.NoMedia)))
        val first = m.scanLuns(r)
        assertTrue(first.multi); assertEquals(3, first.lunCount)
        assertEquals(listOf(r.deviceName to 0), f.scanCalls.toList())    // 何も開いていなければ skip なし
        m.open(r, 0); m.open(r, 2)
        m.scanLuns(r)
        assertEquals(r.deviceName to 0b101, f.scanCalls.last())          // 開いている LUN 0 と 2 は調べない
        // 他のデバイスの LUN は skip に影響しない
        m.open(A)
        m.scanLuns(r)
        assertEquals(r.deviceName to 0b101, f.scanCalls.last())
    }

    @Test fun singleLunDevicesAreUnaffected() {
        val f = FakeFactory(); val m = mgr(f)
        val scan = m.scanLuns(A)
        assertFalse(scan.multi); assertEquals(1, scan.lunCount); assertTrue(scan.luns.isEmpty())
        val s = m.open(A)                                                 // 従来の呼び方 (LUN 0)
        assertEquals(0, s.lun); assertSame(s, m.session(A.deviceName))
        assertEquals(listOf(A.deviceName to 0), f.lunRequests.toList())
    }

    @Test fun releaseDeviceClosesItsLunsAndTheHoldButNotOtherDevices() {
        val f = FakeFactory(); val m = mgr(f); val r = reader()
        m.open(r, 1); m.open(A)
        assertEquals(1, m.releaseDevice(r.deviceName))
        assertEquals(listOf(A.deviceName), m.openDevices())
        assertEquals(listOf(r.deviceName), f.released)
    }

    @Test fun concurrentOpenOfDifferentLunsOfOneDeviceBothSucceed() {
        val f = FakeFactory(); val m = mgr(f); val r = reader()
        val gate = CountDownLatch(2)
        f.gate = { gate.countDown(); gate.await(5, TimeUnit.SECONDS) }   // 2 つの open が同時に factory の中にいる
        val pool = Executors.newFixedThreadPool(2)
        val futs = (0..1).map { l -> pool.submit<UsbSession> { m.open(r, l) } }
        futs.forEach { it.get(10, TimeUnit.SECONDS) }
        pool.shutdown()
        assertEquals(2, m.openCount()); assertEquals(0, gate.count)
    }
}

