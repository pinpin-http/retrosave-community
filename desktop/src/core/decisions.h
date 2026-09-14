// « Faut-il envoyer cette sauvegarde ? en télécharger une ? est-ce un
// conflit ? » Ce fichier ne fait que RÉPONDRE. Il n'ouvre aucun fichier,
// n'appelle aucun serveur, ne connaît ni disque ni réseau.
//
// C'est volontaire, et c'est ce qui rend ces règles comparables entre Python,
// Kotlin et C++ : elles ne prennent que des valeurs et n'en rendent que.
// Chaque fonction rejoue les mêmes vecteurs que les deux autres langages.
#pragma once

#include <QString>
#include <optional>
#include <vector>

namespace retrosave::core
{

// Une sauvegarde n'est capturée que si elle a cessé de bouger. L'émulateur
// écrit parfois en plusieurs fois : capturer au milieu donnerait une
// sauvegarde à moitié écrite, et personne ne s'en apercevrait avant de vouloir
// la recharger.
struct StabilityInput {
    QString state;                        // active | paused | missing | error
    std::optional<QString> observedQfHash; // empreinte vue au scan courant
    std::optional<QString> stableQfHash;   // empreinte vue au scan précédent
    std::optional<qint64> stableSinceMs;   // depuis quand elle ne bouge plus
    qint64 nowMs = 0;
    qint64 stabilizationMs = 10000;
    bool emulatorRunning = false;          // desktop seulement (Android n'a pas cette détection)
};
bool isStable(const StabilityInput &input);
// Nécessaire aussi lors de la reprise d’une écriture interrompue.
bool mayApplyWithEmulator(bool emulatorRunning);

// Dix unités qui disparaissent d'un coup, c'est un formatage ou un rançongiciel,
// pas dix parties effacées à la main. On met la synchronisation en pause plutôt
// que de propager la disparition.
bool shouldPauseForMissing(int missingCount, int threshold = 10);

// Une même clé de jeu trouvée à deux endroits différents : on ne devine pas
// laquelle est la bonne, on met l'unité en quarantaine et on demande.
QString quarantineState(const std::vector<QString> &relPaths);

// `catch` ne suffit pas : un crash natif ou un kill système n'exécutent aucun
// bloc de reprise. Le compteur d'échecs est donc écrit AVANT le traitement, et
// cette fonction dit ce qu'il faut faire de ce qu'on lit au tour suivant.
//
// Trois issues, jamais deux :
//   « proceed »    — sous le seuil, l'unité passe normalement ;
//   « rearm »      — au seuil, mais la tête serveur a CHANGÉ : le poison est le
//                    plus souvent lié à un contenu précis, donc on remet le
//                    compteur à zéro et on retente ;
//   « quarantine » — au seuil sur la MÊME tête : l'unité est écartée, la passe
//                    et l'application restent utilisables.
//
// Sans le troisième cas, une seule unité empoisonnée peut faire tomber le
// process à chaque passe, indéfiniment, et plus rien ne se synchronise.
QString quarantineDecision(int consecutiveFailures,
                           const std::optional<int> &lastFailureHead,
                           const std::optional<int> &headVersion, int threshold = 3);

// Le serveur résout le vrai titre d'un jeu 3DS à partir de son `titleid_low` ;
// un client qui sait lire `PARAM.SFO` résout celui d'un jeu PSP. Ces noms
// doivent redescendre — sinon la bibliothèque affiche `000f3000` là où le
// serveur sait écrire « Professor Layton and the Azran Legacy ».
//
// Deux refus, et un seul protège vraiment :
//
// 1. un libellé saisi À LA MAIN n'est jamais écrasé. C'est la garantie du §7 ;
// 2. un titre déjà résolu n'est pas remplacé par un identifiant brut. Un
//    client ancien envoie le serial ; le laisser gagner ferait régresser la
//    bibliothèque à chaque passe.
//
// Rend le libellé à écrire, ou rien s'il ne faut pas toucher. Même règle et
// mêmes vecteurs que `server/app/core/labels.py`
// (`tests/vectors/label_adoption.json`).
bool isRawLabel(const QString &label, const QString &unitKey);
std::optional<QString> adoptedLabel(const QString &storedLabel, const QString &storedSource,
                                    const QString &incomingLabel, const QString &unitKey);

// Deux demandes distinctes côté produit, une seule règle ici : cet appareil
// doit-il s'occuper de cette sauvegarde maintenant ?
//
//   « sync »           — oui, comme d'habitude ;
//   « skip_paused »    — non, l'utilisateur a mis en pause (ce jeu, ou tout) ;
//   « skip_excluded »  — non, ce jeu n'est pas sélectionné sur CET appareil.
//
// Trois propriétés, et chacune a une raison :
//
// 1. **C'est local et ça le reste.** Rien n'est envoyé au serveur, rien n'est
//    supprimé, ni ici ni ailleurs. Un autre appareil continue de synchroniser
//    la même sauvegarde sans rien savoir de cette décision. Mettre en pause
//    n'est pas se désinscrire, et se désinscrire n'est pas effacer.
// 2. **L'exclusion gagne sur la pause globale**, et la distinction est visible
//    dans le résultat : lever la pause générale ne doit pas remettre en route
//    un jeu que l'utilisateur avait retiré de cet appareil.
// 3. **Une valeur inconnue synchronise.** Un réglage illisible — fichier édité
//    à la main, version antérieure, base à moitié migrée — ne doit jamais
//    arrêter silencieusement la protection des sauvegardes. Synchroniser n'est
//    jamais destructeur (copie de sécurité, conflit explicite) ; croire à tort
//    qu'on est protégé, si.
//
// Vecteur partagé : `tests/vectors/local_sync_mode.json`.
QString localSyncDecision(const QString &mode, bool globalPause);

// Rend « blocked_conflict », « pull », ou rien du tout — auquel cas la table
// de décision habituelle reprend la main.
std::optional<QString> pendingBranchDecision(
    bool openConflict, const QString &localContentSha256,
    const std::optional<QString> &pendingBranchContentSha256);

// Si le processus est mort pendant l'écriture d'une unité, la passe suivante
// doit savoir quoi faire. Seule une correspondance exacte referme le marqueur ;
// tout le reste ré-applique, ce qui n'est jamais destructeur.
struct ApplyJournal {
    enum class Kind {
        Absent,    // aucune écriture n'était en cours
        Malformed, // marqueur incomplet : ne devrait pas arriver, on ré-applique
        Present,   // une écriture était en cours, voici ce qu'elle visait
    };
    Kind kind = Kind::Absent;
    QString expectedContentSha256;
};

ApplyJournal readApplyJournal(const std::optional<int> &version,
                              const std::optional<QString> &contentSha256,
                              const std::optional<double> &startedAt);

// Rend « no_journal », « close » ou « reapply ».
QString applyJournalDecision(const ApplyJournal &journal,
                             const std::optional<QString> &localContentSha256);

// `pathGuard` est l'étage LEXICAL de la protection contre le « zip-slip » :
// un chemin venu de l'extérieur — entrée d'archive, cible distante — ne doit
// jamais pouvoir désigner un endroit hors de l'unité. Il ne s'applique pas aux
// chemins que le scan a lui-même construits sous la racine.
//
// À ne pas confondre avec la garde des entrées d'archive, plus stricte : ici
// un segment « . » est accepté, car il ne fait pas sortir du dossier.
// Rend « ok » ou « reject ».
QString pathGuard(const QString &relPath);

// `renameOutcome` juge un renommage sur le nom RÉELLEMENT obtenu, jamais sur
// un code de retour. Certains systèmes « réussissent » en créant « nom (1).ext » :
// c'est une déviation, pas un succès, et l'appelant ne doit pas croire que sa
// cible a été écrite. Rend « accepted », « deviated » ou « refused ».
QString renameOutcome(const QString &requested, const std::optional<QString> &obtained);

// Le joueur ne doit pas démarrer une partie sur une sauvegarde périmée, jouer
// une heure, et découvrir le problème à la fusion. Cette règle ne rend qu'une
// liste de noms à afficher.
struct RemoteUnitView {
    QString emulator;
    QString unitKey;
    int headVersion = 0;
    QString state;
};
struct LocalUnitView {
    QString emulator;
    QString unitKey;
    int lastSyncedVersion = 0;
};
std::vector<QString> unitsBehindCloud(const QString &emulator,
                                      const std::vector<RemoteUnitView> &remote,
                                      const std::vector<LocalUnitView> &local);

} // namespace retrosave::core
