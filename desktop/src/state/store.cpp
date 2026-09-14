#include "state/store.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>

namespace retrosave::state
{
namespace
{
// Toute évolution de schéma doit préserver les marqueurs de reprise et les
// versions synchronisées ; une migration ne doit jamais les réinitialiser.
constexpr auto Schema = R"(
CREATE TABLE IF NOT EXISTS units (
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
  -- SYN-06 / SYN-07 : « sync », « paused » ou « excluded ». Strictement LOCAL
  -- à cet appareil — jamais envoyé au serveur, jamais déduit de lui. C'est
  -- aussi pourquoi ce n'est pas la colonne `state` : celle-ci décrit ce que la
  -- synchronisation constate (active, missing, error), pas ce que
  -- l'utilisateur a demandé. Les confondre ferait qu'une disparition massive
  -- — qui met `state` à « paused » — passerait pour un choix de l'utilisateur.
  local_mode TEXT NOT NULL DEFAULT 'sync',
  UNIQUE (emulator, unit_key)
);
CREATE TABLE IF NOT EXISTS file_fingerprints (
  unit_local_id INTEGER NOT NULL REFERENCES units(local_id) ON DELETE CASCADE,
  rel_path TEXT NOT NULL,
  size_bytes INTEGER NOT NULL,
  mtime_ms INTEGER,
  sha256_hex TEXT,
  PRIMARY KEY (unit_local_id, rel_path)
);
CREATE TABLE IF NOT EXISTS passes (
  id INTEGER PRIMARY KEY CHECK (id = 1),
  last_success_at REAL,
  last_success_trigger TEXT,
  last_periodic_success_at REAL
);
CREATE TABLE IF NOT EXISTS sync_journal (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  ts REAL NOT NULL,
  unit_local_id INTEGER REFERENCES units(local_id) ON DELETE SET NULL,
  event TEXT NOT NULL,
  detail TEXT NOT NULL
);
)";

constexpr int JournalLimit = 500;

std::optional<QString> optionalText(const QVariant &value)
{
    return value.isNull() ? std::nullopt : std::optional<QString>(value.toString());
}
std::optional<double> optionalReal(const QVariant &value)
{
    return value.isNull() ? std::nullopt : std::optional<double>(value.toDouble());
}
std::optional<int> optionalInt(const QVariant &value)
{
    return value.isNull() ? std::nullopt : std::optional<int>(value.toInt());
}
UnitRecord readRow(const QSqlQuery &query)
{
    UnitRecord unit;
    unit.localId = query.value("local_id").toLongLong();
    unit.serverId = query.value("server_id").toString();
    unit.emulator = query.value("emulator").toString();
    unit.unitKey = query.value("unit_key").toString();
    unit.unitType = query.value("unit_type").toString();
    unit.gameKey = query.value("game_key").toString();
    unit.gameLabel = query.value("game_label").toString();
    unit.labelSource = query.value("label_source").toString();
    unit.rootId = query.value("root_id").toString();
    unit.relPath = query.value("rel_path").toString();
    unit.observedQfHash = optionalText(query.value("observed_qf_hash"));
    unit.observedAt = optionalReal(query.value("observed_at"));
    unit.stableQfHash = optionalText(query.value("stable_qf_hash"));
    unit.stableSince = optionalReal(query.value("stable_since"));
    unit.lastSyncedVersion = query.value("last_synced_version").toInt();
    unit.lastSyncedContentSha256 = optionalText(query.value("last_synced_content_sha256"));
    unit.state = query.value("state").toString();
    unit.openConflictId = optionalText(query.value("open_conflict_id"));
    unit.pendingBranchNumber = optionalInt(query.value("pending_branch_number"));
    unit.pendingBranchContentSha256 = optionalText(query.value("pending_branch_content_sha256"));
    unit.applyJournalVersion = optionalInt(query.value("apply_journal_version"));
    unit.applyJournalContentSha256 = optionalText(query.value("apply_journal_content_sha256"));
    unit.applyJournalStartedAt = optionalReal(query.value("apply_journal_started_at"));
    unit.consecutiveFailures = query.value("consecutive_failures").toInt();
    unit.lastFailureHead = optionalInt(query.value("last_failure_head"));
    // Une base créée par une version antérieure n'a pas la colonne. La lecture
    // rend alors une valeur nulle, et « sync » est le bon repli : ne pas
    // synchroniser par accident serait la seule issue vraiment dommageable.
    const auto mode = query.value("local_mode").toString();
    unit.localMode = mode.isEmpty() ? QStringLiteral("sync") : mode;
    return unit;
}
} // namespace

