#include "agent/syncservice.h"

#include "adapters/registry.h"
#include "agent/processwatch.h"
#include "core/gameart.h"
#include "engine/folderscan.h"
#include "engine/pass.h"
#include "engine/v0adapter.h"
#include "network/s3transfer.h"
#include "network/v0client.h"
#include "state/store.h"

#include "core/decisions.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopedPointer>
#include <QSet>
#include <QStringList>
#include <QSysInfo>
#include <QUrl>
#include <QUuid>
#include <algorithm>

namespace retrosave::agent
{
namespace
{

// Attend la fin d'un appel réseau depuis le fil de travail. Même forme que
// `engine::await` : l'appel est libéré même si la suite lève.
network::ApiResult awaitCall(network::ApiCall *call)
{
    QScopedPointer<network::ApiCall, QScopedPointerDeleteLater> owned(call);
    network::ApiResult result;
    QEventLoop loop;
    QObject::connect(call, &network::ApiCall::finished, &loop,
                     [&](const network::ApiResult &value) {
                         result = value;
                         loop.quit();
                     });
    loop.exec();
    return result;
}

// AD-31 : comparer deux versions SANS leur métadonnée de build.
//
// La CI exige que toute image publiée porte `<version>+<sha>`, quand un client
// installé depuis le dépôt n'a pas de suffixe. Comparer les chaînes entières
// déclencherait donc l'avertissement chez tout le monde, en permanence — et un
// avertissement permanent n'avertit plus de rien. Semver ignore déjà cette
// métadonnée pour la préséance ; on fait pareil.
QString compareVersions(const QString &client, const QString &server)
{
    if (server.isEmpty())
        return QStringLiteral("inconnue");
    const auto strip = [](const QString &value) { return value.section('+', 0, 0); };
    return strip(client) == strip(server) ? QStringLiteral("compatible")
                                          : QStringLiteral("ecart");
}

// Copie un fichier ou un dossier entier vers `target` (PRO-08).
//
// Ne suit jamais un lien symbolique : le suivre copierait quelque chose hors du
// périmètre que l'utilisateur a désigné. N'écrase jamais : l'appelant a déjà
// vérifié que la destination est vide.
bool copyTree(const QString &source, const QString &target)
{
    const QFileInfo info(source);
    if (info.isSymLink())
        return false;
    if (info.isFile()) {
        if (!QDir().mkpath(QFileInfo(target).absolutePath()))
            return false;
        return QFile::copy(source, target);
    }
    if (!info.isDir())
        return false;
    if (!QDir().mkpath(target))
        return false;
    const QDir directory(source);
    for (const auto &entry :
         directory.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (!copyTree(entry.absoluteFilePath(), QDir(target).filePath(entry.fileName())))
            return false;
    }
    return true;
}

// L'ouvrier : tout ce qui touche au disque, au réseau et au carnet vit ici,
// donc dans le fil de travail. Il ne garde AUCUN état entre deux passes — le
// carnet SQLite est le seul état durable, et le reconstruire à chaque passe
// évite qu'une configuration changée en cours de route soit à moitié appliquée.
class SyncWorker final : public QObject
{
    Q_OBJECT
  public:
    static network::ApiConfig apiConfig(const Configuration &configuration)
    {
        network::ApiConfig config;
        config.serverUrl = QUrl(configuration.serverUrl);
        config.token = configuration.token.toUtf8();
        config.deviceId = configuration.deviceId.toUtf8();
        config.timeoutMs = 30000;
        // HTTP n'est admis que lorsqu'il est configuré explicitement pour un
        // environnement local ; une erreur TLS ne provoque aucun repli.
        config.allowHttp = QUrl(configuration.serverUrl).scheme() == "http";
        return config;
    }

    // Le catalogue des connecteurs, tel que l'interface le montre : ce que
    // RetroSave sait lire, et où il regarde.
    static QJsonArray catalogue(const Configuration &configuration)
    {
        adapters::AdapterRegistry registry;
        registry.loadDirectory(adaptersPath());
        QJsonArray list;
        for (const auto &id : registry.ids()) {
            const auto *adapter = registry.adapter(id);
            if (adapter == nullptr)
                continue;
            const auto root = configuration.roots.value(id);
            list.append(QJsonObject{{"id", id},
                                    {"name", adapter->manifest().name},
                                    {"root", root},
                                    // Une racine configurée mais absente du
                                    // disque doit se voir : un disque externe
                                    // débranché ne doit pas passer pour une
                                    // synchronisation qui marche.
                                    {"present", !root.isEmpty() && QFileInfo(root).isDir()},
                                    {"unit_type", adapter->manifest().discovery.unitType}});
        }
        return list;
    }

    // Les racines capables d'accueillir une sauvegarde **jamais vue ici**.
    //
    // La condition est que le chemin se déduise de la clé d'unité, donc que le
    // motif de découverte ne descende pas dans des sous-dossiers : `*` chez
    // PPSSPP, oui ; `GC/**/*.gci` chez Dolphin, non — une carte posée à la
    // racine ne serait lue par personne. Le dossier générique en fait partie
    // par construction.
    static QSet<QString> placeableRoots(const Configuration &configuration)
    {
        QSet<QString> placeable;
        if (!configuration.root.isEmpty())
            placeable.insert("folder");
        if (configuration.roots.isEmpty())
            return placeable;
        adapters::AdapterRegistry registry;
        registry.loadDirectory(adaptersPath());
        for (auto it = configuration.roots.constBegin(); it != configuration.roots.constEnd();
             ++it) {
            const auto *adapter = registry.adapter(it.key());
            if (adapter != nullptr && !adapter->manifest().discovery.pattern.contains('/'))
                placeable.insert(it.key());
        }
        return placeable;
    }

    // Toutes les racines configurées, chacune scannée avec SON connecteur. Le
    // dossier générique reste possible en parallèle : c'est celui qui n'exige
    // aucun émulateur connu.
    static std::vector<engine::ScannedUnit> scanAll(const Configuration &configuration,
                                                    QHash<QString, QString> &roots)
    {
        std::vector<engine::ScannedUnit> scanned;
        if (!configuration.root.isEmpty()) {
            roots.insert("folder", configuration.root);
            for (const auto &unit : engine::scanFolderRoot(configuration.root))
                scanned.push_back(unit);
        }
        if (configuration.roots.isEmpty())
            return scanned;

        adapters::AdapterRegistry registry;
        registry.loadDirectory(adaptersPath());
        for (auto it = configuration.roots.constBegin(); it != configuration.roots.constEnd();
             ++it) {
            const auto *adapter = registry.adapter(it.key());
            // Un identifiant que le registre ne connaît pas est ignoré, pas
            // fatal : un réglage laissé par une version antérieure ne doit pas
            // empêcher les autres émulateurs de se synchroniser.
            if (adapter == nullptr)
                continue;
            roots.insert(it.key(), it.value());
            for (const auto &found : adapter->discover(it.value())) {
                // Une unité que le connecteur signale comme problématique n'est
                // pas synchronisée : mieux vaut l'ignorer que publier ce qu'on
                // n'a pas su lire.
                if (!found.problem.isEmpty())
                    continue;
                scanned.push_back({found.emulator, found.unitKey, found.unitType, found.gameKey,
                                   found.gameLabel, it.key(), found.relPath});
            }
        }
        return scanned;
    }

