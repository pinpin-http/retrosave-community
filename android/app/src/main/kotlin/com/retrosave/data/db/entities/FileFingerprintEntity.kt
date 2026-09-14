package com.retrosave.data.db.entities

import androidx.room.ColumnInfo
import androidx.room.Entity
import androidx.room.ForeignKey

@Entity(
    tableName = "file_fingerprints",
    primaryKeys = ["unit_local_id", "rel_path"],
    foreignKeys = [
        ForeignKey(
            entity = UnitEntity::class,
            parentColumns = ["local_id"],
            childColumns = ["unit_local_id"],
            onDelete = ForeignKey.CASCADE,
        ),
    ],
)
data class FileFingerprintEntity(
    @ColumnInfo(name = "unit_local_id", index = true)
    val unitLocalId: Long,
    @ColumnInfo(name = "rel_path")
    val relPath: String,
    @ColumnInfo(name = "size_bytes")
    val sizeBytes: Long,
    @ColumnInfo(name = "mtime_ms")
    val mtimeMs: Long?,
    @ColumnInfo(name = "sha256_hex")
    val sha256Hex: String? = null,
)