Store::Store(const QString &databasePath)
{
    if (!QDir().mkpath(QFileInfo(databasePath).absolutePath()))
        throw StoreError("dossier de la base impossible à créer");
    // Un nom de connexion unique par instance : deux Store ouverts en même
    // temps — un test et l'application — ne doivent pas se voler leur base.
    m_connection = "retrosave-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto db = QSqlDatabase::addDatabase("QSQLITE", m_connection);
    db.setDatabaseName(databasePath);
    if (!db.open())
        throw StoreError(("base illisible : " + db.lastError().text()).toStdString());

    QSqlQuery pragma(db);
    // WAL : une lecture n'attend pas une écriture. L'interface peut donc
    // afficher l'état pendant qu'une passe écrit.
    pragma.exec("PRAGMA journal_mode=WAL");
    pragma.exec("PRAGMA foreign_keys=ON");
    // Le schéma contient plusieurs instructions : QSqlQuery n'en exécute qu'une
    // à la fois, on les sépare donc explicitement.
    for (const auto &statement : QString(Schema).split(';', Qt::SkipEmptyParts)) {
        const auto trimmed = statement.trimmed();
        if (trimmed.isEmpty())
            continue;
        QSqlQuery query(db);
        if (!query.exec(trimmed))
            throw StoreError(("schéma refusé : " + query.lastError().text()).toStdString());
    }

    // ─── Reprise d'une base antérieure ────────────────────────────────────
    // `CREATE TABLE IF NOT EXISTS` ne touche pas une table déjà là : une base
    // créée avant SYN-06 n'aurait donc jamais la colonne, et chaque lecture
    // rendrait une valeur vide. On l'ajoute une fois, sans rien réécrire
    // d'autre — un carnet existant garde son `last_synced` intact.
    QSqlQuery columns(db);
    bool hasLocalMode = false;
    if (columns.exec("PRAGMA table_info(units)")) {
        while (columns.next()) {
            if (columns.value(1).toString() == QLatin1String("local_mode")) {
                hasLocalMode = true;
                break;
            }
        }
    }
    if (!hasLocalMode) {
        QSqlQuery alter(db);
        if (!alter.exec("ALTER TABLE units ADD COLUMN local_mode TEXT NOT NULL DEFAULT 'sync'"))
            throw StoreError(
                ("colonne local_mode impossible à ajouter : " + alter.lastError().text())
                    .toStdString());
    }
}

Store::~Store()
{
    // La connexion doit être fermée AVANT d'être retirée, sinon Qt avertit et
    // le fichier reste verrouillé pour le prochain ouvreur.
    {
        auto db = QSqlDatabase::database(m_connection, false);
        if (db.isOpen())
            db.close();
    }
    QSqlDatabase::removeDatabase(m_connection);
}

UnitRecord Store::rememberDiscovered(const QString &emulator, const QString &unitKey,
                                     const QString &unitType, const QString &gameKey,
                                     const QString &gameLabel, const QString &rootId,
                                     const QString &relPath)
{
    auto db = QSqlDatabase::database(m_connection);
    QSqlQuery query(db);
    // ON CONFLICT : on met à jour ce que la découverte sait — où l'unité est —
    // et RIEN d'autre. Le libellé n'est repris que s'il n'a pas été renommé à
    // la main : sinon un scan effacerait le nom choisi par l'utilisateur (M8).
    query.prepare(R"(
INSERT INTO units (emulator, unit_key, unit_type, game_key, game_label, root_id, rel_path)
VALUES (:emulator, :unit_key, :unit_type, :game_key, :game_label, :root_id, :rel_path)
ON CONFLICT (emulator, unit_key) DO UPDATE SET
  unit_type = excluded.unit_type,
  game_key = excluded.game_key,
  root_id = excluded.root_id,
  rel_path = excluded.rel_path,
  game_label = CASE
    -- Un renommage manuel ne bouge jamais (M8 §7).
    WHEN units.label_source = 'user' THEN units.game_label
    -- Le libellé stocké est encore BRUT — il n'est qu'un préfixe de la clé,
    -- comme un titleid ou un serial : le scan a le droit de l'améliorer.
    WHEN instr(units.unit_key, units.game_label) = 1 THEN excluded.game_label
    -- Sinon il a déjà été résolu, par ce client ou par le serveur, et un scan
    -- ne doit pas le faire régresser. Sans cette branche, le titre adopté ne
    -- tenait que le temps d'une passe : le scan suivant le remplaçait par
    -- l'identifiant que le connecteur sait poser.
    ELSE units.game_label END
)");
    query.bindValue(":emulator", emulator);
    query.bindValue(":unit_key", unitKey);
    query.bindValue(":unit_type", unitType);
    query.bindValue(":game_key", gameKey);
    query.bindValue(":game_label", gameLabel);
    query.bindValue(":root_id", rootId);
    query.bindValue(":rel_path", relPath);
    if (!query.exec())
        throw StoreError(("unité non enregistrée : " + query.lastError().text()).toStdString());
    const auto stored = unit(emulator, unitKey);
    if (!stored)
        throw StoreError("unité introuvable juste après son enregistrement");
    return *stored;
}

