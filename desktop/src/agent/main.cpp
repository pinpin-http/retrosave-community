#include "agent/syncservice.h"
#include "ipc/localservice.h"
#include "ipc/protocol.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDebug>
#include <QJsonObject>

using retrosave::agent::SyncService;

namespace
{
// Le rapport tel que l'interface le lira. Volontairement plat et borné : la
// trame du canal local est limitée, et un rapport bavard n'aide personne.
QJsonObject summarize(const SyncService &sync)
{
    const auto last = sync.lastPass();
    QJsonObject report{{"ran", last.ran},
                       {"at", last.finishedAt},
                       {"scanned", last.scanned},
                       {"pulled", last.pulled},
                       {"pushed", last.pushed},
                       {"duplicates", last.duplicates},
                       {"conflicts", last.conflicts},
                       {"reapplied", last.reapplied},
                       {"missing", last.missing},
                       {"errors", last.errors},
                       // SYN-06 / SYN-07 : mis de côté à la demande, donc ni
                       // erreurs ni oublis. Le rapport doit pouvoir dire
                       // « rien n'a bougé, et c'est normal ».
                       {"skipped_paused", last.skippedPaused},
                       {"skipped_excluded", last.skippedExcluded},
                       {"paused", last.paused}};
    if (!last.message.isEmpty())
        report.insert("message", last.message);
    return report;
}
} // namespace

