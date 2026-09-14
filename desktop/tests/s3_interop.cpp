// Outil de test uniquement : ses chemins désignent des archives synthétiques
// créées par test_s3_interop.py. Les signatures arrivent sur stdin, jamais argv.
#include "network/s3transfer.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
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
    QTemporaryDir staging;
    if (!staging.isValid())
        return 2;
    S3Transfer transfer(true, config.value("timeout_ms").toInt(30000));
    TransferCall *call;
    if (config.value("mode") == QJsonValue("upload")) {
        auto source = std::make_shared<QFile>(config.value("source").toString());
        if (!source->open(QIODevice::ReadOnly))
            return 3;
        call = transfer.upload(QUrl(config.value("url").toString()), source,
                               config.value("bytes").toInteger());
    } else if (config.value("mode") == QJsonValue("candidate")) {
        call = transfer.downloadCandidate(QUrl(config.value("url").toString()),
                                          config.value("bytes").toInteger(), staging.path());
    } else {
        call = transfer.downloadExact(QUrl(config.value("url").toString()),
                                 config.value("bytes").toInteger(),
                                 config.value("sha256").toString().toLatin1(), staging.path());
    }
    QJsonObject output;
    QObject::connect(call, &TransferCall::finished, &app, [&](const TransferResult &result) {
        output = {{"ok", result.failure == TransferFailure::None},
                  {"integrity_error", result.failure == TransferFailure::Integrity},
                  {"http_error", result.failure == TransferFailure::Http},
                  {"redirect", result.failure == TransferFailure::Redirect},
                  {"timeout", result.failure == TransferFailure::Timeout},
                  {"cancelled", result.failure == TransferFailure::Cancelled},
                  {"network_error", result.failure == TransferFailure::Network},
                  {"status", result.httpStatus},
                  {"bytes", result.bytes},
                  {"sha256", QString::fromLatin1(result.archiveSha256)},
                  {"staged", bool(result.archive)}};
        app.quit();
    });
    if (config.value("cancel").toBool())
        QTimer::singleShot(20, call, &TransferCall::cancel);
    app.exec();
    // Le résultat n'a pas été conservé : le staging doit avoir disparu aussi
    // bien après un succès qu'après un checksum faux, une coupure ou un cancel.
    output.insert("staging_clean",
                  QDir(staging.path()).entryList(QDir::Files | QDir::Hidden).isEmpty());
    QFile out;
    if (!out.open(stdout, QIODevice::WriteOnly))
        return 2;
    out.write(QJsonDocument(output).toJson(QJsonDocument::Compact) + '\n');
    return 0;
}
