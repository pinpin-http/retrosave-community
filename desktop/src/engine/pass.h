// ─── La passe de synchronisation ──────────────────────────────────────────
// Le chef d'orchestre. Il ne calcule rien lui-même : il enchaîne, dans l'ordre
// figé du §9.3, les briques déjà écrites et testées séparément.
//
//   scan → reprise des écritures interrompues → PULL → PUSH → rapport
//
// **Pourquoi PULL avant PUSH.** Descendre d'abord minimise les conflits : si
// le serveur a du neuf et que nous aussi, prendre le sien d'abord évite de
// pousser sur une base périmée. L'ordre inverse fabriquerait des conflits que
// personne n'a demandés.
//
// **Une unité fautive n'arrête jamais la passe** (banc d'acceptation, cas 17).
// Chaque unité est traitée dans son propre bloc de rattrapage ; le rapport
// compte les erreurs, et les autres unités sont synchronisées quand même.
#pragma once

#include <functional>

#include "engine/ports.h"
#include "state/store.h"

#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <vector>

namespace retrosave::engine
{

// Une unité telle que la découverte vient de la voir sur le disque.
struct ScannedUnit {
    QString emulator;
    QString unitKey;
    QString unitType;
    QString gameKey;
    QString gameLabel;
    QString rootId;
    QString relPath;
};

struct PassReport {
    int scanned = 0;
    int pulled = 0;
    int pushed = 0;
    int duplicates = 0; // contenu déjà connu du serveur : rien à téléverser
    int conflicts = 0;
    int reapplied = 0; // écritures interrompues reprises
    int missing = 0;
    int errors = 0;
    // SYN-06 / SYN-07 : ce que l'utilisateur a mis de côté sur CET appareil.
    // Compté séparément des erreurs, parce que ce n'en est pas une : le
    // rapport doit pouvoir dire « rien n'a bougé, et c'est normal ».
    int skippedPaused = 0;
    int skippedExcluded = 0;
    // Protection §9.2 : au-delà de dix disparitions dans une même passe, on
    // suspend TOUT plutôt que de propager ce qui ressemble à un formatage.
    bool pausedForMassiveDisappearance = false;
    QStringList messages;
};

class SyncPass
{
  public:
    // `roots` associe un identifiant de racine à son chemin absolu : c'est ce
    // qui permet de retrouver une unité sur le disque à partir du carnet.
    // `placeableRoots` : les racines où une sauvegarde **jamais vue ici** peut
    // être déposée, parce que son chemin s'y déduit de la clé d'unité. C'est
    // vrai du dossier générique et de PPSSPP (un sous-dossier direct par
    // sauvegarde) ; ce ne l'est pas de Dolphin, dont les cartes vivent sous
    // `GC/<région>/Card A/`. Placer au mauvais endroit donnerait un fichier
    // qu'aucun émulateur ne lirait — on préfère ne rien placer.
    SyncPass(state::Store &store, SyncApi &api, QHash<QString, QString> roots,
             QString stagingDirectory, QSet<QString> placeableRoots = {},
             bool globalPause = false);

    // Rendre la réponse PAR ÉMULATEUR, et non pour la passe entière : PPSSPP
    // ouvert ne doit pas empêcher la capture d'une sauvegarde melonDS. Un
    // prédicat vide signifie « aucun émulateur ne tourne » — c'est ce que fait
    // Android, qui n'a pas cette détection (AD-16).
    using EmulatorRunning = std::function<bool(const QString &emulator)>;
    PassReport run(const std::vector<ScannedUnit> &scanned, double nowSeconds,
                   EmulatorRunning emulatorRunning = {});

  private:
    QString absolutePath(const state::UnitRecord &unit) const;
    void resumeInterruptedApply(state::UnitRecord &unit, PassReport &report);
    void pull(state::UnitRecord &unit, const RemoteUnit &remote, PassReport &report,
              const EmulatorRunning &emulatorRunning);
    void push(state::UnitRecord &unit, PassReport &report, double nowSeconds,
              bool emulatorRunning);

    state::Store &m_store;
    SyncApi &m_api;
    QHash<QString, QString> m_roots;
    QString m_staging;
    QSet<QString> m_placeable;
    // SYN-06 : la pause générale. Elle vit dans la configuration de l'agent et
    // non dans le carnet — c'est un réglage d'appareil, pas un état d'unité.
    bool m_globalPause = false;
};

} // namespace retrosave::engine
