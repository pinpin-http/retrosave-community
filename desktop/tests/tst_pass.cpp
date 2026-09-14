// ─── Ce que la passe doit faire, et surtout ce qu'elle ne doit JAMAIS faire ──
// La passe est le seul endroit où toutes les briques se rencontrent : le
// carnet, la découverte, l'archive, le réseau et — c'est là que ça devient
// sérieux — les fichiers de sauvegarde du joueur.
//
// Ces tests utilisent un **faux serveur en mémoire**. Ce n'est pas un pis-aller
// en attendant le vrai : un conflit, une archive corrompue ou un plantage au
// milieu d'une écriture se provoquent ici en trois lignes, alors qu'il faudrait
// un banc complet pour les obtenir contre un vrai serveur. Un cas pénible à
// provoquer est un cas qu'on ne teste pas.
//
// Les fichiers, eux, sont RÉELS : chaque test écrit dans un dossier temporaire
// et vérifie ensuite les octets sur le disque. C'est la seule façon de prouver
// qu'une sauvegarde n'a pas été abîmée.
#include "core/archive.h"
#include "core/identity.h"
#include "engine/pass.h"
#include "state/store.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

using namespace retrosave::core;
using namespace retrosave::engine;
using namespace retrosave::state;

namespace
{

// ── Le faux serveur ───────────────────────────────────────────────────────
// Il applique les mêmes règles que le vrai sur les deux points qui comptent :
// le compare-and-swap sur `base_version` (§9.2) et la déduplication par
// identité de contenu. Tout le reste est du rangement en mémoire.
class FakeServer final : public SyncApi
{
  public:
    struct StoredVersion {
        int number = 0;
        QString contentSha256;
        QByteArray archive;
    };
    struct StoredUnit {
        QString serverId;
        QString emulator;
        QString unitKey;
        QString unitType;
        QString state = "active";
        int head = 0;
        std::vector<StoredVersion> versions;
    };

    // Journal des appels reçus : c'est ce qui permet d'affirmer qu'un appel
    // n'a PAS eu lieu — une assertion négative vaut souvent mieux qu'une
    // positive quand on parle de destruction de données.
    QStringList calls;
    QHash<QString, StoredUnit> unitsById;
    QHash<QString, QByteArray> uploads; // objectKey -> octets reçus
    int conflictsOpened = 0;
    QStringList openConflictIds;

    // Injections de panne, toutes désactivées par défaut.
    bool listThrows = false;
    std::function<void()> onDownload;
    QString throwOnPrepareForKey;
    QString corruptDownloadForKey;

    // Publie une version « depuis un autre appareil », sans passer par la
    // passe testée. C'est ainsi qu'on met le cloud en avance.
    int publish(const QString &serverId, std::vector<ContentFile> files)
    {
        auto &unit = unitsById[serverId];
        const auto blob = createArchive(unit.unitType, std::move(files));
        const int number = unit.versions.empty() ? 1 : unit.versions.back().number + 1;
        unit.versions.push_back({number, blob.contentSha256, blob.bytes});
        unit.head = number;
        return number;
    }

    QString declare(const QString &emulator, const QString &unitKey, const QString &unitType)
    {
        const auto serverId = "srv-" + unitKey;
        if (!unitsById.contains(serverId))
            unitsById.insert(serverId,
                             StoredUnit{serverId, emulator, unitKey, unitType, "active", 0, {}});
        return serverId;
    }

    std::vector<RemoteUnit> listUnits() override
    {
        calls << "listUnits";
        if (listThrows)
            throw ApiError("serveur injoignable (simulé)");
        std::vector<RemoteUnit> remote;
        for (const auto &unit : unitsById)
            remote.push_back({unit.serverId, unit.emulator, unit.unitKey, unit.unitType,
                              "jeu:" + unit.unitKey, unit.unitKey, unit.head, unit.state});
        return remote;
    }

    QString declareUnit(const QString &emulator, const QString &unitKey, const QString &unitType,
                        const QString &, const QString &) override
    {
        calls << "declareUnit:" + unitKey;
        return declare(emulator, unitKey, unitType);
    }

    PrepareOutcome prepare(const QString &serverId, int, const QString &contentSha256,
                           const QString &archiveSha256, qint64, qint64) override
    {
        auto &unit = unitsById[serverId];
        calls << "prepare:" + unit.unitKey;
        if (!throwOnPrepareForKey.isEmpty() && unit.unitKey == throwOnPrepareForKey)
            throw ApiError("panne serveur simulée sur cette unité");
        // Le vrai serveur range cette empreinte dès le prepare et la compare au
        // confirm : un faux serveur qui l'ignorerait laisserait passer un
        // client incapable de publier en production.
        if (archiveSha256.size() != 64)
            throw ApiError("empreinte d'archive manquante au prepare");
        // Déduplication : le contenu est déjà la tête, il n'y a rien à
        // téléverser. C'est le cas d'un autre appareil qui a poussé avant nous.
        if (unit.head > 0 && unit.versions.back().contentSha256 == contentSha256)
            return {true, unit.head, {}, {}};
        const auto objectKey = "obj-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
        return {false, 0, "fake://" + objectKey, objectKey};
    }

    void upload(const QString &url, const QString &archivePath) override
    {
        calls << "upload";
        QFile file(archivePath);
        if (!file.open(QIODevice::ReadOnly))
            throw ApiError("archive de téléversement introuvable");
        uploads.insert(url.mid(QStringLiteral("fake://").size()), file.readAll());
    }

    ConfirmOutcome confirm(const QString &serverId, const QString &objectKey,
                           const QString &contentSha256, const QString &, int baseVersion) override
    {
        auto &unit = unitsById[serverId];
        calls << "confirm:" + unit.unitKey;
        const int number = unit.versions.empty() ? 1 : unit.versions.back().number + 1;
        unit.versions.push_back({number, contentSha256, uploads.value(objectKey)});
        // Le compare-and-swap : notre base doit encore être la tête. Sinon le
        // contenu est rangé en branche et un conflit s'ouvre — rien n'est perdu.
        if (baseVersion == unit.head) {
            unit.head = number;
            return {false, number, {}, 0};
        }
        ++conflictsOpened;
        const auto conflictId = "cf-" + unit.unitKey;
        openConflictIds << conflictId;
        return {true, number, conflictId, unit.head};
    }

