package com.retrosave.data.db.entities

import androidx.room.ColumnInfo
import androidx.room.Entity
import androidx.room.ForeignKey
import androidx.room.PrimaryKey

@Entity(
    tableName = "sync_journal",
    foreignKeys = [
        ForeignKey(
            entity = UnitEntity::class,
            parentColumns = ["local_id"],
            childColumns = ["unit_local_id"],
            onDelete = ForeignKey.SET_NULL,
        ),
    ],
)
data class JournalEntity(
    @PrimaryKey(autoGenerate = true)
    val id: Long = 0,
    val ts: Long,
    @ColumnInfo(name = "unit_local_id", index = true)
    val unitLocalId: Long?,
    val event: String,
    val detail: String,
)
