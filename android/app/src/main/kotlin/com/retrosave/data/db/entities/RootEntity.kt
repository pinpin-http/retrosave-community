package com.retrosave.data.db.entities

import androidx.room.ColumnInfo
import androidx.room.Entity
import androidx.room.PrimaryKey

@Entity(tableName = "roots")
data class RootEntity(
    @PrimaryKey
    @ColumnInfo(name = "root_id")
    val rootId: String,
    val emulator: String,
    @ColumnInfo(name = "tree_uri")
    val treeUri: String,
    val label: String,
    @ColumnInfo(name = "azahar_identity")
    val azaharIdentity: String?,
)
