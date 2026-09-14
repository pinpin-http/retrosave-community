// ─── Ce que l'adaptateur promet au moteur ─────────────────────────────────
// Le moteur parle en intentions (« publie ceci »), le serveur en HTTP. Ce
// fichier vérifie la traduction, contre un VRAI serveur HTTP local — même
// transport Qt, mêmes octets sur le fil — plutôt que contre une imitation du
// client réseau, qui ne prouverait que la cohérence d'une simulation.
//
// Ce banc ne prouve pas le comportement du vrai serveur (CAS PostgreSQL,
// stockage S3) : c'est l'objet du banc deux clients, qui demande des services
// jetables. Il prouve que nous parlons correctement le contrat.
#include "engine/v0adapter.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <functional>
#include <memory>

using namespace retrosave::engine;
using namespace retrosave::network;

// Serveur local qui répond selon le chemin demandé : une même passe interroge
// l'API puis va chercher son archive « chez S3 », et les deux doivent tenir
// dans le même banc.
class RoutingHttp final : public QTcpServer
{
  public:
    struct Request {
        QByteArray method;
        QByteArray path;
        QMap<QByteArray, QByteArray> headers;
        QByteArray body;
    };
    struct Reply {
        int status = 200;
        QByteArray body;
        QByteArray contentType = "application/json";
    };
    QList<Request> requests;
    std::function<Reply(const Request &)> handler;

    RoutingHttp()
    {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (auto *socket = nextPendingConnection()) {
                auto buffer = std::make_shared<QByteArray>();
                auto handled = std::make_shared<bool>(false);
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer, handled] {
                    buffer->append(socket->readAll());
                    const auto end = buffer->indexOf("\r\n\r\n");
                    if (*handled || end < 0)
                        return;
                    const auto lines = buffer->left(end).split('\n');
                    Request request;
                    request.method = lines.first().split(' ').value(0);
                    request.path = lines.first().split(' ').value(1);
                    for (const auto &line : lines.mid(1)) {
                        const auto colon = line.indexOf(':');
                        if (colon > 0)
                            request.headers.insert(line.left(colon).trimmed().toLower(),
                                                   line.mid(colon + 1).trimmed());
                    }
                    const auto length = request.headers.value("content-length").toLongLong();
                    if (buffer->size() < end + 4 + length)
                        return;
                    *handled = true;
                    request.body = buffer->mid(end + 4, length);
                    requests.append(request);
                    const auto reply = handler(request);
                    const auto headers = "HTTP/1.1 " + QByteArray::number(reply.status) +
                                         " Test\r\nContent-Type: " + reply.contentType +
                                         "\r\nConnection: close\r\nContent-Length: " +
                                         QByteArray::number(reply.body.size()) + "\r\n";
                    socket->write(headers + "\r\n" + reply.body);
                    socket->disconnectFromHost();
                });
            }
        });
    }
};

class V0AdapterTest final : public QObject
{
    Q_OBJECT
    const QString unitId = "11111111-1111-4111-8111-111111111111";
    const QString content = QString(64, 'a');
    const QString archive = QString(64, 'b');
    std::unique_ptr<RoutingHttp> server;
    std::unique_ptr<V0SyncApi> api;
    QTemporaryDir workspace;
    QString base;

    QJsonObject headJson(int number) const
    {
        return {{"number", number},
                {"content_sha256", content},
                {"size_bytes", 120},
                {"created_at", "2026-09-07T10:00:00Z"},
                {"origin_device_name", QJsonValue::Null}};
    }
    QJsonObject unitJson() const
    {
        return {{"id", unitId},
                {"emulator", "ppsspp"},
                {"unit_key", "ULUS10041GAMEDATA"},
                {"unit_type", "dir"},
                {"game_key", "ULUS10041"},
                {"game_label", "Démo"},
                {"label_source", "auto"},
                {"head_version", 2},
                {"state", "active"},
                {"updated_at", "2026-09-07T10:00:00Z"},
                {"head", headJson(2)}};
    }
    static QByteArray json(const QJsonObject &object)
    {
        return QJsonDocument(object).toJson(QJsonDocument::Compact);
    }

