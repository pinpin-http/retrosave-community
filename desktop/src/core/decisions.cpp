#include "core/decisions.h"

#include <QStringList>
#include <algorithm>

namespace retrosave::core
{

bool mayApplyWithEmulator(bool emulatorRunning)
{
    return !emulatorRunning;
}

bool isStable(const StabilityInput &input)
{
    // Cinq conditions, toutes nécessaires. Écrites à plat plutôt qu'imbriquées :
    // la table de décision doit se relire aussi vite qu'elle se compare aux
    // deux autres implémentations.
    return input.state == "active" && input.observedQfHash.has_value() &&
           input.observedQfHash == input.stableQfHash && input.stableSinceMs.has_value() &&
           // Comparaison « au moins » : la limite exacte des dix secondes
           // compte comme stable. Un vecteur teste la milliseconde d'avant.
           input.nowMs - *input.stableSinceMs >= input.stabilizationMs && !input.emulatorRunning;
}

bool shouldPauseForMissing(int missingCount, int threshold)
{
    // On met en pause AU seuil, jamais une disparition plus tôt.
    return missingCount >= threshold;
}

QString quarantineState(const std::vector<QString> &relPaths)
{
    // Le même chemin listé deux fois n'est pas une collision : c'est deux fois
    // le même fichier. Seuls des chemins DISTINCTS rendent la clé ambiguë.
    std::vector<QString> distinct = relPaths;
    std::sort(distinct.begin(), distinct.end());
    distinct.erase(std::unique(distinct.begin(), distinct.end()), distinct.end());
    return distinct.size() > 1 ? QStringLiteral("duplicate") : QStringLiteral("active");
}

QString quarantineDecision(int consecutiveFailures, const std::optional<int> &lastFailureHead,
                           const std::optional<int> &headVersion, int threshold)
{
    if (consecutiveFailures < threshold)
        return QStringLiteral("proceed");
    // La quarantaine ne vaut que pour la tête qui l'a provoquée. Comparer des
    // optionnels compare aussi « absent » à « absent » : une unité qui a échoué
    // trois fois avant d'exister sur le serveur reste écartée, et c'est voulu.
    if (lastFailureHead != headVersion)
        return QStringLiteral("rearm");
    return QStringLiteral("quarantine");
}

std::optional<QString> pendingBranchDecision(
    bool openConflict, const QString &localContentSha256,
    const std::optional<QString> &pendingBranchContentSha256)
{
    // Un conflit ouvert bloque tout : c'est à l'utilisateur de trancher, et
    // pousser par-dessus fabriquerait un second conflit.
    if (openConflict)
        return QStringLiteral("blocked_conflict");
    // Notre contenu local est exactement la branche perdante déjà rangée par
    // le serveur : il n'y a rien à repousser, il faut prendre la tête résolue.
    if (pendingBranchContentSha256.has_value() && localContentSha256 == *pendingBranchContentSha256)
        return QStringLiteral("pull");
    return std::nullopt;
}

ApplyJournal readApplyJournal(const std::optional<int> &version,
                              const std::optional<QString> &contentSha256,
                              const std::optional<double> &startedAt)
{
    // Trois colonnes écrites ensemble, donc lues ensemble : un seul lecteur
    // pour un seul écrivain. Sans cela, une ligne partielle s'interpréterait
    // de trois façons différentes à trois endroits du code.
    const int present = int(version.has_value()) + int(contentSha256.has_value()) +
                        int(startedAt.has_value());
    if (present == 0)
        return {ApplyJournal::Kind::Absent, {}};
    if (present < 3)
        return {ApplyJournal::Kind::Malformed, {}};
    return {ApplyJournal::Kind::Present, *contentSha256};
}

QString applyJournalDecision(const ApplyJournal &journal,
                             const std::optional<QString> &localContentSha256)
{
    if (journal.kind == ApplyJournal::Kind::Absent)
        return QStringLiteral("no_journal");
    if (journal.kind == ApplyJournal::Kind::Malformed)
        return QStringLiteral("reapply");
    // Seule une correspondance exacte referme le marqueur. Tout le reste
    // ré-applique — ce n'est jamais destructeur : la copie `.rsc-bak` tient
    // toujours l'état d'avant, et une unité marquée n'est jamais poussée.
    if (localContentSha256.has_value() && *localContentSha256 == journal.expectedContentSha256)
        return QStringLiteral("close");
    return QStringLiteral("reapply");
}

QString pathGuard(const QString &relPath)
{
    if (relPath.isEmpty())
        return QStringLiteral("reject");
    // Les deux séparateurs sont ramenés à un seul : un chemin venu de Windows
    // ne doit pas contourner la garde en changeant d'antislash.
    const auto normalized = QString(relPath).replace('\\', '/');
    if (normalized.startsWith('/'))
        return QStringLiteral("reject");
    // « C:/… » : une lettre de lecteur est un chemin absolu déguisé.
    const auto head = normalized.section('/', 0, 0);
    if (head.size() >= 2 && head.at(1) == ':')
        return QStringLiteral("reject");
    const auto segments = normalized.split('/');
    if (std::any_of(segments.cbegin(), segments.cend(),
                    [](const QString &segment) { return segment == ".."; }))
        return QStringLiteral("reject");
    return QStringLiteral("ok");
}

QString renameOutcome(const QString &requested, const std::optional<QString> &obtained)
{
    if (!obtained.has_value())
        return QStringLiteral("refused");
    // Comparaison exacte, casse comprise : « Save0.bin » rendu « save0.bin »
    // est une déviation, pas un détail cosmétique.
    return *obtained == requested ? QStringLiteral("accepted") : QStringLiteral("deviated");
}

bool isRawLabel(const QString &label, const QString &unitKey)
{
    // Une seule règle couvre tous les connecteurs : un libellé brut n'est
    // qu'un morceau de l'identifiant. Azahar pose le `titleid_low`, qui EST la
    // clé ; PPSSPP pose le serial, qui en est le préfixe. Un vrai titre n'en
    // est pas un préfixe.
    return !label.isEmpty() && unitKey.startsWith(label);
}

std::optional<QString> adoptedLabel(const QString &storedLabel, const QString &storedSource,
                                    const QString &incomingLabel, const QString &unitKey)
{
    if (storedSource != QLatin1String("auto"))
        return std::nullopt;
    if (incomingLabel.isEmpty() || incomingLabel == storedLabel)
        return std::nullopt;
    // Déjà résolu contre brut : c'est une régression, pas une amélioration.
    if (!isRawLabel(storedLabel, unitKey) && isRawLabel(incomingLabel, unitKey))
        return std::nullopt;
    return incomingLabel;
}

QString localSyncDecision(const QString &mode, bool globalPause)
{
    // L'exclusion d'abord : elle survit à la pause générale, et le résultat
    // doit dire laquelle des deux raisons s'applique. Sans cet ordre, lever la
    // pause générale relancerait un jeu que l'utilisateur avait retiré de cet
    // appareil — exactement ce que SYN-07 interdit.
    if (mode == QLatin1String("excluded"))
        return QStringLiteral("skip_excluded");
    if (globalPause || mode == QLatin1String("paused"))
        return QStringLiteral("skip_paused");
    // Tout le reste — y compris une valeur vide ou inconnue — synchronise.
    // Un réglage illisible ne doit jamais interrompre la protection en silence.
    return QStringLiteral("sync");
}

std::vector<QString> unitsBehindCloud(const QString &emulator,
                                      const std::vector<RemoteUnitView> &remote,
                                      const std::vector<LocalUnitView> &local)
{
    std::vector<QString> behind;
    for (const auto &distant : remote) {
        if (distant.emulator != emulator || distant.state != "active")
            continue;
        const auto match = std::find_if(local.cbegin(), local.cend(), [&](const LocalUnitView &l) {
            return l.emulator == emulator && l.unitKey == distant.unitKey;
        });
        // Une unité absente localement n'est PAS en retard : elle est à placer
        // ou non configurée. Annoncer « une sauvegarde plus récente existe »
        // enverrait le joueur fermer un jeu pour un problème qui n'est pas le sien.
        if (match == local.cend())
            continue;
        if (distant.headVersion > match->lastSyncedVersion)
            behind.push_back(distant.unitKey);
    }
    std::sort(behind.begin(), behind.end());
    return behind;
}

} // namespace retrosave::core
