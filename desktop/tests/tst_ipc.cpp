#include "app/agentclient.h"
#include "app/preferences.h"
#include "ipc/protocol.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>
#include <QUuid>

// Les cas lancent un vrai processus agent afin de vérifier le protocole entre
// deux boucles d'événements et deux espaces mémoire distincts.
class IpcTest final : public QObject
{
    Q_OBJECT
  private:
    QProcess m_agent;
    QString m_endpoint;
    QTemporaryDir m_data;

    QString executable() const
    {
        QString name = "retrosave-agent";
#ifdef Q_OS_WIN
        name += ".exe";
#endif
        return QDir(QCoreApplication::applicationDirPath()).filePath(name);
    }

    bool start()
    {
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("XDG_DATA_HOME", m_data.path());
        // Isoler la configuration empêche le test de lire un serveur, un jeton
        // ou un dossier de sauvegarde du poste de développement.
        environment.insert("XDG_CONFIG_HOME", m_data.path());
#ifdef Q_OS_WIN
        // QStandardPaths ignore XDG_* sous Windows. Les deux variables Windows
        // isolent donc le carnet et les réglages avec la même garantie.
        environment.insert("APPDATA", m_data.path());
        environment.insert("LOCALAPPDATA", m_data.path());
#endif
        m_agent.setProcessEnvironment(environment);
        m_agent.start(executable(), {"--socket", m_endpoint});
        if (!m_agent.waitForStarted(10000))
            return false;
        QElapsedTimer deadline;
        deadline.start();
        while (deadline.elapsed() < 10000) {
            QLocalSocket probe;
            probe.connectToServer(m_endpoint);
            if (probe.waitForConnected(100))
                return true;
            QTest::qWait(20);
        }
        return false;
    }

    // La fenêtre est lancée hors écran et avec les mêmes dossiers isolés que
    // l'agent : aucun verrou ni réglage du poste n'est touché par les tests.
    void startWindow(QProcess &window, const QString &endpoint) const
    {
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("XDG_DATA_HOME", m_data.path());
        environment.insert("XDG_CONFIG_HOME", m_data.path());
#ifdef Q_OS_WIN
        environment.insert("APPDATA", m_data.path());
        environment.insert("LOCALAPPDATA", m_data.path());
#endif
        environment.insert("QT_QPA_PLATFORM", "offscreen");
        environment.insert("QT_QUICK_BACKEND", "software");
        window.setProcessEnvironment(environment);
        QString name = "retrosave-desktop";
#ifdef Q_OS_WIN
        name += ".exe";
#endif
        window.start(QDir(QCoreApplication::applicationDirPath()).filePath(name),
                     {"--socket", endpoint});
    }

    QByteArray exchangeWith(const QString &endpoint, const QByteArray &request) const
    {
        QLocalSocket socket;
        socket.connectToServer(endpoint);
        if (!socket.waitForConnected(2000))
            return {};
        socket.write(request);
        socket.waitForBytesWritten(1000);
        QByteArray response;
        QElapsedTimer deadline;
        deadline.start();
        while (!response.contains('\n') && deadline.elapsed() < 2000) {
            if (socket.bytesAvailable() || socket.waitForReadyRead(200))
                response += socket.readAll();
            else if (socket.state() == QLocalSocket::UnconnectedState)
                break;
        }
        return response;
    }

    QByteArray exchange(const QByteArray &request)
    {
        QLocalSocket socket;
        socket.connectToServer(m_endpoint);
        if (!socket.waitForConnected(1000))
            return {};
        socket.write(request);
        socket.waitForBytesWritten(1000);
        QByteArray response;
        QElapsedTimer deadline;
        deadline.start();
        while (!response.contains('\n') && deadline.elapsed() < 2000) {
            if (socket.bytesAvailable() || socket.waitForReadyRead(200))
                response += socket.readAll();
            else if (socket.state() == QLocalSocket::UnconnectedState)
                break;
        }
        return response;
    }

  private slots:
    void init()
    {
        QVERIFY(m_data.isValid());
        m_endpoint = "rsc-test-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
        QVERIFY2(start(), qPrintable(QString::fromUtf8(m_agent.readAllStandardError())));
    }