  private slots:
    void init()
    {
        QVERIFY(workspace.isValid());
        server = std::make_unique<RoutingHttp>();
        // L'URL du banc emploie explicitement 127.0.0.1. Sous Windows,
        // QHostAddress::LocalHost peut choisir ::1 et rendre cette URL
        // injoignable malgré un listen() réussi.
        QVERIFY(server->listen(QHostAddress(QStringLiteral("127.0.0.1"))));
        base = QStringLiteral("http://127.0.0.1:%1/service").arg(server->serverPort());
        ApiConfig config;
        config.serverUrl = QUrl(base);
        config.token = "jeton-de-test";
        config.deviceId = "22222222-2222-4222-8222-222222222222";
        config.allowHttp = true; // banc local uniquement, jamais un défaut produit
        config.timeoutMs = 4000;
        api = std::make_unique<V0SyncApi>(config);
    }
    void cleanup()
    {
        api.reset();
        server.reset();
    }

    void unitsAreTranslatedFromTheCatalog()
    {
        server->handler = [this](const RoutingHttp::Request &) {
            return RoutingHttp::Reply{200, json({{"units", QJsonArray{unitJson()}}})};
        };
        const auto units = api->listUnits();
        QCOMPARE(units.size(), size_t(1));
        QCOMPARE(units[0].serverId, unitId);
        QCOMPARE(units[0].emulator, QString("ppsspp"));
        QCOMPARE(units[0].unitKey, QString("ULUS10041GAMEDATA"));
        QCOMPARE(units[0].headVersion, 2);
        QCOMPARE(units[0].state, QString("active"));
    }

    void prepareSendsBothDigestsAndReadsADuplicate()
    {
        server->handler = [](const RoutingHttp::Request &) {
            return RoutingHttp::Reply{200, R"({"duplicate":true,"version":7})"};
        };
        const auto outcome = api->prepare(unitId, 3, content, archive, 4096, 900);
        QVERIFY(outcome.duplicate);
        QCOMPARE(outcome.version, 7);

        // Le serveur EXIGE l'empreinte d'archive dès le prepare : il la range
        // avec la réservation et la compare au confirm. L'oublier ferait
        // échouer toute publication réelle — et un banc en mémoire ne l'aurait
        // jamais dit.
        const auto sent = QJsonDocument::fromJson(server->requests.first().body).object();
        QCOMPARE(sent.value("archive_sha256").toString(), archive);
        QCOMPARE(sent.value("content_sha256").toString(), content);
        QCOMPARE(sent.value("base_version").toInt(), 3);
        QCOMPARE(sent.value("archive_bytes").toInt(), 900);
    }

    void anUploadTargetIsPassedThroughUntouched()
    {
        server->handler = [](const RoutingHttp::Request &) {
            return RoutingHttp::Reply{
                200, R"({"upload":{"url":"https://stockage.test/objet?signature=secret",)"
                     R"("object_key":"u/1/2.tar.zst","expires_at":"2026-09-07T11:00:00Z"}})"};
        };
        const auto outcome = api->prepare(unitId, 0, content, archive, 10, 10);
        QVERIFY(!outcome.duplicate);
        QCOMPARE(outcome.objectKey, QString("u/1/2.tar.zst"));
        QVERIFY(outcome.uploadUrl.contains("signature=secret"));
    }

