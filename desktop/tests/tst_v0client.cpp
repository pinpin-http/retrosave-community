#include "network/v0client.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkProxy>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <functional>
#include <memory>

using namespace retrosave::network;

// Serveur HTTP de contrat, sur un port éphémère local. QNetworkAccessManager
// reste le VRAI transport : on teste les en-têtes, les octets et les pannes,
// pas un mock de sa méthode post(). Aucune base ni sauvegarde n'est utilisée.
class HttpFixture final : public QTcpServer
{
  public:
    struct Request {
        QByteArray method;
        QByteArray path;
        QMap<QByteArray, QByteArray> headers;
        QByteArray body;
    };
    QList<Request> requests;
    int status = 200;
    QByteArray response = "{\"duplicate\":true,\"version\":3}";
    QByteArray location;
    QByteArray contentEncoding;
    bool silent = false;
    bool truncate = false;

    HttpFixture()
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
                    if (silent)
                        return;
                    QByteArray headers = "HTTP/1.1 " + QByteArray::number(status) +
                                         " Test\r\nContent-Type: application/json\r\nConnection: "
                                         "close\r\nContent-Length: " +
                                         QByteArray::number(response.size() + (truncate ? 20 : 0)) +
                                         "\r\n";
                    if (!contentEncoding.isEmpty())
                        headers += "Content-Encoding: " + contentEncoding + "\r\n";
                    if (!location.isEmpty())
                        headers += "Location: " + location + "\r\n";
                    socket->write(headers + "\r\n" + response);
                    socket->disconnectFromHost();
                });
            }
        });
    }
};

class V0ClientTest final : public QObject
{
    Q_OBJECT
    const QString unit = "11111111-1111-4111-8111-111111111111";
    const QString digest = QString(64, 'a');
    const QByteArray key = "operation-stable-1";
    std::unique_ptr<HttpFixture> server;
    std::unique_ptr<V0Client> client;
    ApiConfig config;

    PrepareVersion prepareBody() const { return {2, digest, QString(64, 'b'), 123, 80}; }
    ConfirmVersion confirmBody() const
    {
        return {2,           "u/user/unit/archive", digest, QString(64, 'b'), {{"os", "linux"}},
                std::nullopt};
    }
    std::optional<ApiResult> wait(ApiCall *call)
    {
        QSignalSpy completed(call, &ApiCall::finished);
        if (completed.isEmpty() && !completed.wait(2500))
            return {};
        const auto result = qvariant_cast<ApiResult>(completed.first().first());
        call->deleteLater();
        return result;
    }

    QJsonObject headJson(int number = 2) const
    {
        return {{"number", number},
                {"content_sha256", digest},
                {"size_bytes", 123},
                {"created_at", "2026-09-06T12:00:00Z"},
                {"origin_device_name", QJsonValue::Null}};
    }
    QJsonObject unitJson() const
    {
        return {{"id", unit},
                {"emulator", "ppsspp"},
                {"unit_key", "ULUS10041GAMEDATA"},
                {"unit_type", "dir"},
                {"game_key", "ULUS10041"},
                {"game_label", "Démo 日本語"},
                {"label_source", "user"},
                {"head_version", 2},
                {"state", "active"},
                {"updated_at", "2026-09-06T12:00:00Z"},
                {"head", headJson()}};
    }
    ApiCall *catalogCall(const QString &operation, V0Client *target = nullptr)
    {
        auto *api = target ? target : client.get();
        if (operation == "health")
            return api->health();
        if (operation == "register")
            return api->registerDevice({"PC français", "linux", "0.1.0"}, key);
        if (operation == "devices")
            return api->listDevices();
        if (operation == "units")
            return api->listUnits();
        if (operation == "create")
            return api->createUnit(
                {"ppsspp", "ULUS10041GAMEDATA", "dir", "ULUS10041", "Démo 日本語"}, key);
        if (operation == "rename")
            return api->renameUnit(unit, "Démo 日本語", key);
        if (operation == "history")
            return api->history(unit);
        if (operation == "restore")
            return api->restore(unit, 2, key);
        if (operation == "missing")
            return api->markMissing(unit, key);
        if (operation == "conflicts")
            return api->listConflicts();
        return api->resolveConflict(unit, 2, key);
    }

