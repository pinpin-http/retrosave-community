#pragma once

#include <QIcon>
#include <QObject>
#include <QString>
#include <memory>

class QSystemTrayIcon;
class QAction;
class QMenu;

namespace retrosave
{

// Zone de notification et son repli. Toute la présentation sait interroger
// `available` : quand la zone n'existe pas, la fenêtre ne se cache jamais,
// sans quoi l'utilisateur perdrait tout moyen de revenir et de tout arrêter.
// Pas de `final` : qmltyperegistrar instancie un gabarit dérivé pour tout
// type exposé et constructible par défaut, même déclaré non créable en QML.
class TrayShell : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool available READ available CONSTANT)
  public:
    explicit TrayShell(QObject *parent = nullptr);
    ~TrayShell() override;
    bool available() const { return m_tray != nullptr; }
    // Appelée quand la fenêtre se réduit : l'utilisateur doit apprendre où
    // elle est partie, au moment où elle disparaît.
    Q_INVOKABLE void announceHidden();
    void describeAgent(bool online, const QString &pid);
    // Notification système sans contenu de sauvegarde ni
    // de chemin : un fait, et ce qu'il y a à faire. Sans zone de notification,
    // l'appel ne fait rien — l'information reste dans la fenêtre, qui est de
    // toute façon la source de vérité.
    void notify(const QString &title, const QString &body);
    static QIcon applicationIcon();

  signals:
    void showRequested();
    void stopAgentRequested();
    void quitRequested();

  private:
    QSystemTrayIcon *m_tray = nullptr;
    // QSystemTrayIcon ne possède pas son menu. Le destructeur est défini dans
    // le .cpp, où QMenu est complet, pour permettre ce unique_ptr.
    std::unique_ptr<QMenu> m_menu;
    QAction *m_stopAgent = nullptr;
};

} // namespace retrosave
