package com.retrosave.data.db.entities

import androidx.room.ColumnInfo
import androidx.room.Entity
import androidx.room.Index
import androidx.room.PrimaryKey

@Entity(
    tableName = "units",
    indices = [
        Index(
            value = ["emulator", "unit_key"],
            unique = true,
        ),
    ],
)
data class UnitEntity(
    @PrimaryKey(autoGenerate = true)
    @ColumnInfo(name = "local_id")
    val localId: Long = 0,
    @ColumnInfo(name = "server_id")
    val serverId: String? = null,
    val emulator: String,
    @ColumnInfo(name = "unit_key")
    val unitKey: String,
    @ColumnInfo(name = "unit_type")
    val unitType: String,
    @ColumnInfo(name = "game_key")
    val gameKey: String,
    @ColumnInfo(name = "game_label")
    val gameLabel: String,
    // M8 §7 : 'user' verrouille le libellé contre toute résolution automatique.
    @ColumnInfo(name = "label_source", defaultValue = "auto")
    val labelSource: String = "auto",
    @ColumnInfo(name = "root_id")
    val rootId: String,
    @ColumnInfo(name = "rel_path")
    val relPath: String,
    @ColumnInfo(name = "observed_qf_hash")
    val observedQfHash: String? = null,
    @ColumnInfo(name = "observed_at")
    val observedAt: Long? = null,
    @ColumnInfo(name = "stable_qf_hash")
    val stableQfHash: String? = null,
    @ColumnInfo(name = "stable_since")
    val stableSince: Long? = null,
    @ColumnInfo(name = "last_synced_version", defaultValue = "0")
    val lastSyncedVersion: Int = 0,
    @ColumnInfo(name = "last_synced_content_sha256")
    val lastSyncedContentSha256: String? = null,
    @ColumnInfo(defaultValue = "'active'")
    val state: String = "active",
    @ColumnInfo(name = "open_conflict_id")
    val openConflictId: String? = null,
    @ColumnInfo(name = "pending_branch_number")
    val pendingBranchNumber: Int? = null,
    @ColumnInfo(name = "pending_branch_content_sha256")
    val pendingBranchContentSha256: String? = null,
    // AD-27 : posé avant la première écriture d'une application, effacé après
    // la mise à jour de last_synced. Présent au démarrage d'une passe = une
    // application a été interrompue.
    @ColumnInfo(name = "apply_journal_version")
    val applyJournalVersion: Int? = null,
    @ColumnInfo(name = "apply_journal_content_sha256")
    val applyJournalContentSha256: String? = null,
    @ColumnInfo(name = "apply_journal_started_at")
    val applyJournalStartedAt: Long? = null,
    @ColumnInfo(name = "consecutive_failures")
    val consecutiveFailures: Int = 0,
    @ColumnInfo(name = "last_failure_head")
    val lastFailureHead: Int? = null,
)
