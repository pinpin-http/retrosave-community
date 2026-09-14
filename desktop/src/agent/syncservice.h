// L'agent doit rester joignable pendant qu'il synchronise : une passe peut
// durer des minutes (un transfert de 200 Mo), et l'interface doit pouvoir
// demander son état ou son arrêt pendant ce temps. La passe tourne donc dans
// un **fil de travail**, et ce fil est le seul à toucher au carnet, au réseau
// et aux sauvegardes.
//
// Le canal local, lui, ne fait jamais attendre : il répond « acceptée » ou
// « déjà en cours », et l'interface relit l'état quand elle veut.
//
// Le worker appartient à un QThread séparé. Les signaux en file d'attente sont
// l'unique passage entre les fils ; les objets d'état ne sont pas partagés.
#pragma once

#include "agent/configuration.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QThread>
#include <QTimer>

namespace retrosave::agent
{

// Le résultat d'une passe, réduit à ce qu'une interface peut montrer. Borné à
// dessein : le canal local a une taille de trame maximale, et un rapport
// bavard n'aiderait personne.
struct PassSummary {
    bool ran = false;
    QString finishedAt;
    int scanned = 0;
    int pulled = 0;
    int pushed = 0;
    int duplicates = 0;
    int conflicts = 0;
    int reapplied = 0;
    int missing = 0;
    int errors = 0;
    // SYN-06 / SYN-07 : mis de côté à la demande de l'utilisateur. Ce ne sont
    // pas des erreurs, et les compter comme telles ferait clignoter une alerte
    // pour un réglage volontaire.
    int skippedPaused = 0;
    int skippedExcluded = 0;
    bool paused = false;
    QString message;
};

class SyncService final : public QObject
{
    Q_OBJECT
  public:
    explicit SyncService(QObject *parent = nullptr);
    ~SyncService() override;

    // Relie ce poste à un serveur : enregistre l'appareil, puis range l'URL, le
    // jeton et l'identifiant obtenu. Le travail se fait dans le fil de travail ;
    // l'appelant est prévenu par `changed()`.
    void connectAccount(const QString &url, const QString &token, const QString &deviceName);

    // Demande une passe. Rend la raison du refus, ou une chaîne vide si elle a
    // été acceptée — l'appelant a besoin de le dire à l'utilisateur, pas de
    // deviner.
    QString requestSync();

    // Va rechercher les conflits ouverts. L'interface en a besoin dès qu'on
    // ouvre l'écran, sans attendre la passe périodique.
    QString refreshConflicts();

    // Tranche. Le perdant reste dans l'historique du serveur : c'est un
    // arbitrage, pas une suppression (invariant I3).
    QString resolveConflict(const QString &conflictId, int winner);

    // Les conflits connus, déjà mis en forme pour le canal local.
    QJsonArray conflicts() const { return m_conflicts; }

    // Les unités du carnet local. Elles viennent du disque, pas du serveur :
    // l'interface doit pouvoir montrer ce que cet appareil suit même hors ligne.
    QString refreshUnits();
    QJsonArray units() const { return m_units; }
    // Ce que RetroSave sait lire, et où il regarde. Publié en même temps que
    // les unités : les deux se lisent ensemble.
    QJsonArray adapters() const { return m_adapters; }

    // L'historique d'une unité, lui, vient du serveur : c'est lui qui garde
    // toutes les versions, y compris la branche perdante d'un conflit.
    QString fetchHistory(const QString &unitKey);
    QJsonObject history() const { return m_history; }

    // Revenir à une version. Le serveur en crée une NOUVELLE qui reprend le
    // contenu : rien n'est réécrit, l'aller-retour reste dans l'historique.
    QString restoreVersion(const QString &unitKey, int number);

    // Remettre en jeu une unité écartée après des échecs répétés. Écrit
    // le carnet, ne touche pas au serveur.
    QString retryUnit(const QString &unitKey);

    // Choisir l'image d'une unité, ou la retirer avec un chemin vide. Purement
    // décoratif : aucune décision de synchronisation n'en dépend.
    QString chooseArtwork(const QString &unitKey, const QString &imagePath);

    // Aucun appel réseau, aucune suppression : ni le fichier du joueur, ni la
    // moindre version sur le serveur. Un autre appareil n'en sait rien.
    QString setLocalMode(const QString &unitKey, const QString &mode);
    // La pause générale. Elle n'arrête pas l'agent : l'interface reste
    // consultable et la reprise est immédiate.
    QString setGlobalPause(bool paused);
    bool globalPause() const { return m_configuration.globalPause; }