    void cleanup()
    {
        // Toujours nettoyer notre processus, même après un QVERIFY en échec.
        m_agent.kill();
        m_agent.waitForFinished(3000);
        QLocalServer::removeServer(m_endpoint);
        // Les fenêtres lancées par les tests tiennent le canal dérivé. Tué
        // brutalement, un processus ne retire pas son socket : sans cette
        // ligne, chaque exécution laisserait des fichiers dans le dossier
        // temporaire du poste.
        QLocalServer::removeServer(retrosave::ipc::windowEndpoint(m_endpoint));
    }

    void reportsActualProcessAndItsEngineState()
    {
        const auto bytes = exchange(retrosave::ipc::encode(retrosave::ipc::statusRequest("one")));
        const auto reply = QJsonDocument::fromJson(bytes).object();
        QCOMPARE(reply.value("id").toString(), "one");
        const auto result = reply.value("result").toObject();
        QCOMPARE(result.value("pid").toInteger(), m_agent.processId());
        QCOMPARE(result.value("version").toString(), RETROSAVE_VERSION);
        QCOMPARE(result.value("sync_available"), QJsonValue(true));
        // Réglages isolés : cet agent n'est relié à rien, et doit le dire.
        QCOMPARE(result.value("connected"), QJsonValue(false));
        QCOMPARE(result.value("ready"), QJsonValue(false));
        QCOMPARE(result.value("state").toString(), "idle");
        QVERIFY(result.value("root").toString().isEmpty());
    }

    void syncIsRefusedWithAReasonWhenNothingIsConfigured()
    {
        const auto bytes = exchange(retrosave::ipc::encode(retrosave::ipc::syncRequest("go")));
        const auto result = QJsonDocument::fromJson(bytes).object().value("result").toObject();
        QCOMPARE(result.value("accepted"), QJsonValue(false));
        // Un refus doit dire quoi faire. « false » tout seul enverrait
        // l'utilisateur chercher la panne au mauvais endroit.
        QVERIFY(result.value("reason").toString().contains("server"));

        // Et rien ne doit avoir été écrit : l'agent n'a pas de dossier désigné.
        QVERIFY(!QFileInfo::exists(QDir(m_data.path()).filePath("retrosave-community/state.db")));
    }

    void fragmentedRequestWaitsForDelimiter()
    {
        QLocalSocket socket;
        socket.connectToServer(m_endpoint);
        QVERIFY(socket.waitForConnected(1000));
        const auto frame = retrosave::ipc::encode(retrosave::ipc::statusRequest("fragmented"));
        socket.write(frame.left(12));
        QVERIFY(socket.waitForBytesWritten(1000));
        QVERIFY(!socket.waitForReadyRead(60));
        socket.write(frame.mid(12));
        QVERIFY(socket.waitForBytesWritten(1000));
        QVERIFY(socket.waitForReadyRead(1000));
        QCOMPARE(QJsonDocument::fromJson(socket.readAll()).object().value("id").toString(),
                 "fragmented");
    }

    void rejectsUnsupportedProtocolAndCommands()
    {
        auto request = retrosave::ipc::statusRequest("two");
        request.insert("protocol", 99);
        auto response = QJsonDocument::fromJson(exchange(retrosave::ipc::encode(request))).object();
        QCOMPARE(response.value("error").toString(), "unsupported_protocol");
        request.insert("protocol", 1);
        // Le vocabulaire s'est encore étoffé — `export` en fait partie depuis
        // PRO-08, et ce test l'utilisait justement comme exemple de verbe
        // inconnu. On prend donc un verbe que le produit ne peut pas acquérir :
        // RetroSave ne supprime rien, c'est l'invariant I2.
        request.insert("method", "supprimer_tout");
        response = QJsonDocument::fromJson(exchange(retrosave::ipc::encode(request))).object();
        QCOMPARE(response.value("error").toString(), "unknown_method");
        request.insert("id", "");
        response = QJsonDocument::fromJson(exchange(retrosave::ipc::encode(request))).object();
        QCOMPARE(response.value("error").toString(), "invalid_id");
    }

