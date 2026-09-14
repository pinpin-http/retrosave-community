package com.retrosave.data.db.daos

import androidx.room.Dao
import androidx.room.Insert
import androidx.room.Query
import com.retrosave.data.db.entities.FileFingerprintEntity

@Dao
interface FingerprintDao {
    @Query(
        "SELECT * FROM file_fingerprints WHERE unit_local_id = :localId " +
            "ORDER BY rel_path",
    )
    suspend fun listForUnit(localId: Long): List<FileFingerprintEntity>

    @Query("DELETE FROM file_fingerprints WHERE unit_local_id = :localId")
    suspend fun deleteForUnit(localId: Long)

    @Insert
    suspend fun insertAll(entities: List<FileFingerprintEntity>)
}