    // Deux sources, dans cet ordre : le choix de l'utilisateur, puis l'image
    // en cache — soit la jaquette téléchargée depuis l'adresse résolue par le
    // serveur (Q45), soit l'`ICON0.PNG` extraite du dossier PSP que
    // l'utilisateur nous a confié. Aucune ROM n'est jamais ouverte (I1).
    //
    // Ne lève jamais : une icône absente, illisible ou trop grosse laisse la
    // place au dessin de repli, et la liste des unités s'affiche quand même.
    static QString cacheIcon(const Configuration &configuration, const state::UnitRecord &unit)
    {
        // Le choix de l'utilisateur passe AVANT l'icône trouvée dans la
        // sauvegarde : il l'a fait exprès, et une mise à jour du jeu ne doit pas
        // le lui reprendre.
        const auto chosen = customArtworkPath(unit.emulator, unit.unitKey);
        if (QFileInfo::exists(chosen))
            return chosen;
        const auto cached = iconPathFor(unit.emulator, unit.unitKey);
        // Chaque unité connaît SA racine : celle du dossier générique ou celle
        // de son émulateur. Prendre la mauvaise ferait chercher l'icône ailleurs.
        const auto root =
            unit.rootId == "folder" ? configuration.root : configuration.roots.value(unit.rootId);
        // Une unité-fichier n'a pas d'ICON0.PNG adjacent ; sa jaquette ne peut
        // venir que du cache alimenté par l'adresse proposée par le serveur.
        if (root.isEmpty() || unit.unitType != "dir")
            return QFileInfo::exists(cached) ? cached : QString();
        const auto source = QDir(QDir(root).filePath(unit.relPath)).filePath("ICON0.PNG");
        const QFileInfo sourceInfo(source);
        if (!sourceInfo.isFile() || sourceInfo.size() > core::MaxIconBytes)
            return QFileInfo::exists(cached) ? cached : QString();

        // Déjà en cache et plus récente que la sauvegarde : rien à refaire.
        const QFileInfo cachedInfo(cached);
        if (cachedInfo.exists() && cachedInfo.lastModified() >= sourceInfo.lastModified())
            return cached;

        QFile file(source);
        if (!file.open(QIODevice::ReadOnly))
            return {};
        const auto data = file.read(core::MaxIconBytes);
        // On valide AVANT d'écrire : un `.sav` renommé en PNG ne doit pas
        // atterrir dans le cache et faire échouer un décodeur plus tard.
        if (!core::isValidIcon(data))
            return {};
        QFile out(cached + ".tmp");
        if (!out.open(QIODevice::WriteOnly))
            return {};
        out.write(data);
        out.close();
        QFile::remove(cached);
        return out.rename(cached) ? cached : QString();
    }

    // Le carnet connaît l'identifiant serveur d'une unité ; l'interface, elle,
    // ne manipule que la clé lisible. La traduction se fait ici, une fois.
    static QString serverIdFor(const QString &unitKey)
    {
        state::Store store(stateDatabasePath());
        for (const auto &unit : store.units()) {
            if (unit.unitKey == unitKey && !unit.serverId.isEmpty())
                return unit.serverId;
        }
        throw engine::ApiError("this save is not known to the server yet");
    }

    static QJsonObject readHistory(const Configuration &configuration, const QString &unitKey)
    {
        const auto serverId = serverIdFor(unitKey);
        engine::V0SyncApi api(apiConfig(configuration));
        const auto history = api.history(serverId);
        QJsonArray versions;
        // Les plus récentes d'abord, et bornées : un historique long se
        // consulte, il ne se déverse pas.
        auto ordered = history.versions;
        std::sort(ordered.begin(), ordered.end(),
                  [](const engine::VersionRecord &a, const engine::VersionRecord &b) {
                      return a.number > b.number;
                  });
        for (const auto &version : ordered) {
            if (versions.size() >= 12)
                break;
            versions.append(QJsonObject{{"number", version.number},
                                        {"kind", version.kind},
                                        {"size", version.sizeBytes},
                                        {"at", version.createdAt.left(19)},
                                        {"device", version.originDevice.left(28)}});
        }
        return QJsonObject{
            {"unit", unitKey}, {"head", history.headVersion}, {"versions", versions}};
    }

  public slots:
    void connectAccount(QString url, QString token, QString deviceName)
    {
        network::ApiConfig config;
        config.serverUrl = QUrl(url);
        config.token = token.toUtf8();
        config.timeoutMs = 20000;
        // HTTP n'est admis que lorsqu'il est configuré explicitement pour un
        // environnement local ; une erreur TLS ne provoque aucun repli.
        config.allowHttp = QUrl(url).scheme() == "http";

        network::V0Client client(config);
        // `registerDevice` est le seul appel qui n'exige pas déjà un appareil.
        auto *call = client.registerDevice(
            {deviceName, QStringLiteral(RETROSAVE_PLATFORM), QStringLiteral(RETROSAVE_VERSION)},
            QUuid::createUuid().toRfc4122().toHex());
        network::ApiResult result;
        QEventLoop loop;
        QObject::connect(call, &network::ApiCall::finished, &loop,
                         [&](const network::ApiResult &value) {
                             result = value;
                             loop.quit();
                         });
        loop.exec();
        call->deleteLater();

        if (const auto *device = std::get_if<network::RegisteredDevice>(&result.payload)) {
            emit connected(device->id, url, token);
            return;
        }
        // Aucun corps de réponse recopié : il pourrait contenir le jeton envoyé.
        emit failed(result.httpStatus == 401
                        ? QStringLiteral("Token refused by the server.")
                        : QStringLiteral("Could not connect (%1).")
                              .arg(result.httpStatus > 0 ? QString::number(result.httpStatus)
                                                         : QStringLiteral("server unreachable")));
    }

    // Les conflits, mis en forme ICI et pas plus tard : le canal local a une
    // trame bornée, et c'est l'ouvrier qui connaît déjà les données.
    void fetchConflicts(retrosave::agent::Configuration configuration)
    {
        try {
            engine::V0SyncApi api(apiConfig(configuration));
            QJsonArray list;
            // Quatre au plus, et des libellés coupés : une trame trop grande
            // serait refusée par le canal, et l'interface n'afficherait RIEN
            // plutôt qu'un conflit de trop.
            for (const auto &conflict : api.openConflicts()) {
                if (list.size() >= 4)
                    break;
                list.append(QJsonObject{{"id", conflict.id},
                                        {"unit", conflict.unitLabel.left(48)},
                                        {"a_number", conflict.first.number},
                                        {"a_size", conflict.first.sizeBytes},
                                        {"a_at", conflict.first.createdAt.left(19)},
                                        {"a_device", conflict.first.originDevice.left(28)},
                                        {"b_number", conflict.second.number},
                                        {"b_size", conflict.second.sizeBytes},
                                        {"b_at", conflict.second.createdAt.left(19)},
                                        {"b_device", conflict.second.originDevice.left(28)}});
            }
            emit conflictsFetched(list);
        } catch (const std::exception &error) {
            emit failed(QString::fromUtf8(error.what()).left(200));
        }
    }

