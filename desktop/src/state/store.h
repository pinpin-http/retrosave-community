// Ce que l'appareil se rappelle entre deux passes : les unités connues, ce
// qu'il en a vu au dernier scan, jusqu'où elles ont été synchronisées, et —
// le plus important — ce qui était en cours d'écriture au moment où le
// programme s'est arrêté.
//
// La base est propre à l'édition Community. Le schéma garde séparés l'état
// observé, l'état synchronisé et le journal d'application afin qu'un scan ne
// puisse pas effacer une information de reprise.
#pragma once

#include <QString>
#include <optional>
#include <stdexcept>
#include <vector>

namespace retrosave::state
{

class StoreError : public std::runtime_error
{
  public:
    using std::runtime_error::runtime_error;
};

// Une unité telle que l'appareil s'en souvient. Les `optional` distinguent
// « jamais renseigné » de « valeur nulle » : un `last_synced_version` à 0
// signifie « jamais synchronisée », pas « inconnu ».
struct UnitRecord {
    qint64 localId = 0;
    QString serverId; // vide tant que le serveur ne la connaît pas
    QString emulator;
    QString unitKey;
    QString unitType;
    QString gameKey;
    QString gameLabel;
    QString labelSource = "auto"; // « auto » ou « user » : un renommage manuel gagne
    QString rootId;
    QString relPath;
    std::optional<QString> observedQfHash;
    std::optional<double> observedAt;
    std::optional<QString> stableQfHash;
    std::optional<double> stableSince;
    int lastSyncedVersion = 0;
    std::optional<QString> lastSyncedContentSha256;
    QString state = "active"; // active | missing | paused | error | duplicate
    std::optional<QString> openConflictId;
    std::optional<int> pendingBranchNumber;
    std::optional<QString> pendingBranchContentSha256;
    // Les trois colonnes du journal d'application vont ensemble : écrites
    // ensemble, lues ensemble (voir core/decisions.h).
    std::optional<int> applyJournalVersion;
    std::optional<QString> applyJournalContentSha256;
    std::optional<double> applyJournalStartedAt;
    int consecutiveFailures = 0;
    std::optional<int> lastFailureHead;
    // Ce que l'UTILISATEUR a demandé pour cette sauvegarde sur CET appareil
    // (SYN-06, SYN-07) : « sync », « paused » ou « excluded ». À ne pas
    // confondre avec `state`, qui dit ce que la synchronisation CONSTATE.
    // Jamais envoyé au serveur : un autre appareil n'en sait rien.
    QString localMode = "sync";
};

class Store final
{
  public:
    explicit Store(const QString &databasePath);
    ~Store();
    Store(const Store &) = delete;
    Store &operator=(const Store &) = delete;

    // Enregistre une unité vue par la découverte, sans écraser ce que la
    // synchronisation en sait déjà. Un scan ne doit jamais faire oublier un
    // `last_synced` ni un renommage fait par l'utilisateur.
    UnitRecord rememberDiscovered(const QString &emulator, const QString &unitKey,
                                  const QString &unitType, const QString &gameKey,
                                  const QString &gameLabel, const QString &rootId,
                                  const QString &relPath);

    std::vector<UnitRecord> units() const;
    std::optional<UnitRecord> unit(const QString &emulator, const QString &unitKey) const;

    // Empreinte rapide observée à ce scan. La date de stabilité n'est remise à
    // maintenant que si l'empreinte a CHANGÉ : sinon le compteur des dix
    // secondes repartirait de zéro à chaque passage et rien ne serait jamais
    // jugé stable.
    void recordObservation(qint64 localId, const QString &quickFingerprint, double nowSeconds);

    // À écrire AVANT de toucher au disque, et à effacer après. C'est ce
    // marqueur qui permet à la passe suivante de savoir qu'une écriture a été
    // interrompue, et de la refaire.
    void openApplyJournal(qint64 localId, int version, const QString &contentSha256,
                          double startedAtSeconds);
    void closeApplyJournal(qint64 localId);

    void recordSynced(qint64 localId, int version, const QString &contentSha256);

    // AD-30 : compter AVANT de traiter, pour survivre à ce qui tue le process.
    //
    // L'incrément est écrit avant le traitement de l'unité et validé tout de
    // suite : c'est ce qui distingue ce compteur d'un `catch`. Un dépassement
    // mémoire, un crash natif de la couche zstd ou un kill système n'exécutent
    // aucun bloc de reprise — seule une écriture déjà commise survit.
    //
    // `headVersion` est la tête serveur du moment, absente si l'unité n'y
    // existe pas encore : la quarantaine ne vaut que pour la tête qui l'a
    // provoquée, et une nouvelle version réarme d'elle-même.
    void recordFailure(qint64 localId, const std::optional<int> &headVersion);

    // Une unité dont le tour s'achève repart de zéro — et c'est aussi le
    // réarmement manuel, celui du bouton « Réessayer ».
    void clearFailures(qint64 localId);

    // Un conflit ouvert et la branche que le serveur a rangée pour nous. Les
    // trois valeurs vont ensemble : sans l'identifiant, l'interface ne peut
    // rien proposer à l'utilisateur ; sans la branche, l'appareil perdant ne
    // saurait pas reconnaître sa propre version une fois le conflit tranché.
    void recordConflictBranch(qint64 localId, const QString &conflictId, int branchNumber,
                              const QString &contentSha256);
    // Le conflit n'est plus ouvert côté serveur. La branche en attente, elle,
    // RESTE : c'est elle qui autorise la réception de la tête résolue même si
    // le contenu local en diffère (AD-24).
    void clearOpenConflict(qint64 localId);
    // La convergence est faite : le contenu local est celui de la tête.
    void clearPendingBranch(qint64 localId);
    void setState(qint64 localId, const QString &state);
    // SYN-06 / SYN-07 : mettre en pause ou retirer une sauvegarde de cet
    // appareil. Purement local, purement réversible, et ne supprime rien — ni
    // le fichier du joueur, ni la moindre version sur le serveur.
    void setLocalMode(qint64 localId, const QString &mode);
    void setServerId(qint64 localId, const QString &serverId);
    // Le serveur ne connaît plus cette unité : on oublie son identifiant et
    // tout ce qu'on croyait synchronisé, pour qu'elle soit redéclarée et
    // republiée. Le CONTENU local n'est pas touché — c'est la comptabilité qui
    // était fausse, pas la sauvegarde.
    void forgetRemoteState(qint64 localId);
    void renameForDisplay(qint64 localId, const QString &label);
    // M8 §7 : adopter un libellé venu du serveur SANS le marquer « user ».
    // Distinct de `renameForDisplay`, qui enregistre un choix de l'utilisateur
    // et interdit toute amélioration ultérieure.
    void adoptDisplayLabel(qint64 localId, const QString &label);

    // Journal d'activité lisible par l'utilisateur (EXP-03). Volontairement
    // borné : un journal qui grossit sans fin finit par gêner plus qu'il n'aide.
    void appendActivity(const QString &event, const QString &detail,
                        std::optional<qint64> localId = std::nullopt);
    std::vector<QString> recentActivity(int limit = 50) const;

  private:
    QString m_connection;
};

} // namespace retrosave::state