std::vector<UnitRecord> Store::units() const
{
    auto db = QSqlDatabase::database(m_connection);
    QSqlQuery query(db);
    // Ordre stable : deux exécutions doivent traiter les unités dans le même
    // ordre, sinon un rapport de passe devient impossible à comparer.
    if (!query.exec("SELECT * FROM units ORDER BY emulator, unit_key"))
        throw StoreError(("lecture impossible : " + query.lastError().text()).toStdString());
    std::vector<UnitRecord> all;
    while (query.next())
        all.push_back(readRow(query));
    return all;
}

std::optional<UnitRecord> Store::unit(const QString &emulator, const QString &unitKey) const
{
    auto db = QSqlDatabase::database(m_connection);
    QSqlQuery query(db);
    query.prepare("SELECT * FROM units WHERE emulator = :emulator AND unit_key = :unit_key");
    query.bindValue(":emulator", emulator);
    query.bindValue(":unit_key", unitKey);
    if (!query.exec() || !query.next())
        return std::nullopt;
    return readRow(query);
}

void Store::recordObservation(qint64 localId, const QString &quickFingerprint, double nowSeconds)
{
    auto db = QSqlDatabase::database(m_connection);
    QSqlQuery query(db);
    // La date de stabilité ne repart que si l'empreinte a changé. Sinon le
    // délai des dix secondes ne s'écoulerait jamais et rien ne serait capturé.
    query.prepare(R"(
UPDATE units SET
  observed_qf_hash = :hash,
  observed_at = :now,
  stable_since = CASE WHEN stable_qf_hash IS :hash THEN stable_since ELSE :now END,
  stable_qf_hash = :hash
WHERE local_id = :id
)");
    query.bindValue(":hash", quickFingerprint);
    query.bindValue(":now", nowSeconds);
    query.bindValue(":id", localId);
    if (!query.exec())
        throw StoreError(
            ("observation non enregistrée : " + query.lastError().text()).toStdString());
}

void Store::openApplyJournal(qint64 localId, int version, const QString &contentSha256,
                             double startedAtSeconds)
{
    auto db = QSqlDatabase::database(m_connection);
    QSqlQuery query(db);
    // Les trois colonnes sont écrites d'un seul geste : une ligne partielle
    // serait lue comme un marqueur « malformé », donc ré-appliquée pour rien.
    query.prepare(R"(
UPDATE units SET apply_journal_version = :version,
                 apply_journal_content_sha256 = :sha,
                 apply_journal_started_at = :started
WHERE local_id = :id
)");
    query.bindValue(":version", version);
    query.bindValue(":sha", contentSha256);
    query.bindValue(":started", startedAtSeconds);
    query.bindValue(":id", localId);
    if (!query.exec())
        throw StoreError(("journal non ouvert : " + query.lastError().text()).toStdString());
}

void Store::closeApplyJournal(qint64 localId)
{
    auto db = QSqlDatabase::database(m_connection);
    QSqlQuery query(db);
    query.prepare(R"(
UPDATE units SET apply_journal_version = NULL,
                 apply_journal_content_sha256 = NULL,
                 apply_journal_started_at = NULL
WHERE local_id = :id
)");
    query.bindValue(":id", localId);
    if (!query.exec())
        throw StoreError(("journal non refermé : " + query.lastError().text()).toStdString());
}