    void decideConflict(retrosave::agent::Configuration configuration, QString conflictId,
                        int winner)
    {
        try {
            engine::V0SyncApi api(apiConfig(configuration));
            api.resolveConflict(conflictId, winner);
            // On relit dans la foulée : l'interface doit voir le conflit
            // disparaître, pas le supposer disparu.
            QJsonArray list;
            for (const auto &conflict : api.openConflicts()) {
                if (list.size() >= 4)
                    break;
                list.append(QJsonObject{{"id", conflict.id},
                                        {"unit", conflict.unitLabel.left(48)},
                                        {"a_number", conflict.first.number},
                                        {"a_size", conflict.first.sizeBytes},
                                        {"a_at", conflict.first.createdAt.left(19)},
                                        {"a_device", conflict.first.originDevice.left(28)},
                                        {"b_number", conflict.second.number},
                                        {"b_size", conflict.second.sizeBytes},
                                        {"b_at", conflict.second.createdAt.left(19)},
                                        {"b_device", conflict.second.originDevice.left(28)}});
            }
            emit conflictsFetched(list);
        } catch (const std::exception &error) {
            emit failed(QString::fromUtf8(error.what()).left(200));
        }
    }

    // Les unités viennent du CARNET, pas du serveur : l'interface doit pouvoir
    // montrer ce que cet appareil suit même hors ligne.
    void listUnits(retrosave::agent::Configuration configuration)
    {
        try {
            state::Store store(stateDatabasePath());
            QJsonArray list;
            for (const auto &unit : store.units()) {
                // Borne haute, pas une pagination : la recherche et les filtres
                // de la bibliothèque (BIB-02) doivent porter sur TOUT ce que
                // l'appareil suit, sinon ils mentent. Au-delà, la trame du
                // canal serait refusée et l'interface n'afficherait plus rien —
                // c'est le seul cas où couper vaut mieux que tout perdre.
                if (list.size() >= 300)
                    break;
                list.append(QJsonObject{{"emulator", unit.emulator},
                                        {"key", unit.unitKey},
                                        {"game_key", unit.gameKey},
                                        {"label", unit.gameLabel.left(64)},
                                        {"state", unit.state},
                                        // SYN-06 / SYN-07 : ce que l'utilisateur
                                        // a demandé, distinct de ce que la
                                        // synchronisation constate.
                                        {"local_mode", unit.localMode},
                                        {"version", unit.lastSyncedVersion},
                                        {"known", !unit.serverId.isEmpty()},
                                        {"conflict", unit.openConflictId.has_value()},
                                        {"icon", cacheIcon(configuration, unit)},
                                        // L'interface doit pouvoir proposer
                                        // « Retirer l'image » seulement quand il
                                        // y en a une à retirer.
                                        {"custom_art",
                                         QFileInfo::exists(customArtworkPath(unit.emulator,
                                                                             unit.unitKey))},
                                        // Teinte et initiales calculées ICI, par le noyau partagé :
                                        // les recalculer en QML ferait une quatrième implémentation
                                        // d'une règle qui doit rendre le même résultat sur les
                                        // trois clients (CLAUDE.md §15).
                                        {"hue", core::placeholderHue(unit.gameKey)},
                                        {"initials", core::placeholderInitials(unit.gameLabel)}});
            }
            emit unitsListed(list);
            emit cataloguePublished(catalogue(configuration));
        } catch (const std::exception &error) {
            emit failed(QString::fromUtf8(error.what()).left(200));
        }
    }

    // AD-30 : le réarmement manuel d'une unité mise de côté.
    //
    // Purement LOCAL — aucun appel réseau, et c'est voulu : une unité écartée
    // doit pouvoir repartir même serveur injoignable, puisque la cause est le
    // plus souvent de ce côté-ci. Le fil de travail traite ses demandes une par
    // une : si une passe tourne, ce réarmement attend son tour au lieu de lui
    // retirer le carnet sous les pieds.
    // ─── Choisir ou retirer l'image d'une unité ───────────────────────────
    // Purement local et purement DÉCORATIF : rien ici ne touche au nom des
    // fichiers de sauvegarde, à `unit_key`, à `game_key` ni à la moindre
    // décision de synchronisation. Une image absente ou refusée laisse
    // simplement le dessin de repli — elle ne bloque jamais une passe.
    void setArtwork(retrosave::agent::Configuration configuration, QString unitKey,
                    QString sourcePath)
    {
        try {
            state::Store store(stateDatabasePath());
            std::optional<state::UnitRecord> found;
            for (const auto &unit : store.units()) {
                if (unit.unitKey == unitKey) {
                    found = unit;
                    break;
                }
            }
            if (!found) {
                emit failed(QStringLiteral("Unknown save: %1").arg(unitKey));
                return;
            }
            const auto destination = customArtworkPath(found->emulator, found->unitKey);

            if (sourcePath.isEmpty()) {
                QFile::remove(destination);
                listUnits(configuration);
                return;
            }

            const QFileInfo sourceInfo(sourcePath);
            if (!sourceInfo.isFile()) {
                emit failed(QStringLiteral("Image not found."));
                return;
            }
            if (sourceInfo.size() > core::MaxIconBytes) {
                emit failed(QStringLiteral("Image too large: %1 kB maximum.")
                                .arg(core::MaxIconBytes / 1024));
                return;
            }
            QFile source(sourcePath);
            if (!source.open(QIODevice::ReadOnly)) {
                emit failed(QStringLiteral("Unreadable image."));
                return;
            }
            const auto data = source.read(core::MaxIconBytes);
            // On valide AVANT d'écrire, et par l'EN-TÊTE : aucune image n'est
            // décodée ici. Un décodeur est une surface d'attaque, et ce fichier
            // vient du disque de l'utilisateur — bornes de taille et de
            // dimensions suffisent à ce que l'interface doit savoir.
            if (!core::isValidIcon(data)) {
                emit failed(QStringLiteral(
                    "Not a usable PNG image."));
                return;
            }
            // Écriture atomique : un remplacement interrompu ne doit pas laisser
            // une image tronquée à la place de l'ancienne.
            QFile out(destination + ".tmp");
            if (!out.open(QIODevice::WriteOnly) || out.write(data) != data.size()) {
                emit failed(QStringLiteral("Image not saved."));
                return;
            }
            out.close();
            QFile::remove(destination);
            if (!QFile::rename(destination + ".tmp", destination)) {
                QFile::remove(destination + ".tmp");
                emit failed(QStringLiteral("Image not saved."));
                return;
            }
            listUnits(configuration);
        } catch (const std::exception &error) {
            emit failed(QString::fromUtf8(error.what()).left(200));
        }
    }

    void retryUnit(retrosave::agent::Configuration configuration, QString unitKey)
    {
        try {
            state::Store store(stateDatabasePath());
            bool found = false;
            for (const auto &unit : store.units()) {
                if (unit.unitKey != unitKey)
                    continue;
                store.clearFailures(unit.localId);
                // « error » redevient « active » : sans cela l'unité resterait
                // exclue de la capture, et le compteur remis à zéro ne servirait
                // à rien. Les autres états ne sont pas touchés — une unité en
                // pause ou disparue ne se réarme pas depuis ce bouton.
                if (unit.state == "error")
                    store.setState(unit.localId, "active");
                found = true;
                break;
            }
            if (!found) {
                emit failed(QStringLiteral("Unknown save: %1").arg(unitKey));
                return;
            }
            // L'interface doit voir le nouvel état tout de suite, sinon le
            // bouton semble n'avoir rien fait.
            listUnits(configuration);
        } catch (const std::exception &error) {
            emit failed(QString::fromUtf8(error.what()).left(200));
        }
    }

    void fetchHistory(retrosave::agent::Configuration configuration, QString unitKey)
    {
        try {
            emit historyFetched(readHistory(configuration, unitKey));
        } catch (const std::exception &error) {
            emit failed(QString::fromUtf8(error.what()).left(200));
        }
    }