  private slots:
    void init()
    {
        QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
        server = std::make_unique<HttpFixture>();
        QVERIFY(server->listen(QHostAddress::LocalHost, 0));
        config = {QUrl("http://127.0.0.1:" + QString::number(server->serverPort()) + "/service/"),
                  "rsc_synthetic_test_token", "22222222-2222-4222-8222-222222222222", 500, true};
        client = std::make_unique<V0Client>(config);
    }
    void cleanup()
    {
        client.reset();
        server.reset();
    }

    void catalogRoutes_data()
    {
        QTest::addColumn<QString>("operation");
        QTest::addColumn<QByteArray>("method");
        QTest::addColumn<QByteArray>("path");
        QTest::addColumn<QJsonObject>("body");
        QTest::addColumn<QJsonObject>("response");
        QTest::addColumn<int>("status");
        const auto base = "/service/v0/units/" + unit.toUtf8();
        QTest::newRow("health") << QString("health") << QByteArray("GET")
                                << QByteArray("/service/healthz") << QJsonObject{}
                                << QJsonObject{{"status", "ok"}, {"db", false}, {"s3", true}}
                                << 200;
        QTest::newRow("register") << QString("register") << QByteArray("POST")
                                  << QByteArray("/service/v0/devices")
                                  << QJsonObject{{"name", "PC français"},
                                                 {"os", "linux"},
                                                 {"app_version", "0.1.0"}}
                                  << QJsonObject{{"device_id", unit}} << 201;
        QTest::newRow("devices")
            << QString("devices") << QByteArray("GET") << QByteArray("/service/v0/devices")
            << QJsonObject{}
            << QJsonObject{{"devices", QJsonArray{QJsonObject{{"id", unit},
                                                              {"name", "Thor"},
                                                              {"os", "android"},
                                                              {"last_seen_at", QJsonValue::Null}}}}}
            << 200;
        QTest::newRow("units") << QString("units") << QByteArray("GET")
                               << QByteArray("/service/v0/units") << QJsonObject{}
                               << QJsonObject{{"units", QJsonArray{unitJson()}}} << 200;
        for (int status : {200, 201})
            QTest::newRow(status == 200 ? "existing-unit" : "new-unit")
                << QString("create") << QByteArray("POST") << QByteArray("/service/v0/units")
                << QJsonObject{{"emulator", "ppsspp"},
                               {"unit_key", "ULUS10041GAMEDATA"},
                               {"unit_type", "dir"},
                               {"game_key", "ULUS10041"},
                               {"game_label", "Démo 日本語"}}
                << QJsonObject{{"unit", unitJson()}} << status;
        QTest::newRow("rename") << QString("rename") << QByteArray("PATCH") << base
                                << QJsonObject{{"game_label", "Démo 日本語"}}
                                << QJsonObject{{"unit", unitJson()}} << 200;
        auto version = headJson();
        version.insert("kind", "normal");
        version.insert("client_mtime", QJsonValue::Null);
        version.insert("parent_number", QJsonValue::Null);
        auto branch = version;
        branch.insert("number", 3);
        branch.insert("parent_number", 2);
        branch.insert("kind", "conflict_branch");
        QTest::newRow("history-head-below-branch")
            << QString("history") << QByteArray("GET") << base + "/versions" << QJsonObject{}
            << QJsonObject{{"versions", QJsonArray{branch, version}}, {"head_version", 2}} << 200;
        QTest::newRow("restore") << QString("restore") << QByteArray("POST") << base + "/restore"
                                 << QJsonObject{{"version", 2}} << QJsonObject{{"version", 4}}
                                 << 201;
        QTest::newRow("missing") << QString("missing") << QByteArray("POST") << base + "/missing"
                                 << QJsonObject{} << QJsonObject{{"state", "missing"}} << 200;
        QTest::newRow("conflicts")
            << QString("conflicts") << QByteArray("GET")
            << QByteArray("/service/v0/conflicts?open=1") << QJsonObject{}
            << QJsonObject{{"conflicts",
                            QJsonArray{QJsonObject{{"id", unit},
                                                   {"unit_id", unit},
                                                   {"unit_label", "Démo 日本語"},
                                                   {"version_a", headJson()},
                                                   {"version_b", headJson(3)},
                                                   {"created_at", "2026-09-06T12:00:00Z"}}}}}
            << 200;
        QTest::newRow("resolve") << QString("resolve") << QByteArray("POST")
                                 << "/service/v0/conflicts/" + unit.toUtf8() + "/resolve"
                                 << QJsonObject{{"winner", 2}}
                                 << QJsonObject{{"unit_id", unit}, {"head_version", 2}} << 200;
    }
    void catalogRoutes()
    {
        QFETCH(QString, operation);
        QFETCH(QByteArray, method);
        QFETCH(QByteArray, path);
        QFETCH(QJsonObject, body);
        QFETCH(QJsonObject, response);
        QFETCH(int, status);
        response.insert("future_field", QJsonObject{{"ignored", true}});
        server->status = status;
        server->response = QJsonDocument(response).toJson();
        const auto result = wait(catalogCall(operation));
        QVERIFY(result);
        QCOMPARE(result->failure, Failure::None);
        QVERIFY(!std::holds_alternative<std::monostate>(result->payload));
        QVERIFY(!result->mayHaveCommitted);
        QCOMPARE(server->requests.size(), 1);
        const auto request = server->requests.first();
        QCOMPARE(request.method, method);
        QCOMPARE(request.path, path);
        if (method == "GET") {
            QVERIFY(request.body.isEmpty());
            QVERIFY(!request.headers.contains("idempotency-key"));
        } else {
            QCOMPARE(QJsonDocument::fromJson(request.body).object(), body);
            QCOMPARE(request.headers.value("idempotency-key"), key);
        }
        QCOMPARE(request.headers.contains("authorization"), operation != "health");
        QCOMPARE(request.headers.contains("x-device-id"),
                 operation != "health" && operation != "register");
        if (operation == "history") {
            const auto history = std::get<History>(result->payload);
            QCOMPARE(history.headVersion, 2);
            QCOMPARE(history.versions.first().summary.number, 3);
            QVERIFY(!history.versions.last().parentNumber);
        } else if (operation == "units" || operation == "create" || operation == "rename") {
            const auto remote = operation == "units"
                                    ? std::get<Units>(result->payload).items.first()
                                    : std::get<RemoteUnit>(result->payload);
            QCOMPARE(remote.identity.gameLabel, "Démo 日本語");
            QCOMPARE(remote.head->number, 2);
            QVERIFY(!remote.head->originDeviceName);
            QCOMPARE(remote.labelSource, "user");
        } else if (operation == "devices") {
            QVERIFY(!std::get<Devices>(result->payload).items.first().lastSeenAt);
        } else if (operation == "health") {
            QVERIFY(!std::get<Health>(result->payload).database); // HTTP 200 ≠ DB saine
            QVERIFY(!std::get<Health>(result->payload).version);
        } else if (operation == "conflicts") {
            const auto conflict = std::get<Conflicts>(result->payload).items.first();
            QCOMPARE(conflict.first.number, 2);
            QCOMPARE(conflict.second.number, 3);
        }
    }
    void registrationDoesNotRequireOrInventDevice()
    {
        config.deviceId.clear();
        V0Client unregistered(config);
        server->status = 201;
        server->response = QJsonDocument(QJsonObject{{"device_id", unit}}).toJson();
        auto result = wait(unregistered.registerDevice({"PC", "linux", "0.1.0"}, key));
        QVERIFY(result);
        QCOMPARE(result->failure, Failure::None);
        QCOMPARE(std::get<RegisteredDevice>(result->payload).id, unit);
        result = wait(unregistered.listUnits());
        QVERIFY(result);
        QCOMPARE(result->failure, Failure::InvalidRequest);
        QCOMPARE(server->requests.size(), 1);
        config.token.clear();
        V0Client anonymous(config);
        server->status = 200;
        server->response = R"({"status":"ok","db":true,"s3":true,"version":"future"})";
        result = wait(anonymous.health());
        QVERIFY(result);
        QCOMPARE(result->failure, Failure::None);
        QVERIFY(!server->requests.last().headers.contains("authorization"));
    }
    void invalidCatalogNeverReturnsPartialList()
    {
        const auto valid = unitJson();
        for (const auto &field : valid.keys()) {
            auto broken = valid;
            broken.remove(field);
            server->response =
                QJsonDocument(QJsonObject{{"units", QJsonArray{valid, broken}}}).toJson();
            auto result = wait(client->listUnits());
            QVERIFY(result);
            QCOMPARE(result->failure, Failure::InvalidResponse);
            QVERIFY(std::holds_alternative<std::monostate>(result->payload));
        }
        for (const auto &field : {"head_version", "head", "state", "label_source"}) {
            auto broken = valid;
            broken.insert(field, true);
            server->response = QJsonDocument(QJsonObject{{"units", QJsonArray{broken}}}).toJson();
            const auto result = wait(client->listUnits());
            QVERIFY(result);
            QCOMPARE(result->failure, Failure::InvalidResponse);
        }
        auto fresh = valid;
        fresh.insert("head_version", 0);
        fresh.insert("head", QJsonValue::Null);
        server->response = QJsonDocument(QJsonObject{{"units", QJsonArray{fresh}}}).toJson();
        auto result = wait(client->listUnits());
        QVERIFY(result);
        QCOMPARE(result->failure, Failure::None);
        QVERIFY(!std::get<Units>(result->payload).items.first().head);
        fresh.insert("head_version", 2);
        server->response = QJsonDocument(QJsonObject{{"units", QJsonArray{fresh}}}).toJson();
        result = wait(client->listUnits());
        QVERIFY(result);
        QCOMPARE(result->failure, Failure::InvalidResponse);
        server->response = QJsonDocument(QJsonObject{{"units", QJsonArray{valid, valid}}}).toJson();
        result = wait(client->listUnits());
        QVERIFY(result);
        QCOMPARE(result->failure, Failure::InvalidResponse);
    }
    void malformedCatalog_data()
    {
        QTest::addColumn<QString>("operation");
        QTest::addColumn<QByteArray>("response");
        QTest::addColumn<int>("status");
        QTest::newRow("devices-absent") << QString("devices") << QByteArray("{}") << 200;
        QTest::newRow("devices-null")
            << QString("devices") << QByteArray(R"({"devices":null})") << 200;
        QTest::newRow("units-object") << QString("units") << QByteArray(R"({"units":{}})") << 200;
        QTest::newRow("conflicts-invalid-entry")
            << QString("conflicts") << QByteArray(R"({"conflicts":[{}]})") << 200;
        QTest::newRow("history-no-head")
            << QString("history") << QByteArray(R"({"versions":[],"head_version":1})") << 200;
        QTest::newRow("history-fraction")
            << QString("history") << QByteArray(R"({"versions":[],"head_version":0.5})") << 200;
        QTest::newRow("missing-wrong-state")
            << QString("missing") << QByteArray(R"({"state":"active"})") << 200;
        QTest::newRow("register-invalid-id")
            << QString("register") << QByteArray(R"({"device_id":"not-a-uuid"})") << 201;
        QTest::newRow("resolve-invalid-id")
            << QString("resolve") << QByteArray(R"({"unit_id":42,"head_version":1})") << 200;
        QTest::newRow("health-string-bool")
            << QString("health") << QByteArray(R"({"status":"ok","db":"true","s3":true})") << 200;
    }
    void malformedCatalog()
    {
        QFETCH(QString, operation);
        QFETCH(QByteArray, response);
        QFETCH(int, status);
        server->status = status;
        server->response = response;
        const auto result = wait(catalogCall(operation));
        QVERIFY(result);
        QCOMPARE(result->failure, Failure::InvalidResponse);
        QCOMPARE(result->mayHaveCommitted,
                 operation == "missing" || operation == "register" || operation == "resolve");
    }
    void restorePreservesOpenConflict()
    {
        server->status = 409;
        server->response =
            QJsonDocument(QJsonObject{{"error", QJsonObject{{"code", "open_conflict"}}},
                                      {"conflict_id", unit},
                                      {"version_a", headJson()},
                                      {"version_b", headJson(3)}})
                .toJson();
        const auto result = wait(client->restore(unit, 1, key));
        QVERIFY(result);
        QCOMPARE(result->failure, Failure::None);
        const auto conflict = std::get<Conflict>(result->payload);
        QCOMPARE(conflict.first.number, 2);
        QCOMPARE(conflict.second.number, 3);
        QCOMPARE(server->requests.size(), 1);
    }
    void invalidNestedMetadataIsRejected()
    {
        for (const auto &field : headJson().keys()) {
            auto brokenHead = headJson();
            brokenHead.remove(field);
            auto brokenUnit = unitJson();
            brokenUnit.insert("head", brokenHead);
            server->response =
                QJsonDocument(QJsonObject{{"units", QJsonArray{brokenUnit}}}).toJson();
            const auto result = wait(client->listUnits());
            QVERIFY(result);
            QCOMPARE(result->failure, Failure::InvalidResponse);
        }
        auto mismatch = unitJson();
        mismatch.insert("head", headJson(3));
        server->response = QJsonDocument(QJsonObject{{"units", QJsonArray{mismatch}}}).toJson();
        auto result = wait(client->listUnits());
        QVERIFY(result);
        QCOMPARE(result->failure, Failure::InvalidResponse);
        auto version = headJson();
        version.insert("kind", "normal");
        version.insert("parent_number", QJsonValue::Null);
        version.insert("client_mtime", QJsonValue::Null);
        for (const auto &field : {"parent_number", "client_mtime", "kind"}) {
            auto broken = version;
            broken.remove(field);
            server->response =
                QJsonDocument(QJsonObject{{"versions", QJsonArray{broken}}, {"head_version", 2}})
                    .toJson();
            result = wait(client->history(unit));
            QVERIFY(result);
            QCOMPARE(result->failure, Failure::InvalidResponse);
        }
        server->response = QJsonDocument(QJsonObject{{"versions", QJsonArray{version, version}},
                                                     {"head_version", 2}})
                               .toJson();
        result = wait(client->history(unit));
        QVERIFY(result);
        QCOMPARE(result->failure, Failure::InvalidResponse);
    }