    DownloadedVersion downloadVersion(const QString &serverId, int number,
                                      const QString &stagingDirectory) override
    {
        auto &unit = unitsById[serverId];
        calls << "download:" + unit.unitKey;
        if (onDownload)
            onDownload();
        for (const auto &version : unit.versions) {
            if (version.number != number)
                continue;
            auto bytes = version.archive;
            if (!corruptDownloadForKey.isEmpty() && unit.unitKey == corruptDownloadForKey) {
                // Un bit retourné côté stockage : les octets ne portent plus le
                // contenu annoncé. On garde l'empreinte ATTENDUE dans la
                // réponse, comme le ferait un vrai serveur qui, lui, ignore que
                // son objet a pourri.
                bytes = createArchive(unit.unitType, {{"SAVE.BIN", QByteArray("contenu pourri")}})
                            .bytes;
            }
            const auto path =
                QDir(stagingDirectory)
                    .filePath("dl-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
            QDir().mkpath(stagingDirectory);
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly))
                throw ApiError("staging illisible");
            file.write(bytes);
            file.close();
            return {path, version.contentSha256};
        }
        throw ApiError("version inconnue");
    }

    void markMissing(const QString &serverId) override
    {
        calls << "markMissing:" + unitsById[serverId].unitKey;
        unitsById[serverId].state = "missing";
    }

    std::vector<OpenConflict> openConflicts() override
    {
        calls << "openConflicts";
        std::vector<OpenConflict> open;
        for (const auto &id : openConflictIds) {
            // Le faux serveur reconstruit les deux côtés comme le vrai : c'est
            // ce que l'interface doit montrer pour qu'on puisse trancher.
            for (const auto &unit : unitsById) {
                if (id != "cf-" + unit.unitKey || unit.versions.size() < 2)
                    continue;
                const auto &head = unit.versions[unit.versions.size() - 2];
                const auto &branch = unit.versions.back();
                open.push_back({id,
                                unit.serverId,
                                unit.unitKey,
                                {head.number, head.contentSha256, 100, "2026-09-08T10:00:00Z",
                                 "Autre appareil"},
                                {branch.number, branch.contentSha256, 120, "2026-09-08T10:05:00Z",
                                 "Ce poste"}});
            }
        }
        return open;
    }

    UnitHistory history(const QString &serverId) override
    {
        calls << "history:" + unitsById[serverId].unitKey;
        const auto &unit = unitsById[serverId];
        UnitHistory result;
        result.headVersion = unit.head;
        for (const auto &version : unit.versions)
            result.versions.push_back({version.number, "normal", version.contentSha256, 100,
                                       "2026-09-08T10:00:00Z", "Un appareil"});
        return result;
    }

    void restore(const QString &serverId, int number) override
    {
        calls << "restore:" + QString::number(number);
        auto &unit = unitsById[serverId];
        for (const auto &version : unit.versions) {
            if (version.number != number)
                continue;
            // Comme le vrai serveur : une NOUVELLE version qui reprend le
            // contenu, jamais une réécriture de l'ancienne.
            const int created = unit.versions.back().number + 1;
            unit.versions.push_back({created, version.contentSha256, version.archive});
            unit.head = created;
            return;
        }
        throw ApiError("version à restaurer inconnue");
    }

    void resolveConflict(const QString &conflictId, int winner) override
    {
        calls << "resolveConflict:" + conflictId;
        for (const auto &unit : unitsById) {
            if (conflictId == "cf-" + unit.unitKey)
                resolve(unit.serverId, winner);
        }
    }

    // L'utilisateur tranche depuis n'importe quel appareil : le conflit se
    // referme et la version gagnante devient la tête.
    void resolve(const QString &serverId, int winner)
    {
        auto &unit = unitsById[serverId];
        openConflictIds.removeAll("cf-" + unit.unitKey);
        for (const auto &version : unit.versions) {
            if (version.number != winner)
                continue;
            const int number = unit.versions.back().number + 1;
            unit.versions.push_back({number, version.contentSha256, version.archive});
            unit.head = number;
            return;
        }
    }
};

} // namespace

class PassTest final : public QObject
{
    Q_OBJECT

  private:
    QTemporaryDir m_workspace;

    QString savesRoot() const { return QDir(m_workspace.path()).filePath("saves"); }
    QString staging() const { return QDir(m_workspace.path()).filePath("staging"); }
    QString dbPath() const { return QDir(m_workspace.path()).filePath("state.db"); }
    QHash<QString, QString> roots() const { return {{"root-1", savesRoot()}}; }