    void restoreVersion(retrosave::agent::Configuration configuration, QString unitKey, int number)
    {
        try {
            const auto serverId = serverIdFor(unitKey);
            engine::V0SyncApi api(apiConfig(configuration));
            api.restore(serverId, number);
            // On relit dans la foulée : l'utilisateur doit VOIR la nouvelle
            // version apparaître, pas la supposer créée.
            emit historyFetched(readHistory(configuration, unitKey));
        } catch (const std::exception &error) {
            emit failed(QString::fromUtf8(error.what()).left(200));
        }
    }

    // ─── SYN-06 / SYN-07 : pause et sélection, strictement locales ────────
    //
    // Aucun appel réseau et aucune suppression. Le fichier du joueur reste où
    // il est, la version distante aussi, et les autres appareils continuent
    // exactement comme avant : ce choix ne quitte jamais ce poste.
    void setLocalMode(retrosave::agent::Configuration configuration, QString unitKey, QString mode)
    {
        try {
            state::Store store(stateDatabasePath());
            std::optional<state::UnitRecord> found;
            for (const auto &unit : store.units()) {
                if (unit.unitKey == unitKey) {
                    found = unit;
                    break;
                }
            }
            if (!found) {
                emit failed(QStringLiteral("Unknown save: %1").arg(unitKey));
                return;
            }
            // Le carnet refuse lui-même une valeur hors des trois admises : une
            // chaîne arbitraire venue du canal ne doit pas s'y installer.
            store.setLocalMode(found->localId, mode);
            store.appendActivity(mode == QLatin1String("sync") ? "reprise"
                                 : mode == QLatin1String("paused") ? "pause"
                                                                   : "retiree",
                                 found->unitKey, found->localId);
            listUnits(configuration);
        } catch (const std::exception &error) {
            emit failed(QString::fromUtf8(error.what()).left(200));
        }
    }

    // ─── EXP-03 : le journal d'activité, tel que l'utilisateur peut le lire ──
    void listActivity(retrosave::agent::Configuration configuration)
    {
        Q_UNUSED(configuration)
        try {
            state::Store store(stateDatabasePath());
            QJsonArray list;
            for (const auto &line : store.recentActivity(60))
                list.append(line.left(200));
            emit activityListed(list);
        } catch (const std::exception &error) {
            emit failed(QString::fromUtf8(error.what()).left(200));
        }
    }

    // ─── EXP-01 : les appareils du compte ────────────────────────────────
    static QJsonArray readDevices(const Configuration &configuration)
    {
        network::V0Client client(apiConfig(configuration));
        const auto result = awaitCall(client.listDevices());
        const auto *devices = std::get_if<network::Devices>(&result.payload);
        if (result.failure != network::Failure::None || devices == nullptr)
            throw engine::ApiError("liste des appareils indisponible");
        QJsonArray list;
        for (const auto &device : devices->items) {
            if (list.size() >= 24)
                break;
            list.append(QJsonObject{
                {"id", device.id},
                {"name", device.name.left(48)},
                {"os", device.os},
                {"last_seen", device.lastSeenAt.value_or(QString()).left(19)},
                {"revoked", device.revokedAt.has_value()},
                // L'appareil courant ne doit pas pouvoir se révoquer lui-même :
                // l'interface a besoin de le savoir pour ne pas le proposer.
                {"current", device.id.toUtf8() == configuration.deviceId}});
        }
        return list;
    }

    void listDevices(retrosave::agent::Configuration configuration)
    {
        try {
            emit devicesListed(readDevices(configuration));
        } catch (const std::exception &error) {
            emit failed(QString::fromUtf8(error.what()).left(200));
        }
    }

    void renameDevice(retrosave::agent::Configuration configuration, QString deviceId, QString name)
    {
        try {
            network::V0Client client(apiConfig(configuration));
            const auto result = awaitCall(
                client.renameDevice(deviceId, name, QUuid::createUuid().toRfc4122().toHex()));
            if (result.failure != network::Failure::None) {
                emit failed(QStringLiteral("Rename refused (%1).")
                                .arg(result.httpStatus > 0 ? QString::number(result.httpStatus)
                                                           : QStringLiteral("server unreachable")));
                return;
            }
            // On relit : l'utilisateur doit VOIR le nouveau nom, pas le supposer.
            emit devicesListed(readDevices(configuration));
        } catch (const std::exception &error) {
            emit failed(QString::fromUtf8(error.what()).left(200));
        }
    }

    void revokeDevice(retrosave::agent::Configuration configuration, QString deviceId)
    {
        try {
            network::V0Client client(apiConfig(configuration));
            const auto result =
                awaitCall(client.revokeDevice(deviceId, QUuid::createUuid().toRfc4122().toHex()));
            if (result.failure != network::Failure::None) {
                // 409 est le refus d'auto-révocation : il mérite sa phrase,
                // parce que l'utilisateur peut corriger son geste.
                emit failed(result.httpStatus == 409
                                ? QStringLiteral("A device cannot revoke itself.")
                                : QStringLiteral("Revoke refused (%1).")
                                      .arg(result.httpStatus > 0
                                               ? QString::number(result.httpStatus)
                                               : QStringLiteral("server unreachable")));
                return;
            }
            emit devicesListed(readDevices(configuration));
        } catch (const std::exception &error) {
            emit failed(QString::fromUtf8(error.what()).left(200));
        }
    }

    // ─── EXP-05 : version du serveur et compatibilité (AD-31) ────────────
    void checkServer(retrosave::agent::Configuration configuration)
    {
        try {
            network::V0Client client(apiConfig(configuration));
            const auto result = awaitCall(client.health());
            const auto *health = std::get_if<network::Health>(&result.payload);
            if (result.failure != network::Failure::None || health == nullptr) {
                emit serverChecked(QString(), QStringLiteral("inconnue"));
                return;
            }
            const auto version = health->version.value_or(QString());
            emit serverChecked(version, compareVersions(QStringLiteral(RETROSAVE_VERSION), version));
        } catch (const std::exception &error) {
            emit failed(QString::fromUtf8(error.what()).left(200));
        }
    }

