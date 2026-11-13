package org.usbmem.app

import android.app.Application
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager

/**
 * プロセス全体で 1 つの [UsbSessionManager] を持つ入口。セッションは Activity ではなくここ (プロセス) に属するので、
 * 画面の再生成 (回転・システムによる Activity の破棄) で失われない。プロセスが終われば全部消える (USB の自動再オープンはしない)。docs/multi-usb.md
 */
object UsbSessions {
    @Volatile private var mgr: UsbSessionManager? = null

    fun manager(ctx: Context): UsbSessionManager = mgr ?: synchronized(this) { mgr ?: create(ctx.applicationContext).also { mgr = it } }

    private fun create(app: Context): UsbSessionManager {
        val usb = app.getSystemService(Context.USB_SERVICE) as UsbManager
        return UsbSessionManager(
            UsbStorageSessionFactory(app, usb),
            writeSupported = BuildConfig.WRITE_SUPPORTED,
            // そのデバイスの公開中ボリューム (ネストした子も) を閉じて、SAF のルートを消す。他のデバイスのルートは残る
            onDeviceClosing = { name -> try { VolumeRegistry.removeByDevice(name) } finally { VolumeRegistry.notifyRoots(app) } },
        )
    }
}

/**
 * USB の抜去を、画面 (Activity) が無くても受けて、抜かれたデバイスのセッションだけを閉じる。
 * Activity が止まっている間 (ファイルアプリで USB の中身を見ている間など) に抜かれても、ボリュームと SAF のルートを後始末する。
 */
class UsbApp : Application() {
    private val detachReceiver = object : BroadcastReceiver() {
        override fun onReceive(c: Context, i: Intent) {
            if (i.action != UsbManager.ACTION_USB_DEVICE_DETACHED) return
            val dev: UsbDevice? =
                if (android.os.Build.VERSION.SDK_INT >= 33) i.getParcelableExtra(UsbManager.EXTRA_DEVICE, UsbDevice::class.java)
                else @Suppress("DEPRECATION") i.getParcelableExtra(UsbManager.EXTRA_DEVICE)
            val name = dev?.deviceName ?: return
            val m = UsbSessions.manager(c)
            val pending = goAsync()
            // 閉じる処理 (書き込みの確定) は遅いことがあるのでメインスレッドでは行わない
            Thread({ try { m.onDetached(name) } finally { pending.finish() } }, "usbmem-detach").start()
        }
    }

    override fun onCreate() {
        super.onCreate()
        val f = IntentFilter(UsbManager.ACTION_USB_DEVICE_DETACHED)
        if (android.os.Build.VERSION.SDK_INT >= 33) registerReceiver(detachReceiver, f, Context.RECEIVER_NOT_EXPORTED) else @Suppress("UnspecifiedRegisterReceiverFlag") registerReceiver(detachReceiver, f)
    }
}
