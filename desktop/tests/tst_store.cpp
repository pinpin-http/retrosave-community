// Le carnet local doit survivre aux redémarrages sans rien oublier ni rien
// écraser. Ces tests portent surtout sur ce qu'un scan n'a PAS le droit de
// faire : effacer une progression déjà synchronisée, ou un nom choisi à la main.
#include "core/decisions.h"
#include "state/store.h"

#include <QDir>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>

using namespace retrosave::state;
using namespace retrosave::core;

class StoreTest final : public QObject
{
    Q_OBJECT
  private:
    QTemporaryDir m_workspace;
    QString path() const { return QDir(m_workspace.path()).filePath("state.db"); }

  private slots:
    void init() { QVERIFY(m_workspace.isValid()); }
    void cleanup() { QFile::remove(path()); }

    // ── M8 §7 : un scan ne fait pas régresser un libellé ──────────────────

    void aScanNeverReplacesAResolvedTitleByTheRawIdentifier()
    {
        Store store(path());
        // Le connecteur Azahar ne sait poser que le titleid : c'est tout ce
        // qu'il lit sans ouvrir une ROM.
        const auto decouverte = store.rememberDiscovered("azahar", "000f3000", "dir",
                                                         "3ds:000f3000", "000f3000", "azahar",
                                                         "000f3000");
        // Le serveur, lui, connaît le vrai titre et le redescend.
        store.adoptDisplayLabel(decouverte.localId, "Professor Layton and the Azran Legacy");

        // Puis un scan ordinaire repasse — c'est ce qui arrive à chaque passe.
        store.rememberDiscovered("azahar", "000f3000", "dir", "3ds:000f3000", "000f3000",
                                 "azahar", "000f3000");

        // Sans la garde, la bibliothèque réaffichait « 000f3000 » à chaque
        // passe, et le nom résolu ne tenait que le temps d'un tour.
        QCOMPARE(store.unit("azahar", "000f3000")->gameLabel,
                 QStringLiteral("Professor Layton and the Azran Legacy"));
    }

    void aScanStillImprovesALabelThatIsStillRaw()
    {
        Store store(path());
        // Le libellé stocké est un préfixe de la clé : il est encore brut, et
        // un connecteur qui apprend à lire le PARAM.SFO doit pouvoir le
        // corriger.
        const auto unit = store.rememberDiscovered("ppsspp", "UCET00357_GameData0", "dir",
                                                   "UCET00357", "UCET00357", "ppsspp",
                                                   "UCET00357_GameData0");
        QCOMPARE(unit.gameLabel, QStringLiteral("UCET00357"));

        store.rememberDiscovered("ppsspp", "UCET00357_GameData0", "dir", "UCET00357", "LocoRoco",
                                 "ppsspp", "UCET00357_GameData0");

        QCOMPARE(store.unit("ppsspp", "UCET00357_GameData0")->gameLabel,
                 QStringLiteral("LocoRoco"));
    }

    void aScanNeverTouchesAManualRename()
    {
        Store store(path());
        const auto unit = store.rememberDiscovered("azahar", "000f3000", "dir", "3ds:000f3000",
                                                   "000f3000", "azahar", "000f3000");
        store.renameForDisplay(unit.localId, "Ma partie de Layton");

        store.rememberDiscovered("azahar", "000f3000", "dir", "3ds:000f3000", "000f3000",
                                 "azahar", "000f3000");

        QCOMPARE(store.unit("azahar", "000f3000")->gameLabel,
                 QStringLiteral("Ma partie de Layton"));
    }

    // ── SYN-06 / SYN-07 : le mode local ───────────────────────────────────

    void aNewUnitSynchronisesUntilSomeoneSaysOtherwise()
    {
        Store store(path());
        const auto unit = store.rememberDiscovered("ppsspp", "ULUS10041SAVE", "dir", "ULUS10041",
                                                   "ULUS10041SAVE", "root-1", "ULUS10041SAVE");
        // Le défaut ne peut pas être « en pause » : une sauvegarde découverte
        // et jamais protégée serait le pire des silences.
        QCOMPARE(unit.localMode, QStringLiteral("sync"));
    }

    void aLocalModeSurvivesAScanAndARestart()
    {
        {
            Store store(path());
            const auto unit = store.rememberDiscovered("ppsspp", "ULUS10041SAVE", "dir",
                                                       "ULUS10041", "ULUS10041SAVE", "root-1",
                                                       "ULUS10041SAVE");
            store.setLocalMode(unit.localId, "paused");
            // Un nouveau scan repasse sur la même unité : il met à jour où elle
            // est, jamais ce que l'utilisateur a demandé.
            store.rememberDiscovered("ppsspp", "ULUS10041SAVE", "dir", "ULUS10041",
                                     "ULUS10041SAVE", "root-1", "ULUS10041SAVE");
            QCOMPARE(store.unit("ppsspp", "ULUS10041SAVE")->localMode, QStringLiteral("paused"));
        }
        Store reopened(path());
        QCOMPARE(reopened.unit("ppsspp", "ULUS10041SAVE")->localMode, QStringLiteral("paused"));
    }

