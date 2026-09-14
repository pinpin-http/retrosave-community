#pragma once

#include <QObject>
#include <QString>
#include <QUrl>
#include <QVariantMap>

class QProcess;

namespace retrosave
{

// Préférences propres au poste. L'agent démarre avec une unité systemd user
// sous Linux et une tâche planifiée sous Windows ; la clé Run n'est pas utilisée
// afin que l'activation reste visible et révocable par les outils du système.
class Preferences : public QObject
{
    Q_OBJECT
    // « unknown » tant que le système n'a pas répondu : sous Windows, la
    // réponse vient d'un processus, et l'interface ne doit pas afficher un
    // état inventé pendant ce temps.
    Q_PROPERTY(QString autostart READ autostart NOTIFY changed)
    Q_PROPERTY(bool autostartBusy READ autostartBusy NOTIFY changed)
    Q_PROPERTY(QString problem READ problem NOTIFY changed)
    Q_PROPERTY(QString folder READ folder NOTIFY changed)
    Q_PROPERTY(QString folderProblem READ folderProblem NOTIFY changed)
    // Une racine par émulateur : clé = identifiant du connecteur, valeur =
    // chemin. Le dossier générique reste à part, il n'appartient à personne.
    Q_PROPERTY(QVariantMap roots READ roots NOTIFY changed)
  public:
    explicit Preferences(QObject *parent = nullptr);

    QString autostart() const { return m_autostart; }
    bool autostartBusy() const { return m_busy; }
    QString problem() const { return m_problem; }
    QString folder() const { return m_folder; }
    QString folderProblem() const { return m_folderProblem; }
    QVariantMap roots() const { return m_roots; }

    Q_INVOKABLE void setStartWithSession(bool enabled);
    Q_INVOKABLE void chooseFolder(const QUrl &url);
    Q_INVOKABLE void forgetFolder();
    // Une racine n'est enregistrée qu'après sélection explicite par l'utilisateur.
    Q_INVOKABLE void chooseRoot(const QString &emulator, const QUrl &url);
    Q_INVOKABLE void forgetRoot(const QString &emulator);

    // Exposés pour les tests : ce qui est écrit dans la session de
    // l'utilisateur doit être vérifiable sans dépendre d'un systemd vivant.
    static QString unitPath();
    static QString unitLinkPath();
    static QString unitText();

  signals:
    void changed();

  private:
    void refreshAutostart();
    void applyAutostart(bool enabled);
    void checkFolder();
#ifdef Q_OS_WIN
    QProcess *runPowerShell(const QString &script);
#endif

    QString m_autostart = "unknown";
    bool m_busy = false;
    QString m_problem;
    QString m_folder;
    QString m_folderProblem;
    QVariantMap m_roots;
};

} // namespace retrosave