    // ─── EXP-04 : un diagnostic qu'on peut envoyer sans y penser ─────────
    //
    // Ce que ce fichier NE contient pas, et c'est la seule chose qui compte :
    // le jeton, aucun contenu de sauvegarde, aucun octet lu dans un fichier de
    // jeu. Ce qu'il contient est écrit en clair dans son propre en-tête, pour
    // que l'utilisateur sache ce qu'il transmet avant de le transmettre.
    void writeDiagnostic(retrosave::agent::Configuration configuration, QString path)
    {
        try {
            QStringList lines;
            lines << QStringLiteral("# Diagnostic RetroSave");
            lines << QStringLiteral("#");
            lines << QStringLiteral("# This file lists: versions, settings, watched folders,");
            lines << QStringLiteral("# tracked save names and the activity log. It contains");
            lines << QStringLiteral("# NEITHER your token NOR the contents of a save.");
            lines << QStringLiteral("# Read it before sending it.");
            lines << QString();
            lines << QStringLiteral("Generated        : %1")
                         .arg(QDateTime::currentDateTime().toString(Qt::ISODate));
            lines << QStringLiteral("Client version   : %1").arg(QStringLiteral(RETROSAVE_VERSION));
            lines << QStringLiteral("Plateforme       : %1").arg(QStringLiteral(RETROSAVE_PLATFORM));
            lines << QStringLiteral("System           : %1").arg(QSysInfo::prettyProductName());
            lines << QStringLiteral("Architecture     : %1").arg(QSysInfo::currentCpuArchitecture());
            // L'hôte seulement : le chemin pourrait porter un identifiant, et
            // l'hôte suffit à distinguer « mon serveur » de « le cloud ».
            const QUrl server(configuration.serverUrl);
            lines << QStringLiteral("Serveur          : %1 (%2)")
                         .arg(server.host().isEmpty() ? QStringLiteral("not linked") : server.host(),
                              server.scheme().isEmpty() ? QStringLiteral("—") : server.scheme());
            lines << QStringLiteral("Device linked    : %1")
                         .arg(configuration.deviceId.isEmpty() ? QStringLiteral("non")
                                                               : QStringLiteral("oui"));
            lines << QStringLiteral("Global pause     : %1")
                         .arg(configuration.globalPause ? QStringLiteral("oui")
                                                        : QStringLiteral("non"));
            lines << QStringLiteral("Intervalle       : %1 s").arg(configuration.intervalSeconds);
            lines << QString();
            lines << QStringLiteral("## Watched folders");
            if (!configuration.root.isEmpty())
                lines << QStringLiteral("  generic folder : %1").arg(configuration.root);
            for (auto it = configuration.roots.constBegin(); it != configuration.roots.constEnd();
                 ++it)
                lines << QStringLiteral("  %1 : %2").arg(it.key(), it.value());
            if (configuration.root.isEmpty() && configuration.roots.isEmpty())
                lines << QStringLiteral("  aucun");

            state::Store store(stateDatabasePath());
            const auto units = store.units();
            lines << QString();
            lines << QStringLiteral("## Tracked saves (%1)").arg(units.size());
            for (const auto &unit : units) {
                lines << QStringLiteral("  %1/%2 — state %3, mode %4, version %5%6")
                             .arg(unit.emulator, unit.unitKey, unit.state, unit.localMode)
                             .arg(unit.lastSyncedVersion)
                             .arg(unit.consecutiveFailures > 0
                                      ? QStringLiteral(", %1 failure(s)").arg(unit.consecutiveFailures)
                                      : QString());
            }
            lines << QString();
            lines << QStringLiteral("## Activity log");
            for (const auto &line : store.recentActivity(200))
                lines << QStringLiteral("  ") + line;

            const auto body = lines.join('\n').toUtf8() + '\n';
            // Écriture atomique, comme partout ailleurs : un diagnostic tronqué
            // ferait perdre du temps à celui qui le lit.
            QFile out(path + ".tmp");
            if (!out.open(QIODevice::WriteOnly) || out.write(body) != body.size()) {
                emit failed(QStringLiteral("Diagnostics not written: folder unreachable."));
                return;
            }
            out.close();
            QFile::remove(path);
            if (!QFile::rename(path + ".tmp", path)) {
                QFile::remove(path + ".tmp");
                emit failed(QStringLiteral("Diagnostics not written: rename refused."));
                return;
            }
            emit outcomeReady(QStringLiteral("Diagnostics written: %1").arg(path));
        } catch (const std::exception &error) {
            emit failed(QString::fromUtf8(error.what()).left(200));
        }
    }

    // ─── PRO-08 : sortir ses sauvegardes du service ──────────────────────
    //
    // Une COPIE, jamais un déplacement : la sauvegarde du joueur reste où elle
    // est, et le serveur n'est pas touché. Le résultat est un dossier ordinaire
    // que n'importe quel gestionnaire de fichiers ouvre — c'est tout l'intérêt
    // d'un export, il doit survivre à la disparition de RetroSave.
    void writeExport(retrosave::agent::Configuration configuration, QString path)
    {
        try {
            QDir destination(path);
            if (path.isEmpty()) {
                emit failed(QStringLiteral("No folder chosen."));
                return;
            }
            // Exporter DANS un dossier surveillé se copierait lui-même à la
            // passe suivante, et fabriquerait des unités fantômes.
            for (const auto &root : configuration.roots.values() + QStringList{configuration.root}) {
                if (root.isEmpty())
                    continue;
                const auto canonicalRoot = QDir(root).absolutePath() + '/';
                if ((destination.absolutePath() + '/').startsWith(canonicalRoot)) {
                    emit failed(QStringLiteral(
                        "Choose a folder outside the watched ones."));
                    return;
                }
            }
            if (!destination.exists() && !QDir().mkpath(path)) {
                emit failed(QStringLiteral("Could not create the export folder."));
                return;
            }
            // On n'écrase rien : un export dans un dossier déjà rempli
            // remplacerait silencieusement le précédent.
            if (!destination.isEmpty()) {
                emit failed(QStringLiteral("This folder is not empty."));
                return;
            }

            state::Store store(stateDatabasePath());
            QJsonArray manifest;
            int copied = 0;
            int skipped = 0;
            for (const auto &unit : store.units()) {
                const auto root = unit.rootId == "folder" ? configuration.root
                                                          : configuration.roots.value(unit.rootId);
                const auto source =
                    root.isEmpty() ? QString() : QDir(root).filePath(unit.relPath);
                QJsonObject entry{{"emulator", unit.emulator},
                                  {"unit_key", unit.unitKey},
                                  {"unit_type", unit.unitType},
                                  {"game_label", unit.gameLabel},
                                  {"last_synced_version", unit.lastSyncedVersion},
                                  {"state", unit.state}};
                if (unit.lastSyncedContentSha256)
                    entry.insert("content_sha256", *unit.lastSyncedContentSha256);
                if (source.isEmpty() || !QFileInfo::exists(source)) {
                    // Une unité absente du disque n'est pas une erreur d'export :
                    // elle est simplement ailleurs, ou pas encore reçue ici.
                    entry.insert("exported", false);
                    ++skipped;
                    manifest.append(entry);
                    continue;
                }
                // Un chemin dérivé d'une clé d'unité ne sort jamais du dossier
                // d'export : la garde lexicale est la même que pour l'accueil.
                if (core::pathGuard(unit.unitKey) != QLatin1String("ok")) {
                    entry.insert("exported", false);
                    ++skipped;
                    manifest.append(entry);
                    continue;
                }
                const auto target = destination.filePath(unit.emulator + "/" + unit.unitKey);
                if (!copyTree(source, target)) {
                    entry.insert("exported", false);
                    ++skipped;
                    manifest.append(entry);
                    continue;
                }
                entry.insert("exported", true);
                entry.insert("path", unit.emulator + "/" + unit.unitKey);
                ++copied;
                manifest.append(entry);
            }

            QJsonObject document{
                {"format", "retrosave-export-1"},
                {"generated_at", QDateTime::currentDateTime().toString(Qt::ISODate)},
                {"client_version", QStringLiteral(RETROSAVE_VERSION)},
                {"note", "Plain copies of this device's saves. No ROM, no token, no archive."},
                {"units", manifest}};
            QFile index(destination.filePath("retrosave-export.json"));
            if (!index.open(QIODevice::WriteOnly) ||
                index.write(QJsonDocument(document).toJson()) <= 0) {
                emit failed(QStringLiteral("Export index not written."));
                return;
            }
            index.close();
            store.appendActivity("export", QStringLiteral("%1 save(s)").arg(copied));
            emit outcomeReady(
                skipped == 0
                    ? QStringLiteral("%1 save(s) exported to %2").arg(copied).arg(path)
                    : QStringLiteral("%1 save(s) exported to %2; %3 not found on this "
                                     "device, listed in the index.")
                          .arg(copied)
                          .arg(path)
                          .arg(skipped));
        } catch (const std::exception &error) {
            emit failed(QString::fromUtf8(error.what()).left(200));
        }
    }