void Store::recordSynced(qint64 localId, int version, const QString &contentSha256)
{
    auto db = QSqlDatabase::database(m_connection);
    QSqlQuery query(db);
    // Cette écriture NE touche PAS au compteur d'échecs (AD-30). Elle marque une
    // réussite PARTIELLE — la réception a abouti, la publication vient après —
    // et effacer le compteur ici rendrait le casse-boucle inopérant : une panne
    // qui tue le process juste après laisserait un compteur à zéro, la passe
    // suivante recommencerait à l'identique, et le seuil ne serait jamais
    // atteint. Seule la fin du tour remet à zéro (`clearFailures`).
    query.prepare(R"(
UPDATE units SET last_synced_version = :version,
                 last_synced_content_sha256 = :sha
WHERE local_id = :id
)");
    query.bindValue(":version", version);
    query.bindValue(":sha", contentSha256);
    query.bindValue(":id", localId);
    if (!query.exec())
        throw StoreError(
            ("synchronisation non enregistrée : " + query.lastError().text()).toStdString());
}

void Store::recordFailure(qint64 localId, const std::optional<int> &headVersion)
{
    auto db = QSqlDatabase::database(m_connection);
    QSqlQuery query(db);
    query.prepare(R"(
UPDATE units SET consecutive_failures = consecutive_failures + 1,
                 last_failure_head = :head
WHERE local_id = :id
)");
    query.bindValue(":head",
                    headVersion ? QVariant(*headVersion) : QVariant(QMetaType(QMetaType::Int)));
    query.bindValue(":id", localId);
    if (!query.exec())
        throw StoreError(("tentative non comptée : " + query.lastError().text()).toStdString());
}

void Store::clearFailures(qint64 localId)
{
    auto db = QSqlDatabase::database(m_connection);
    QSqlQuery query(db);
    query.prepare(R"(
UPDATE units SET consecutive_failures = 0, last_failure_head = NULL WHERE local_id = :id
)");
    query.bindValue(":id", localId);
    if (!query.exec())
        throw StoreError(("compteur d'échecs non remis à zéro : " + query.lastError().text())
                             .toStdString());
}

void Store::recordConflictBranch(qint64 localId, const QString &conflictId, int branchNumber,
                                 const QString &contentSha256)
{
    auto db = QSqlDatabase::database(m_connection);
    QSqlQuery query(db);
    query.prepare(R"(
UPDATE units SET open_conflict_id = :conflict,
                 pending_branch_number = :number,
                 pending_branch_content_sha256 = :sha
WHERE local_id = :id
)");
    query.bindValue(":conflict", conflictId);
    query.bindValue(":number", branchNumber);
    query.bindValue(":sha", contentSha256);
    query.bindValue(":id", localId);
    if (!query.exec())
        throw StoreError(("conflit non enregistré : " + query.lastError().text()).toStdString());
}

void Store::clearOpenConflict(qint64 localId)
{
    auto db = QSqlDatabase::database(m_connection);
    QSqlQuery query(db);
    query.prepare("UPDATE units SET open_conflict_id = NULL WHERE local_id = :id");
    query.bindValue(":id", localId);
    if (!query.exec())
        throw StoreError(("conflit non refermé : " + query.lastError().text()).toStdString());
}

void Store::clearPendingBranch(qint64 localId)
{
    auto db = QSqlDatabase::database(m_connection);
    QSqlQuery query(db);
    query.prepare(R"(
UPDATE units SET pending_branch_number = NULL, pending_branch_content_sha256 = NULL
WHERE local_id = :id
)");
    query.bindValue(":id", localId);
    if (!query.exec())
        throw StoreError(("branche non oubliée : " + query.lastError().text()).toStdString());
}

void Store::setState(qint64 localId, const QString &state)
{
    auto db = QSqlDatabase::database(m_connection);
    QSqlQuery query(db);
    query.prepare("UPDATE units SET state = :state WHERE local_id = :id");
    query.bindValue(":state", state);
    query.bindValue(":id", localId);
    if (!query.exec())
        throw StoreError(("état non enregistré : " + query.lastError().text()).toStdString());
}

void Store::setServerId(qint64 localId, const QString &serverId)
{
    auto db = QSqlDatabase::database(m_connection);
    QSqlQuery query(db);
    query.prepare("UPDATE units SET server_id = :server WHERE local_id = :id");
    query.bindValue(":server", serverId);
    query.bindValue(":id", localId);
    if (!query.exec())
        throw StoreError(
            ("identifiant serveur non enregistré : " + query.lastError().text()).toStdString());
}

