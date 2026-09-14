package com.retrosave.data.db

import androidx.room.Database
import androidx.room.RoomDatabase
import androidx.room.migration.Migration
import androidx.sqlite.db.SupportSQLiteDatabase
import com.retrosave.data.db.daos.FingerprintDao
import com.retrosave.data.db.daos.JournalDao
import com.retrosave.data.db.daos.RootDao
import com.retrosave.data.db.daos.UnitDao
import com.retrosave.data.db.entities.FileFingerprintEntity
import com.retrosave.data.db.entities.JournalEntity
import com.retrosave.data.db.entities.RootEntity
import com.retrosave.data.db.entities.UnitEntity

@Database(
    entities = [
        UnitEntity::class,
        FileFingerprintEntity::class,
        JournalEntity::class,
        RootEntity::class,
    ],
    version = 4,
    exportSchema = true,
)
abstract class AppDatabase : RoomDatabase() {
    abstract fun units(): UnitDao

    abstract fun fingerprints(): FingerprintDao

    abstract fun journal(): JournalDao

    abstract fun roots(): RootDao

    companion object {
        /**
         * Ajoute les colonnes du journal d'application (AD-27).
         *
         * Strictement additive : une base v1 garde ses unités, ses empreintes
         * et ses racines. Une reconstruction coûterait à l'utilisateur ses
         * grants SAF et son miroir, donc `fallbackToDestructiveMigration` reste
         * interdit ici.
         */
        val MIGRATION_1_2 =
            object : Migration(1, 2) {
                override fun migrate(db: SupportSQLiteDatabase) {
                    db.execSQL("ALTER TABLE units ADD COLUMN apply_journal_version INTEGER")
                    db.execSQL("ALTER TABLE units ADD COLUMN apply_journal_content_sha256 TEXT")
                    db.execSQL("ALTER TABLE units ADD COLUMN apply_journal_started_at INTEGER")
                }
            }

        /**
         * AD-30 : deux colonnes additives pour le compteur d'échecs.
         *
         * Même discipline que la v1 → v2 : additive, testée sur la base réelle
         * de l'appareil avant livraison. `fallbackToDestructiveMigration` reste
         * interdit — une reconstruction coûterait les grants SAF et le miroir.
         */
        val MIGRATION_2_3 =
            object : Migration(2, 3) {
                override fun migrate(db: SupportSQLiteDatabase) {
                    db.execSQL(
                        "ALTER TABLE units ADD COLUMN consecutive_failures INTEGER NOT NULL DEFAULT 0",
                    )
                    db.execSQL("ALTER TABLE units ADD COLUMN last_failure_head INTEGER")
                }
            }

        /**
         * M8 §7 : `label_source` verrouille un libellé saisi à la main.
         *
         * Même discipline additive. Les lignes existantes deviennent `'auto'`,
         * ce qui est la vérité : leurs libellés ont été posés par des
         * adaptateurs, jamais tapés par quelqu'un.
         */
        val MIGRATION_3_4 =
            object : Migration(3, 4) {
                override fun migrate(db: SupportSQLiteDatabase) {
                    db.execSQL(
                        "ALTER TABLE units ADD COLUMN label_source TEXT NOT NULL DEFAULT 'auto'",
                    )
                }
            }
    }
}
