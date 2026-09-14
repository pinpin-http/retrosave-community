package com.retrosave.data.db.daos

import androidx.room.Dao
import androidx.room.Insert
import androidx.room.Query
import androidx.room.Update
import com.retrosave.data.db.entities.UnitEntity
import kotlinx.coroutines.flow.Flow

@Dao
interface UnitDao {
    @Query("SELECT * FROM units ORDER BY emulator, unit_key")
    suspend fun listAll(): List<UnitEntity>

    @Query("SELECT * FROM units ORDER BY emulator, unit_key")
    fun observeAll(): Flow<List<UnitEntity>>

    @Query(
        "SELECT * FROM units WHERE emulator = :emulator " +
            "ORDER BY unit_key",
    )
    suspend fun listByEmulator(emulator: String): List<UnitEntity>

    @Query(
        "SELECT * FROM units WHERE emulator = :emulator " +
            "AND unit_key = :unitKey LIMIT 1",
    )
    suspend fun find(
        emulator: String,
        unitKey: String,
    ): UnitEntity?

    @Query("SELECT * FROM units WHERE local_id = :localId LIMIT 1")
    suspend fun findByLocalId(localId: Long): UnitEntity?

    @Query("SELECT * FROM units WHERE local_id = :localId LIMIT 1")
    fun observeByLocalId(localId: Long): Flow<UnitEntity?>

    @Query("SELECT * FROM units WHERE server_id = :serverId LIMIT 1")
    suspend fun findByServerId(serverId: String): UnitEntity?

    @Insert
    suspend fun insert(entity: UnitEntity): Long

    @Update
    suspend fun update(entity: UnitEntity)

    @Query("UPDATE units SET state = 'paused'")
    suspend fun pauseAll()

    @Query("UPDATE units SET state = 'active' WHERE state = 'paused'")
    suspend fun resumeAll()
}
