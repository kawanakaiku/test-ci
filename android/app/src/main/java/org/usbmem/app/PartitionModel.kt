package org.usbmem.app

import org.json.JSONObject

data class Partition(
    val index: Int,
    val startLba: Long,
    val sectorCount: Long,
    val startBytes: Long,
    val sizeBytes: Long,
    val logical: Boolean,
    val extendedContainer: Boolean,
    val protective: Boolean,
    val bootable: Boolean,
    val mbrType: Int,
    val typeGuid: String,
    val uniqueGuid: String,
    val name: String,
    val typeName: String,
    val fs: String,
)

data class Diagnostic(val severity: String, val code: String, val message: String)

data class PartitionTable(
    val kind: String,
    val sectorSize: Int,
    val diskBytes: Long,
    val diskGuid: String,
    val usedBackupGpt: Boolean,
    val wholeDeviceVolume: Boolean,
    val partitions: List<Partition>,
    val diagnostics: List<Diagnostic>,
) {
    companion object {
        // 64bit 値は精度保持のため JSON 文字列で来る(ネイティブ側の仕様)。
        fun parse(json: String): PartitionTable {
            val o = JSONObject(json)
            val ps = o.getJSONArray("partitions")
            val ds = o.getJSONArray("diagnostics")
            return PartitionTable(
                kind = o.getString("kind"),
                sectorSize = o.getInt("sectorSize"),
                diskBytes = o.getString("diskBytes").toULong().toLong(),
                diskGuid = o.getString("diskGuid"),
                usedBackupGpt = o.getBoolean("usedBackupGpt"),
                wholeDeviceVolume = o.getBoolean("wholeDeviceVolume"),
                partitions = List(ps.length()) { i ->
                    val p = ps.getJSONObject(i)
                    Partition(
                        index = p.getInt("index"),
                        startLba = p.getString("startLba").toULong().toLong(),
                        sectorCount = p.getString("sectorCount").toULong().toLong(),
                        startBytes = p.getString("startBytes").toULong().toLong(),
                        sizeBytes = p.getString("sizeBytes").toULong().toLong(),
                        logical = p.getBoolean("logical"),
                        extendedContainer = p.getBoolean("extendedContainer"),
                        protective = p.getBoolean("protective"),
                        bootable = p.getBoolean("bootable"),
                        mbrType = p.getInt("mbrType"),
                        typeGuid = p.getString("typeGuid"),
                        uniqueGuid = p.getString("uniqueGuid"),
                        name = p.getString("name"),
                        typeName = p.getString("typeName"),
                        fs = p.getString("fs"),
                    )
                },
                diagnostics = List(ds.length()) { i ->
                    val d = ds.getJSONObject(i)
                    Diagnostic(d.getString("severity"), d.getString("code"), d.getString("message"))
                },
            )
        }
    }
}

fun formatBytes(b: Long): String {
    val units = arrayOf("B", "KiB", "MiB", "GiB", "TiB")
    var v = b.toDouble()
    var i = 0
    while (v >= 1024 && i < units.size - 1) { v /= 1024; i++ }
    return if (i == 0) "$b B" else "%.1f %s".format(v, units[i])
}