    void emptyCatalogListsAreValid()
    {
        for (const auto &operation : {"devices", "units", "conflicts", "history"}) {
            QJsonObject response{
                {operation == QString("history") ? "versions" : operation, QJsonArray{}}};
            if (operation == QString("history"))
                response.insert("head_version", 0);
            server->response = QJsonDocument(response).toJson();
            const auto result = wait(catalogCall(operation));
            QVERIFY(result);
            QCOMPARE(result->failure, Failure::None);
        }
    }
    void catalogMutationFailuresAndInvalidInputs()
    {
        server->status = 410;
        server->response = R"({"error":{"code":"already_resolved"}})";
        auto result = wait(client->resolveConflict(unit, 3, key));
        QVERIFY(result);
        QCOMPARE(result->failure, Failure::Http);
        QVERIFY(!result->mayHaveCommitted);
        QCOMPARE(result->serverCode, "already_resolved");
        const auto count = server->requests.size();
        for (const auto &makeCall : QList<std::function<ApiCall *()>>{
                 [this] { return client->restore(unit, 0, key); },
                 [this] { return client->resolveConflict("../bad", 1, key); },
                 [this] { return client->renameUnit(unit, "title", "bad\r\nkey"); },
                 [this] { return client->markMissing(unit, {}); },
                 [this] { return client->registerDevice({"PC", "unknown", "0.1.0"}, key); },
                 [this] {
                     // Mal FORMÉ, et non « inconnu » : depuis que la liste des
                     // émulateurs appartient au serveur, un identifiant bien
                     // formé mais qu'il ne connaît pas doit partir et revenir
                     // en 422, pas être escamoté ici.
                     return client->createUnit({"Bad Emu!", "unit", "file", "game", "game"}, key);
                 }}) {
            const auto invalid = wait(makeCall());
            QVERIFY(invalid);
            QCOMPARE(invalid->failure, Failure::InvalidRequest);
        }
        QCOMPARE(server->requests.size(), count);
        server->silent = true;
        result = wait(client->renameUnit(unit, "title", key));
        QVERIFY(result);
        QCOMPARE(result->failure, Failure::Timeout);
        QVERIFY(result->mayHaveCommitted);
    }

