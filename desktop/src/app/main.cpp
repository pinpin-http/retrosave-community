#include "app/agentclient.h"
#include "app/instanceguard.h"
#include "app/preferences.h"
#include "app/trayshell.h"
#include "ipc/protocol.h"

// Le canal de pilotage des tests n'existe que dans une compilation dédiée :
// `-DRSC_WITH_TEST_DRIVER=ON`. Le livrable ne contient pas une ligne de ce code.
#ifdef RETROSAVE_TEST_DRIVER
#include "testdriver/localtestserver.h"
#include <Spix/QtQmlBot.h>
#endif

#include <QApplication>
#include <QCommandLineParser>
#include <QDebug>
#include <QFont>
#include <QQmlApplicationEngine>
#include <QQmlError>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTimer>
#include <memory>

namespace
{
// Point d'entrée commun à la zone de notification et au transfert d'instance.
void present(QQuickWindow *window)
{
    if (!window)
        return;
    window->show();
    window->raise();
    window->requestActivate();
}
} // namespace

int main(int argc, char *argv[])
{
    // QApplication (et non QGuiApplication) : la zone de notification et son
    // menu contextuel appartiennent au module Widgets de Qt.
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName("RetroSave Desktop");
    QCoreApplication::setApplicationVersion(RETROSAVE_VERSION);
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption(
        {"socket", "Canal local de l'agent.", "name", retrosave::ipc::defaultEndpoint()});
    parser.addOption({"smoke-test", "Charge QML puis quitte, sans contacter ni démarrer d'agent."});
    parser.addOption({"smoke-screenshot", "Capture de la fenêtre pendant --smoke-test.", "path"});
#ifdef RETROSAVE_TEST_DRIVER
    // Deuxième barrière : même compilé, le canal ne s'ouvre QUE si un port est
    // demandé explicitement. Un lancement ordinaire de ce binaire de test reste
    // une application ordinaire.
    parser.addOption({"test-driver-port",
                      "Ouvre le canal de pilotage des tests sur 127.0.0.1:<port>.", "port"});
#endif
    parser.process(app);
    // Comme pour l'agent, la demande de sortie de Qt arrive avant app.exec().
    // Il faut donc retourner explicitement pour que --version ne lance pas la
    // fenêtre et ne reste pas ouvert dans un script d'installation.
    if (parser.isSet("version"))
        return 0;
    if (parser.isSet("smoke-screenshot") && !parser.isSet("smoke-test"))
        parser.showHelp(2);

    const auto endpoint = parser.value("socket");
    retrosave::InstanceGuard guard;
    if (!parser.isSet("smoke-test")) {
        switch (guard.claim(retrosave::ipc::windowEndpoint(endpoint))) {
        case retrosave::InstanceGuard::Outcome::HandedOver:
            // Une interface existait déjà : elle vient d'être rappelée à
            // l'écran, ce processus n'a plus rien à faire.
            return 0;
        case retrosave::InstanceGuard::Outcome::Unprotected:
            qWarning().noquote() << "Unicité non garantie :" << guard.errorString();
            break;
        case retrosave::InstanceGuard::Outcome::Owner:
            break;
        }
    }

    QQuickStyle::setStyle("Basic");

    // QML n'expose qu'une famille globale ; Qt choisit ici la première police
    // installée afin de conserver une métrique stable sans téléchargement.
    QFont interfaceFont;
    interfaceFont.setFamilies({"Plus Jakarta Sans", "Poppins", "Inter", "sans-serif"});
    QApplication::setFont(interfaceFont);
    QApplication::setWindowIcon(retrosave::TrayShell::applicationIcon());
    // L'engine est déclaré après les objets exposés à QML afin d'être détruit
    // avant eux et de ne jamais conserver de référence pendante.
    retrosave::AgentClient agent(endpoint);
    retrosave::TrayShell tray;
    retrosave::Preferences preferences;
    bool qmlWarning = false;
    QQmlApplicationEngine engine;
    // La connexion remplace le gestionnaire par défaut : réémettre chaque
    // avertissement conserve le diagnostic dans les journaux de CI.
    QObject::connect(&engine, &QQmlApplicationEngine::warnings, &app,
                     [&qmlWarning](const QList<QQmlError> &problems) {
                         qmlWarning = true;
                         for (const auto &problem : problems)
                             qWarning().noquote() << problem.toString();
                     });
    engine.setInitialProperties({{"agent", QVariant::fromValue(&agent)},
                                 {"tray", QVariant::fromValue(&tray)},
                                 {"preferences", QVariant::fromValue(&preferences)},
                                 {"appVersion", QStringLiteral(RETROSAVE_VERSION)}});
    engine.loadFromModule("RetroSave.Desktop", "Main");
    if (engine.rootObjects().isEmpty())
        return 1;
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());