    void historyAndRestoreAreRefusedWithAReasonWhenNothingIsConfigured()
    {
        // Un agent sans serveur ne doit pas tenter l'appel : il doit dire quoi
        // faire. Ces refus arrivent AVANT toute requête réseau.
        auto reply =
            QJsonDocument::fromJson(exchange(retrosave::ipc::encode(
                                        retrosave::ipc::historyRequest("h", "ULUS10041DATA"))))
                .object()
                .value("result")
                .toObject();
        QCOMPARE(reply.value("accepted"), QJsonValue(false));
        QVERIFY(reply.value("reason").toString().contains("server"));

        reply =
            QJsonDocument::fromJson(exchange(retrosave::ipc::encode(
                                        retrosave::ipc::restoreRequest("r", "ULUS10041DATA", 2))))
                .object()
                .value("result")
                .toObject();
        QCOMPARE(reply.value("accepted"), QJsonValue(false));

        // Une restauration sans numéro n'est pas une restauration : refusée,
        // même reliée à un serveur.
        reply =
            QJsonDocument::fromJson(exchange(retrosave::ipc::encode(
                                        retrosave::ipc::restoreRequest("r2", "ULUS10041DATA", 0))))
                .object()
                .value("result")
                .toObject();
        QCOMPARE(reply.value("accepted"), QJsonValue(false));

        // La liste des unités, elle, vient du carnet local : elle est acceptée
        // même sans serveur, et rend une liste vide.
        const auto units = QJsonDocument::fromJson(
                               exchange(retrosave::ipc::encode(retrosave::ipc::unitsRequest("u"))))
                               .object()
                               .value("result")
                               .toObject();
        QCOMPARE(units.value("accepted"), QJsonValue(true));
    }

    void malformedAndOversizedRequestsDoNotKillAgent()
    {
        QVERIFY(exchange("not-json\n").isEmpty());
        QVERIFY(exchange(QByteArray(retrosave::ipc::MaxFrameBytes + 1, 'x')).isEmpty());
        QVERIFY(!exchange(retrosave::ipc::encode(retrosave::ipc::statusRequest("still-alive")))
                     .isEmpty());
    }

    void secondAgentCannotStealEndpoint()
    {
        QProcess second;
        second.setProcessEnvironment(m_agent.processEnvironment());
        second.start(executable(), {"--socket", m_endpoint});
        QVERIFY(second.waitForFinished(3000));
        QCOMPARE(second.exitCode(), 2);
        const auto result =
            QJsonDocument::fromJson(
                exchange(retrosave::ipc::encode(retrosave::ipc::statusRequest("owner"))))
                .object()
                .value("result")
                .toObject();
        QCOMPARE(result.value("pid").toInteger(), m_agent.processId());
    }

    void restartsAfterCrashWithStaleSocket()
    {
        m_agent.kill();
        QVERIFY(m_agent.waitForFinished(3000));
        QVERIFY(start());
        QVERIFY(
            !exchange(retrosave::ipc::encode(retrosave::ipc::statusRequest("restart"))).isEmpty());
    }

    void stalledRequestIsDisconnected()
    {
        QLocalSocket socket;
        socket.connectToServer(m_endpoint);
        QVERIFY(socket.waitForConnected(1000));
        socket.write("{"); // Aucun LF : le serveur doit borner cette attente.
        socket.waitForBytesWritten(1000);
        // Sous Windows, le named pipe peut déjà être passé à UnconnectedState
        // lorsque waitForDisconnected démarre ; la fonction rend alors false
        // malgré la bonne fermeture. Observer l'état couvre les deux ordres.
        QTRY_COMPARE_WITH_TIMEOUT(socket.state(), QLocalSocket::UnconnectedState, 6500);
        QVERIFY(!exchange(retrosave::ipc::encode(retrosave::ipc::statusRequest("after-timeout")))
                     .isEmpty());
    }

    void closingPresentationLeavesAgentRunning()
    {
        QProcess window;
        startWindow(window, m_endpoint);
        QVERIFY(window.waitForStarted(2000));
        QTest::qWait(500);
        QCOMPARE(window.state(), QProcess::Running);
        window.kill(); // Même une disparition brutale de l'UI ne tue pas l'agent.
        QVERIFY(window.waitForFinished(2000));
        const auto result =
            QJsonDocument::fromJson(
                exchange(retrosave::ipc::encode(retrosave::ipc::statusRequest("after-ui"))))
                .object()
                .value("result")
                .toObject();
        QCOMPARE(result.value("pid").toInteger(), m_agent.processId());
    }

    void presentationTracksConnectionAndLoss()
    {
        retrosave::AgentClient client(m_endpoint);
        client.refresh();
        QTRY_VERIFY_WITH_TIMEOUT(!client.busy(), 3500);
        QCOMPARE(client.state(), "online");
        QCOMPARE(client.agentPid(), QString::number(m_agent.processId()));
        const auto lastSuccess = client.lastChecked();
        QVERIFY(!lastSuccess.isEmpty());
        m_agent.kill();
        QVERIFY(m_agent.waitForFinished(3000));
        client.refresh();
        QTRY_VERIFY_WITH_TIMEOUT(!client.busy(), 3500);
        QCOMPARE(client.state(), "offline");
        QVERIFY(client.agentPid().isEmpty());
        QCOMPARE(client.lastChecked(), lastSuccess);
    }

