package com.retrosave.data.db.daos

import androidx.room.Dao
import androidx.room.Delete
import androidx.room.Query
import androidx.room.Upsert
import com.retrosave.data.db.entities.RootEntity
import kotlinx.coroutines.flow.Flow

@Dao
interface RootDao {
    @Query("SELECT * FROM roots ORDER BY emulator, label")
    suspend fun listAll(): List<RootEntity>

    @Query("SELECT * FROM roots ORDER BY emulator, label")
    fun observeAll(): Flow<List<RootEntity>>

    @Query("SELECT * FROM roots WHERE root_id = :rootId LIMIT 1")
    suspend fun find(rootId: String): RootEntity?

    @Query("SELECT * FROM roots WHERE emulator = :emulator ORDER BY label")
    suspend fun listByEmulator(emulator: String): List<RootEntity>

    @Upsert
    suspend fun upsert(entity: RootEntity)

    @Delete
    suspend fun delete(entity: RootEntity)
}