    QString setNotifications(bool conflicts, bool errors, bool restores);
    bool notifyConflicts() const { return m_configuration.notifyConflicts; }
    bool notifyErrors() const { return m_configuration.notifyErrors; }
    bool notifyRestores() const { return m_configuration.notifyRestores; }

    QString refreshDevices();
    QString renameDevice(const QString &deviceId, const QString &name);
    QString revokeDevice(const QString &deviceId);
    QJsonArray devices() const { return m_devices; }

    QString refreshActivity();
    QJsonArray activity() const { return m_activity; }
    // Écrit un rapport lisible à l'emplacement demandé. Sans jeton, sans URL
    // complète, sans le moindre contenu de sauvegarde.
    QString exportDiagnostic(const QString &path);

    // Copie ce que cet appareil détient, tel quel, dans un dossier choisi.
    // Ne lit rien d'autre, n'efface rien, ne touche pas au serveur.
    QString exportData(const QString &path);

    QString checkServer();
    QString serverVersion() const { return m_serverVersion; }
    QString serverCompatibility() const { return m_serverCompatibility; }

    // Le résultat de la dernière opération longue destinée à l'interface
    // (export, diagnostic) : un chemin, ou une phrase d'échec.
    QString lastOutcome() const { return m_outcome; }

    bool busy() const { return m_busy; }
    bool connected() const { return m_configuration.connected(); }
    bool ready() const { return m_configuration.complete(); }
    QString root() const { return m_configuration.root; }
    QString problem() const { return m_problem; }
    PassSummary lastPass() const { return m_last; }

  signals:
    void changed();
    // Demandes internes au fil de travail. Non destinées à l'extérieur.
    void passRequested(retrosave::agent::Configuration configuration);
    void connectionRequested(QString url, QString token, QString deviceName);
    void conflictsRequested(retrosave::agent::Configuration configuration);
    void decisionRequested(retrosave::agent::Configuration configuration, QString conflictId,
                           int winner);
    void unitsRequested(retrosave::agent::Configuration configuration);
    void historyRequested(retrosave::agent::Configuration configuration, QString unitKey);
    void retryRequested(retrosave::agent::Configuration configuration, QString unitKey);
    void artworkRequested(retrosave::agent::Configuration configuration, QString unitKey,
                          QString imagePath);
    void restoreRequested(retrosave::agent::Configuration configuration, QString unitKey,
                          int number);
    void localModeRequested(retrosave::agent::Configuration configuration, QString unitKey,
                            QString mode);
    void devicesRequested(retrosave::agent::Configuration configuration);
    void deviceRenameRequested(retrosave::agent::Configuration configuration, QString deviceId,
                               QString name);
    void deviceRevokeRequested(retrosave::agent::Configuration configuration, QString deviceId);
    void activityRequested(retrosave::agent::Configuration configuration);
    void diagnosticRequested(retrosave::agent::Configuration configuration, QString path);
    void exportRequested(retrosave::agent::Configuration configuration, QString path);
    void serverCheckRequested(retrosave::agent::Configuration configuration);

  private:
    void onFinished(const PassSummary &summary);
    void onConnected(const QString &deviceId, const QString &url, const QString &token);
    void onFailed(const QString &message);

    void onConflicts(const QJsonArray &conflicts);
    void onDevices(const QJsonArray &devices);
    void onActivity(const QJsonArray &activity);
    void onOutcome(const QString &outcome);
    void onServerChecked(const QString &version, const QString &compatibility);
    void onUnits(const QJsonArray &units);
    void onCatalogue(const QJsonArray &adapters);
    void onHistory(const QJsonObject &history);

    QThread m_thread;
    QTimer m_periodic;
    QJsonArray m_conflicts;
    QJsonArray m_units;
    QJsonArray m_adapters;
    QJsonObject m_history;
    QJsonArray m_devices;
    QJsonArray m_activity;
    QString m_outcome;
    QString m_serverVersion;
    // « inconnue », « compatible » ou « ecart » : l'interface traduit, le
    // moteur constate. Voir AD-31 — la comparaison ignore le suffixe de build.
    QString m_serverCompatibility = QStringLiteral("inconnue");
    Configuration m_configuration;
    PassSummary m_last;
    QString m_problem;
    bool m_busy = false;
};

} // namespace retrosave::agent

Q_DECLARE_METATYPE(retrosave::agent::Configuration)
Q_DECLARE_METATYPE(retrosave::agent::PassSummary)