    void theOperationKeyIsStableAcrossReplays()
    {
        server->handler = [](const RoutingHttp::Request &) {
            return RoutingHttp::Reply{200, R"({"duplicate":true,"version":1})"};
        };
        api->prepare(unitId, 3, content, archive, 10, 10);
        api->prepare(unitId, 3, content, archive, 10, 10);
        const auto first = server->requests.at(0).headers.value("idempotency-key");
        const auto second = server->requests.at(1).headers.value("idempotency-key");
        QVERIFY(!first.isEmpty());
        // C'est LA propriété qui rend une reprise sûre : rejouer la même
        // opération après une coupure doit rendre la même réponse, pas créer
        // une seconde version.
        QCOMPARE(first, second);

        // Un contenu différent est une autre opération, et doit donc porter une
        // autre clé — sans quoi le serveur renverrait la réponse de la
        // précédente et la nouvelle sauvegarde serait silencieusement perdue.
        api->prepare(unitId, 3, QString(64, 'c'), archive, 10, 10);
        QVERIFY(server->requests.at(2).headers.value("idempotency-key") != first);
    }

    void confirmPublishesOrKeepsBothSidesOfAConflict()
    {
        server->handler = [](const RoutingHttp::Request &) {
            return RoutingHttp::Reply{201, R"({"version":9})"};
        };
        const auto published = api->confirm(unitId, "u/1/2.tar.zst", content, archive, 8);
        QVERIFY(!published.conflict);
        QCOMPARE(published.version, 9);
        // La date locale n'est jamais envoyée : aucune horloge cliente ne pèse
        // sur l'ordre des versions (invariant I4).
        const auto sent = QJsonDocument::fromJson(server->requests.first().body).object();
        QVERIFY(sent.value("client_mtime").isNull());

        server->handler = [this](const RoutingHttp::Request &) {
            return RoutingHttp::Reply{409, json({{"error", QJsonObject{{"code", "cas_conflict"}}},
                                                 {"conflict_id", unitId},
                                                 {"head", headJson(9)},
                                                 {"yours", headJson(10)}})};
        };
        const auto conflicted = api->confirm(unitId, "u/1/3.tar.zst", content, archive, 8);
        QVERIFY(conflicted.conflict);
        QCOMPARE(conflicted.conflictId, unitId);
        QCOMPARE(conflicted.head, 9);     // la tête du serveur
        QCOMPARE(conflicted.version, 10); // notre branche, conservée elle aussi
    }

    void aVersionIsDownloadedIntoAFileTheEngineOwns()
    {
        const QByteArray payload(2048, '\x7f');
        server->handler = [this, payload](const RoutingHttp::Request &request) {
            if (request.path.endsWith("/objet"))
                return RoutingHttp::Reply{200, payload, "application/octet-stream"};
            return RoutingHttp::Reply{200, json({{"url", base + "/objet"},
                                                 {"content_sha256", content},
                                                 {"archive_sha256", archive},
                                                 {"archive_bytes", payload.size()},
                                                 {"expires_at", "2026-09-07T11:00:00Z"}})};
        };
        const auto staging = QDir(workspace.path()).filePath("staging");
        retrosave::engine::DownloadedVersion version;
        try {
            version = api->downloadVersion(unitId, 4, staging);
        } catch (const retrosave::engine::ApiError &problem) {
            QFAIL(problem.what());
        }

        QCOMPARE(version.contentSha256, content);
        // Le fichier doit SURVIVRE au retour : le temporaire de Qt s'efface à la
        // destruction de son gardien, et le moteur écrirait alors depuis rien.
        QFile received(version.archivePath);
        QVERIFY(received.open(QIODevice::ReadOnly));
        QCOMPARE(received.readAll(), payload);
        QVERIFY(version.archivePath.startsWith(staging));
    }