    void anUnknownButWellFormedEmulatorIsLeftToTheServer()
    {
        // Le client ne tranche plus « quels émulateurs existent » : c'est la
        // question du serveur, et lui seul peut y répondre pour de bon. Un
        // client plus RÉCENT que son serveur doit donc poser la question et
        // recevoir un refus lisible, pas se refuser lui-même en silence — le
        // silence est précisément ce qui a caché le défaut du 13/09/2026.
        const auto count = server->requests.size();
        const auto result =
            wait(client->createUnit({"emulateur-de-demain", "partie", "file", "jeu", "Jeu"}, key));
        QVERIFY(result);
        QVERIFY(result->failure != Failure::InvalidRequest);
        // La preuve que ce n'est pas un refus local : la requête est partie.
        QCOMPARE(server->requests.size(), count + 1);
    }

    void everyShippedAdapterCanDeclareAUnit()
    {
        // Le test lit les manifestes réellement présents dans `adapters/`
        // plutôt qu'une liste recopiée : le prochain adaptateur ajouté au
        // dépôt échouera ici tant que la garde ne le connaîtra pas.
        QDir manifests(QStringLiteral(RETROSAVE_ADAPTERS_DIR));
        const auto shipped = manifests.entryList({"*.toml"}, QDir::Files, QDir::Name);
        QVERIFY(!shipped.isEmpty());
        for (const auto &file : shipped) {
            const auto emulator = QFileInfo(file).completeBaseName();
            const auto result =
                wait(client->createUnit({emulator, "Golden Sun (FR).sav", "file",
                                         "golden sun", "Golden Sun"},
                                        key));
            QVERIFY(result);
            QVERIFY2(result->failure != Failure::InvalidRequest, qPrintable(emulator));
        }
        // `folder` n'a pas de manifeste : c'est l'adaptateur générique.
        const auto generic =
            wait(client->createUnit({"folder", "Parties", "dir", "parties", "Parties"}, key));
        QVERIFY(generic);
        QVERIFY(generic->failure != Failure::InvalidRequest);
    }