#ifdef RETROSAVE_TEST_DRIVER
    // Déclaré après l'engine, le serveur de test s'arrête avant la scène QML.
    std::unique_ptr<retrosave::testdriver::LocalTestServer> driver;
    if (parser.isSet("test-driver-port")) {
        bool valid = false;
        const auto port = parser.value("test-driver-port").toUShort(&valid);
        if (!valid || port == 0) {
            qCritical().noquote() << "Port de pilotage invalide.";
            return 2;
        }
        driver = std::make_unique<retrosave::testdriver::LocalTestServer>(port);
        if (!driver->listening()) {
            qCritical().noquote() << "Canal de pilotage : port" << port << "indisponible.";
            return 2;
        }
        // QtQmlBot rejoue les événements dans le fil de l'interface.
        auto *bot = new spix::QtQmlBot(&app);
        bot->runTestServer(*driver);
        qInfo().noquote() << "Canal de pilotage des tests ouvert sur 127.0.0.1:" << port;
    }
    const bool drivenByTest = driver != nullptr;
#endif

    QObject::connect(&tray, &retrosave::TrayShell::showRequested, &app,
                     [window] { present(window); });
    QObject::connect(&guard, &retrosave::InstanceGuard::showRequested, &app,
                     [window] { present(window); });
    QObject::connect(&tray, &retrosave::TrayShell::stopAgentRequested, &agent,
                     &retrosave::AgentClient::stopAgent);
    // Quitter l'interface n'arrête pas l'agent : c'est l'autre entrée du menu.
    QObject::connect(&tray, &retrosave::TrayShell::quitRequested, &app, &QCoreApplication::quit);
    // Mémoriser le dernier état évite de répéter une notification à chaque
    // sondage, tout en laissant remonter deux erreurs distinctes.
    struct NotifiedState {
        int conflicts = -1;
        QString errors;
        QString restoredUnit;
        int restoredHead = -1;
    };
    auto seen = std::make_shared<NotifiedState>();
    QObject::connect(&agent, &retrosave::AgentClient::changed, &tray, [&agent, &tray, seen] {
        tray.describeAgent(agent.state() == "online", agent.agentPid());
        if (agent.state() != "online")
            return;

        const int conflicts = int(agent.conflicts().size());
        if (agent.notifyConflicts() && conflicts > 0 && conflicts != seen->conflicts)
            tray.notify(QCoreApplication::translate("main", "Conflict to resolve"),
                        QCoreApplication::translate(
                            "main", "%n save(s) changed on both sides. Both versions are "
                                    "kept: open RetroSave to choose.",
                            nullptr, conflicts));
        seen->conflicts = conflicts;

        const auto problem = agent.syncProblem();
        if (agent.notifyErrors() && !problem.isEmpty() && problem != seen->errors)
            tray.notify(QCoreApplication::translate("main", "Sync interrupted"),
                        problem);
        seen->errors = problem;

        // Une restauration est observable comme un changement de tête de
        // l'unité actuellement consultée.
        const auto unit = agent.historyUnit();
        const int head = agent.historyHead();
        if (agent.notifyRestores() && !unit.isEmpty() && head > 0 && unit == seen->restoredUnit
            && head != seen->restoredHead && seen->restoredHead > 0)
            tray.notify(QCoreApplication::translate("main", "Restore finished"),
                        QCoreApplication::translate("main", "“%1” is back to a previous "
                                                            "version. Nothing was deleted.")
                            .arg(unit));
        seen->restoredUnit = unit;
        seen->restoredHead = head;
    });

    QTimer poll;
    if (parser.isSet("smoke-test")) {
        // Attendre la fin des animations avant la capture de contrôle.
        const int settle = parser.isSet("smoke-screenshot") ? 1200 : 250;
        QTimer::singleShot(settle, &app, [&app, window, &parser] {
            if (parser.isSet("smoke-screenshot")) {
                if (!window || !window->grabWindow().save(parser.value("smoke-screenshot"))) {
                    app.exit(3);
                    return;
                }
            }
            app.quit();
        });
    } else {
        // Le pilote injecte des clics dans la vraie scène QML. Son
        // rafraîchissement après chaque action suffit ; le sondage périodique
        // pourrait désactiver un bouton entre l'injection et son traitement.
#ifdef RETROSAVE_TEST_DRIVER
        if (!drivenByTest) {
#endif
            QObject::connect(&poll, &QTimer::timeout, &agent, &retrosave::AgentClient::refresh);
            poll.start(5000);
#ifdef RETROSAVE_TEST_DRIVER
        }
#endif
        agent.refresh();
    }
    const int result = app.exec();
    return parser.isSet("smoke-test") && qmlWarning ? 2 : result;
}