    // Écrit un fichier, en créant son dossier. Rend le chemin absolu.
    QString writeFile(const QString &relative, const QByteArray &content) const
    {
        const auto path = QDir(savesRoot()).filePath(relative);
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly))
            qFatal("fixture non écrite : %s", qPrintable(path));
        file.write(content);
        return path;
    }

    QByteArray readFile(const QString &relative) const
    {
        QFile file(QDir(savesRoot()).filePath(relative));
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }

    // Une unité-dossier PPSSPP, la forme la plus répandue et la plus exposée :
    // c'est elle qui est remplacée en entier lors d'une application.
    static ScannedUnit psp(const QString &key)
    {
        return {"ppsspp", key, "dir", key.left(9), key, "root-1", key};
    }

  private slots:
    void init()
    {
        QVERIFY(m_workspace.isValid());
        QDir().mkpath(savesRoot());
        QDir().mkpath(staging());
    }

    void cleanup()
    {
        QDir(savesRoot()).removeRecursively();
        QDir(staging()).removeRecursively();
        QFile::remove(dbPath());
    }

    // ── Le serveur a oublié une unité que nous croyions synchronisée ──────
    //
    // Arrive pour de vrai : restauration d'une sauvegarde de base plus
    // ancienne, base recréée, ou changement de compte. Le carnet local porte
    // alors un `server_id` qui ne désigne plus rien, et un `last_synced` qui
    // affirme que tout va bien.
    //
    // Sans la remise à zéro, l'appareil ne republie JAMAIS : il croit être à
    // jour, et la copie distante reste absente en silence. C'est le seul cas
    // où le carnet peut mentir sans que rien ne le signale.
    void aUnitTheServerNoLongerKnowsIsPublishedAgain()
    {
        writeFile("ULUS10041DATA/SAVE.BIN", "partie 1");
        Store store(dbPath());
        FakeServer server;
        {
            SyncPass pass(store, server, roots(), staging(), {"folder"});
            pass.run({psp("ULUS10041DATA")}, 1000.0);
            QCOMPARE(pass.run({psp("ULUS10041DATA")}, 1011.0).pushed, 1);
        }
        const auto avant = store.unit("ppsspp", "ULUS10041DATA");
        QVERIFY(avant.has_value());
        QCOMPARE(avant->lastSyncedVersion, 1);
        QVERIFY(!avant->serverId.isEmpty());

        // Le serveur perd tout — exactement ce que produit une base restaurée
        // depuis une sauvegarde antérieure à la première publication.
        server.unitsById.clear();

        SyncPass pass(store, server, roots(), staging(), {"folder"});
        const auto report = pass.run({psp("ULUS10041DATA")}, 2011.0);

        // L'unité est redéclarée et republiée, sans intervention.
        QCOMPARE(report.pushed, 1);
        QCOMPARE(report.errors, 0);
        QCOMPARE(server.unitsById.size(), 1);
        // Et le contenu du joueur n'a pas bougé d'un octet au passage.
        QCOMPARE(readFile("ULUS10041DATA/SAVE.BIN"), QByteArray("partie 1"));
    }

    // ── SYN-06 / SYN-07 : pause et sélection locales ──────────────────────
    //
    // Le point à prouver n'est pas « la passe saute l'unité » mais « elle ne
    // touche à RIEN » : ni au fichier du joueur, ni au serveur. Une pause qui
    // publierait quand même, ou qui effacerait une version distante, serait
    // pire qu'une pause absente.

    void aPausedUnitIsNeitherPublishedNorTouched()
    {
        writeFile("ULUS10041DATA/SAVE.BIN", "partie 1");
        Store store(dbPath());
        FakeServer server;
        {
            SyncPass pass(store, server, roots(), staging(), {"folder"});
            // Deux passes : la sauvegarde est stabilisée et publiée une fois.
            pass.run({psp("ULUS10041DATA")}, 1000.0);
            QCOMPARE(pass.run({psp("ULUS10041DATA")}, 1011.0).pushed, 1);
        }

        const auto unit = store.unit("ppsspp", "ULUS10041DATA");
        QVERIFY(unit.has_value());
        store.setLocalMode(unit->localId, "paused");

        // Le joueur rejoue : le contenu local change.
        writeFile("ULUS10041DATA/SAVE.BIN", "partie 2");
        server.calls.clear();
        SyncPass pass(store, server, roots(), staging(), {"folder"});
        pass.run({psp("ULUS10041DATA")}, 2000.0);
        const auto report = pass.run({psp("ULUS10041DATA")}, 2011.0);

        QCOMPARE(report.pushed, 0);
        QCOMPARE(report.skippedPaused, 1);
        QCOMPARE(report.errors, 0);
        // Aucun appel d'écriture n'a été tenté sur cette unité.
        QVERIFY(!server.calls.contains("confirm:ULUS10041DATA"));
        QVERIFY(!server.calls.contains("prepare:ULUS10041DATA"));
        // Le serveur garde exactement ce qu'il avait.
        QCOMPARE(server.unitsById["srv-ULUS10041DATA"].head, 1);
        // Et le fichier du joueur n'a pas bougé d'un octet.
        QCOMPARE(readFile("ULUS10041DATA/SAVE.BIN"), QByteArray("partie 2"));
    }

    void aPausedUnitDoesNotReceiveARemoteHead()
    {
        writeFile("ULUS10041DATA/SAVE.BIN", "partie 1");
        Store store(dbPath());
        FakeServer server;
        {
            SyncPass pass(store, server, roots(), staging(), {"folder"});
            pass.run({psp("ULUS10041DATA")}, 1000.0);
            pass.run({psp("ULUS10041DATA")}, 1011.0);
        }
        const auto unit = store.unit("ppsspp", "ULUS10041DATA");
        QVERIFY(unit.has_value());
        store.setLocalMode(unit->localId, "paused");

        // Un autre appareil publie une version plus récente.
        server.publish("srv-ULUS10041DATA", {{"SAVE.BIN", QByteArray("venu d ailleurs")}});

        SyncPass pass(store, server, roots(), staging(), {"folder"});
        const auto report = pass.run({psp("ULUS10041DATA")}, 2011.0);

        QCOMPARE(report.pulled, 0);
        QCOMPARE(report.skippedPaused, 1);
        // Le fichier local est intact : une pause ne laisse rien s'appliquer.
        QCOMPARE(readFile("ULUS10041DATA/SAVE.BIN"), QByteArray("partie 1"));
        // Et aucune copie de sécurité n'a été créée, puisque rien n'a été écrit.
        QVERIFY(!QFileInfo::exists(QDir(savesRoot()).filePath("ULUS10041DATA.rsc-bak")));
    }

    void anExcludedUnitIsCountedSeparatelyFromAPause()
    {
        writeFile("ULUS10041DATA/SAVE.BIN", "partie 1");
        Store store(dbPath());
        FakeServer server;
        {
            SyncPass pass(store, server, roots(), staging(), {"folder"});
            pass.run({psp("ULUS10041DATA")}, 1000.0);
            pass.run({psp("ULUS10041DATA")}, 1011.0);
        }
        const auto unit = store.unit("ppsspp", "ULUS10041DATA");
        QVERIFY(unit.has_value());
        store.setLocalMode(unit->localId, "excluded");

        writeFile("ULUS10041DATA/SAVE.BIN", "partie 2");
        SyncPass pass(store, server, roots(), staging(), {"folder"});
        pass.run({psp("ULUS10041DATA")}, 2000.0);
        const auto report = pass.run({psp("ULUS10041DATA")}, 2011.0);

        QCOMPARE(report.pushed, 0);
        QCOMPARE(report.skippedExcluded, 1);
        // La distinction compte : l'interface doit pouvoir dire POURQUOI une
        // sauvegarde ne bouge pas, et « retirée » n'est pas « en pause ».
        QCOMPARE(report.skippedPaused, 0);
    }

    void theGlobalPauseStopsEverythingAndChangesNothing()
    {
        writeFile("ULUS10041DATA/SAVE.BIN", "partie 1");
        writeFile("ULUS10042DATA/SAVE.BIN", "autre partie");
        Store store(dbPath());
        FakeServer server;
        // Pause générale dès la construction : c'est un réglage d'appareil.
        SyncPass pass(store, server, roots(), staging(), {"folder"}, true);

        pass.run({psp("ULUS10041DATA"), psp("ULUS10042DATA")}, 1000.0);
        const auto report = pass.run({psp("ULUS10041DATA"), psp("ULUS10042DATA")}, 1011.0);

        QCOMPARE(report.pushed, 0);
        QCOMPARE(report.skippedPaused, 2);
        QVERIFY(server.unitsById.isEmpty());
        QCOMPARE(readFile("ULUS10041DATA/SAVE.BIN"), QByteArray("partie 1"));
        QCOMPARE(readFile("ULUS10042DATA/SAVE.BIN"), QByteArray("autre partie"));
    }

    void liftingTheGlobalPauseDoesNotReviveAnExcludedUnit()
    {
        writeFile("ULUS10041DATA/SAVE.BIN", "partie 1");
        Store store(dbPath());
        FakeServer server;
        {
            SyncPass pass(store, server, roots(), staging(), {"folder"});
            pass.run({psp("ULUS10041DATA")}, 1000.0);
            pass.run({psp("ULUS10041DATA")}, 1011.0);
        }
        const auto unit = store.unit("ppsspp", "ULUS10041DATA");
        QVERIFY(unit.has_value());
        store.setLocalMode(unit->localId, "excluded");

        writeFile("ULUS10041DATA/SAVE.BIN", "partie 2");
        // Pause générale posée puis levée : l'exclusion doit survivre aux deux.
        {
            SyncPass paused(store, server, roots(), staging(), {"folder"}, true);
            paused.run({psp("ULUS10041DATA")}, 2000.0);
        }
        SyncPass resumed(store, server, roots(), staging(), {"folder"}, false);
        resumed.run({psp("ULUS10041DATA")}, 2011.0);
        const auto report = resumed.run({psp("ULUS10041DATA")}, 2022.0);

        QCOMPARE(report.skippedExcluded, 1);
        QCOMPARE(report.pushed, 0);
        QCOMPARE(server.unitsById["srv-ULUS10041DATA"].head, 1);
    }

    void aPausedUnitThatDisappearsIsNotReportedToTheServer()
    {
        writeFile("ULUS10041DATA/SAVE.BIN", "partie 1");
        Store store(dbPath());
        FakeServer server;
        {
            SyncPass pass(store, server, roots(), staging(), {"folder"});
            pass.run({psp("ULUS10041DATA")}, 1000.0);
            pass.run({psp("ULUS10041DATA")}, 1011.0);
        }
        const auto unit = store.unit("ppsspp", "ULUS10041DATA");
        QVERIFY(unit.has_value());
        store.setLocalMode(unit->localId, "paused");

        // Le dossier disparaît du disque pendant la pause.
        QDir(QDir(savesRoot()).filePath("ULUS10041DATA")).removeRecursively();
        server.calls.clear();
        SyncPass pass(store, server, roots(), staging(), {"folder"});
        const auto report = pass.run({}, 2011.0);

        // Une unité mise de côté ne parle plus au serveur, pas même pour
        // signaler sa disparition.
        QVERIFY(!server.calls.contains("missing:ULUS10041DATA"));
        QCOMPARE(report.missing, 0);
        // Et son état local reste celui que l'utilisateur a laissé.
        QCOMPARE(store.unit("ppsspp", "ULUS10041DATA")->state, QStringLiteral("active"));
    }

    // ── Publication ───────────────────────────────────────────────────────

    void aSettledUnitIsPublishedOnceAndOnlyOnce()
    {
        writeFile("ULUS10041DATA/SAVE.BIN", "partie 1");
        Store store(dbPath());
        FakeServer server;
        SyncPass pass(store, server, roots(), staging(), {"folder"});

        // Première passe : le contenu vient d'être vu, il n'a pas encore prouvé
        // qu'il ne bougeait plus. Rien ne doit partir.
        auto report = pass.run({psp("ULUS10041DATA")}, 1000.0);
        QCOMPARE(report.pushed, 0);
        QVERIFY(!server.calls.contains("confirm:ULUS10041DATA"));

        // Onze secondes plus tard, même empreinte : la capture est autorisée.
        report = pass.run({psp("ULUS10041DATA")}, 1011.0);
        QCOMPARE(report.pushed, 1);
        QCOMPARE(report.errors, 0);
        QCOMPARE(server.unitsById["srv-ULUS10041DATA"].head, 1);

        // Troisième passe, contenu inchangé : rien ne repart. Republier à
        // chaque passe ferait grossir l'historique sans raison.
        server.calls.clear();
        report = pass.run({psp("ULUS10041DATA")}, 1022.0);
        QCOMPARE(report.pushed, 0);
        QVERIFY(!server.calls.contains("upload"));
    }

    void nothingIsCapturedWhileTheEmulatorIsRunning()
    {
        writeFile("ULUS10041DATA/SAVE.BIN", "partie en cours");
        Store store(dbPath());
        FakeServer server;
        SyncPass pass(store, server, roots(), staging(), {"folder"});

        const auto ppssppTourne = [](const QString &) { return true; };
        pass.run({psp("ULUS10041DATA")}, 1000.0, ppssppTourne);
        // Même stabilisée, une unité n'est pas capturée pendant que l'émulateur
        // tourne (banc d'acceptation, cas 19) : il peut encore écrire.
        const auto report = pass.run({psp("ULUS10041DATA")}, 1011.0, ppssppTourne);
        QCOMPARE(report.pushed, 0);
        QVERIFY(!server.calls.contains("upload"));

        // Émulateur fermé : la même passe publie.
        QCOMPARE(pass.run({psp("ULUS10041DATA")}, 1022.0).pushed, 1);
    }

    // ── Réception ─────────────────────────────────────────────────────────

    void remoteApplyWaitsForTheEmulatorToClose()
    {
        writeFile("ULUS10041DATA/SAVE.BIN", "partie 1");
        Store store(dbPath());
        FakeServer server;
        SyncPass pass(store, server, roots(), staging(), {"folder"});
        pass.run({psp("ULUS10041DATA")}, 1000.0);
        QCOMPARE(pass.run({psp("ULUS10041DATA")}, 1011.0).pushed, 1);
        server.publish("srv-ULUS10041DATA", {{"SAVE.BIN", QByteArray("partie 2")}});
        server.calls.clear();
        const auto running = [](const QString &emulator) { return emulator == "ppsspp"; };
        for (int tour = 0; tour < 4; ++tour) {
            const auto report = pass.run({psp("ULUS10041DATA")}, 1022.0 + tour * 11, running);
            QCOMPARE(report.errors, 0);
            QCOMPARE(report.pulled, 0);
            QCOMPARE(readFile("ULUS10041DATA/SAVE.BIN"), QByteArray("partie 1"));
            QCOMPARE(store.unit("ppsspp", "ULUS10041DATA")->lastSyncedVersion, 1);
            QVERIFY(!store.unit("ppsspp", "ULUS10041DATA")->applyJournalVersion);
        }
        QVERIFY(!server.calls.contains("download:ULUS10041DATA"));
        QCOMPARE(pass.run({psp("ULUS10041DATA")}, 1100.0).pulled, 1);
        QCOMPARE(readFile("ULUS10041DATA/SAVE.BIN"), QByteArray("partie 2"));
        QCOMPARE(readFile("ULUS10041DATA.rsc-bak/SAVE.BIN"), QByteArray("partie 1"));
    }

    void emulatorStartingDuringDownloadDefersApply()
    {
        writeFile("ULUS10041DATA/SAVE.BIN", "partie 1");
        Store store(dbPath());
        FakeServer server;
        SyncPass pass(store, server, roots(), staging(), {"folder"});
        pass.run({psp("ULUS10041DATA")}, 1000.0);
        QCOMPARE(pass.run({psp("ULUS10041DATA")}, 1011.0).pushed, 1);
        server.publish("srv-ULUS10041DATA", {{"SAVE.BIN", QByteArray("partie 2")}});
        bool running = false;
        server.onDownload = [&] { running = true; };
        const auto report = pass.run({psp("ULUS10041DATA")}, 1022.0,
                                     [&](const QString &) { return running; });
        QCOMPARE(report.pulled, 0);
        QCOMPARE(report.errors, 0);
        QCOMPARE(readFile("ULUS10041DATA/SAVE.BIN"), QByteArray("partie 1"));
        QVERIFY(!store.unit("ppsspp", "ULUS10041DATA")->applyJournalVersion);
        QVERIFY(!QFileInfo::exists(savesRoot() + "/ULUS10041DATA.rsc-bak"));
        server.onDownload = {};
        QCOMPARE(pass.run({psp("ULUS10041DATA")}, 1033.0).pulled, 1);
        QCOMPARE(readFile("ULUS10041DATA.rsc-bak/SAVE.BIN"), QByteArray("partie 1"));
    }

    void aSaveNeverSeenLocallyIsPlacedAndReceived()
    {
        // Un poste neuf : le dossier surveillé est VIDE, et le serveur porte
        // déjà une sauvegarde publiée par un autre appareil. Sans placement, la
        // passe ne verrait rien à faire — et tout l'intérêt du produit
        // disparaîtrait : « ma partie apparaît sur mon autre PC ».
        Store store(dbPath());
        FakeServer server;
        server.declare("folder", "aventure", "dir");
        server.publish("srv-aventure", {{"SAVE.BIN", QByteArray("partie venue d'ailleurs")}});

        // En production, l'identifiant de racine EST celui de l'émulateur :
        // c'est ce qui permet de savoir où déposer une unité inconnue.
        SyncPass pass(store, server, {{"folder", savesRoot()}}, staging(), {"folder"});
        const auto report = pass.run({}, 1000.0);

        QCOMPARE(report.errors, 0);
        QCOMPARE(report.pulled, 1);
        QCOMPARE(readFile("aventure/SAVE.BIN"), QByteArray("partie venue d'ailleurs"));
        // Et le carnet la connaît désormais comme n'importe quelle autre unité.
        const auto placed = store.unit("folder", "aventure");
        QVERIFY(placed.has_value());
        QCOMPARE(placed->lastSyncedVersion, 1);
        QCOMPARE(placed->rootId, QString("folder"));
    }

    void aSaveIsNeverPlacedInARootThatCannotHostIt()
    {
        // Le placement suppose que le chemin se déduit de la clé d'unité. Ce
        // n'est vrai que pour certaines racines : chez Dolphin, la sauvegarde
        // vit sous `GC/<région>/Card A/`, et la poser à la racine donnerait un
        // fichier qu'aucun émulateur ne lirait. Mieux vaut ne rien placer.
        Store store(dbPath());
        FakeServer server;
        server.declare("dolphin", "carte.gci", "file");
        server.publish("srv-carte.gci", {{"carte.gci", QByteArray("sauvegarde gamecube")}});

        SyncPass pass(store, server, {{"dolphin", savesRoot()}}, staging(), {"folder"});
        const auto report = pass.run({}, 1000.0);

        QCOMPARE(report.pulled, 0);
        QCOMPARE(report.errors, 0);
        QVERIFY(!QFileInfo::exists(QDir(savesRoot()).filePath("carte.gci")));
    }

    void aRemoteVersionReplacesTheLocalUnitAndKeepsABackup()
    {
        writeFile("ULUS10041DATA/SAVE.BIN", "partie 1");
        Store store(dbPath());
        FakeServer server;
        SyncPass pass(store, server, roots(), staging(), {"folder"});
        pass.run({psp("ULUS10041DATA")}, 1000.0);
        QCOMPARE(pass.run({psp("ULUS10041DATA")}, 1011.0).pushed, 1);

        // Un autre appareil publie une suite de la partie.
        server.publish("srv-ULUS10041DATA", {{"SAVE.BIN", QByteArray("partie 2")}});

        const auto report = pass.run({psp("ULUS10041DATA")}, 1022.0);
        QCOMPARE(report.errors, 0);
        QCOMPARE(report.pulled, 1);
        // Le contenu du joueur est bien celui du cloud…
        QCOMPARE(readFile("ULUS10041DATA/SAVE.BIN"), QByteArray("partie 2"));
        // …et l'ancien reste récupérable : rien n'est jamais remplacé sans
        // copie de sécurité (invariant I2).
        QCOMPARE(readFile("ULUS10041DATA.rsc-bak/SAVE.BIN"), QByteArray("partie 1"));
        // La comptabilité suit : sans cela la passe suivante retéléchargerait.
        QCOMPARE(store.unit("ppsspp", "ULUS10041DATA")->lastSyncedVersion, 2);
        QVERIFY(!store.unit("ppsspp", "ULUS10041DATA")->applyJournalVersion.has_value());
    }

    void aMultiFileUnitIsAppliedWholeOrNotAtAll()
    {
        writeFile("ULUS10041DATA/SAVE.BIN", "un");
        writeFile("ULUS10041DATA/00000001/DATA.BIN", "deux");
        Store store(dbPath());
        FakeServer server;
        SyncPass pass(store, server, roots(), staging(), {"folder"});
        pass.run({psp("ULUS10041DATA")}, 1000.0);
        QCOMPARE(pass.run({psp("ULUS10041DATA")}, 1011.0).pushed, 1);

        server.publish("srv-ULUS10041DATA", {{"00000001/DATA.BIN", QByteArray("deux bis")},
                                             {"SAVE.BIN", QByteArray("un bis")}});
        const auto report = pass.run({psp("ULUS10041DATA")}, 1022.0);

        QCOMPARE(report.errors, 0);
        QCOMPARE(report.pulled, 1);
        QCOMPARE(readFile("ULUS10041DATA/SAVE.BIN"), QByteArray("un bis"));
        QCOMPARE(readFile("ULUS10041DATA/00000001/DATA.BIN"), QByteArray("deux bis"));
    }

    // ── Cas 11 du banc : une archive corrompue ne touche à rien ────────────

    void aCorruptedArchiveNeverReachesThePlayersFiles()
    {
        writeFile("ULUS10041DATA/SAVE.BIN", "partie précieuse");
        Store store(dbPath());
        FakeServer server;
        SyncPass pass(store, server, roots(), staging(), {"folder"});
        pass.run({psp("ULUS10041DATA")}, 1000.0);
        QCOMPARE(pass.run({psp("ULUS10041DATA")}, 1011.0).pushed, 1);

        server.publish("srv-ULUS10041DATA", {{"SAVE.BIN", QByteArray("partie 2")}});
        server.corruptDownloadForKey = "ULUS10041DATA";

        const auto report = pass.run({psp("ULUS10041DATA")}, 1022.0);

        QCOMPARE(report.pulled, 0);
        QCOMPARE(report.errors, 1);
        // Le verdict tombe pendant l'étalement, donc AVANT la première écriture.
        QCOMPARE(readFile("ULUS10041DATA/SAVE.BIN"), QByteArray("partie précieuse"));
        // Aucune copie de sécurité : la preuve qu'on n'a même pas commencé.
        QVERIFY(!QFileInfo::exists(QDir(savesRoot()).filePath("ULUS10041DATA.rsc-bak")));
        // Et surtout : aucun marqueur d'écriture ne reste ouvert. Un marqueur
        // resté ouvert bloquerait à jamais la publication des parties suivantes.
        QVERIFY(!store.unit("ppsspp", "ULUS10041DATA")->applyJournalVersion.has_value());

        // La panne était passagère : la passe suivante réussit sans intervention.
        server.corruptDownloadForKey.clear();
        const auto recovered = pass.run({psp("ULUS10041DATA")}, 1033.0);
        QCOMPARE(recovered.pulled, 1);
        QCOMPARE(readFile("ULUS10041DATA/SAVE.BIN"), QByteArray("partie 2"));
    }

    // ── Cas 16 du banc : une écriture interrompue est reprise ──────────────

    void anInterruptedApplyIsRedoneAndNeverPushed()
    {
        writeFile("ULUS10041DATA/SAVE.BIN", "partie 1");
        writeFile("ULUS10041DATA/00000001/DATA.BIN", "annexe 1");
        Store store(dbPath());
        FakeServer server;
        SyncPass pass(store, server, roots(), staging(), {"folder"});
        pass.run({psp("ULUS10041DATA")}, 1000.0);
        QCOMPARE(pass.run({psp("ULUS10041DATA")}, 1011.0).pushed, 1);

        // Le cloud a une version 2, et notre appareil s'est arrêté EN PLEINE
        // application : le dossier est déchiré, le marqueur est resté ouvert.
        const auto head =
            server.publish("srv-ULUS10041DATA", {{"00000001/DATA.BIN", QByteArray("annexe 2")},
                                                 {"SAVE.BIN", QByteArray("partie 2")}});
        const auto expected = server.unitsById["srv-ULUS10041DATA"].versions.back().contentSha256;
        const auto unit = *store.unit("ppsspp", "ULUS10041DATA");
        store.openApplyJournal(unit.localId, head, expected, 900.0);
        QFile::remove(QDir(savesRoot()).filePath("ULUS10041DATA/00000001/DATA.BIN"));

        server.calls.clear();
        const auto report = pass.run({psp("ULUS10041DATA")}, 1022.0);

        // La reprise est constatée, l'unité est ré-appliquée en entier…
        QCOMPARE(report.reapplied, 1);
        QCOMPARE(report.errors, 0);
        QCOMPARE(readFile("ULUS10041DATA/SAVE.BIN"), QByteArray("partie 2"));
        QCOMPARE(readFile("ULUS10041DATA/00000001/DATA.BIN"), QByteArray("annexe 2"));
        // …et surtout le contenu déchiré n'a JAMAIS été publié. C'est le cœur
        // du cas 16 : publier un état à moitié écrit le propagerait partout.
        QVERIFY(!server.calls.contains("upload"));
        QVERIFY(!server.calls.contains("confirm:ULUS10041DATA"));
        QVERIFY(!store.unit("ppsspp", "ULUS10041DATA")->applyJournalVersion.has_value());
    }

    void anApplyThatActuallyFinishedIsOnlyBookkept()
    {
        writeFile("ULUS10041DATA/SAVE.BIN", "partie 2");
        Store store(dbPath());
        FakeServer server;
        server.declare("ppsspp", "ULUS10041DATA", "dir");
        const auto head =
            server.publish("srv-ULUS10041DATA", {{"SAVE.BIN", QByteArray("partie 2")}});
        const auto expected = server.unitsById["srv-ULUS10041DATA"].versions.back().contentSha256;

        SyncPass pass(store, server, roots(), staging(), {"folder"});
        pass.run({psp("ULUS10041DATA")}, 1000.0);
        const auto unit = *store.unit("ppsspp", "ULUS10041DATA");
        // Le processus est mort APRÈS l'écriture, avant d'avoir noté qu'elle
        // avait réussi. Le disque porte déjà le bon contenu.
        store.openApplyJournal(unit.localId, head, expected, 900.0);

        server.calls.clear();
        const auto report = pass.run({psp("ULUS10041DATA")}, 1011.0);

        // Rien à refaire : on referme le marqueur, on ne retéléchargera pas.
        QCOMPARE(report.reapplied, 0);
        QCOMPARE(report.errors, 0);
        QVERIFY(!server.calls.contains("download:ULUS10041DATA"));
        const auto after = *store.unit("ppsspp", "ULUS10041DATA");
        QCOMPARE(after.lastSyncedVersion, head);
        QVERIFY(!after.applyJournalVersion.has_value());
    }

    // ── Conflits et convergence ───────────────────────────────────────────

    void aRejectedPushOpensExactlyOneConflictAndKeepsBothSides()
    {
        writeFile("ULUS10041DATA/SAVE.BIN", "partie 1");
        Store store(dbPath());
        FakeServer server;
        SyncPass pass(store, server, roots(), staging(), {"folder"});
        pass.run({psp("ULUS10041DATA")}, 1000.0);
        QCOMPARE(pass.run({psp("ULUS10041DATA")}, 1011.0).pushed, 1);

        // Pendant que nous étions hors ligne : l'autre appareil publie, et nous
        // jouons de notre côté. Les deux versions existent, aucune n'est fausse.
        server.publish("srv-ULUS10041DATA", {{"SAVE.BIN", QByteArray("partie serveur")}});
        writeFile("ULUS10041DATA/SAVE.BIN", "partie locale");

        pass.run({psp("ULUS10041DATA")}, 1022.0);
        const auto report = pass.run({psp("ULUS10041DATA")}, 1033.0);

        QCOMPARE(report.conflicts, 1);
        QCOMPARE(server.conflictsOpened, 1);
        // Notre contenu local n'est pas écrasé par la réception : c'est le PUSH
        // qui doit trancher, et il l'a fait proprement.
        QCOMPARE(readFile("ULUS10041DATA/SAVE.BIN"), QByteArray("partie locale"));
        // Les deux côtés sont sur le serveur : rien n'est perdu (invariant I3).
        QCOMPARE(server.unitsById["srv-ULUS10041DATA"].versions.size(), size_t(3));
        // Le conflit est identifiable, sinon l'interface ne pourrait pas le
        // présenter à l'utilisateur — et un conflit invisible est un conflit
        // insoluble.
        const auto conflicted = *store.unit("ppsspp", "ULUS10041DATA");
        QVERIFY(conflicted.openConflictId.has_value());
        QCOMPARE(*conflicted.openConflictId, QString("cf-ULUS10041DATA"));
        // La branche est retenue avec son numéro : c'est ce qui permettra de
        // reconnaître notre propre contenu une fois le conflit tranché.
        QVERIFY(conflicted.pendingBranchNumber.has_value());

        // Et l'insistance est interdite : une passe de plus ne doit pas ouvrir
        // un second conflit sur la même unité (banc d'acceptation, cas 13).
        const auto again = pass.run({psp("ULUS10041DATA")}, 1044.0);
        QCOMPARE(again.conflicts, 0);
        QCOMPARE(server.conflictsOpened, 1);
    }

    void aResolvedConflictLetsTheLosingDeviceConverge()
    {
        writeFile("ULUS10041DATA/SAVE.BIN", "partie 1");
        Store store(dbPath());
        FakeServer server;
        SyncPass pass(store, server, roots(), staging(), {"folder"});
        pass.run({psp("ULUS10041DATA")}, 1000.0);
        QCOMPARE(pass.run({psp("ULUS10041DATA")}, 1011.0).pushed, 1);

        server.publish("srv-ULUS10041DATA", {{"SAVE.BIN", QByteArray("partie serveur")}});
        writeFile("ULUS10041DATA/SAVE.BIN", "partie locale");
        pass.run({psp("ULUS10041DATA")}, 1022.0);
        QCOMPARE(pass.run({psp("ULUS10041DATA")}, 1033.0).conflicts, 1);

        // L'utilisateur tranche en faveur de la version du cloud, depuis
        // l'autre appareil ou depuis l'interface.
        server.resolve("srv-ULUS10041DATA", 2);

        const auto report = pass.run({psp("ULUS10041DATA")}, 1044.0);

        // L'appareil perdant adopte la tête résolue. Sans la règle AD-24 il
        // resterait bloqué pour toujours : sa réception refuserait d'écraser un
        // contenu local non poussé, et sa publication resterait interdite.
        QCOMPARE(report.pulled, 1);
        QCOMPARE(report.errors, 0);
        QCOMPARE(readFile("ULUS10041DATA/SAVE.BIN"), QByteArray("partie serveur"));
        // Et la partie perdue reste récupérable des deux côtés : en copie de
        // sécurité locale, et dans l'historique du serveur (invariant I3).
        QCOMPARE(readFile("ULUS10041DATA.rsc-bak/SAVE.BIN"), QByteArray("partie locale"));
        const auto after = *store.unit("ppsspp", "ULUS10041DATA");
        QVERIFY(!after.openConflictId.has_value());
        QVERIFY(!after.pendingBranchNumber.has_value());
        QCOMPARE(after.state, QString("active"));
    }

    void contentAlreadyKnownToTheServerIsNeverUploadedTwice()
    {
        writeFile("ULUS10041DATA/SAVE.BIN", "partie 1");
        Store store(dbPath());
        FakeServer server;
        SyncPass pass(store, server, roots(), staging(), {"folder"});
        pass.run({psp("ULUS10041DATA")}, 1000.0);
        QCOMPARE(pass.run({psp("ULUS10041DATA")}, 1011.0).pushed, 1);

        // Les deux appareils arrivent au MÊME contenu, chacun de son côté :
        // l'autre a publié le premier (banc d'acceptation, cas 14).
        server.publish("srv-ULUS10041DATA", {{"SAVE.BIN", QByteArray("partie 2")}});
        writeFile("ULUS10041DATA/SAVE.BIN", "partie 2");

        pass.run({psp("ULUS10041DATA")}, 1022.0);
        server.calls.clear();
        const auto report = pass.run({psp("ULUS10041DATA")}, 1033.0);

        QCOMPARE(report.duplicates, 1);
        QCOMPARE(report.conflicts, 0);
        // Rien n'est monté : le serveur a reconnu le contenu à son empreinte.
        QVERIFY(!server.calls.contains("upload"));
        QCOMPARE(store.unit("ppsspp", "ULUS10041DATA")->lastSyncedVersion, 2);
    }

    // ── Disparitions ──────────────────────────────────────────────────────

    void aVanishedUnitIsReportedButNeverDeletedRemotely()
    {
        writeFile("ULUS10041DATA/SAVE.BIN", "partie 1");
        Store store(dbPath());
        FakeServer server;
        SyncPass pass(store, server, roots(), staging(), {"folder"});
        pass.run({psp("ULUS10041DATA")}, 1000.0);
        QCOMPARE(pass.run({psp("ULUS10041DATA")}, 1011.0).pushed, 1);

        // Le joueur a déplacé son dossier : l'unité n'est plus vue au scan.
        const auto report = pass.run({}, 1022.0);

        QCOMPARE(report.missing, 1);
        QVERIFY(server.calls.contains("markMissing:ULUS10041DATA"));
        // La version reste téléchargeable : une disparition locale ne supprime
        // jamais rien à distance (PRO-06).
        QCOMPARE(server.unitsById["srv-ULUS10041DATA"].versions.size(), size_t(1));
        QCOMPARE(store.unit("ppsspp", "ULUS10041DATA")->state, QString("missing"));
    }

    void aMassiveDisappearanceStopsEverythingBeforeAnyRemoteCall()
    {
        Store store(dbPath());
        FakeServer server;
        SyncPass pass(store, server, roots(), staging(), {"folder"});
        std::vector<ScannedUnit> units;
        for (int index = 0; index < 12; ++index) {
            const auto key = QStringLiteral("ULUS1004%1DATA").arg(index);
            writeFile(key + "/SAVE.BIN", "partie " + QByteArray::number(index));
            units.push_back(psp(key));
        }
        pass.run(units, 1000.0);
        QCOMPARE(pass.run(units, 1011.0).pushed, 12);

        // Formatage, disque débranché ou rançongiciel : on ne sait pas, et
        // c'est précisément pour cela qu'on ne propage rien.
        server.calls.clear();
        const auto report = pass.run({}, 1022.0);

        QVERIFY(report.pausedForMassiveDisappearance);
        QCOMPARE(report.missing, 12);
        QVERIFY(!server.calls.contains("markMissing:ULUS10040DATA"));
        // La passe s'arrête avant même de demander l'état du serveur : agir sur
        // un disque qu'on ne comprend pas serait la faute suivante.
        QVERIFY(!server.calls.contains("listUnits"));
        for (const auto &unit : store.units())
            QCOMPARE(unit.state, QString("paused"));
    }

    // ── Cas 17 du banc : une unité fautive n'arrête pas la passe ───────────

    void oneFailingUnitNeverStopsTheOthers()
    {
        writeFile("ULUS10041DATA/SAVE.BIN", "partie 1");
        writeFile("ULES00250DATA/SAVE.BIN", "partie 2");
        Store store(dbPath());
        FakeServer server;
        server.throwOnPrepareForKey = "ULUS10041DATA";
        SyncPass pass(store, server, roots(), staging(), {"folder"});

        const std::vector<ScannedUnit> scan{psp("ULUS10041DATA"), psp("ULES00250DATA")};
        pass.run(scan, 1000.0);
        const auto report = pass.run(scan, 1011.0);

        QCOMPARE(report.errors, 1);
        // L'autre unité est passée : c'est toute la différence entre un incident
        // et une panne de synchronisation.
        QCOMPARE(report.pushed, 1);
        QCOMPARE(server.unitsById["srv-ULES00250DATA"].head, 1);
        QCOMPARE(report.messages.size(), 1);
        QVERIFY(report.messages.first().contains("ULUS10041DATA"));
    }

    // AD-30, cas 18 du banc d'acceptation.
    //
    // Le cas qui compte n'est pas l'exception — celle-là est déjà attrapée, et
    // l'unité passe « error » — mais la panne qui TUE le process : dépassement
    // mémoire, crash natif, kill système. Aucun bloc de reprise ne s'exécute
    // alors, l'unité reste « active », et la passe suivante la reprend au même
    // endroit : elle retombe, indéfiniment, et plus rien ne se synchronise.
    //
    // Seule une écriture déjà commise survit à cette mort-là. C'est pourquoi le
    // compteur est écrit AVANT le traitement — et c'est cet état-là que le test
    // reconstitue, puisqu'on ne peut pas faire mourir le process ici.
    void aUnitThatKilledThePassThreeTimesIsSetAside()
    {
        writeFile("ULUS10041DATA/SAVE.BIN", "partie 1");
        writeFile("ULES00250DATA/SAVE.BIN", "partie 2");
        Store store(dbPath());
        FakeServer server;
        SyncPass pass(store, server, roots(), staging(), {"folder"});
        const std::vector<ScannedUnit> scan{psp("ULUS10041DATA"), psp("ULES00250DATA")};
        pass.run(scan, 1000.0);

        // Trois passes mortes sur la même unité, sans tête serveur : c'est
        // exactement ce que trois compteurs commis puis un kill laissent
        // derrière eux.
        const auto poisonedId = store.unit("ppsspp", "ULUS10041DATA")->localId;
        for (int death = 0; death < 3; ++death)
            store.recordFailure(poisonedId, std::nullopt);

        server.calls.clear();
        const auto report = pass.run(scan, 1011.0);

        // L'unité est écartée, et le message dit quoi faire.
        const auto poisoned = store.unit("ppsspp", "ULUS10041DATA");
        QCOMPARE(poisoned->state, QStringLiteral("error"));
        QCOMPARE(report.errors, 1);
        QCOMPARE(report.messages.size(), 1);
        QVERIFY(report.messages.first().contains("repeated failures"));

        // On ne la présente même plus au serveur : le compteur cesse de monter.
        QVERIFY(!server.calls.contains("prepare:ULUS10041DATA"));
        QCOMPARE(poisoned->consecutiveFailures, 3);

        // Et tout le reste continue — c'est ce qui distingue une unité écartée
        // d'une synchronisation en panne.
        QCOMPARE(report.pushed, 1);
        QCOMPARE(server.unitsById["srv-ULES00250DATA"].head, 1);
    }

    // Le réarmement automatique : le poison est presque toujours lié à un
    // contenu précis, donc une nouvelle tête serveur remet l'unité en jeu sans
    // que personne ait à intervenir.
    void aNewServerHeadRearmsAUnitThatWasSetAside()
    {
        writeFile("ULUS10041DATA/SAVE.BIN", "partie 1");
        Store store(dbPath());
        FakeServer server;
        SyncPass pass(store, server, roots(), staging(), {"folder"});
        const std::vector<ScannedUnit> scan{psp("ULUS10041DATA")};
        pass.run(scan, 1000.0);

        const auto poisonedId = store.unit("ppsspp", "ULUS10041DATA")->localId;
        // Les trois morts ont eu lieu alors que le serveur en était à sa
        // version 1.
        const auto serverId = server.declare("ppsspp", "ULUS10041DATA", "dir");
        server.publish(serverId, {{"SAVE.BIN", QByteArray("version 1")}});
        for (int death = 0; death < 3; ++death)
            store.recordFailure(poisonedId, 1);
        QCOMPARE(pass.run(scan, 1011.0).errors, 1);
        QCOMPARE(store.unit("ppsspp", "ULUS10041DATA")->state, QStringLiteral("error"));

        // Un autre appareil publie : la tête change, donc la cause probable a
        // changé aussi.
        server.publish(serverId, {{"SAVE.BIN", QByteArray("depuis l’autre appareil")}});
        const auto report = pass.run(scan, 1022.0);

        QCOMPARE(report.errors, 0);
        const auto healed = store.unit("ppsspp", "ULUS10041DATA");
        QCOMPARE(healed->consecutiveFailures, 0);
        QCOMPARE(readFile("ULUS10041DATA/SAVE.BIN"), QByteArray("depuis l’autre appareil"));
    }

    void anUnreachableServerEndsThePassWithoutTouchingAnything()
    {
        writeFile("ULUS10041DATA/SAVE.BIN", "partie 1");
        Store store(dbPath());
        FakeServer server;
        server.listThrows = true;
        SyncPass pass(store, server, roots(), staging(), {"folder"});

        const auto report = pass.run({psp("ULUS10041DATA")}, 1000.0);

        QCOMPARE(report.errors, 1);
        QCOMPARE(report.pushed, 0);
        QCOMPARE(readFile("ULUS10041DATA/SAVE.BIN"), QByteArray("partie 1"));
    }

    // ── Une unité-fichier (melonDS) suit les mêmes règles ──────────────────

    void aFileUnitRoundTripsLikeADirectory()
    {
        writeFile("zelda.sav", "sauvegarde 1");
        Store store(dbPath());
        FakeServer server;
        SyncPass pass(store, server, roots(), staging(), {"folder"});
        const ScannedUnit sav{"melonds",   "zelda.sav", "file",     "zelda",
                              "zelda.sav", "root-1",    "zelda.sav"};

        pass.run({sav}, 1000.0);
        QCOMPARE(pass.run({sav}, 1011.0).pushed, 1);

        server.publish("srv-zelda.sav", {{"zelda.sav", QByteArray("sauvegarde 2")}});
        const auto report = pass.run({sav}, 1022.0);

        QCOMPARE(report.errors, 0);
        QCOMPARE(report.pulled, 1);
        QCOMPARE(readFile("zelda.sav"), QByteArray("sauvegarde 2"));
        QCOMPARE(readFile("zelda.sav.rsc-bak"), QByteArray("sauvegarde 1"));
    }
};

QTEST_GUILESS_MAIN(PassTest)
#include "tst_pass.moc"