    void prepareCarriesExactContractAndStableKey()
    {
        auto result = wait(client->prepare(unit, prepareBody(), key));
        QVERIFY(result);
        QCOMPARE(result->failure, Failure::None);
        QCOMPARE(std::get<Duplicate>(result->payload).version, 3);
        QCOMPARE(server->requests.size(), 1);
        const auto request = server->requests.first();
        QCOMPARE(request.path, "/service/v0/units/" + unit.toUtf8() + "/versions:prepare");
        QCOMPARE(request.headers.value("authorization"), "Bearer " + config.token);
        QCOMPARE(request.headers.value("x-device-id"), config.deviceId);
        QCOMPARE(request.headers.value("idempotency-key"), key);
        QCOMPARE(QJsonDocument::fromJson(request.body).object(),
                 (QJsonObject{{"base_version", 2},
                              {"content_sha256", digest},
                              {"archive_sha256", QString(64, 'b')},
                              {"size", 123},
                              {"archive_bytes", 80}}));
        result = wait(client->prepare(unit, prepareBody(), key));
        QVERIFY(result);
        QCOMPARE(server->requests.size(), 2);
        QCOMPARE(server->requests.last().body, request.body);
        QCOMPARE(server->requests.last().headers.value("idempotency-key"), key);
    }