    void openConflictsAreAskedForAndListed()
    {
        server->handler = [this](const RoutingHttp::Request &) {
            return RoutingHttp::Reply{
                200, json({{"conflicts",
                            QJsonArray{QJsonObject{{"id", unitId},
                                                   {"unit_id", unitId},
                                                   {"unit_label", "Démo"},
                                                   {"version_a", headJson(2)},
                                                   {"version_b", headJson(3)},
                                                   {"created_at", "2026-09-07T10:00:00Z"}}}}})};
        };
        const auto open = api->openConflicts();
        QCOMPARE(open.size(), size_t(1));
        QCOMPARE(open[0].id, unitId);
        QCOMPARE(open[0].unitLabel, QString("Démo"));
        // Les deux côtés doivent arriver entiers : c'est avec eux qu'on
        // tranche, pas avec un identifiant.
        QCOMPARE(open[0].first.number, 2);
        QCOMPARE(open[0].second.number, 3);
        QCOMPARE(open[0].first.sizeBytes, 120);
        // Un appareil d'origine absent ne s'invente pas.
        QVERIFY(open[0].first.originDevice.isEmpty());
        // Seuls les conflits ouverts nous intéressent : un conflit déjà tranché
        // ne doit plus bloquer l'unité.
        QVERIFY(server->requests.first().path.contains("open=1"));
    }

    void resolvingSendsTheWinnerAndAStableKey()
    {
        server->handler = [this](const RoutingHttp::Request &) {
            return RoutingHttp::Reply{200, json({{"unit_id", unitId}, {"head_version", 2}})};
        };
        api->resolveConflict(unitId, 2);
        const auto sent = QJsonDocument::fromJson(server->requests.first().body).object();
        QCOMPARE(sent.value("winner").toInt(), 2);
        QVERIFY(server->requests.first().path.endsWith("/resolve"));

        // Rejouer la MÊME décision après une coupure doit porter la même clé :
        // le serveur rend alors sa réponse d'origine au lieu de trancher deux
        // fois.
        api->resolveConflict(unitId, 2);
        QCOMPARE(server->requests.at(0).headers.value("idempotency-key"),
                 server->requests.at(1).headers.value("idempotency-key"));
        // Une décision DIFFÉRENTE n'est pas un rejeu : elle doit être distincte,
        // sans quoi choisir l'autre version serait silencieusement ignoré.
        api->resolveConflict(unitId, 3);
        QVERIFY(server->requests.at(2).headers.value("idempotency-key") !=
                server->requests.at(0).headers.value("idempotency-key"));
    }

    void aDisappearanceIsSignalledWithoutDeletingAnything()
    {
        server->handler = [](const RoutingHttp::Request &) {
            return RoutingHttp::Reply{200, R"({"state":"missing"})"};
        };
        api->markMissing(unitId);
        QCOMPARE(server->requests.first().method, QByteArray("POST"));
        QVERIFY(server->requests.first().path.endsWith("/missing"));
    }

    void aRefusalBecomesAnErrorThatCarriesNoSecret()
    {
        server->handler = [](const RoutingHttp::Request &) {
            return RoutingHttp::Reply{
                500, R"({"error":{"code":"server_error","message":"jeton-de-test a échoué"}})"};
        };
        QString message;
        try {
            api->listUnits();
            QFAIL("une panne serveur doit lever");
        } catch (const ApiError &error) {
            message = QString::fromUtf8(error.what());
        }
        // Le code machine suffit à diagnostiquer. Le message du serveur, lui,
        // peut contenir n'importe quoi — y compris ce qu'on lui a envoyé.
        QVERIFY(message.contains("server_error"));
        QVERIFY(!message.contains("jeton-de-test"));
    }

    void anUnreachableServerFailsWithoutHanging()
    {
        server->close(); // le port ne répond plus
        // QVERIFY_THROWS_EXCEPTION dépend du modèle d'exceptions du compilateur
        // et laissait échapper ApiError sous MSVC. Le catch explicite vérifie le
        // même contrat sur les deux plateformes.
        bool refused = false;
        try {
            api->listUnits();
        } catch (const ApiError &) {
            refused = true;
        }
        QVERIFY(refused);
    }
};

QTEST_MAIN(V0AdapterTest)
#include "tst_v0adapter.moc"
