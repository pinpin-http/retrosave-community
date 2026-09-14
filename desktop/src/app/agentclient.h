#pragma once

#include <QHash>
#include <QJsonObject>
#include <QLocalSocket>
#include <QObject>
#include <QTimer>
#include <QVariantList>

namespace retrosave
{

// Adaptateur de présentation entre l'IPC de l'agent et QML. Il expose un état
// en lecture seule et ne prend aucune décision de synchronisation.
class AgentClient final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString state READ state NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)
    Q_PROPERTY(QString agentVersion READ agentVersion NOTIFY changed)
    Q_PROPERTY(QString agentPid READ agentPid NOTIFY changed)
    Q_PROPERTY(QString lastChecked READ lastChecked NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    // L'agent reste l'unique source de vérité sur l'état du moteur.
    Q_PROPERTY(bool linked READ linked NOTIFY changed)
    Q_PROPERTY(bool ready READ ready NOTIFY changed)
    Q_PROPERTY(bool syncing READ syncing NOTIFY changed)
    Q_PROPERTY(QString syncProblem READ syncProblem NOTIFY changed)
    Q_PROPERTY(QString lastSummary READ lastSummary NOTIFY changed)
    Q_PROPERTY(QString statPushed READ statPushed NOTIFY changed)
    Q_PROPERTY(QString statPulled READ statPulled NOTIFY changed)
    Q_PROPERTY(QString statConflicts READ statConflicts NOTIFY changed)
    Q_PROPERTY(QString statErrors READ statErrors NOTIFY changed)
    // Les conflits ouverts, prêts pour un Repeater. Chaque entrée porte les
    // deux côtés : on ne tranche pas entre deux identifiants.
    Q_PROPERTY(QVariantList conflicts READ conflicts NOTIFY changed)
    Q_PROPERTY(QVariantList units READ units NOTIFY changed)
    Q_PROPERTY(QVariantList adapters READ adapters NOTIFY changed)
    // L'historique de la sauvegarde consultée, et sa tête.
    Q_PROPERTY(QVariantList versions READ versions NOTIFY changed)
    Q_PROPERTY(QString historyUnit READ historyUnit NOTIFY changed)
    Q_PROPERTY(int historyHead READ historyHead NOTIFY changed)
    Q_PROPERTY(QVariantList devices READ devices NOTIFY changed)
    Q_PROPERTY(QVariantList activity READ activity NOTIFY changed)
    Q_PROPERTY(bool globalPause READ globalPause NOTIFY changed)
    Q_PROPERTY(bool notifyConflicts READ notifyConflicts NOTIFY changed)
    Q_PROPERTY(bool notifyErrors READ notifyErrors NOTIFY changed)
    Q_PROPERTY(bool notifyRestores READ notifyRestores NOTIFY changed)
    Q_PROPERTY(QString serverVersion READ serverVersion NOTIFY changed)
    Q_PROPERTY(QString serverCompatibility READ serverCompatibility NOTIFY changed)
    // Le résultat de la dernière opération longue : un chemin, ou un échec.
    Q_PROPERTY(QString outcome READ outcome NOTIFY changed)
  public:
    explicit AgentClient(QString endpoint, QObject *parent = nullptr);
    QString state() const { return m_state; }
    QString message() const { return m_message; }
    QString agentVersion() const { return m_version; }
    QString agentPid() const { return m_pid; }
    QString lastChecked() const { return m_lastChecked; }
    // Le transport peut avoir répondu avant que l'agent termine son travail.
    // Garder l'action désactivée dans les deux cas évite un second ordre reçu
    // pendant une synchronisation ou une résolution déjà en cours.
    bool busy() const { return m_pending || m_syncing; }
    bool linked() const { return m_linked; }
    bool ready() const { return m_ready; }
    bool syncing() const { return m_syncing; }
    QString syncProblem() const { return m_syncProblem; }
    QString lastSummary() const { return m_lastSummary; }
    QString statPushed() const { return m_stats.value("pushed", "0"); }
    QString statPulled() const { return m_stats.value("pulled", "0"); }
    QString statConflicts() const { return m_stats.value("conflicts", "0"); }
    QString statErrors() const { return m_stats.value("errors", "0"); }
    QVariantList conflicts() const { return m_conflicts; }
    QVariantList units() const { return m_units; }
    QVariantList adapters() const { return m_adapters; }
    QVariantList versions() const { return m_versions; }
    QString historyUnit() const { return m_historyUnit; }
    int historyHead() const { return m_historyHead; }
    QVariantList devices() const { return m_devices; }
    QVariantList activity() const { return m_activity; }
    bool globalPause() const { return m_globalPause; }
    bool notifyConflicts() const { return m_notifyConflicts; }
    bool notifyErrors() const { return m_notifyErrors; }
    bool notifyRestores() const { return m_notifyRestores; }
    QString serverVersion() const { return m_serverVersion; }
    QString serverCompatibility() const { return m_serverCompatibility; }
    QString outcome() const { return m_outcome; }

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void startAgent();
    Q_INVOKABLE void stopAgent();
    // Relier le compte. Le jeton ne fait que traverser : l'interface ne
    // l'écrit nulle part, c'est l'agent qui le range et le protège.
    Q_INVOKABLE void connectAccount(const QString &url, const QString &token,
                                    const QString &deviceName);
    Q_INVOKABLE void synchronize();
    Q_INVOKABLE void refreshConflicts();
    // `winner` est le numéro de version choisi par l'utilisateur. L'interface
    // ne propose jamais de « choisir automatiquement » : c'est sa partie.
    Q_INVOKABLE void resolveConflict(const QString &conflictId, int winner);
    Q_INVOKABLE void refreshUnits();
    Q_INVOKABLE void openHistory(const QString &unitKey);
    // `version` est choisie explicitement par l'utilisateur : l'interface ne
    // restaure jamais « la dernière bonne » d'elle-même.
    Q_INVOKABLE void restoreVersion(const QString &unitKey, int version);
    // Réarmement local d'une unité mise de côté après des échecs répétés.
    Q_INVOKABLE void retryUnit(const QString &unitKey);
    // Image d'une unité : un chemin vide la retire. Purement décoratif.
    Q_INVOKABLE void chooseArtwork(const QString &unitKey, const QString &imagePath);
    // « sync », « paused » ou « excluded ». Local à cet
    // appareil ; ne supprime jamais rien, ici comme à distance.
    Q_INVOKABLE void setLocalMode(const QString &unitKey, const QString &mode);
    Q_INVOKABLE void setGlobalPause(bool paused);
    Q_INVOKABLE void setNotifications(bool conflicts, bool errors, bool restores);
    Q_INVOKABLE void refreshDevices();
    Q_INVOKABLE void renameDevice(const QString &deviceId, const QString &name);
    Q_INVOKABLE void revokeDevice(const QString &deviceId);
    Q_INVOKABLE void refreshActivity();
    // Le diagnostic est expurgé par l'agent avant écriture.
    Q_INVOKABLE void exportDiagnostic(const QString &path);
    Q_INVOKABLE void exportData(const QString &path);
    Q_INVOKABLE void checkServer();

  signals:
    void changed();

  private:
    void send(const QString &method, const QJsonObject &extra = {});
    void receive();
    void fail(const QString &message);
    void finish();
    QString unavailableMessage() const;

    QString m_endpoint;
    QLocalSocket m_socket;
    QTimer m_timeout;
    QByteArray m_buffer;
    QString m_requestId;
    QString m_method;
    QString m_state = "offline";
    QString m_message;
    QString m_version;
    QString m_pid;
    QString m_lastChecked;
    bool m_pending = false;
    // Une action utilisateur arrivée pendant le sondage est envoyée dès que le
    // canal, limité à une requête, se libère.
    bool m_queued = false;
    QString m_queuedMethod;
    QJsonObject m_queuedExtra;
    bool m_linked = false;
    bool m_ready = false;
    bool m_syncing = false;
    QString m_syncProblem;
    QString m_lastSummary;
    QHash<QString, QString> m_stats;
    QVariantList m_conflicts;
    QVariantList m_units;
    QVariantList m_adapters;
    QVariantList m_versions;
    QString m_historyUnit;
    int m_historyHead = 0;
    QVariantList m_devices;
    QVariantList m_activity;
    bool m_globalPause = false;
    bool m_notifyConflicts = true;
    bool m_notifyErrors = true;
    bool m_notifyRestores = true;
    QString m_serverVersion;
    QString m_serverCompatibility;
    QString m_outcome;
    QJsonObject m_extra;
    // Un agent absent parce que l'utilisateur l'a arrêté n'est pas une panne :
    // l'interface doit le dire autrement qu'un agent jamais démarré.
    bool m_stoppedByUser = false;
};

} // namespace retrosave