    void uploadTargetIsDataNotAnAutomaticTransfer()
    {
        server->response =
            R"({"upload":{"url":"https://storage.example/archive?signature=secret","object_key":"u/a","expires_at":"2026-09-06T12:00:00Z"},"future_field":true})";
        const auto result = wait(client->prepare(unit, prepareBody(), key));
        QVERIFY(result);
        QCOMPARE(result->failure, Failure::None);
        const auto target = std::get<UploadTarget>(result->payload);
        QCOMPARE(target.objectKey, "u/a");
        QCOMPARE(target.url.host(), "storage.example");
        QCOMPARE(server->requests.size(), 1);
    }

    void confirmSendsNullTimestampAndReturnsVersion()
    {
        server->status = 201;
        server->response = R"({"version":4,"future_field":false})";
        const auto result = wait(client->confirm(unit, confirmBody(), key));
        QVERIFY(result);
        QCOMPARE(result->failure, Failure::None);
        QCOMPARE(std::get<Published>(result->payload).version, 4);
        const auto body = QJsonDocument::fromJson(server->requests.first().body).object();
        QVERIFY(body.contains("client_mtime"));
        QVERIFY(body.value("client_mtime").isNull());
        QCOMPARE(body.value("env").toObject(), (QJsonObject{{"os", "linux"}}));
        QCOMPARE(body.size(), 6);
        QVERIFY(!result->mayHaveCommitted); // succès déterminé, pas résultat incertain
    }

