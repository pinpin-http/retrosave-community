#include "engine/pass.h"
#include "core/archive.h"
#include "core/decisions.h"
#include "core/identity.h"
#include "core/localfs.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QUuid>
#include <algorithm>

namespace retrosave::engine
{
namespace
{
using namespace retrosave::core;

// Identité du contenu tel qu'il est SUR LE DISQUE, ou rien si l'unité a
// disparu. C'est cette valeur qui décide s'il y a quelque chose à pousser.
std::optional<QString> contentOnDisk(const QString &unitType, const QString &path)
{
    if (!QFileInfo::exists(path))
        return std::nullopt;
    try {
        std::vector<ContentDigest> digests;
        for (const auto &node : listUnitFiles(unitType, path)) {
            const auto absolute = unitType == "file" ? path : QDir(path).filePath(node.relPath);
            QFile file(absolute);
            if (!file.open(QIODevice::ReadOnly))
                return std::nullopt;
            QCryptographicHash hash(QCryptographicHash::Sha256);
            if (!hash.addData(&file))
                return std::nullopt;
            digests.push_back(ContentDigest{node.relPath, node.sizeBytes,
                                            QString::fromLatin1(hash.result().toHex())});
        }
        if (unitType == "file")
            return digests.empty() ? std::nullopt
                                   : std::optional<QString>(digests.front().sha256Hex);
        return directoryContentSha256(digests);
    } catch (const std::exception &) {
        return std::nullopt;
    }
}
} // namespace

SyncPass::SyncPass(state::Store &store, SyncApi &api, QHash<QString, QString> roots,
                   QString stagingDirectory, QSet<QString> placeableRoots, bool globalPause)
    : m_store(store), m_api(api), m_roots(std::move(roots)),
      m_staging(std::move(stagingDirectory)), m_placeable(std::move(placeableRoots)),
      m_globalPause(globalPause)
{
}

QString SyncPass::absolutePath(const state::UnitRecord &unit) const
{
    const auto root = m_roots.value(unit.rootId);
    return root.isEmpty() ? QString() : QDir(root).filePath(unit.relPath);
}

PassReport SyncPass::run(const std::vector<ScannedUnit> &scanned, double nowSeconds,
                         SyncPass::EmulatorRunning emulatorRunning)
{
    PassReport report;
    report.scanned = int(scanned.size());
    QDir().mkpath(m_staging);

    // ── 1. Ce que le scan vient de voir entre dans le carnet ──────────────
    QStringList seenKeys;
    for (const auto &unit : scanned) {
        m_store.rememberDiscovered(unit.emulator, unit.unitKey, unit.unitType, unit.gameKey,
                                   unit.gameLabel, unit.rootId, unit.relPath);
        seenKeys << unit.emulator + "/" + unit.unitKey;
    }

    // ── 2. Les disparitions, avant toute autre décision ───────────────────
    // Rien n'est jamais propagé : une disparition locale ne supprime rien à
    // distance (PRO-06). Au-delà du seuil, on suspend même la passe.
    std::vector<state::UnitRecord> vanished;
    for (auto &unit : m_store.units()) {
        if (unit.state != "active")
            continue;
        if (!seenKeys.contains(unit.emulator + "/" + unit.unitKey))
            vanished.push_back(unit);
    }
    if (shouldPauseForMissing(int(vanished.size()))) {
        report.pausedForMassiveDisappearance = true;
        report.missing = int(vanished.size());
        for (auto &unit : vanished)
            m_store.setState(unit.localId, "paused");
        const auto message =
            QStringLiteral("%1 saves vanished at once: sync suspended, nothing was propagated.")
                .arg(vanished.size());
        report.messages << message;
        m_store.appendActivity("pause_protection", message);
        // On s'arrête ICI. Continuer reviendrait à agir sur un disque dont on
        // ne comprend pas l'état — formatage, disque débranché, rançongiciel.
        return report;
    }
    for (auto &unit : vanished) {
        // Une sauvegarde mise en pause ou retirée de cet appareil ne parle plus
        // au serveur — pas même pour signaler sa disparition. Elle a quand même
        // été COMPTÉE dans le seuil ci-dessus : la protection contre un
        // formatage regarde le disque, pas les préférences de synchronisation.
        if (localSyncDecision(unit.localMode, m_globalPause) != QLatin1String("sync"))
            continue;
        try {
            if (!unit.serverId.isEmpty())
                m_api.markMissing(unit.serverId);
            m_store.setState(unit.localId, "missing");
            ++report.missing;
            m_store.appendActivity("disparition", unit.unitKey, unit.localId);
        } catch (const ApiError &error) {
            ++report.errors;
            report.messages << QString::fromUtf8(error.what());
        }
    }

    // ── 3. L'état du serveur, une seule fois pour toute la passe ──────────
    std::vector<RemoteUnit> remote;
    QSet<QString> openConflicts;
    try {
        remote = m_api.listUnits();
        // Les conflits encore ouverts disent, par différence, lesquels
        // l'utilisateur a déjà tranchés depuis la passe précédente.
        for (const auto &conflict : m_api.openConflicts())
            openConflicts.insert(conflict.id);
    } catch (const ApiError &error) {
        ++report.errors;
        report.messages
            << QStringLiteral("Server unreachable: %1").arg(QString::fromUtf8(error.what()));
        return report;
    }
    QHash<QString, RemoteUnit> remoteByKey;
    for (const auto &unit : remote)
        remoteByKey.insert(unit.emulator + "/" + unit.unitKey, unit);

    // ── 3 bis. Accueillir ce que ce poste n'a jamais vu (AD-26) ───────────
    // Sans cette étape, un appareil neuf ne télécharge RIEN : la passe ne
    // parcourt que les unités déjà présentes dans son carnet, et un dossier
    // vide n'en contient aucune. C'est pourtant la promesse du produit — « ma
    // partie apparaît sur mon autre PC ».
    //
    // On ne place que dans une racine qui peut l'accueillir : le chemin doit se
    // déduire de la clé d'unité. Vrai du dossier générique et de PPSSPP, faux
    // de Dolphin, dont les cartes vivent sous `GC/<région>/Card A/`. Placer au
    // mauvais endroit donnerait un fichier qu'aucun émulateur ne lirait.
    for (const auto &candidate : remote) {
        if (candidate.state != "active" || candidate.headVersion <= 0)
            continue;
        if (m_store.unit(candidate.emulator, candidate.unitKey).has_value())
            continue;
        if (!m_placeable.contains(candidate.emulator) ||
            m_roots.value(candidate.emulator).isEmpty())
            continue;
        // La clé d'unité EST le chemin relatif dans ces racines-là ; la garde
        // lexicale refuse tout ce qui tenterait de sortir du dossier.
        if (pathGuard(candidate.unitKey) != QStringLiteral("ok"))
            continue;
        const auto placed = m_store.rememberDiscovered(
            candidate.emulator, candidate.unitKey, candidate.unitType,
            candidate.gameKey.isEmpty() ? candidate.unitKey : candidate.gameKey,
            candidate.gameLabel.isEmpty() ? candidate.unitKey : candidate.gameLabel,
            candidate.emulator, candidate.unitKey);
        m_store.setServerId(placed.localId, candidate.serverId);
        m_store.appendActivity("accueil", candidate.unitKey, placed.localId);
    }

    // ── 4. Puis chaque unité, dans son propre filet ───────────────────────
    for (auto unit : m_store.units()) {
        if (unit.state == "paused" || unit.state == "missing")
            continue;

        // ── Ce que l'utilisateur a demandé (SYN-06, SYN-07) ───────────────
        // Décidé par le noyau partagé, pas ici : Android rejoue le même
        // vecteur. On ne touche NI au carnet NI au serveur pour ces unités —
        // pas de scan appliqué, pas d'envoi, pas de réception. Le fichier du
        // joueur reste exactement où il est, et la version distante aussi.
        const auto local = localSyncDecision(unit.localMode, m_globalPause);
        if (local == QLatin1String("skip_paused")) {
            ++report.skippedPaused;
            continue;
        }
        if (local == QLatin1String("skip_excluded")) {
            ++report.skippedExcluded;
            continue;
        }

        // ── Casse-boucle (AD-30) ──────────────────────────────────────────
        // La tête serveur du moment : absente si l'unité n'y existe pas encore.
        // C'est elle qui borne la quarantaine — le poison est presque toujours
        // lié à un contenu précis.
        const auto known = remoteByKey.constFind(unit.emulator + "/" + unit.unitKey);
        const std::optional<int> headVersion = known != remoteByKey.constEnd()
                                                   ? std::optional<int>(known->headVersion)
                                                   : std::nullopt;
        // ── Le serveur a oublié cette unité ──────────────────────────────
        // Un `server_id` qui ne désigne plus rien, avec un `last_synced` qui
        // affirme que tout va bien : l'appareil ne republierait JAMAIS, et la
        // copie distante resterait absente en silence. Arrive après une
        // restauration de base antérieure, ou un changement de compte.
        //
        // Oublier la comptabilité suffit : le contenu local est intact, et la
        // suite de la passe redéclare puis republie normalement.
        if (!unit.serverId.isEmpty() && known == remoteByKey.constEnd()) {
            m_store.forgetRemoteState(unit.localId);
            m_store.appendActivity("oubli_serveur", unit.unitKey, unit.localId);
            unit = *m_store.unit(unit.emulator, unit.unitKey);
        }

        const auto verdict =
            quarantineDecision(unit.consecutiveFailures, unit.lastFailureHead, headVersion);
        if (verdict == "rearm") {
            m_store.clearFailures(unit.localId);
            unit = *m_store.unit(unit.emulator, unit.unitKey);
        } else if (verdict == "quarantine") {
            // Écartée : on ne la présente même plus au serveur. C'est toute la
            // différence entre « une erreur de plus à chaque passe » et « le
            // compteur cesse de monter ».
            if (unit.state != "error")
                m_store.setState(unit.localId, "error");
            ++report.errors;
            const auto message =
                unit.unitKey
                + QStringLiteral(" : repeated failures, quarantined — use Retry once fixed.");
            report.messages << message;
            m_store.appendActivity("erreur", message, unit.localId);
            continue;
        }
        // Écrit et validé AVANT le traitement : un crash natif ou un kill
        // n'exécuteraient aucun bloc de reprise, et seule une écriture déjà
        // commise survit à la mort du process.
        m_store.recordFailure(unit.localId, headVersion);

        try {
            // Un conflit que le serveur ne déclare plus ouvert a été tranché :
            // l'unité peut revivre. La branche en attente, elle, est conservée —
            // c'est elle qui autorisera la réception de la tête résolue même si
            // le contenu local en diffère (AD-24).
            if (unit.openConflictId && !openConflicts.contains(*unit.openConflictId)) {
                m_store.clearOpenConflict(unit.localId);
                unit = *m_store.unit(unit.emulator, unit.unitKey);
            }

            if (mayApplyWithEmulator(emulatorRunning && emulatorRunning(unit.emulator)))
                resumeInterruptedApply(unit, report);

            const auto path = absolutePath(unit);
            if (path.isEmpty())
                throw std::runtime_error("unknown root for this save");

            // Empreinte rapide : elle ne lit aucun contenu et alimente
            // l'horloge de stabilisation.
            m_store.recordObservation(
                unit.localId, quickFingerprintHash(listUnitFiles(unit.unitType, path)), nowSeconds);
            unit = *m_store.unit(unit.emulator, unit.unitKey);

            if (known != remoteByKey.constEnd()) {
                if (unit.serverId.isEmpty()) {
                    m_store.setServerId(unit.localId, known->serverId);
                    unit.serverId = known->serverId;
                }
                // M8 §7 : le serveur connaît parfois un meilleur nom que nous —
                // il résout le titre d'un jeu 3DS depuis son `titleid_low`, que
                // le connecteur ne sait pas traduire. Sans cette adoption, la
                // bibliothèque affiche `000f3000` alors que le serveur sait
                // écrire « Professor Layton and the Azran Legacy ».
                const auto better =
                    adoptedLabel(unit.gameLabel, unit.labelSource, known->gameLabel, unit.unitKey);
                if (better) {
                    m_store.adoptDisplayLabel(unit.localId, *better);
                    unit.gameLabel = *better;
                }
                pull(unit, *known, report, emulatorRunning);
                unit = *m_store.unit(unit.emulator, unit.unitKey);
            }
            // Nouvelle observation des processus après le réseau : un émulateur
            // peut avoir démarré depuis le début de cette passe.
            push(unit, report, nowSeconds,
                 emulatorRunning ? emulatorRunning(unit.emulator) : false);
            // Le tour s'est achevé : le compteur d'échecs repart de zéro. Sans
            // cette remise, une unité tranquille qui n'a rien à faire finirait
            // par atteindre le seuil sans avoir jamais échoué.
            m_store.clearFailures(unit.localId);
        } catch (const std::exception &error) {
            // Une unité empoisonnée ne prend pas les autres avec elle. Son
            // compteur, lui, RESTE incrémenté : c'est ce qui la fera écarter au
            // troisième tour si elle échoue toujours sur la même tête.
            if (unit.state != "error")
                m_store.setState(unit.localId, "error");
            ++report.errors;
            const auto message = unit.unitKey + " : " + QString::fromUtf8(error.what());
            report.messages << message;
            m_store.appendActivity("erreur", message, unit.localId);
        }
    }
    return report;
}

void SyncPass::resumeInterruptedApply(state::UnitRecord &unit, PassReport &report)
{
    const auto journal = readApplyJournal(unit.applyJournalVersion, unit.applyJournalContentSha256,
                                          unit.applyJournalStartedAt);
    if (journal.kind == ApplyJournal::Kind::Absent)
        return;
    const auto path = absolutePath(unit);
    const auto decision = applyJournalDecision(journal, contentOnDisk(unit.unitType, path));
    if (decision == "close") {
        // L'écriture avait abouti ; seule la comptabilité manquait.
        m_store.recordSynced(unit.localId, *unit.applyJournalVersion,
                             *unit.applyJournalContentSha256);
        m_store.closeApplyJournal(unit.localId);
    } else {
        // Contenu déchiré ou cible absente : on redescendra la version visée.
        // Tant que le marqueur est ouvert, l'unité n'est JAMAIS poussée — ce
        // serait publier un contenu à moitié écrit.
        ++report.reapplied;
        m_store.appendActivity("reprise", unit.unitKey, unit.localId);
    }
    unit = *m_store.unit(unit.emulator, unit.unitKey);
}

void SyncPass::pull(state::UnitRecord &unit, const RemoteUnit &remote, PassReport &report,
                    const EmulatorRunning &emulatorRunning)
{
    const auto allowed = [&] {
        return mayApplyWithEmulator(emulatorRunning && emulatorRunning(unit.emulator));
    };
    if (!allowed())
        return;
    const bool interrupted = unit.applyJournalVersion.has_value();
    if (remote.headVersion <= unit.lastSyncedVersion && !interrupted)
        return;

    const auto path = absolutePath(unit);
    const auto local = contentOnDisk(unit.unitType, path);

    // Règle AD-24, écrite et vectorisée dans `decisions.*` : un conflit encore
    // ouvert bloque tout — c'est à l'utilisateur de trancher ; à l'inverse, un
    // contenu local qui est exactement la branche déjà rangée par le serveur
    // autorise la réception, même s'il diffère du dernier contenu synchronisé.
    // Sans ce second cas, l'appareil dont la branche a perdu resterait sur son
    // contenu pour toujours : le PULL le refuserait, et le PUSH aussi.
    const auto branch =
        pendingBranchDecision(unit.openConflictId.has_value(), local.value_or(QString()),
                              unit.pendingBranchContentSha256);
    if (branch == QStringLiteral("blocked_conflict"))
        return;
    const bool adoptResolvedHead = branch == QStringLiteral("pull");

    // Sinon, un contenu local différent du dernier contenu synchronisé est une
    // modification non poussée. On la laisse au PUSH, qui produira un conflit
    // propre — l'écraser ici perdrait la partie du joueur (§9.2).
    if (!interrupted && !adoptResolvedHead && local && unit.lastSyncedContentSha256 &&
        *local != *unit.lastSyncedContentSha256)
        return;

    const auto version = m_api.downloadVersion(unit.serverId, remote.headVersion, m_staging);
    struct Cleanup {
        QString path;
        ~Cleanup() { QFile::remove(path); }
    } cleanup{version.archivePath};

    // Notre dossier de travail, distinct de celui que l'extraction ouvre pour
    // elle-même. Effacé quoi qu'il arrive : une archive refusée ne laisse rien.
    const auto scratch =
        QDir(m_staging).filePath("apply-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
    QDir().mkpath(scratch);
    struct ScratchGuard {
        QString path;
        ~ScratchGuard() { QDir(path).removeRecursively(); }
    } scratchGuard{scratch};

    // L'identité de contenu est le verdict. Une archive qui ne correspond pas
    // lève ici, donc AVANT que le moindre fichier du joueur soit approché.
    //
    // **Le déplacement ci-dessous n'est pas un détail de confort.**
    // `extractArchiveTo` efface son dossier d'attente dès qu'elle rend la main :
    // les chemins reçus par ce rappel ne survivent pas à l'appel. Les garder
    // pour plus tard reviendrait à écrire depuis des fichiers qui n'existent
    // plus — et, pour une unité-dossier, l'écriture supprime la cible AVANT de
    // la réécrire. Le dossier du joueur serait donc vidé, puis rien n'y serait
    // remis. C'est exactement ce que faisait la première version de ce code.
    std::vector<StagedEntry> staged;
    int kept = 0;
    extractArchiveTo(version.archivePath, unit.unitType, version.contentSha256, scratch,
                     [&](const StagedEntry &entry) {
                         // Un renommage, pas une copie : les deux chemins sont
                         // sous le même dossier, donc sur le même système de
                         // fichiers. Une unité de 200 Mo ne se recopie pas pour
                         // le plaisir.
                         const auto held = QDir(scratch).filePath(QString::number(kept++));
                         if (!QFile::rename(entry.stagedPath, held))
                             throw LocalFsError("pending entry not kept: " +
                                                entry.relPath.toStdString());
                         staged.push_back({entry.relPath, held, entry.sha256Hex});
                     });

    // Le marqueur n'est ouvert qu'ICI, et c'est voulu : le verdict de contenu
    // est rendu, l'archive est acceptée, et la première écriture chez le joueur
    // n'a toujours pas eu lieu. L'ouvrir plus tôt laisserait, après le refus
    // d'une archive corrompue, une fausse trace d'écriture interrompue — et
    // cette trace bloquerait ensuite toute publication de cette unité.
    // Le téléchargement peut durer : relire les processus juste avant le
    // marqueur et l’écriture, sans transformer une attente normale en erreur.
    if (!allowed())
        return;
    m_store.openApplyJournal(unit.localId, remote.headVersion, version.contentSha256,
                             double(QDateTime::currentSecsSinceEpoch()));

    applyUnit(unit.unitType, path, staged);

    m_store.recordSynced(unit.localId, remote.headVersion, version.contentSha256);
    m_store.closeApplyJournal(unit.localId);
    // Le contenu local est désormais celui de la tête : la branche que le
    // serveur gardait pour nous n'est plus en attente de quoi que ce soit.
    m_store.clearPendingBranch(unit.localId);
    ++report.pulled;
    m_store.appendActivity("download", unit.unitKey, unit.localId);
}

void SyncPass::push(state::UnitRecord &unit, PassReport &report, double nowSeconds,
                    bool emulatorRunning)
{
    if (unit.applyJournalVersion.has_value())
        return; // une écriture interrompue n'est jamais republiée
    // Un conflit encore ouvert bloque toute publication (AD-24) : insister
    // fabriquerait un conflit de plus à chaque passe. Inutile de hacher le
    // contenu pour le savoir, la règle tranche sur le seul identifiant.
    if (pendingBranchDecision(unit.openConflictId.has_value(), QString(),
                              unit.pendingBranchContentSha256) ==
        QStringLiteral("blocked_conflict"))
        return;

    StabilityInput stability;
    stability.state = unit.state;
    stability.observedQfHash = unit.observedQfHash;
    stability.stableQfHash = unit.stableQfHash;
    stability.stableSinceMs =
        unit.stableSince ? std::optional<qint64>(qint64(*unit.stableSince * 1000)) : std::nullopt;
    stability.nowMs = qint64(nowSeconds * 1000);
    stability.emulatorRunning = emulatorRunning;
    if (!isStable(stability))
        return;

    const auto path = absolutePath(unit);
    const auto content = contentOnDisk(unit.unitType, path);
    if (!content)
        return;
    if (unit.lastSyncedContentSha256 && *unit.lastSyncedContentSha256 == *content)
        return; // rien n'a changé depuis la dernière fois
    // Notre contenu est exactement la branche que le serveur a déjà rangée : il
    // est donc conservé là-bas, et c'est la réception qui fera converger cet
    // appareil. Le republier créerait un doublon sans rien apporter.
    if (pendingBranchDecision(false, *content, unit.pendingBranchContentSha256).has_value())
        return;

    if (unit.serverId.isEmpty()) {
        m_store.setServerId(unit.localId,
                            m_api.declareUnit(unit.emulator, unit.unitKey, unit.unitType,
                                              unit.gameKey, unit.gameLabel));
        unit = *m_store.unit(unit.emulator, unit.unitKey);
    }

    const auto archivePath =
        QDir(m_staging).filePath("push-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
    const auto info =
        createArchiveFrom(unit.unitType, unitSources(unit.unitType, path), archivePath);
    // Nettoyage garanti : une archive de travail ne doit pas s'accumuler sur le
    // disque du joueur, même si le téléversement échoue.
    struct Cleanup {
        QString path;
        ~Cleanup() { QFile::remove(path); }
    } cleanup{archivePath};

    const auto prepared = m_api.prepare(unit.serverId, unit.lastSyncedVersion, info.contentSha256,
                                        info.archiveSha256, info.sizeBytes, info.archiveBytes);
    if (prepared.duplicate) {
        // Un autre appareil a déjà publié ce contenu : on avance seulement
        // notre comptabilité, sans rien téléverser.
        m_store.recordSynced(unit.localId, prepared.version, info.contentSha256);
        ++report.duplicates;
        return;
    }

    m_api.upload(prepared.uploadUrl, archivePath);
    const auto confirmed = m_api.confirm(unit.serverId, prepared.objectKey, info.contentSha256,
                                         info.archiveSha256, unit.lastSyncedVersion);
    if (confirmed.conflict) {
        // Le serveur a rangé notre contenu en branche. On NE réessaie PAS :
        // insister fabriquerait un conflit de plus à chaque passe.
        //
        // L'état de l'unité ne change PAS : « conflit » est une situation, pas
        // une étape du cycle de vie. L'écrire dans `state` la rendrait au
        // passage définitivement instable — donc impossible à republier une
        // fois l'utilisateur ayant tranché.
        ++report.conflicts;
        m_store.recordConflictBranch(unit.localId, confirmed.conflictId, confirmed.version,
                                     info.contentSha256);
        const auto message = unit.unitKey + " : conflict to resolve";
        report.messages << message;
        m_store.appendActivity("conflit", message, unit.localId);
        return;
    }
    m_store.recordSynced(unit.localId, confirmed.version, info.contentSha256);
    ++report.pushed;
    m_store.appendActivity("envoi", unit.unitKey, unit.localId);
}

} // namespace retrosave::engine
