// ─── Ce que le moteur attend du serveur ───────────────────────────────────
// Une interface, pas une implémentation. Deux raisons, dans cet ordre :
//
// 1. **La passe doit se lire de haut en bas**, comme la table de décision
//    qu'elle applique. Elle est donc écrite en style SYNCHRONE. L'adaptateur
//    réel enferme l'asynchronisme de Qt dans son propre fil d'exécution ;
//    l'agent, lui, reste répondant pendant toute la passe.
// 2. **La passe doit se tester sans serveur.** Un faux serveur en mémoire
//    permet de rejouer un conflit, une disparition ou une reprise après
//    plantage en quelques millisecondes, là où un vrai serveur rendrait ces
//    cas pénibles à provoquer — donc rarement testés.
#pragma once

#include <QString>
#include <stdexcept>
#include <vector>

namespace retrosave::engine
{

// Une panne du serveur ou du réseau. Elle interrompt UNE unité, jamais la
// passe entière (banc d'acceptation, cas 17).
class ApiError : public std::runtime_error
{
  public:
    using std::runtime_error::runtime_error;
};

struct RemoteUnit {
    QString serverId;
    QString emulator;
    QString unitKey;
    QString unitType;
    // Portés pour qu'une unité **jamais vue localement** puisse être créée
    // dans le carnet avec un libellé lisible et la bonne clé de regroupement.
    QString gameKey;
    QString gameLabel;
    int headVersion = 0;
    QString state; // active | missing | paused
    // Q45 : l'adresse d'une jaquette proposée par le serveur. Purement
    // décorative — aucune décision de synchronisation n'en dépend.
    QString artworkUrl;
};

struct PrepareOutcome {
    // Le serveur connaît déjà ce contenu : un autre appareil l'a poussé avant
    // nous. Rien à téléverser, on se contente d'avancer notre comptabilité.
    bool duplicate = false;
    int version = 0;
    QString uploadUrl;
    QString objectKey;
};

struct ConfirmOutcome {
    // Le serveur a rangé notre contenu en branche de conflit : notre base
    // n'était plus la tête. On NE réessaie PAS en boucle (§9.2).
    bool conflict = false;
    int version = 0;
    QString conflictId;
    int head = 0;
};

// Un côté d'un conflit, tel qu'on doit pouvoir le présenter à quelqu'un qui
// n'a pas envie de lire une empreinte : d'où il vient, quand, et combien il
// pèse. Les dates restent informatives (invariant I4) : elles ne décident rien,
// elles aident seulement à reconnaître SA partie.
struct ConflictSide {
    int number = 0;
    QString contentSha256;
    qint64 sizeBytes = 0;
    QString createdAt;
    QString originDevice;
};

struct OpenConflict {
    QString id;
    QString serverId;
    QString unitLabel;
    ConflictSide first;  // la tête au moment du conflit
    ConflictSide second; // la branche rangée par le serveur
};

// Une version de l'historique, telle qu'on doit pouvoir la présenter. `kind`
// distingue une publication ordinaire d'une restauration et d'une branche de
// conflit : c'est ce qui permet de dire « celle-ci a perdu un arbitrage, elle
// est toujours là » plutôt que d'aligner des numéros.
struct VersionRecord {
    int number = 0;
    QString kind;
    QString contentSha256;
    qint64 sizeBytes = 0;
    QString createdAt;
    QString originDevice;
};

struct UnitHistory {
    // La tête n'est PAS forcément le plus grand numéro : une branche de conflit
    // porte un numéro supérieur sans être la tête (Q7). Elle est donc donnée
    // explicitement, jamais déduite.
    int headVersion = 0;
    std::vector<VersionRecord> versions;
};

struct DownloadedVersion {
    QString archivePath;
    QString contentSha256;
};

class SyncApi
{
  public:
    virtual ~SyncApi() = default;
    virtual std::vector<RemoteUnit> listUnits() = 0;
    virtual QString declareUnit(const QString &emulator, const QString &unitKey,
                                const QString &unitType, const QString &gameKey,
                                const QString &gameLabel) = 0;
    // `archiveSha256` est exigée DÈS le prepare : le serveur la range avec la
    // réservation d'envoi et la compare au confirm. L'omettre ferait échouer
    // toute publication contre le vrai serveur.
    virtual PrepareOutcome prepare(const QString &serverId, int baseVersion,
                                   const QString &contentSha256, const QString &archiveSha256,
                                   qint64 sizeBytes, qint64 archiveBytes) = 0;
    virtual void upload(const QString &url, const QString &archivePath) = 0;
    virtual ConfirmOutcome confirm(const QString &serverId, const QString &objectKey,
                                   const QString &contentSha256, const QString &archiveSha256,
                                   int baseVersion) = 0;
    // Une version téléchargée : l'archive sur disque, ET l'empreinte de
    // contenu que le serveur lui attribue. Sans cette seconde valeur, le
    // client n'aurait rien contre quoi juger l'archive avant d'écrire.
    virtual DownloadedVersion downloadVersion(const QString &serverId, int number,
                                              const QString &stagingDirectory) = 0;
    // Signale une disparition locale. N'efface JAMAIS rien côté serveur.
    virtual void markMissing(const QString &serverId) = 0;
    // Les conflits encore ouverts, tels que le serveur les voit.
    //
    // Deux usages, et c'est pour cela que la liste porte les DEUX côtés plutôt
    // que de simples identifiants :
    //
    // 1. la passe s'en sert pour savoir ce qui a été tranché depuis la fois
    //    précédente. Sans elle, l'appareil dont la branche a perdu resterait
    //    bloqué pour toujours : rien ne lui apprendrait que l'utilisateur a
    //    décidé, et son contenu — pourtant conservé côté serveur — ne
    //    convergerait jamais ;
    // 2. l'interface les montre pour qu'on puisse trancher. Or on ne tranche
    //    pas entre deux identifiants : il faut voir quel appareil, quand, et
    //    quelle taille. C'est la route `GET /v0/conflicts?open=1`.
    virtual std::vector<OpenConflict> openConflicts() = 0;

    // Trancher. Le perdant N'EST PAS supprimé : il reste une version de
    // l'historique, téléchargeable et restaurable (invariant I3). C'est la
    // différence entre un arbitrage et une destruction, et c'est ce que
    // l'interface doit pouvoir promettre à l'utilisateur.
    virtual void resolveConflict(const QString &conflictId, int winner) = 0;

    virtual UnitHistory history(const QString &serverId) = 0;

    // Revenir à une version. Le serveur crée une NOUVELLE version qui reprend
    // le contenu de l'ancienne : rien n'est réécrit, rien n'est supprimé, et
    // l'aller-retour reste dans l'historique. Les autres appareils la reçoivent
    // par une réception ordinaire.
    virtual void restore(const QString &serverId, int number) = 0;
};

} // namespace retrosave::engine