    void conflict_data()
    {
        QTest::addColumn<QString>("code");
        QTest::newRow("cas") << QString("cas_conflict");
        QTest::newRow("already-open") << QString("open_conflict");
    }
    void conflict()
    {
        QFETCH(QString, code);
        const bool cas = code == "cas_conflict";
        QJsonObject first{{"number", 3}, {"content_sha256", digest}, {"size_bytes", 100}};
        QJsonObject second{
            {"number", 4}, {"content_sha256", QString(64, 'b')}, {"size_bytes", 120}};
        server->status = 409;
        server->response =
            QJsonDocument(
                QJsonObject{{"error", QJsonObject{{"code", code}, {"message", "conflit"}}},
                            {"conflict_id", unit},
                            {cas ? "head" : "version_a", first},
                            {cas ? "yours" : "version_b", second}})
                .toJson();
        const auto result = wait(client->confirm(unit, confirmBody(), key));
        QVERIFY(result);
        QCOMPARE(result->failure, Failure::None);
        const auto conflict = std::get<Conflict>(result->payload);
        QCOMPARE(conflict.first.number, 3);
        QCOMPARE(conflict.second.number, 4);
        QCOMPARE(conflict.code, code);
        QCOMPARE(server->requests.size(), 1); // un 409 ne déclenche pas une boucle de retry
    }

    void downloadMetadataHasIntegrityFields()
    {
        server->response = QJsonDocument(QJsonObject{{"url", "https://storage.example/data"},
                                                     {"content_sha256", digest},
                                                     {"archive_sha256", QString(64, 'b')},
                                                     {"archive_bytes", 80},
                                                     {"expires_at", "2026-09-06T12:00:00Z"}})
                               .toJson();
        const auto result = wait(client->downloadTarget(unit, 3));
        QVERIFY(result);
        QCOMPARE(result->failure, Failure::None);
        QCOMPARE(std::get<DownloadTarget>(result->payload).archiveBytes, 80);
        QVERIFY(!server->requests.first().headers.contains("idempotency-key"));
        QVERIFY(server->requests.first().body.isEmpty());
    }

    void httpErrors_data()
    {
        QTest::addColumn<int>("status");
        QTest::addColumn<QString>("code");
        QTest::newRow("auth") << 401 << QString("invalid_token");
        QTest::newRow("expired-upload") << 404 << QString("unknown_upload");
        QTest::newRow("too-large") << 413 << QString("payload_too_large");
        QTest::newRow("checksum") << 422 << QString("checksum_mismatch");
        QTest::newRow("server") << 503 << QString("server_error");
    }
    void httpErrors()
    {
        QFETCH(int, status);
        QFETCH(QString, code);
        server->status = status;
        server->response =
            QJsonDocument(QJsonObject{{"error", QJsonObject{{"code", code},
                                                            {"message",
                                                             "message non exposé par le client"}}}})
                .toJson();
        const auto result = wait(client->confirm(unit, confirmBody(), key));
        QVERIFY(result);
        QCOMPARE(result->failure, Failure::Http);
        QCOMPARE(result->serverCode, code);
        QCOMPARE(result->httpStatus, status);
        QCOMPARE(result->mayHaveCommitted, status >= 500);
        QCOMPARE(server->requests.size(), 1);
    }

