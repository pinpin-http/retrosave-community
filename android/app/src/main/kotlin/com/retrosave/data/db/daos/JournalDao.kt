package com.retrosave.data.db.daos

import androidx.room.Dao
import androidx.room.Insert
import androidx.room.Query
import com.retrosave.data.db.entities.JournalEntity

@Dao
interface JournalDao {
    @Insert
    suspend fun insert(entity: JournalEntity)

    @Query(
        "DELETE FROM sync_journal WHERE id NOT IN " +
            "(SELECT id FROM sync_journal ORDER BY id DESC LIMIT 500)",
    )
    suspend fun trim()

    @Query("SELECT * FROM sync_journal ORDER BY id DESC LIMIT :limit")
    suspend fun recent(limit: Int): List<JournalEntity>
}