    // ─── Q45 : aller chercher les jaquettes proposées par le serveur ──────
    //
    // Trois propriétés, et chacune a sa raison :
    //
    // 1. **Rien de tout cela ne peut faire échouer une passe.** Une jaquette
    //    est décorative ; ce bloc est appelé APRÈS la synchronisation et tout
    //    ce qui rate y est silencieux.
    // 2. **Une adresse n'est tentée qu'une fois.** Sans marqueur, un jeu sans
    //    jaquette provoquerait un téléchargement raté à chaque passe. Le
    //    marqueur retient l'adresse essayée : si le serveur en propose une
    //    autre plus tard, elle sera tentée.
    // 3. **L'image est validée par son EN-TÊTE avant d'être écrite**, comme
    //    celle que l'utilisateur choisit lui-même. Un fichier qui n'est pas un
    //    PNG exploitable n'entre pas dans le cache.
    static void fetchArtwork(const Configuration &configuration,
                             const std::vector<engine::RemoteUnit> &remote)
    {
        if (!configuration.downloadArtwork)
            return;
        network::S3Transfer transfer(false);
        for (const auto &unit : remote) {
            if (unit.artworkUrl.isEmpty())
                continue;
            const auto cached = iconPathFor(unit.emulator, unit.unitKey);
            if (QFileInfo::exists(cached))
                continue;
            // Le choix de l'utilisateur passe avant tout : s'il a posé sa
            // propre image, on ne va rien chercher.
            if (QFileInfo::exists(customArtworkPath(unit.emulator, unit.unitKey)))
                continue;

            const auto marker = cached + ".tried";
            QFile seen(marker);
            if (seen.open(QIODevice::ReadOnly)) {
                const auto previous = QString::fromUtf8(seen.readAll()).trimmed();
                seen.close();
                if (previous == unit.artworkUrl)
                    continue;
            }

            QByteArray data;
            try {
                auto *call = transfer.downloadCandidate(QUrl(unit.artworkUrl),
                                                        core::MaxArtworkBytes, stagingPath());
                network::TransferResult result;
                QEventLoop loop;
                QObject::connect(call, &network::TransferCall::finished, &loop,
                                 [&](const network::TransferResult &value) {
                                     result = value;
                                     loop.quit();
                                 });
                loop.exec();
                call->deleteLater();
                // Le transfert REFERME son fichier temporaire après l'avoir
                // écrit ; il faut donc le rouvrir. Mais `open()` sur un QFile
                // déjà ouvert rend false — traiter un seul des deux cas faisait
                // échouer chaque téléchargement en silence.
                if (result.failure == network::TransferFailure::None && result.archive
                    && (result.archive->isOpen() || result.archive->open())) {
                    result.archive->seek(0);
                    data = result.archive->read(core::MaxArtworkBytes);
                }
            } catch (const std::exception &) {
                data.clear();
            }

            // Le marqueur est posé dans TOUS les cas : un échec comme une
            // réussite valent « cette adresse a été tentée ».
            QFile stamp(marker);
            if (stamp.open(QIODevice::WriteOnly)) {
                stamp.write(unit.artworkUrl.toUtf8());
                stamp.close();
            }
            // Même validation par en-tête que pour une image choisie à la
            // main, avec la borne des jaquettes : une image de catalogue pèse
            // plus qu'un ICON0.PNG, et la refuser pour cela seul n'aurait
            // protégé de rien.
            if (data.isEmpty() || !core::isValidIcon(data, core::MaxArtworkBytes))
                continue;

            // Écriture atomique : une jaquette à moitié écrite vaut moins
            // qu'une pastille de repli.
            QFile out(cached + ".tmp");
            if (!out.open(QIODevice::WriteOnly) || out.write(data) != data.size())
                continue;
            out.close();
            QFile::remove(cached);
            if (!QFile::rename(cached + ".tmp", cached))
                QFile::remove(cached + ".tmp");
        }
    }

    void runPass(retrosave::agent::Configuration configuration)
    {
        PassSummary summary;
        summary.finishedAt = QDateTime::currentDateTime().toString(Qt::ISODate);
        try {
            engine::V0SyncApi api(apiConfig(configuration));
            state::Store store(stateDatabasePath());
            QHash<QString, QString> roots;
            const auto scanned = scanAll(configuration, roots);
            engine::SyncPass pass(store, api, roots, stagingPath(),
                                  placeableRoots(configuration), configuration.globalPause);
            // ─── Ne jamais capturer ni appliquer pendant qu’un émulateur écrit ──
            // Les noms de processus viennent des MANIFESTES, pas du code : un
            // connecteur ajouté demain est protégé sans qu'on y touche. La
            // liste doit être fraîche à chaque garde, notamment après le réseau.
            adapters::AdapterRegistry registry;
            registry.loadDirectory(adaptersPath());
            const auto report = pass.run(
                scanned, double(QDateTime::currentMSecsSinceEpoch()) / 1000.0,
                [&registry](const QString &emulator) {
                    const auto *adapter = registry.adapter(emulator);
                    // Une unité venue de la racine générique n'appartient à
                    // aucun émulateur connu : rien à surveiller, donc rien à
                    // bloquer. C'est le seul cas où l'absence de protection est
                    // le bon comportement.
                    return adapter != nullptr
                        && anyProcessRunning(adapter->manifest().processNames, runningProcessNames());
                });
            summary.ran = true;
            summary.scanned = report.scanned;
            summary.pulled = report.pulled;
            summary.pushed = report.pushed;
            summary.duplicates = report.duplicates;
            summary.conflicts = report.conflicts;
            summary.reapplied = report.reapplied;
            summary.missing = report.missing;
            summary.errors = report.errors;
            summary.skippedPaused = report.skippedPaused;
            summary.skippedExcluded = report.skippedExcluded;
            summary.paused = report.pausedForMassiveDisappearance;
            if (!report.messages.isEmpty())
                summary.message = report.messages.first().left(300);
            // Les jaquettes, APRÈS la synchronisation et hors de son filet :
            // rien ici ne peut changer le résultat d'une passe.
            try {
                fetchArtwork(configuration, api.listUnits());
            } catch (const std::exception &) {
                // Silencieux par construction : une jaquette absente laisse la
                // pastille de repli, et l'utilisateur n'a rien à faire de ça.
            }
        } catch (const std::exception &error) {
            // Une passe entière peut échouer — serveur injoignable, carnet
            // illisible. Ce n'est jamais une raison de faire tomber l'agent :
            // il doit rester joignable pour qu'on puisse le diagnostiquer.
            summary.ran = false;
            summary.errors = 1;
            summary.message = QString::fromUtf8(error.what()).left(300);
        }
        emit finished(summary);
    }