    void malformedSuccess_data()
    {
        QTest::addColumn<QByteArray>("body");
        QTest::newRow("html") << QByteArray("<html>proxy error</html>");
        QTest::newRow("empty") << QByteArray("{}");
        QTest::newRow("fraction") << QByteArray(R"({"version":1.5})");
        QTest::newRow("string") << QByteArray(R"({"version":"4"})");
        QTest::newRow("contradictory")
            << QByteArray(R"({"version":4,"error":{"code":"server_error"}})");
    }
    void malformedSuccess()
    {
        QFETCH(QByteArray, body);
        server->status = 201;
        server->response = body;
        const auto result = wait(client->confirm(unit, confirmBody(), key));
        QVERIFY(result);
        QCOMPARE(result->failure, Failure::InvalidResponse);
        QVERIFY(result->mayHaveCommitted);
    }

    void invalidRequestNeverOpensConnection()
    {
        auto result = wait(client->prepare("../other", prepareBody(), key));
        QVERIFY(result);
        QCOMPARE(result->failure, Failure::InvalidRequest);
        result = wait(client->confirm(unit, confirmBody(), "bad\r\nHeader: injection"));
        QVERIFY(result);
        QCOMPARE(result->failure, Failure::InvalidRequest);
        config.allowHttp = false;
        V0Client secureOnly(config);
        result = wait(secureOnly.prepare(unit, prepareBody(), key));
        QVERIFY(result);
        QCOMPARE(result->failure, Failure::InvalidRequest);
        QVERIFY(server->requests.isEmpty());
        config.allowHttp = true;
        config.deviceId += '\n';
        V0Client badDevice(config);
        result = wait(badDevice.prepare(unit, prepareBody(), key));
        QVERIFY(result);
        QCOMPARE(result->failure, Failure::InvalidRequest);
        QVERIFY(server->requests.isEmpty());
    }

    void redirectDoesNotForwardCredentials()
    {
        HttpFixture other;
        QVERIFY(other.listen(QHostAddress::LocalHost, 0));
        server->status = 307;
        server->location = "http://127.0.0.1:" + QByteArray::number(other.serverPort()) + "/stolen";
        const auto result = wait(client->prepare(unit, prepareBody(), key));
        QVERIFY(result);
        QCOMPARE(result->failure, Failure::Redirect);
        QVERIFY(other.requests.isEmpty());
    }

    void timeoutCancelAndDestruction()
    {
        server->silent = true;
        auto result = wait(client->confirm(unit, confirmBody(), key));
        QVERIFY(result);
        QCOMPARE(result->failure, Failure::Timeout);
        QVERIFY(result->mayHaveCommitted);
        auto *call = client->confirm(unit, confirmBody(), key);
        QSignalSpy done(call, &ApiCall::finished);
        call->cancel();
        QCOMPARE(done.size(), 1);
        QCOMPARE(qvariant_cast<ApiResult>(done.first().first()).failure, Failure::Cancelled);
        call->cancel();
        QCOMPARE(done.size(), 1);
        QPointer<ApiCall> pending = client->confirm(unit, confirmBody(), key);
        client.reset();
        QVERIFY(pending.isNull());
    }

    void compressedMetadataKeepsItsOwnLength()
    {
        // Fixture gzip synthétique ; le Content-Length porte les octets sur
        // le fil, pas les octets JSON rendus après décompression par Qt.
        server->contentEncoding = "gzip";
        server->response = QByteArray::fromHex("1f8b08000000000002ffab564a292dc8c94c4e2c4955b22a292"
                                               "a4dd5512a4b2d2acecccf53b232ae05009f716a221e000000");
        const auto result = wait(client->prepare(unit, prepareBody(), key));
        QVERIFY(result);
        QCOMPARE(result->failure, Failure::None);
        QCOMPARE(std::get<Duplicate>(result->payload).version, 3);
    }

    void responseSizeAndTruncationAreNotSuccess()
    {
        server->response = QByteArray(V0Client::MaxResponseBytes + 1, ' ');
        auto result = wait(client->confirm(unit, confirmBody(), key));
        QVERIFY(result);
        QCOMPARE(result->failure, Failure::ResponseTooLarge);
        server->response = R"({"version":4})";
        server->status = 201;
        server->truncate = true;
        result = wait(client->confirm(unit, confirmBody(), key));
        QVERIFY(result);
        QCOMPARE(result->failure, Failure::Transport);
        QVERIFY(result->mayHaveCommitted);
    }
};

QTEST_GUILESS_MAIN(V0ClientTest)
#include "tst_v0client.moc"