    void anUnknownLocalModeIsRefusedRatherThanStored()
    {
        Store store(path());
        const auto unit = store.rememberDiscovered("ppsspp", "ULUS10041SAVE", "dir", "ULUS10041",
                                                   "ULUS10041SAVE", "root-1", "ULUS10041SAVE");
        // Une valeur arbitraire venue du canal ne doit pas s'installer dans le
        // carnet : elle y serait relue à chaque passe, et le noyau la lirait
        // comme « synchronise » — donc en silence.
        QVERIFY_THROWS_EXCEPTION(StoreError, store.setLocalMode(unit.localId, "suspendu"));
        QCOMPARE(store.unit("ppsspp", "ULUS10041SAVE")->localMode, QStringLiteral("sync"));
    }

    void aCarnetWrittenBeforeThisColumnIsUpgradedInPlace()
    {
        // Un carnet d'une version antérieure : la table existe, la colonne non.
        // `CREATE TABLE IF NOT EXISTS` ne l'ajouterait jamais, et chaque lecture
        // rendrait une valeur vide. On simule exactement ce cas.
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", "ancien-carnet");
            db.setDatabaseName(path());
            QVERIFY(db.open());
            QSqlQuery query(db);
            QVERIFY(query.exec(R"(
CREATE TABLE units (
  local_id INTEGER PRIMARY KEY AUTOINCREMENT,
  server_id TEXT,
  emulator TEXT NOT NULL,
  unit_key TEXT NOT NULL,
  unit_type TEXT NOT NULL,
  game_key TEXT NOT NULL,
  game_label TEXT NOT NULL,
  label_source TEXT NOT NULL DEFAULT 'auto',
  root_id TEXT NOT NULL,
  rel_path TEXT NOT NULL,
  observed_qf_hash TEXT,
  observed_at REAL,
  stable_qf_hash TEXT,
  stable_since REAL,
  last_synced_version INTEGER NOT NULL DEFAULT 0,
  last_synced_content_sha256 TEXT,
  state TEXT NOT NULL DEFAULT 'active',
  open_conflict_id TEXT,
  pending_branch_number INTEGER,
  pending_branch_content_sha256 TEXT,
  apply_journal_version INTEGER,
  apply_journal_content_sha256 TEXT,
  apply_journal_started_at REAL,
  consecutive_failures INTEGER NOT NULL DEFAULT 0,
  last_failure_head INTEGER,
  UNIQUE (emulator, unit_key)
))"));
            QVERIFY(query.exec("INSERT INTO units (emulator, unit_key, unit_type, game_key, "
                               "game_label, root_id, rel_path, last_synced_version) VALUES "
                               "('ppsspp','ULUS10041SAVE','dir','ULUS10041','Ma partie',"
                               "'root-1','ULUS10041SAVE', 7)"));
            db.close();
        }
        QSqlDatabase::removeDatabase("ancien-carnet");

        Store store(path());
        const auto unit = store.unit("ppsspp", "ULUS10041SAVE");
        QVERIFY(unit.has_value());
        // La colonne a été ajoutée, et la reprise n'a rien perdu de ce que le
        // carnet savait déjà.
        QCOMPARE(unit->localMode, QStringLiteral("sync"));
        QCOMPARE(unit->lastSyncedVersion, 7);
        QCOMPARE(unit->gameLabel, QStringLiteral("Ma partie"));
        // Et la colonne est bien utilisable, pas seulement lisible.
        store.setLocalMode(unit->localId, "excluded");
        QCOMPARE(store.unit("ppsspp", "ULUS10041SAVE")->localMode, QStringLiteral("excluded"));
    }

    void aDiscoveredUnitIsRememberedAcrossRestarts()
    {
        {
            Store store(path());
            const auto unit = store.rememberDiscovered("ppsspp", "ULUS10041SAVE", "dir",
                                                       "ULUS10041", "ULUS10041SAVE", "root-1",
                                                       "ULUS10041SAVE");
            QVERIFY(unit.localId > 0);
            QCOMPARE(unit.state, QString("active"));
            QCOMPARE(unit.lastSyncedVersion, 0);
        }
        // Nouvelle instance : le fichier doit suffire à tout retrouver.
        Store reopened(path());
        QCOMPARE(reopened.units().size(), size_t(1));
        QCOMPARE(reopened.units()[0].unitKey, QString("ULUS10041SAVE"));
    }