  signals:
    void finished(const retrosave::agent::PassSummary &summary);
    void conflictsFetched(const QJsonArray &conflicts);
    void unitsListed(const QJsonArray &units);
    void cataloguePublished(const QJsonArray &adapters);
    void historyFetched(const QJsonObject &history);
    void devicesListed(const QJsonArray &devices);
    void activityListed(const QJsonArray &activity);
    void outcomeReady(const QString &outcome);
    void serverChecked(const QString &version, const QString &compatibility);
    void connected(const QString &deviceId, const QString &url, const QString &token);
    void failed(const QString &message);
};

} // namespace

SyncService::SyncService(QObject *parent) : QObject(parent)
{
    qRegisterMetaType<Configuration>();
    qRegisterMetaType<PassSummary>();
    m_configuration = loadConfiguration();

    auto *worker = new SyncWorker;
    worker->moveToThread(&m_thread);
    // L'ouvrier meurt avec son fil : `deleteLater` s'exécute dans le fil, donc
    // ses objets Qt sont détruits là où ils ont été créés. Détruire un QObject
    // depuis un autre fil est une faute classique et silencieuse.
    connect(&m_thread, &QThread::finished, worker, &QObject::deleteLater);
    connect(this, &SyncService::passRequested, worker, &SyncWorker::runPass);
    connect(this, &SyncService::connectionRequested, worker, &SyncWorker::connectAccount);
    connect(this, &SyncService::conflictsRequested, worker, &SyncWorker::fetchConflicts);
    connect(this, &SyncService::decisionRequested, worker, &SyncWorker::decideConflict);
    connect(worker, &SyncWorker::conflictsFetched, this, &SyncService::onConflicts);
    connect(this, &SyncService::unitsRequested, worker, &SyncWorker::listUnits);
    connect(this, &SyncService::historyRequested, worker, &SyncWorker::fetchHistory);
    connect(this, &SyncService::restoreRequested, worker, &SyncWorker::restoreVersion);
    connect(this, &SyncService::retryRequested, worker, &SyncWorker::retryUnit);
    connect(this, &SyncService::artworkRequested, worker, &SyncWorker::setArtwork);
    connect(this, &SyncService::localModeRequested, worker, &SyncWorker::setLocalMode);
    connect(this, &SyncService::devicesRequested, worker, &SyncWorker::listDevices);
    connect(this, &SyncService::deviceRenameRequested, worker, &SyncWorker::renameDevice);
    connect(this, &SyncService::deviceRevokeRequested, worker, &SyncWorker::revokeDevice);
    connect(this, &SyncService::activityRequested, worker, &SyncWorker::listActivity);
    connect(this, &SyncService::diagnosticRequested, worker, &SyncWorker::writeDiagnostic);
    connect(this, &SyncService::exportRequested, worker, &SyncWorker::writeExport);
    connect(this, &SyncService::serverCheckRequested, worker, &SyncWorker::checkServer);
    connect(worker, &SyncWorker::devicesListed, this, &SyncService::onDevices);
    connect(worker, &SyncWorker::activityListed, this, &SyncService::onActivity);
    connect(worker, &SyncWorker::outcomeReady, this, &SyncService::onOutcome);
    connect(worker, &SyncWorker::serverChecked, this, &SyncService::onServerChecked);
    connect(worker, &SyncWorker::unitsListed, this, &SyncService::onUnits);
    connect(worker, &SyncWorker::cataloguePublished, this, &SyncService::onCatalogue);
    connect(worker, &SyncWorker::historyFetched, this, &SyncService::onHistory);
    connect(worker, &SyncWorker::finished, this, &SyncService::onFinished);
    connect(worker, &SyncWorker::connected, this,
            [this](const QString &deviceId, const QString &url, const QString &token) {
                onConnected(deviceId, url, token);
            });
    connect(worker, &SyncWorker::failed, this, &SyncService::onFailed);
    m_thread.start();

    // Passe périodique. Le premier tir est différé : au lancement, l'utilisateur
    // vient peut-être de fermer son émulateur, et la stabilisation a besoin de
    // deux observations espacées de dix secondes de toute façon.
    m_periodic.setInterval(m_configuration.intervalSeconds * 1000);
    connect(&m_periodic, &QTimer::timeout, this, [this] { requestSync(); });
    if (m_configuration.complete())
        m_periodic.start();
}

SyncService::~SyncService()
{
    // On attend la fin de la passe en cours plutôt que de tuer le fil : une
    // écriture interrompue est récupérable, mais autant ne pas la provoquer.
    m_thread.quit();
    m_thread.wait(30000);
}

void SyncService::connectAccount(const QString &url, const QString &token,
                                 const QString &deviceName)
{
    if (m_busy) {
        m_problem = QStringLiteral("A sync is running.");
        emit changed();
        return;
    }
    m_busy = true;
    m_problem.clear();
    emit changed();
    emit connectionRequested(url, token, deviceName);
}

QString SyncService::requestSync()
{
    // Refuser clairement vaut mieux que lancer une passe qui échouera : chaque
    // refus ici correspond à une action précise côté utilisateur.
    if (m_busy)
        return QStringLiteral("A sync is already running.");
    if (!m_configuration.connected())
        return QStringLiteral("No server linked.");
    if (m_configuration.root.isEmpty() && m_configuration.roots.isEmpty())
        return QStringLiteral("No folder set.");
    m_busy = true;
    m_problem.clear();
    emit changed();
    emit passRequested(m_configuration);
    return {};
}

QString SyncService::refreshConflicts()
{
    if (m_busy)
        return QStringLiteral("An operation is already running.");
    if (!m_configuration.connected())
        return QStringLiteral("No server linked.");
    m_busy = true;
    m_problem.clear();
    emit changed();
    emit conflictsRequested(m_configuration);
    return {};
}

QString SyncService::resolveConflict(const QString &conflictId, int winner)
{
    if (m_busy)
        return QStringLiteral("An operation is already running.");
    if (!m_configuration.connected())
        return QStringLiteral("No server linked.");
    if (conflictId.isEmpty() || winner <= 0)
        return QStringLiteral("Incomplete decision.");
    m_busy = true;
    m_problem.clear();
    emit changed();
    emit decisionRequested(m_configuration, conflictId, winner);
    return {};
}

QString SyncService::refreshUnits()
{
    // **Ne marque PAS l'agent occupé.** Lister le carnet est une lecture
    // locale de quelques millisecondes ; la faire compter comme une opération
    // ferait refuser un clic sur « Historique » juste après une passe, avec un
    // « une opération est déjà en cours » que rien ne justifie côté joueur.
    emit unitsRequested(m_configuration);
    return {};
}

QString SyncService::fetchHistory(const QString &unitKey)
{
    if (m_busy)
        return QStringLiteral("An operation is already running.");
    if (!m_configuration.connected())
        return QStringLiteral("No server linked.");
    if (unitKey.isEmpty())
        return QStringLiteral("No save chosen.");
    m_busy = true;
    m_problem.clear();
    emit changed();
    emit historyRequested(m_configuration, unitKey);
    return {};
}

QString SyncService::restoreVersion(const QString &unitKey, int number)
{
    if (m_busy)
        return QStringLiteral("An operation is already running.");
    if (!m_configuration.connected())
        return QStringLiteral("No server linked.");
    if (unitKey.isEmpty() || number <= 0)
        return QStringLiteral("Incomplete restore.");
    m_busy = true;
    m_problem.clear();
    emit changed();
    emit restoreRequested(m_configuration, unitKey, number);
    return {};
}

QString SyncService::chooseArtwork(const QString &unitKey, const QString &imagePath)
{
    if (unitKey.isEmpty())
        return QStringLiteral("Unknown save.");
    // Pas de garde `m_busy` : c'est une écriture locale sans réseau, qui se
    // range derrière la passe en cours dans le fil de travail.
    m_problem.clear();
    emit artworkRequested(m_configuration, unitKey, imagePath);
    return {};
}

QString SyncService::retryUnit(const QString &unitKey)
{
    if (unitKey.isEmpty())
        return QStringLiteral("Unknown save.");
    // Pas de garde `m_busy` : ce réarmement n'appelle pas le réseau et n'a donc
    // aucune raison d'attendre qu'une passe se termine. Il se range simplement
    // derrière elle dans le fil de travail.
    m_problem.clear();
    emit retryRequested(m_configuration, unitKey);
    return {};
}

QString SyncService::setLocalMode(const QString &unitKey, const QString &mode)
{
    if (unitKey.isEmpty())
        return QStringLiteral("Unknown save.");
    if (mode != QLatin1String("sync") && mode != QLatin1String("paused") &&
        mode != QLatin1String("excluded"))
        return QStringLiteral("Unknown choice.");
    // Pas de garde `m_busy` : c'est une écriture locale d'une ligne, sans
    // réseau. Elle se range derrière la passe en cours dans le fil de travail,
    // et sera donc prise en compte à la passe SUIVANTE — jamais au milieu de
    // celle qui tourne, où elle laisserait un demi-tour appliqué.
    m_problem.clear();
    emit localModeRequested(m_configuration, unitKey, mode);
    return {};
}

QString SyncService::setGlobalPause(bool paused)
{
    // Écrit tout de suite, sans passer par le fil de travail : c'est un réglage
    // d'appareil, et l'utilisateur doit voir l'interrupteur tenir sa position
    // même si une passe occupe l'ouvrier. La passe en cours va jusqu'au bout —
    // l'interrompre au milieu laisserait une unité à moitié traitée.
    m_configuration.globalPause = paused;
    saveGlobalPause(paused);
    emit changed();
    return {};
}

QString SyncService::setNotifications(bool conflicts, bool errors, bool restores)
{
    m_configuration.notifyConflicts = conflicts;
    m_configuration.notifyErrors = errors;
    m_configuration.notifyRestores = restores;
    saveNotificationPreferences(conflicts, errors, restores);
    emit changed();
    return {};
}

QString SyncService::refreshDevices()
{
    if (m_busy)
        return QStringLiteral("An operation is already running.");
    if (!m_configuration.connected())
        return QStringLiteral("No server linked.");
    m_busy = true;
    m_problem.clear();
    emit changed();
    emit devicesRequested(m_configuration);
    return {};
}

QString SyncService::renameDevice(const QString &deviceId, const QString &name)
{
    if (m_busy)
        return QStringLiteral("An operation is already running.");
    if (!m_configuration.connected())
        return QStringLiteral("No server linked.");
    if (deviceId.isEmpty() || name.trimmed().isEmpty())
        return QStringLiteral("Give this device a name.");
    m_busy = true;
    m_problem.clear();
    emit changed();
    emit deviceRenameRequested(m_configuration, deviceId, name.trimmed().left(64));
    return {};
}

QString SyncService::revokeDevice(const QString &deviceId)
{
    if (m_busy)
        return QStringLiteral("An operation is already running.");
    if (!m_configuration.connected())
        return QStringLiteral("No server linked.");
    if (deviceId.isEmpty())
        return QStringLiteral("No device chosen.");
    // Le refus d'auto-révocation est aussi contrôlé par le serveur ; ici il
    // évite un aller-retour et une phrase d'erreur inutile.
    if (deviceId.toUtf8() == m_configuration.deviceId)
        return QStringLiteral("A device cannot revoke itself.");
    m_busy = true;
    m_problem.clear();
    emit changed();
    emit deviceRevokeRequested(m_configuration, deviceId);
    return {};
}

QString SyncService::refreshActivity()
{
    // Lecture locale, comme la liste des unités : elle ne rend pas l'agent
    // occupé, sinon consulter son journal ferait refuser le clic suivant.
    emit activityRequested(m_configuration);
    return {};
}

QString SyncService::exportDiagnostic(const QString &path)
{
    if (path.isEmpty())
        return QStringLiteral("No location chosen.");
    m_problem.clear();
    m_outcome.clear();
    emit diagnosticRequested(m_configuration, path);
    return {};
}

QString SyncService::exportData(const QString &path)
{
    if (path.isEmpty())
        return QStringLiteral("No folder chosen.");
    if (m_busy)
        return QStringLiteral("A sync is running; try again after.");
    m_problem.clear();
    m_outcome.clear();
    // Marqué occupé : un export copie des fichiers pendant que la passe
    // pourrait les remplacer. Les deux dans le même fil, donc jamais en même
    // temps ; le drapeau sert à le dire à l'utilisateur.
    m_busy = true;
    emit changed();
    emit exportRequested(m_configuration, path);
    return {};
}

QString SyncService::checkServer()
{
    if (m_busy)
        return QStringLiteral("An operation is already running.");
    if (!m_configuration.connected())
        return QStringLiteral("No server linked.");
    m_busy = true;
    m_problem.clear();
    emit changed();
    emit serverCheckRequested(m_configuration);
    return {};
}

void SyncService::onDevices(const QJsonArray &devices)
{
    m_busy = false;
    m_devices = devices;
    emit changed();
}

void SyncService::onActivity(const QJsonArray &activity)
{
    // Comme `onUnits` : cette lecture n'a pas rendu l'agent occupé, elle n'a
    // donc pas à libérer une opération réseau qui, elle, tourne encore.
    m_activity = activity;
    emit changed();
}

void SyncService::onOutcome(const QString &outcome)
{
    m_busy = false;
    m_outcome = outcome;
    emit changed();
}

void SyncService::onServerChecked(const QString &version, const QString &compatibility)
{
    m_busy = false;
    m_serverVersion = version;
    m_serverCompatibility = compatibility;
    emit changed();
}

void SyncService::onUnits(const QJsonArray &units)
{
    // Pas de `m_busy` ici non plus : cette lecture n'a jamais rendu l'agent
    // occupé, elle n'a donc pas à le libérer — sinon elle libérerait une
    // opération réseau qui, elle, tourne encore.
    m_units = units;
    emit changed();
}

void SyncService::onCatalogue(const QJsonArray &adapters)
{
    m_adapters = adapters;
    emit changed();
}

void SyncService::onHistory(const QJsonObject &history)
{
    m_busy = false;
    m_history = history;
    emit changed();
}

void SyncService::onConflicts(const QJsonArray &conflicts)
{
    m_busy = false;
    m_conflicts = conflicts;
    emit changed();
}

void SyncService::onFinished(const PassSummary &summary)
{
    m_busy = false;
    m_last = summary;
    // La configuration peut avoir changé pendant la passe (dossier désigné
    // depuis l'interface) : on la relit plutôt que de la supposer figée.
    m_configuration = loadConfiguration();
    if (m_configuration.complete() && !m_periodic.isActive())
        m_periodic.start();
    emit changed();
    // Une passe change ce que le carnet contient : l'interface doit le voir
    // sans avoir à le redemander.
    emit unitsRequested(m_configuration);
}

void SyncService::onConnected(const QString &deviceId, const QString &url, const QString &token)
{
    saveConnection(url, token, deviceId);
    m_configuration = loadConfiguration();
    m_busy = false;
    if (m_configuration.complete())
        m_periodic.start();
    emit changed();
}

void SyncService::onFailed(const QString &message)
{
    m_busy = false;
    m_problem = message;
    emit changed();
}

} // namespace retrosave::agent

#include "syncservice.moc"