int main(int argc, char *argv[])
{
    // QCoreApplication suffit à un agent sans interface : aucune dépendance
    // au serveur graphique, contrairement à QGuiApplication côté fenêtre.
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("retrosave-agent");
    QCoreApplication::setApplicationVersion(RETROSAVE_VERSION);
    QCommandLineParser parser;
    parser.setApplicationDescription("RetroSave — agent local de synchronisation");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({"socket", "Canal local (isolation des tests).", "name",
                      retrosave::ipc::defaultEndpoint()});
    parser.process(app);
    // Qt signale --version par QCoreApplication::exit(), ce qui n'arrête pas
    // une boucle qui n'a pas encore commencé. Sortir ici évite de démarrer
    // l'agent uniquement pour demander sa version.
    if (parser.isSet("version"))
        return 0;

    // L'agent possède le moteur : le carnet, le réseau et les écritures de
    // sauvegardes ne sont touchés que par lui, et jamais par l'interface.
    SyncService sync;

    // Le canal générique, plus un vocabulaire qui a maintenant un état à
    // consulter. `ipc::reply` reste la fonction pure : elle valide l'enveloppe
    // et répond au diagnostic et à l'arrêt ; ce qui demande l'état du moteur
    // est traité ici, là où cet état vit.
    retrosave::ipc::LocalService service([&sync](const QJsonObject &request) {
        auto base = retrosave::ipc::reply(request);
        const auto method = request.value("method").toString();
        const bool unknown =
            base.message.value("error").toString() == QLatin1String("unknown_method");
        if (!unknown) {
            if (method == "status" && base.message.contains("result")) {
                auto result = base.message.value("result").toObject();
                result.insert("state", sync.busy() ? "syncing" : "idle");
                result.insert("sync_available", true);
                result.insert("connected", sync.connected());
                result.insert("ready", sync.ready());
                result.insert("root", sync.root());
                result.insert("problem", sync.problem());
                result.insert("last", summarize(sync));
                // Les conflits voyagent dans le status : l'interface interroge
                // déjà l'agent régulièrement, inutile d'inventer un second
                // canal pour une liste bornée à quatre entrées.
                result.insert("conflicts", sync.conflicts());
                result.insert("units", sync.units());
                result.insert("adapters", sync.adapters());
                result.insert("history", sync.history());
                result.insert("devices", sync.devices());
                result.insert("activity", sync.activity());
                result.insert("global_pause", sync.globalPause());
                result.insert("notify_conflicts", sync.notifyConflicts());
                result.insert("notify_errors", sync.notifyErrors());
                result.insert("notify_restores", sync.notifyRestores());
                result.insert("server_version", sync.serverVersion());
                result.insert("server_compatibility", sync.serverCompatibility());
                result.insert("outcome", sync.lastOutcome());
                base.message.insert("result", result);
            }
            return base;
        }

        QJsonObject response{{"protocol", retrosave::ipc::ProtocolVersion},
                             {"id", request.value("id")}};
        if (method == "sync") {
            // La réponse ne dit pas « synchronisé » : elle dit « acceptée ».
            // Une passe peut durer des minutes, et le canal ne doit jamais
            // faire attendre l'interface.
            const auto refusal = sync.requestSync();
            response.insert("result",
                            QJsonObject{{"accepted", refusal.isEmpty()}, {"reason", refusal}});
        } else if (method == "conflicts") {
            const auto refusal = sync.refreshConflicts();
            response.insert("result",
                            QJsonObject{{"accepted", refusal.isEmpty()}, {"reason", refusal}});
        } else if (method == "resolve") {
            // Le gagnant est fourni EXPLICITEMENT par l'utilisateur : l'agent
            // ne choisit jamais à sa place, même quand une version semble
            // évidemment plus récente.
            const auto refusal = sync.resolveConflict(request.value("conflict_id").toString(),
                                                      request.value("winner").toInt());
            response.insert("result",
                            QJsonObject{{"accepted", refusal.isEmpty()}, {"reason", refusal}});
        } else if (method == "units") {
            const auto refusal = sync.refreshUnits();
            response.insert("result",
                            QJsonObject{{"accepted", refusal.isEmpty()}, {"reason", refusal}});
        } else if (method == "history") {
            const auto refusal = sync.fetchHistory(request.value("unit_key").toString());
            response.insert("result",
                            QJsonObject{{"accepted", refusal.isEmpty()}, {"reason", refusal}});
        } else if (method == "restore") {
            // La version visée est choisie EXPLICITEMENT : l'agent ne restaure
            // jamais « la dernière bonne » de sa propre initiative.
            const auto refusal = sync.restoreVersion(request.value("unit_key").toString(),
                                                     request.value("version").toInt());
            response.insert("result",
                            QJsonObject{{"accepted", refusal.isEmpty()}, {"reason", refusal}});
        } else if (method == "retry") {
            // AD-30 : remettre en jeu une unité mise de côté. Local, donc
            // accepté même serveur injoignable — la cause est le plus souvent
            // de ce côté-ci.
            const auto refusal = sync.retryUnit(request.value("unit_key").toString());
            response.insert("result",
                            QJsonObject{{"accepted", refusal.isEmpty()}, {"reason", refusal}});
        } else if (method == "artwork") {
            // Choisir l'image d'une unité, ou la retirer avec un chemin vide.
            // Décoratif : aucune décision de synchronisation n'en dépend, et un
            // refus laisse simplement le dessin de repli.
            const auto refusal = sync.chooseArtwork(request.value("unit_key").toString(),
                                                    request.value("path").toString());
            response.insert("result",
                            QJsonObject{{"accepted", refusal.isEmpty()}, {"reason", refusal}});
        } else if (method == "select") {
            // SYN-06 / SYN-07. Strictement local : rien n'est envoyé au
            // serveur, rien n'est supprimé — ni le fichier du joueur, ni la
            // moindre version distante.
            const auto refusal = sync.setLocalMode(request.value("unit_key").toString(),
                                                   request.value("mode").toString());
            response.insert("result",
                            QJsonObject{{"accepted", refusal.isEmpty()}, {"reason", refusal}});
        } else if (method == "pause_all") {
            const auto refusal = sync.setGlobalPause(request.value("paused").toBool());
            response.insert("result",
                            QJsonObject{{"accepted", refusal.isEmpty()}, {"reason", refusal}});
        } else if (method == "notifications") {
            const auto refusal = sync.setNotifications(request.value("conflicts").toBool(),
                                                       request.value("errors").toBool(),
                                                       request.value("restores").toBool());
            response.insert("result",
                            QJsonObject{{"accepted", refusal.isEmpty()}, {"reason", refusal}});
        } else if (method == "devices") {
            const auto refusal = sync.refreshDevices();
            response.insert("result",
                            QJsonObject{{"accepted", refusal.isEmpty()}, {"reason", refusal}});
        } else if (method == "device_rename") {
            const auto refusal = sync.renameDevice(request.value("device_id").toString(),
                                                   request.value("name").toString());
            response.insert("result",
                            QJsonObject{{"accepted", refusal.isEmpty()}, {"reason", refusal}});
        } else if (method == "device_revoke") {
            // Révoquer ne détruit rien : les versions publiées par cet appareil
            // restent dans l'historique (invariant I2).
            const auto refusal = sync.revokeDevice(request.value("device_id").toString());
            response.insert("result",
                            QJsonObject{{"accepted", refusal.isEmpty()}, {"reason", refusal}});
        } else if (method == "activity") {
            const auto refusal = sync.refreshActivity();
            response.insert("result",
                            QJsonObject{{"accepted", refusal.isEmpty()}, {"reason", refusal}});
        } else if (method == "diagnostic") {
            // EXP-04 : rapport volontaire, sans jeton et sans contenu de
            // sauvegarde. L'emplacement est choisi par l'utilisateur.
            const auto refusal = sync.exportDiagnostic(request.value("path").toString());
            response.insert("result",
                            QJsonObject{{"accepted", refusal.isEmpty()}, {"reason", refusal}});
        } else if (method == "export") {
            // PRO-08 : une COPIE vers un dossier ordinaire. La sauvegarde du
            // joueur reste où elle est, le serveur n'est pas touché.
            const auto refusal = sync.exportData(request.value("path").toString());
            response.insert("result",
                            QJsonObject{{"accepted", refusal.isEmpty()}, {"reason", refusal}});
        } else if (method == "server_check") {
            const auto refusal = sync.checkServer();
            response.insert("result",
                            QJsonObject{{"accepted", refusal.isEmpty()}, {"reason", refusal}});
        } else if (method == "connect") {
            sync.connectAccount(request.value("url").toString(), request.value("token").toString(),
                                request.value("device_name").toString());
            response.insert("result", QJsonObject{{"accepted", true}});
        } else {
            response.insert("error", "unknown_method");
        }
        return retrosave::ipc::Reply{response, false};
    });

    if (!service.listen(parser.value("socket"))) {
        qCritical().noquote() << service.errorString();
        return 2;
    }
    QObject::connect(&service, &retrosave::ipc::LocalService::stopRequested, &app, [] {
        qInfo() << "Arrêt demandé par l'interface locale.";
        // Sortie normale de la boucle : le destructeur ferme le canal puis
        // rend le verrou, dans cet ordre, comme après un Ctrl+C.
        QCoreApplication::quit();
    });
    qInfo() << "Agent prêt.";
    // La boucle d'événements reçoit les connexions ; aucune boucle de polling.
    return app.exec();
}