    void rescanningNeverForgetsWhatSyncAlreadyKnows()
    {
        Store store(path());
        auto unit = store.rememberDiscovered("melonds", "zelda.sav", "file", "zelda", "zelda.sav",
                                             "root-1", "zelda.sav");
        store.recordSynced(unit.localId, 7, QString(64, 'a'));
        store.renameForDisplay(unit.localId, "Zelda — ma partie");

        // Le même scan repasse et redécouvre l'unité au même endroit.
        store.rememberDiscovered("melonds", "zelda.sav", "file", "zelda", "zelda.sav", "root-1",
                                 "zelda.sav");
        const auto after = *store.unit("melonds", "zelda.sav");
        // Ce que la synchronisation savait survit : sans cela, tout le contenu
        // local repartirait comme neuf et fabriquerait un conflit par unité.
        QCOMPARE(after.lastSyncedVersion, 7);
        QCOMPARE(*after.lastSyncedContentSha256, QString(64, 'a'));
        // Et le nom choisi par l'utilisateur n'est pas repris par le scan (M8).
        QCOMPARE(after.gameLabel, QString("Zelda — ma partie"));
        QCOMPARE(after.labelSource, QString("user"));
    }

    void aMovedUnitIsFollowedWithoutLosingItsHistory()
    {
        Store store(path());
        auto unit = store.rememberDiscovered("melonds", "zelda.sav", "file", "zelda", "zelda.sav",
                                             "root-1", "roms/zelda.sav");
        store.recordSynced(unit.localId, 3, QString(64, 'b'));
        // L'utilisateur a rangé sa ROM ailleurs : le chemin change, l'unité non.
        store.rememberDiscovered("melonds", "zelda.sav", "file", "zelda", "zelda.sav", "root-1",
                                 "roms/ds/zelda.sav");
        const auto after = *store.unit("melonds", "zelda.sav");
        QCOMPARE(after.relPath, QString("roms/ds/zelda.sav"));
        QCOMPARE(after.lastSyncedVersion, 3);
    }

    void theStabilityClockOnlyRestartsWhenTheContentMoves()
    {
        Store store(path());
        auto unit = store.rememberDiscovered("ppsspp", "SAVE", "dir", "SAVE", "SAVE", "root-1",
                                             "SAVE");

        // Premier scan : l'unité vient d'être vue, elle n'est pas encore stable.
        store.recordObservation(unit.localId, "empreinte-A", 1000.0);
        auto seen = *store.unit("ppsspp", "SAVE");
        QCOMPARE(*seen.stableSince, 1000.0);

        // Deuxième scan, onze secondes plus tard, contenu inchangé : l'horloge
        // ne repart PAS, sinon le délai ne s'écoulerait jamais.
        store.recordObservation(unit.localId, "empreinte-A", 1011.0);
        seen = *store.unit("ppsspp", "SAVE");
        QCOMPARE(*seen.stableSince, 1000.0);
        QCOMPARE(*seen.observedAt, 1011.0);

        StabilityInput stable;
        stable.state = seen.state;
        stable.observedQfHash = seen.observedQfHash;
        stable.stableQfHash = seen.stableQfHash;
        stable.stableSinceMs = qint64(*seen.stableSince * 1000);
        stable.nowMs = qint64(*seen.observedAt * 1000);
        QVERIFY2(isStable(stable), "l'unité aurait dû être jugée stable après onze secondes");

        // Troisième scan : le joueur a sauvegardé, l'empreinte change. L'horloge
        // repart, et l'unité redevient instable — c'est ce qui empêche de
        // capturer une sauvegarde en cours d'écriture.
        store.recordObservation(unit.localId, "empreinte-B", 1012.0);
        seen = *store.unit("ppsspp", "SAVE");
        QCOMPARE(*seen.stableSince, 1012.0);
        stable.observedQfHash = seen.observedQfHash;
        stable.stableQfHash = seen.stableQfHash;
        stable.stableSinceMs = qint64(*seen.stableSince * 1000);
        stable.nowMs = qint64(*seen.observedAt * 1000);
        QVERIFY2(!isStable(stable), "une unité qui vient de bouger ne doit pas être capturée");
    }