void Store::forgetRemoteState(qint64 localId)
{
    auto db = QSqlDatabase::database(m_connection);
    QSqlQuery query(db);
    // Le journal d'application n'est PAS effacé : s'il y avait une écriture en
    // cours, elle doit encore être reprise. Seule la comptabilité distante
    // repart de zéro.
    query.prepare(R"(
UPDATE units SET
  server_id = NULL,
  last_synced_version = 0,
  last_synced_content_sha256 = NULL,
  open_conflict_id = NULL,
  pending_branch_number = NULL,
  pending_branch_content_sha256 = NULL
WHERE local_id = :id
)");
    query.bindValue(":id", localId);
    if (!query.exec())
        throw StoreError(("état distant non oublié : " + query.lastError().text()).toStdString());
}

void Store::renameForDisplay(qint64 localId, const QString &label)
{
    auto db = QSqlDatabase::database(m_connection);
    QSqlQuery query(db);
    // `label_source = 'user'` protège ce nom de tous les scans suivants.
    query.prepare(
        "UPDATE units SET game_label = :label, label_source = 'user' WHERE local_id = :id");
    query.bindValue(":label", label);
    query.bindValue(":id", localId);
    if (!query.exec())
        throw StoreError(("renommage refusé : " + query.lastError().text()).toStdString());
}

void Store::adoptDisplayLabel(qint64 localId, const QString &label)
{
    auto db = QSqlDatabase::database(m_connection);
    QSqlQuery query(db);
    // `label_source` reste « auto » : ce nom vient du serveur, pas de
    // l'utilisateur, et un meilleur nom pourra encore le remplacer demain.
    // La garde `label_source = 'auto'` dans le WHERE est une seconde barrière :
    // même appelée par erreur, cette écriture ne peut pas effacer un renommage.
    query.prepare("UPDATE units SET game_label = :label "
                  "WHERE local_id = :id AND label_source = 'auto'");
    query.bindValue(":label", label);
    query.bindValue(":id", localId);
    if (!query.exec())
        throw StoreError(("libellé non adopté : " + query.lastError().text()).toStdString());
}

void Store::setLocalMode(qint64 localId, const QString &mode)
{
    // SYN-06 / SYN-07. Trois valeurs admises et rien d'autre : une chaîne
    // arbitraire venue du canal ne doit pas pouvoir s'installer dans le carnet,
    // où elle serait relue à chaque passe.
    if (mode != QLatin1String("sync") && mode != QLatin1String("paused") &&
        mode != QLatin1String("excluded"))
        throw StoreError("mode local inconnu");
    auto db = QSqlDatabase::database(m_connection);
    QSqlQuery query(db);
    query.prepare("UPDATE units SET local_mode = :mode WHERE local_id = :id");
    query.bindValue(":mode", mode);
    query.bindValue(":id", localId);
    if (!query.exec())
        throw StoreError(("mode local non enregistré : " + query.lastError().text()).toStdString());
}

void Store::appendActivity(const QString &event, const QString &detail,
                           std::optional<qint64> localId)
{
    auto db = QSqlDatabase::database(m_connection);
    QSqlQuery insert(db);
    insert.prepare("INSERT INTO sync_journal (ts, unit_local_id, event, detail) "
                   "VALUES (:ts, :unit, :event, :detail)");
    insert.bindValue(":ts", QDateTime::currentSecsSinceEpoch());
    insert.bindValue(":unit",
                     localId ? QVariant(*localId) : QVariant(QMetaType(QMetaType::LongLong)));
    insert.bindValue(":event", event);
    insert.bindValue(":detail", detail);
    if (!insert.exec())
        throw StoreError(
            ("journal d'activité refusé : " + insert.lastError().text()).toStdString());

    QSqlQuery trim(db);
    trim.prepare("DELETE FROM sync_journal WHERE id <= "
                 "(SELECT MAX(id) - :limit FROM sync_journal)");
    trim.bindValue(":limit", JournalLimit);
    trim.exec();
}

std::vector<QString> Store::recentActivity(int limit) const
{
    auto db = QSqlDatabase::database(m_connection);
    QSqlQuery query(db);
    query.prepare("SELECT event, detail FROM sync_journal ORDER BY id DESC LIMIT :limit");
    query.bindValue(":limit", limit);
    std::vector<QString> lines;
    if (!query.exec())
        return lines;
    while (query.next())
        lines.push_back(query.value("event").toString() + " : " + query.value("detail").toString());
    return lines;
}

} // namespace retrosave::state
