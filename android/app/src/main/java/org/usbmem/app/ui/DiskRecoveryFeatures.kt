// SPDX-License-Identifier: Apache-2.0
package org.usbmem.app.ui

/**
 * パーティション編集の画面の口 ([DiskSlot]) に、TestDisk 相当の 2 つの画面を差し込む。docs/disk-ui.md 5 章・docs/partition-scan.md。
 * `MainActivity.onCreate` が、画面が出る前に呼ぶ (何度呼んでも同じ)。
 */
object DiskRecoveryFeatures {
    fun registerAll() {
        DiskToolsFeatures.register(DiskSlot.LostPartitions) { ctx -> LostPartitionsPanel(ctx) }
        DiskToolsFeatures.register(DiskSlot.BootSector) { ctx -> BootSectorPanel(ctx) }
    }
}