    void shutdownAnswersThenReleasesChannelAndLock()
    {
        const auto bytes =
            exchange(retrosave::ipc::encode(retrosave::ipc::shutdownRequest("stop")));
        const auto reply = QJsonDocument::fromJson(bytes).object();
        QCOMPARE(reply.value("id").toString(), "stop");
        const auto result = reply.value("result").toObject();
        // L'accusé identifie le processus réellement arrêté, pas un nom générique.
        QCOMPARE(result.value("accepted"), QJsonValue(true));
        QCOMPARE(result.value("pid").toInteger(), m_agent.processId());
        // QProcess peut avoir déjà moissonné le processus : on observe l'état
        // final plutôt que waitForFinished, qui échoue sur un agent déjà sorti.
        QTRY_COMPARE_WITH_TIMEOUT(m_agent.state(), QProcess::NotRunning, 3000);
        QCOMPARE(m_agent.exitStatus(), QProcess::NormalExit);
        QCOMPARE(m_agent.exitCode(), 0);
        // Verrou et socket rendus : un agent neuf reprend le même canal.
        QVERIFY2(start(), "le canal reste occupé après un arrêt demandé");
        QVERIFY(
            !exchange(retrosave::ipc::encode(retrosave::ipc::statusRequest("reborn"))).isEmpty());
    }

    void shutdownIsRefusedWithAnInvalidEnvelope()
    {
        auto request = retrosave::ipc::shutdownRequest("bad-protocol");
        request.insert("protocol", 99);
        const auto response =
            QJsonDocument::fromJson(exchange(retrosave::ipc::encode(request))).object();
        QCOMPARE(response.value("error").toString(), "unsupported_protocol");
        // Un contrôle d'enveloppe raté ne doit jamais arrêter l'agent.
        QTest::qWait(200);
        QCOMPARE(m_agent.state(), QProcess::Running);
        QCOMPARE(QJsonDocument::fromJson(
                     exchange(retrosave::ipc::encode(retrosave::ipc::statusRequest("alive"))))
                     .object()
                     .value("result")
                     .toObject()
                     .value("pid")
                     .toInteger(),
                 m_agent.processId());
    }