    void theApplyJournalIsWrittenAndReadAsOneWhole()
    {
        Store store(path());
        auto unit = store.rememberDiscovered("ppsspp", "SAVE", "dir", "SAVE", "SAVE", "root-1",
                                             "SAVE");
        // Rien en cours : la passe suivante n'a rien à reprendre.
        auto journal = readApplyJournal(unit.applyJournalVersion, unit.applyJournalContentSha256,
                                        unit.applyJournalStartedAt);
        QCOMPARE(applyJournalDecision(journal, QString(64, 'c')), QString("no_journal"));

        store.openApplyJournal(unit.localId, 5, QString(64, 'c'), 1000.0);
        auto marked = *store.unit("ppsspp", "SAVE");
        journal = readApplyJournal(marked.applyJournalVersion, marked.applyJournalContentSha256,
                                   marked.applyJournalStartedAt);
        // Le contenu sur le disque est celui visé : l'écriture avait abouti,
        // seule la comptabilité manquait. On referme sans rien réécrire.
        QCOMPARE(applyJournalDecision(journal, QString(64, 'c')), QString("close"));
        // Le contenu diffère : l'écriture a été coupée, on la refait.
        QCOMPARE(applyJournalDecision(journal, QString(64, 'd')), QString("reapply"));
        // La cible a disparu : on la refait aussi.
        QCOMPARE(applyJournalDecision(journal, std::nullopt), QString("reapply"));

        store.closeApplyJournal(unit.localId);
        marked = *store.unit("ppsspp", "SAVE");
        QVERIFY(!marked.applyJournalVersion.has_value());
        QVERIFY(!marked.applyJournalContentSha256.has_value());
        QVERIFY(!marked.applyJournalStartedAt.has_value());
    }

    void aCompletedTurnClearsTheQuarantineCounter()
    {
        Store store(path());
        auto unit = store.rememberDiscovered("ppsspp", "SAVE", "dir", "SAVE", "SAVE", "root-1",
                                             "SAVE");
        store.setState(unit.localId, "error");
        // Le compteur est réellement à un : sans cela le test passerait quelle
        // que soit l'implémentation, puisqu'une unité neuve est déjà à zéro.
        store.recordFailure(unit.localId, 4);
        QCOMPARE(store.unit("ppsspp", "SAVE")->consecutiveFailures, 1);

        store.clearFailures(unit.localId);
        const auto after = *store.unit("ppsspp", "SAVE");
        // AD-30 : une unité qui guérit ne doit pas rester en quarantaine.
        QCOMPARE(after.consecutiveFailures, 0);
        QVERIFY(!after.lastFailureHead.has_value());
    }

    // AD-30, le point qui décide si le casse-boucle casse vraiment la boucle.
    //
    // Une réussite PARTIELLE — la réception aboutit, la publication tue ensuite
    // le process — ne doit pas effacer le compteur. Si elle l'efface, la mort
    // laisse un compteur à zéro derrière elle : la passe suivante recommence à
    // l'identique, le seuil n'est jamais atteint, et l'unité empoisonnée bloque
    // la synchronisation indéfiniment. C'est exactement ce qu'AD-30 existe pour
    // empêcher.
    //
    // Seule la FIN du tour remet à zéro, et elle seule.
    void aPartialSuccessNeverClearsTheCounterMidTurn()
    {
        Store store(path());
        auto unit = store.rememberDiscovered("ppsspp", "SAVE", "dir", "SAVE", "SAVE", "root-1",
                                             "SAVE");
        store.recordFailure(unit.localId, 4);
        // La réception aboutit et inscrit sa version : le tour, lui, n'est pas
        // terminé — la publication vient après.
        store.recordSynced(unit.localId, 4, QString(64, 'a'));

        const auto midTurn = *store.unit("ppsspp", "SAVE");
        QCOMPARE(midTurn.lastSyncedVersion, 4);
        QCOMPARE(midTurn.consecutiveFailures, 1);
        QCOMPARE(midTurn.lastFailureHead, std::optional<int>(4));
    }

    // Le compteur doit survivre à la mort du processus : c'est sa seule raison
    // d'être. Une écriture non validée ne prouverait rien, alors on relit la
    // base par une AUTRE connexion, comme le ferait un agent relancé.
    void theCounterSurvivesAFreshProcess()
    {
        qint64 localId = 0;
        {
            Store store(path());
            localId = store.rememberDiscovered("ppsspp", "SAVE", "dir", "SAVE", "SAVE", "root-1",
                                               "SAVE")
                          .localId;
            store.recordFailure(localId, 7);
            store.recordFailure(localId, 7);
        }
        Store reopened(path());
        const auto after = *reopened.unit("ppsspp", "SAVE");
        QCOMPARE(after.localId, localId);
        QCOMPARE(after.consecutiveFailures, 2);
        QCOMPARE(after.lastFailureHead, std::optional<int>(7));
    }

    void theActivityJournalStaysBounded()
    {
        Store store(path());
        for (int i = 0; i < 520; ++i)
            store.appendActivity("passe", QString("entrée %1").arg(i));
        // Un journal qui grossit sans fin finit par gêner plus qu'il n'aide.
        QCOMPARE(store.recentActivity(1000).size(), size_t(500));
        // Les plus récentes sont celles qui restent.
        QVERIFY(store.recentActivity(1).front().contains("entrée 519"));
    }
};

QTEST_GUILESS_MAIN(StoreTest)
#include "tst_store.moc"
