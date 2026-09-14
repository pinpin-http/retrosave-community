// Banc uniquement : exécute UNE passe de synchronisation complète contre un
// serveur réel et jetable, et rend son rapport en JSON.
//
// Pourquoi un exécutable plutôt qu'un test Qt : le banc doit faire dialoguer ce
// moteur avec le moteur Python sur le même serveur. Un processus par passe est
// aussi le seul moyen honnête de prouver la reprise — tuer le processus entre
// deux passes reproduit exactement ce que fait un plantage.
//
// Toutes les entrées viennent du harnais Python : jeton et URL présignées
// transitent par stdin, jamais par argv, jamais dans un journal.
#include "engine/folderscan.h"
#include "engine/pass.h"
#include "engine/v0adapter.h"
#include "state/store.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdio>

using namespace retrosave;

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QFile input;
    if (!input.open(stdin, QIODevice::ReadOnly))
        return 2;
    const auto config = QJsonDocument::fromJson(input.readAll()).object();

    network::ApiConfig api;
    api.serverUrl = QUrl(config.value("url").toString());
    api.token = config.value("token").toString().toLatin1();
    api.deviceId = config.value("device").toString().toLatin1();
    api.timeoutMs = 15000;
    api.allowHttp = true; // banc local jetable, jamais un défaut produit

    const auto root = config.value("root").toString();
    const auto staging = config.value("staging").toString();
    QDir().mkpath(staging);

    QJsonObject output;
    try {
        engine::V0SyncApi server(api);
        state::Store store(config.value("db").toString());
        // Le dossier générique peut accueillir une sauvegarde jamais vue ici :
        // c'est ce qui permet à un second poste de recevoir sans rien avoir
        // sur son disque au départ.
        engine::SyncPass pass(store, server, {{"folder", root}}, staging, {"folder"});
        // Le banc pilote la protection par un drapeau : ici tous les émulateurs
        // sont considérés ouverts, ou aucun. C'est suffisant pour ce banc, qui
        // ne surveille qu'une racine générique.
        const bool busy = config.value("emulator_running").toBool(false);
        const auto report = pass.run(engine::scanFolderRoot(root), config.value("now").toDouble(),
                                     [busy](const QString &) { return busy; });
        QJsonArray messages;
        for (const auto &message : report.messages)
            messages.append(message);
        output = QJsonObject{{"ok", true},
                             {"scanned", report.scanned},
                             {"pulled", report.pulled},
                             {"pushed", report.pushed},
                             {"duplicates", report.duplicates},
                             {"conflicts", report.conflicts},
                             {"reapplied", report.reapplied},
                             {"missing", report.missing},
                             {"errors", report.errors},
                             {"paused", report.pausedForMassiveDisappearance},
                             {"messages", messages}};
    } catch (const std::exception &error) {
        // Une passe ne doit jamais rendre une trace brute : le harnais veut un
        // verdict, et un message peut contenir n'importe quoi.
        output = QJsonObject{{"ok", false}, {"error", QString::fromUtf8(error.what())}};
    }
    fputs(QJsonDocument(output).toJson(QJsonDocument::Compact).constData(), stdout);
    return output.value("ok").toBool() ? 0 : 1;
}
