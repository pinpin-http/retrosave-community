// Banc uniquement : toutes les entrées viennent de fixtures synthétiques Python.
// Secrets sur stdin/stdout capturé, jamais argv ou logs. Pas de chemin de save réel.
#include "core/archive.h"
#include "network/s3transfer.h"
#include "network/v0client.h"
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <cstdio>
using namespace retrosave::network;

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QFile input;
    if (!input.open(stdin, QIODevice::ReadOnly))
        return 2;
    const auto config = QJsonDocument::fromJson(input.readAll()).object();
    const auto body = config.value("body").toObject();
    const auto mode = config.value("mode").toString();
    const auto id = config.value("id").toString();
    const auto key = config.value("key").toString().toLatin1();
    V0Client api({QUrl(config.value("url").toString()), config.value("token").toString().toLatin1(),
                  config.value("device").toString().toLatin1(), 3000, true});
    S3Transfer storage(true, 5000);
    QTemporaryDir stage;
    if (!stage.isValid())
        return 2;
    QJsonObject output;
    auto finish = [&](QJsonObject result) {
        output = result;
        app.quit();
    };
    ApiCall *call = nullptr;
    if (mode == "register")
        call = api.registerDevice({"Synthetic C++", "linux", "test"}, key);
    else if (mode == "units")
        call = api.listUnits();
    else if (mode == "create")
        call = api.createUnit({"folder", "synthetic", "dir", "folder:synthetic", "Synthetic"}, key);
    else if (mode == "prepare")
        call = api.prepare(id,
                           {body.value("base_version").toInteger(),
                            body.value("content_sha256").toString(),
                            body.value("archive_sha256").toString(), body.value("size").toInteger(),
                            body.value("archive_bytes").toInteger()},
                           key);
    else if (mode == "confirm")
        call = api.confirm(id,
                           {body.value("base_version").toInteger(),
                            body.value("object_key").toString(),
                            body.value("content_sha256").toString(),
                            body.value("archive_sha256").toString(),
                            {{"os", "linux"}},
                            std::nullopt},
                           key);
    else if (mode == "restore")
        call = api.restore(id, body.value("version").toInteger(), key);
    else if (mode == "resolve")
        call = api.resolveConflict(id, body.value("winner").toInteger(), key);
    else if (mode == "history")
        call = api.history(id);
    else if (mode == "conflicts")
        call = api.listConflicts();
    else if (mode == "pull")
        call = api.downloadTarget(id, body.value("version").toInteger());
    else if (mode == "upload") {
        auto source = std::make_shared<QFile>(config.value("source").toString());
        if (!source->open(QIODevice::ReadOnly))
            return 2;
        auto *transfer =
            storage.upload(QUrl(config.value("target").toString()), source, source->size());
        QObject::connect(transfer, &TransferCall::finished, &app,
                         [&](const TransferResult &result) {
                             finish({{"ok", result.failure == TransferFailure::None},
                                     {"status", result.httpStatus},
                                     {"failure", int(result.failure)}});
                         });
    } else
        return 2;
    if (call)
        QObject::connect(call, &ApiCall::finished, &app, [&](const ApiResult &result) {
            QJsonObject response{{"ok", result.failure == Failure::None},
                                 {"status", result.httpStatus},
                                 {"code", result.serverCode},
                                 {"uncertain", result.mayHaveCommitted}};
            if (auto value = std::get_if<RegisteredDevice>(&result.payload))
                response.insert("id", value->id);
            if (auto value = std::get_if<RemoteUnit>(&result.payload))
                response.insert("id", value->id);
            if (auto value = std::get_if<Published>(&result.payload))
                response.insert("version", value->version);
            if (auto value = std::get_if<Duplicate>(&result.payload))
                response.insert("duplicate", value->version);
            if (auto value = std::get_if<UploadTarget>(&result.payload)) {
                response.insert("target", value->url.toString(QUrl::FullyEncoded));
                response.insert("object_key", value->objectKey);
            }
            if (auto value = std::get_if<Conflict>(&result.payload)) {
                response.insert("conflict", value->id);
                response.insert("first", value->first.number);
                response.insert("second", value->second.number);
            }
            if (auto value = std::get_if<ResolvedConflict>(&result.payload))
                response.insert("head", value->headVersion);
            if (auto value = std::get_if<Conflicts>(&result.payload))
                response.insert("count", value->items.size());
            if (auto value = std::get_if<History>(&result.payload)) {
                response.insert("head", value->headVersion);
                QJsonArray versions;
                for (const auto &version : value->versions)
                    versions.append(version.summary.number);
                response.insert("versions", versions);
            }
            if (auto value = std::get_if<Units>(&result.payload))
                response.insert("count", value->items.size());
            if (auto value = std::get_if<DownloadTarget>(&result.payload)) {
                // Copie par valeur : ApiResult disparaît à la sortie du signal.
                const auto target = *value;
                auto *transfer = storage.downloadCandidate(
                    target.url, config.value("maximum_bytes").toInteger(8 * 1024 * 1024),
                    stage.path());
                QObject::connect(transfer, &TransferCall::finished, &app,
                                 [&, target](const TransferResult &download) {
                                     QJsonObject received{
                                         {"ok", false},
                                         {"content_valid", false},
                                         {"archive_mismatch", download.archiveSha256 !=
                                                                  target.archiveSha256.toLatin1()},
                                         {"size_mismatch", download.bytes != target.archiveBytes}};
                                     int sinks = 0;
                                     if (download.failure == TransferFailure::None) {
                                         try {
                                             // Synchrone dans CET outil headless, pas un modèle
                                             // pour le thread UI. Le sink ne touche aucune cible.
                                             retrosave::core::extractArchiveTo(
                                                 download.archive->fileName(), "dir",
                                                 target.contentSha256, stage.path(),
                                                 [&](const auto &) { ++sinks; });
                                             received.insert("ok", true);
                                             received.insert("content_valid", true);
                                         } catch (const retrosave::core::ArchiveError &) {
                                         }
                                     }
                                     received.insert("sinks", sinks);
                                     finish(received);
                                 });
                return;
            }
            finish(response);
        });
    app.exec();
    output.insert("staging_clean",
                  QDir(stage.path())
                      .entryList(QDir::Files | QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot)
                      .isEmpty());
    QFile out;
    if (!out.open(stdout, QIODevice::WriteOnly))
        return 2;
    out.write(QJsonDocument(output).toJson(QJsonDocument::Compact) + '\n');
    return 0;
}