    // Chaque action de l'interface doit ARRIVER telle qu'elle a été demandée.
    //
    // Le client reconstruit la trame à la connexion, méthode par méthode, et
    // cette chaîne retombait silencieusement sur `status` pour toute méthode
    // qu'elle ne connaissait pas. Résultat : « Réessayer » et le choix d'image
    // partaient en sondage — le bouton s'enfonçait, l'agent répondait
    // poliment, et rien ne se passait. Aucun test ne le voyait, parce que les
    // bancs parlaient au canal directement au lieu de passer par le client.
    //
    // Ce test écoute ce qui arrive VRAIMENT sur le canal.
    void everyClientActionArrivesAsItself()
    {
        const auto endpoint = m_endpoint + "-echo";
        QLocalServer::removeServer(endpoint);
        QLocalServer listener;
        listener.setSocketOptions(QLocalServer::UserAccessOption);
        QVERIFY(listener.listen(endpoint));

        QStringList received;
        // Le client et ce serveur vivent dans le MÊME fil : attendre en bloquant
        // ici figerait la boucle d'événements dont le client a besoin pour
        // envoyer sa trame. On écoute donc, on ne guette pas.
        connect(&listener, &QLocalServer::newConnection, this, [&listener, &received] {
            auto *peer = listener.nextPendingConnection();
            connect(peer, &QLocalSocket::readyRead, peer, [peer, &received] {
                if (!peer->canReadLine())
                    return;
                const auto request = QJsonDocument::fromJson(peer->readLine()).object();
                received << request.value("method").toString();
                // Une réponse que le client accepte : un `status` incomplet le
                // ferait basculer « hors ligne », et il refuserait alors toute
                // action — le test passerait à côté de ce qu'il vérifie.
                const auto method = request.value("method").toString();
                const QJsonObject result =
                    method == "status"
                        ? QJsonObject {{"version", "0.0-test"},
                                       {"pid", 4242},
                                       {"state", "idle"},
                                       {"sync_available", true},
                                       {"connected", true},
                                       {"ready", true}}
                        : QJsonObject {{"accepted", true}};
                const QJsonObject answer {{"protocol", retrosave::ipc::ProtocolVersion},
                                          {"id", request.value("id")},
                                          {"result", result}};
                peer->write(retrosave::ipc::encode(answer));
                peer->flush();
            });
        });

        retrosave::AgentClient client(endpoint);
        // `refresh` amène le client à « online » : les actions sont refusées
        // hors ligne, ce qui est le bon comportement mais masquerait le test.
        client.refresh();
        QTRY_VERIFY_WITH_TIMEOUT(!client.busy(), 3500);

        client.retryUnit("ULUS10041GAMEDATA");
        QTRY_VERIFY_WITH_TIMEOUT(!client.busy(), 3500);
        client.chooseArtwork("ULUS10041GAMEDATA", "/tmp/une-image.png");
        QTRY_VERIFY_WITH_TIMEOUT(!client.busy(), 3500);
        client.openHistory("ULUS10041GAMEDATA");
        QTRY_VERIFY_WITH_TIMEOUT(!client.busy(), 3500);
        // Les dix verbes ajoutés pour la V1. Ils sont vérifiés ICI, avec les
        // trois précédents, parce que c'est exactement l'oubli que ce test
        // existe pour attraper : une action non construite par le client part
        // en sondage, le bouton s'enfonce, et il ne se passe rien.
        client.setLocalMode("ULUS10041GAMEDATA", "paused");
        QTRY_VERIFY_WITH_TIMEOUT(!client.busy(), 3500);
        client.setGlobalPause(true);
        QTRY_VERIFY_WITH_TIMEOUT(!client.busy(), 3500);
        client.setNotifications(true, false, true);
        QTRY_VERIFY_WITH_TIMEOUT(!client.busy(), 3500);
        client.refreshDevices();
        QTRY_VERIFY_WITH_TIMEOUT(!client.busy(), 3500);
        client.renameDevice("11111111-1111-1111-1111-111111111111", "Console du salon");
        QTRY_VERIFY_WITH_TIMEOUT(!client.busy(), 3500);
        client.revokeDevice("11111111-1111-1111-1111-111111111111");
        QTRY_VERIFY_WITH_TIMEOUT(!client.busy(), 3500);
        client.refreshActivity();
        QTRY_VERIFY_WITH_TIMEOUT(!client.busy(), 3500);
        client.exportDiagnostic("/tmp/diagnostic.txt");
        QTRY_VERIFY_WITH_TIMEOUT(!client.busy(), 3500);
        client.exportData("/tmp/export");
        QTRY_VERIFY_WITH_TIMEOUT(!client.busy(), 3500);
        client.checkServer();
        QTRY_VERIFY_WITH_TIMEOUT(!client.busy(), 3500);

        for (const auto &method : {"retry", "artwork", "history", "select", "pause_all",
                                   "notifications", "devices", "device_rename", "device_revoke",
                                   "activity", "diagnostic", "export", "server_check"})
            QCOMPARE(received.count(QLatin1String(method)), 1);
        // Chaque verbe attendu exactement une fois prouve aussi qu'aucune
        // action n'a été transformée en `status`. Des sondages supplémentaires
        // sont légitimes : après une action, le client relit volontairement
        // l'état réel de l'agent et Windows peut laisser ces minuteurs arriver
        // entre deux assertions.
        // Et l'interface reste EN LIGNE : la réponse d'une action ne doit pas
        // être jugée comme un `status` mal formé, ce qui affichait une fausse
        // incompatibilité de versions à chaque « Réessayer ».
        QCOMPARE(client.state(), "online");
    }

