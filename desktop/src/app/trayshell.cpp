#include "app/trayshell.h"

#include <QAction>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QSystemTrayIcon>

namespace retrosave
{

QIcon TrayShell::applicationIcon()
{
    // Icône peinte plutôt qu'un binaire commité : une seule source de vérité
    // pour la couleur, et rien à régénérer pour chaque taille demandée.
    QIcon icon;
    for (const int size : {16, 24, 32, 48, 64, 128}) {
        QPixmap pixmap(size, size);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor("#174d3f"));
        const qreal radius = size * 0.28;
        painter.drawRoundedRect(QRectF(0, 0, size, size), radius, radius);
        painter.setBrush(QColor("#f4f3ec"));
        const qreal margin = size * 0.26;
        painter.drawRoundedRect(QRectF(margin, margin, size - 2 * margin, size - 2 * margin),
                                size * 0.08, size * 0.08);
        painter.setBrush(QColor("#174d3f"));
        painter.drawRect(QRectF(size * 0.38, margin, size * 0.24, size * 0.2));
        icon.addPixmap(pixmap);
    }
    return icon;
}

TrayShell::TrayShell(QObject *parent) : QObject(parent)
{
    // Sur un bureau sans zone de notification — ou en mode hors écran pendant
    // les tests — on n'en crée aucune : `available` reste faux et l'interface
    // adapte son comportement de fermeture.
    if (!QSystemTrayIcon::isSystemTrayAvailable())
        return;

    m_tray = new QSystemTrayIcon(applicationIcon(), this);
    m_menu = std::make_unique<QMenu>();
    auto *menu = m_menu.get();

    auto *open = menu->addAction(tr("Open RetroSave"));
    connect(open, &QAction::triggered, this, &TrayShell::showRequested);
    m_stopAgent = menu->addAction(tr("Stop agent"));
    m_stopAgent->setEnabled(false);
    connect(m_stopAgent, &QAction::triggered, this, &TrayShell::stopAgentRequested);
    menu->addSeparator();
    auto *quit = menu->addAction(tr("Quit"));
    // Quitter la fenêtre n'arrête pas l'agent : les deux gestes restent
    // distincts et nommés comme tels.
    connect(quit, &QAction::triggered, this, &TrayShell::quitRequested);

    m_tray->setContextMenu(menu);
    m_tray->setToolTip(tr("RetroSave — agent unchecked"));
    // Une lambda peut recevoir les arguments du signal : ici la raison de
    // l'activation. Elle n'est pas obligée de les déclarer tous — Qt accepte
    // une lambda qui en prend moins que le signal n'en émet.
    connect(m_tray, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick)
                    emit showRequested();
            });
    m_tray->show();
}

// `= default` demande au compilateur d'écrire le destructeur habituel. Il doit
// être ICI et pas dans l'en-tête : voir le commentaire sur `m_menu`.
TrayShell::~TrayShell() = default;

void TrayShell::announceHidden()
{
    if (!m_tray)
        return;
    m_tray->showMessage(tr("RetroSave stays available"),
                        tr("La fenêtre est réduite dans la zone de notification. "
                           "L'agent, lui, continue de tourner séparément."),
                        applicationIcon(), 6000);
}

void TrayShell::describeAgent(bool online, const QString &pid)
{
    if (!m_tray)
        return;
    m_tray->setToolTip(online ? tr("RetroSave — agent running (process %1)").arg(pid)
                              : tr("RetroSave — agent unreachable"));
    if (m_stopAgent)
        m_stopAgent->setEnabled(online);
}

void TrayShell::notify(const QString &title, const QString &body)
{
    // `supportsMessages` est faux sur certains bureaux : mieux vaut ne rien
    // afficher que de croire avoir prévenu quelqu'un.
    if (!m_tray || !QSystemTrayIcon::supportsMessages())
        return;
    m_tray->showMessage(title, body, applicationIcon(), 8000);
}

} // namespace retrosave