    void presentationStopsAgentAndKeepsSayingSo()
    {
        retrosave::AgentClient client(m_endpoint);
        client.refresh();
        QTRY_VERIFY_WITH_TIMEOUT(!client.busy(), 3500);
        QCOMPARE(client.state(), "online");
        const auto stopped = client.message();
        client.stopAgent();
        QTRY_VERIFY_WITH_TIMEOUT(!client.busy(), 3500);
        QCOMPARE(client.state(), "offline");
        QVERIFY(client.agentPid().isEmpty());
        QVERIFY(client.message() != stopped);
        QVERIFY(!client.lastChecked().isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(m_agent.state(), QProcess::NotRunning, 3000);
        QCOMPARE(m_agent.exitCode(), 0);
        // Le contrôle périodique suivant ne doit pas requalifier cet arrêt
        // volontaire en panne : le message reste le même.
        const auto afterStop = client.message();
        client.refresh();
        QTRY_VERIFY_WITH_TIMEOUT(!client.busy(), 3500);
        QCOMPARE(client.state(), "offline");
        QCOMPARE(client.message(), afterStop);
    }

    void presentationExplainsAnAgentThatCannotStop()
    {
        // Agent factice figé sur l'ancien contrat : il ne connaît que status.
        const auto endpoint = m_endpoint + "-legacy";
        QLocalServer legacy;
        QVERIFY(legacy.listen(endpoint));
        connect(&legacy, &QLocalServer::newConnection, &legacy, [&legacy] {
            auto *socket = legacy.nextPendingConnection();
            connect(socket, &QLocalSocket::readyRead, socket, [socket] {
                const auto request =
                    QJsonDocument::fromJson(socket->readAll().split('\n').first()).object();
                QJsonObject response{{"protocol", retrosave::ipc::ProtocolVersion},
                                     {"id", request.value("id")}};
                if (request.value("method").toString() == "status")
                    response.insert("result", QJsonObject{{"version", "0.0.1"},
                                                          {"pid", 4242},
                                                          {"state", "idle"},
                                                          {"sync_available", true},
                                                          {"connected", false},
                                                          {"ready", false}});
                else
                    response.insert("error", "unknown_method");
                socket->write(retrosave::ipc::encode(response));
                socket->disconnectFromServer();
            });
        });
        retrosave::AgentClient client(endpoint);
        client.refresh();
        QTRY_VERIFY_WITH_TIMEOUT(!client.busy(), 3500);
        QCOMPARE(client.state(), "online");
        client.stopAgent();
        QTRY_VERIFY_WITH_TIMEOUT(!client.busy(), 3500);
        QCOMPARE(client.state(), "offline");
        // Le message doit désigner le processus à terminer soi-même.
        QVERIFY(client.message().contains("4242"));
    }

    void presentationRefusesAnAgentFromAnotherVersion()
    {
        // Un agent d'avant la synchronisation : il annonce l'ancien vocabulaire.
        // L'interface ne doit pas faire semblant de le comprendre — elle
        // afficherait un état inventé, et l'utilisateur cliquerait dans le vide.
        const auto endpoint = m_endpoint + "-ancien";
        QLocalServer old;
        QVERIFY(old.listen(endpoint));
        connect(&old, &QLocalServer::newConnection, &old, [&old] {
            auto *socket = old.nextPendingConnection();
            connect(socket, &QLocalSocket::readyRead, socket, [socket] {
                const auto request =
                    QJsonDocument::fromJson(socket->readAll().split('\n').first()).object();
                socket->write(
                    retrosave::ipc::encode({{"protocol", retrosave::ipc::ProtocolVersion},
                                            {"id", request.value("id")},
                                            {"result", QJsonObject{{"version", "0.0.1"},
                                                                   {"pid", 777},
                                                                   {"state", "prototype"},
                                                                   {"sync_available", false}}}}));
                socket->disconnectFromServer();
            });
        });
        retrosave::AgentClient client(endpoint);
        client.refresh();
        QTRY_VERIFY_WITH_TIMEOUT(!client.busy(), 3500);
        QCOMPARE(client.state(), "offline");
        QVERIFY(client.message().contains("versions"));
    }

    void presentationDoesNotSendShutdownWhileOffline()
    {
        m_agent.kill();
        QVERIFY(m_agent.waitForFinished(3000));
        retrosave::AgentClient client(m_endpoint);
        client.stopAgent();
        // Aucun échange n'est lancé : l'interface reste au repos.
        QVERIFY(!client.busy());
        QCOMPARE(client.state(), "offline");
    }

    void secondWindowIsHandedOverAndDoesNotDuplicateItself()
    {
        QProcess first;
        startWindow(first, m_endpoint);
        QVERIFY(first.waitForStarted(2000));
        const auto windowChannel = retrosave::ipc::windowEndpoint(m_endpoint);
        // On attend que la première fenêtre tienne réellement son canal.
        QTRY_VERIFY_WITH_TIMEOUT(
            !exchangeWith(windowChannel, retrosave::ipc::encode(retrosave::ipc::showRequest("up")))
                 .isEmpty(),
            5000);

        QProcess second;
        startWindow(second, m_endpoint);
        QVERIFY2(second.waitForFinished(6000),
                 qPrintable(QStringLiteral("second stdout: %1\nsecond stderr: %2")
                                .arg(QString::fromUtf8(second.readAllStandardOutput()),
                                     QString::fromUtf8(second.readAllStandardError()))));
        QCOMPARE(second.exitStatus(), QProcess::NormalExit);
        QCOMPARE(second.exitCode(), 0);
        // La première reste seule maîtresse de l'écran et du canal.
        QCOMPARE(first.state(), QProcess::Running);
        first.kill();
        QVERIFY(first.waitForFinished(3000));
    }

    void windowChannelAnswersOnlyItsOwnVocabulary()
    {
        QProcess window;
        startWindow(window, m_endpoint);
        QVERIFY(window.waitForStarted(2000));
        const auto channel = retrosave::ipc::windowEndpoint(m_endpoint);
        QByteArray bytes;
        QTRY_VERIFY_WITH_TIMEOUT(
            !(bytes = exchangeWith(channel,
                                   retrosave::ipc::encode(retrosave::ipc::showRequest("raise"))))
                 .isEmpty(),
            5000);
        auto response = QJsonDocument::fromJson(bytes).object();
        QCOMPARE(response.value("id").toString(), "raise");
        QCOMPARE(response.value("result").toObject().value("shown"), QJsonValue(true));
        // Le canal de la fenêtre n'est pas celui de l'agent : pas de status,
        // et surtout pas d'arrêt d'agent déclenché depuis cette adresse.
        response = QJsonDocument::fromJson(
                       exchangeWith(channel,
                                    retrosave::ipc::encode(retrosave::ipc::statusRequest("nope"))))
                       .object();
        QCOMPARE(response.value("error").toString(), "unknown_method");
        response =
            QJsonDocument::fromJson(
                exchangeWith(channel,
                             retrosave::ipc::encode(retrosave::ipc::shutdownRequest("nope"))))
                .object();
        QCOMPARE(response.value("error").toString(), "unknown_method");
        QCOMPARE(window.state(), QProcess::Running);
        window.kill();
        QVERIFY(window.waitForFinished(3000));
    }

    void autostartEntryReflectsWhatTheSystemHolds()
    {
#ifndef Q_OS_UNIX
        QSKIP("La tâche planifiée Windows s'enregistre dans la session réelle : "
              "elle est vérifiée par la recette manuelle, pas ici.");
#else
        const auto previous = qgetenv("XDG_CONFIG_HOME");
        QVERIFY(qputenv("XDG_CONFIG_HOME", m_data.path().toUtf8()));
        const auto unit = m_data.path() + "/systemd/user/retrosave-community-agent.service";
        const auto link = m_data.path() + "/systemd/user/default.target.wants/"
                                          "retrosave-community-agent.service";

        retrosave::Preferences preferences;
        QCOMPARE(preferences.autostart(), "off");
        preferences.setStartWithSession(true);
        QTRY_COMPARE_WITH_TIMEOUT(preferences.autostart(), QString("on"), 3000);
        QVERIFY(preferences.problem().isEmpty());
        QVERIFY(QFileInfo::exists(unit));
        // Le lien est ce que crée `systemctl --user enable` : sans lui, l'unité
        // existe mais ne démarre jamais, et l'interface mentirait.
        QVERIFY(QFileInfo(link).isSymLink());

        QFile file(unit);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto text = QString::fromUtf8(file.readAll());
        file.close();
        // L'unité désigne l'agent installé, pas un nom résolu via le PATH.
        QVERIFY(text.contains("retrosave-agent"));
        // Un arrêt demandé (code 0) doit rester un arrêt, et un canal déjà
        // servi (code 2) ne doit pas relancer en boucle.
        QVERIFY(text.contains("Restart=on-failure"));
        QVERIFY(text.contains("SuccessExitStatus=2"));

        // Un nouvel objet lit le système et retrouve le même état.
        QCOMPARE(retrosave::Preferences().autostart(), "on");

        preferences.setStartWithSession(false);
        QTRY_COMPARE_WITH_TIMEOUT(preferences.autostart(), QString("off"), 3000);
        QVERIFY(!QFileInfo::exists(unit));
        QVERIFY(!QFileInfo(link).isSymLink());
        // Désactiver deux fois n'est pas une erreur : rien à retirer.
        preferences.setStartWithSession(false);
        QTRY_COMPARE_WITH_TIMEOUT(preferences.autostart(), QString("off"), 3000);
        QVERIFY(preferences.problem().isEmpty());
        qputenv("XDG_CONFIG_HOME", previous);
#endif
    }

    void chosenFolderIsRememberedWithoutBeingOpened()
    {
        const auto previous = qgetenv("XDG_CONFIG_HOME");
        QVERIFY(qputenv("XDG_CONFIG_HOME", m_data.path().toUtf8()));
        QTemporaryDir target;
        QVERIFY(target.isValid());
        QFile hidden(QDir(target.path()).filePath("sauvegarde.bin"));
        QVERIFY(hidden.open(QIODevice::WriteOnly));
        hidden.write("contenu qui ne doit jamais être lu");
        hidden.close();
        // Contenu illisible : si l'interface ouvrait ou listait le dossier
        // pour le valider, ce cas le ferait échouer. Windows ne permet pas de
        // représenter « aucun droit » avec QFile::setPermissions ; la même
        // propriété y est couverte par le fait que chooseFolder ne reçoit que
        // le chemin et ne parcourt jamais son contenu.
#ifndef Q_OS_WIN
        QVERIFY(hidden.setPermissions({}));
#endif

        retrosave::Preferences preferences;
        QVERIFY(preferences.folder().isEmpty());
        preferences.chooseFolder(QUrl::fromLocalFile(target.path()));
        QCOMPARE(preferences.folder(), QDir::toNativeSeparators(target.path()));
        QVERIFY(preferences.folderProblem().isEmpty());
        // Le choix survit au redémarrage de l'interface.
        QCOMPARE(retrosave::Preferences().folder(), QDir::toNativeSeparators(target.path()));

        // Une adresse non locale est refusée sans écraser le choix existant.
        preferences.chooseFolder(QUrl("https://exemple.invalide/dossier"));
        QVERIFY(!preferences.folderProblem().isEmpty());
        QCOMPARE(preferences.folder(), QDir::toNativeSeparators(target.path()));

        preferences.forgetFolder();
        QVERIFY(preferences.folder().isEmpty());
        QVERIFY(retrosave::Preferences().folder().isEmpty());
#ifndef Q_OS_WIN
        hidden.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
#endif
        qputenv("XDG_CONFIG_HOME", previous);
    }

    void aVanishedFolderIsReportedNotForgotten()
    {
        const auto previous = qgetenv("XDG_CONFIG_HOME");
        QVERIFY(qputenv("XDG_CONFIG_HOME", m_data.path().toUtf8()));
        QString path;
        {
            QTemporaryDir target;
            QVERIFY(target.isValid());
            path = target.path();
            retrosave::Preferences preferences;
            preferences.chooseFolder(QUrl::fromLocalFile(path));
            QVERIFY(preferences.folderProblem().isEmpty());
        }
        // Le dossier a disparu : on le signale, on ne l'efface pas en douce.
        retrosave::Preferences reopened;
        QCOMPARE(reopened.folder(), QDir::toNativeSeparators(path));
        QVERIFY(!reopened.folderProblem().isEmpty());
        qputenv("XDG_CONFIG_HOME", previous);
    }

    void presentationRejectsWrongCorrelationId()
    {
        const auto endpoint = m_endpoint + "-fake";
        QLocalServer fake;
        QVERIFY(fake.listen(endpoint));
        connect(&fake, &QLocalServer::newConnection, &fake, [&fake] {
            auto *socket = fake.nextPendingConnection();
            connect(socket, &QLocalSocket::readyRead, socket, [socket] {
                socket->readAll();
                socket->write(retrosave::ipc::encode(
                    retrosave::ipc::reply(retrosave::ipc::statusRequest("wrong-id")).message));
                socket->disconnectFromServer();
            });
        });
        retrosave::AgentClient client(endpoint);
        client.refresh();
        QTRY_VERIFY_WITH_TIMEOUT(!client.busy(), 3500);
        QCOMPARE(client.state(), "offline");
        QVERIFY(client.lastChecked().isEmpty());
    }

    void presentationTimesOutUnresponsivePeer()
    {
        QLocalServer silent;
        QVERIFY(silent.listen(m_endpoint + "-silent"));
        retrosave::AgentClient client(m_endpoint + "-silent");
        client.refresh();
        QTRY_VERIFY_WITH_TIMEOUT(!client.busy(), 4000);
        QCOMPARE(client.state(), "offline");
        QVERIFY(client.lastChecked().isEmpty());
    }
};

QTEST_GUILESS_MAIN(IpcTest)
#include "tst_ipc.moc"
