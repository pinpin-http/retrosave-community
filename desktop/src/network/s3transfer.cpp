#include "network/s3transfer.h"

#include <QCryptographicHash>
#include <QDir>
#include <QRegularExpression>

namespace retrosave::network
{
namespace
{
constexpr qint64 ChunkBytes = 64 * 1024;
TransferResult transportResult(QNetworkReply *reply)
{
    TransferResult result;
    result.httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (result.httpStatus >= 300 && result.httpStatus < 400)
        result.failure = TransferFailure::Redirect;
    else if (result.httpStatus >= 400)
        result.failure = TransferFailure::Http;
    else if (reply->error() != QNetworkReply::NoError)
        result.failure = TransferFailure::Network;
    else if (result.httpStatus != 200 && result.httpStatus != 204)
        result.failure = TransferFailure::Http;
    return result;
}
} // namespace

TransferCall::TransferCall(QObject *parent) : QObject(parent)
{
    m_deadline.setSingleShot(true);
    connect(&m_deadline, &QTimer::timeout, this, [this] {
        TransferResult result;
        result.failure = TransferFailure::Timeout;
        complete(result);
    });
}
TransferCall::~TransferCall()
{
    if (m_reply) {
        disconnect(m_reply, nullptr, this, nullptr);
        m_reply->abort();
        m_reply->deleteLater();
    }
}
void TransferCall::cancel()
{
    TransferResult result;
    result.failure = TransferFailure::Cancelled;
    complete(result);
}
void TransferCall::complete(TransferResult result)
{
    if (m_done)
        return;
    m_done = true;
    m_deadline.stop();
    result.objectMayExist = m_upload; // même une annulation peut suivre un PUT accepté
    if (m_reply) {
        disconnect(m_reply, nullptr, this, nullptr);
        if (!m_reply->isFinished())
            m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
    }
    m_source.reset();
    m_staging.reset(); // échec : supprime le partiel ; succès : result en garde la propriété
    emit finished(result);
}

S3Transfer::S3Transfer(bool allowHttp, int timeoutMs, QObject *parent)
    : QObject(parent), m_allowHttp(allowHttp), m_timeoutMs(timeoutMs)
{
}
S3Transfer::~S3Transfer()
{
    qDeleteAll(findChildren<TransferCall *>(QString(), Qt::FindDirectChildrenOnly));
}
bool S3Transfer::validUrl(const QUrl &url) const
{
    return url.isValid() && !url.host().isEmpty() && url.userInfo().isEmpty() &&
           !url.hasFragment() && m_timeoutMs > 0 &&
           (url.scheme() == "https" || (m_allowHttp && url.scheme() == "http"));
}
QNetworkRequest S3Transfer::request(const QUrl &url) const
{
    QNetworkRequest request(url);
    // Une redirection invaliderait aussi la signature S3 : corriger l'endpoint
    // configuré au serveur, plutôt que suivre une nouvelle destination.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    return request;
}
TransferCall *S3Transfer::invalid()
{
    auto *call = new TransferCall(this);
    QTimer::singleShot(0, call, [call] {
        TransferResult result;
        result.failure = TransferFailure::InvalidRequest;
        call->complete(result);
    });
    return call;
}

TransferCall *S3Transfer::upload(const QUrl &url, std::shared_ptr<QIODevice> source, qint64 bytes)
{
    if (!validUrl(url) || !source || !source->isReadable() || source->isSequential() ||
        source->pos() != 0 || bytes < 0 || source->size() != bytes)
        return invalid();
    auto *call = new TransferCall(this);
    call->m_upload = true;
    call->m_source = std::move(source);
    auto outgoing = request(url);
    outgoing.setHeader(QNetworkRequest::ContentTypeHeader, "application/zstd");
    outgoing.setHeader(QNetworkRequest::ContentLengthHeader, bytes);
    outgoing.setAttribute(QNetworkRequest::DoNotBufferUploadDataAttribute, true);
    // QIODevice*, pas QByteArray : Qt lit progressivement le fichier préparé.
    // La taille explicite évite le chunked transfer que le PUT présigné refuse.
    auto *reply = m_http.put(outgoing, call->m_source.get());
    call->m_reply = reply;
    reply->setReadBufferSize(ChunkBytes);
    call->m_deadline.start(m_timeoutMs);
    connect(reply, &QIODevice::readyRead, call, [reply] {
        // Le corps S3 n'est pas une archive et n'est pas conservé (erreur XML).
        // Le drainer évite de bloquer la fin d'une grosse réponse d'erreur.
        while (reply->bytesAvailable())
            reply->read(ChunkBytes);
    });
    connect(reply, &QNetworkReply::finished, call, [call, reply, bytes] {
        auto result = transportResult(reply);
        if (result.failure == TransferFailure::None) {
            if (call->m_source->pos() != bytes || call->m_source->size() != bytes)
                result.failure = TransferFailure::LocalIo;
            else
                result.bytes = bytes;
        }
        call->complete(result);
    });
    return call;
}

TransferCall *S3Transfer::downloadExact(const QUrl &url, qint64 expectedBytes,
                                        const QByteArray &expectedSha256,
                                        const QString &stagingDirectory)
{
    static const QRegularExpression digestPattern("^[0-9a-f]{64}\\z");
    if (!validUrl(url) || expectedBytes < 0 || stagingDirectory.isEmpty() ||
        !digestPattern.match(QString::fromLatin1(expectedSha256)).hasMatch() ||
        !QDir(stagingDirectory).exists())
        return invalid();
    return receive(url, expectedBytes, expectedSha256, stagingDirectory);
}

TransferCall *S3Transfer::downloadCandidate(const QUrl &url, qint64 maximumBytes,
                                            const QString &stagingDirectory)
{
    if (!validUrl(url) || maximumBytes <= 0 || stagingDirectory.isEmpty() ||
        !QDir(stagingDirectory).exists())
        return invalid();
    return receive(url, maximumBytes, std::nullopt, stagingDirectory);
}

TransferCall *S3Transfer::receive(const QUrl &url, qint64 expectedBytes,
                                  std::optional<QByteArray> expectedSha256,
                                  const QString &stagingDirectory)
{
    auto *call = new TransferCall(this);
    call->m_staging = std::make_shared<QTemporaryFile>(
        QDir(stagingDirectory).filePath("retrosave-download-XXXXXX"));
    if (!call->m_staging->open()) {
        QTimer::singleShot(0, call, [call] {
            TransferResult result;
            result.failure = TransferFailure::LocalIo;
            call->complete(result);
        });
        return call;
    }
    auto hash = std::make_shared<QCryptographicHash>(QCryptographicHash::Sha256);
    auto received = std::make_shared<qint64>(0);
    auto *reply = m_http.get(request(url));
    call->m_reply = reply;
    reply->setReadBufferSize(ChunkBytes);
    call->m_deadline.start(m_timeoutMs);
    auto drain = [call, reply, hash, received, expectedBytes] {
        while (!call->m_done && reply->bytesAvailable()) {
            const auto chunk = reply->read(ChunkBytes);
            // Ne pas écrire une page d'erreur ou une redirection dans le staging.
            if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200)
                continue;
            if (chunk.size() > expectedBytes - *received) {
                TransferResult result;
                result.failure = TransferFailure::Integrity;
                call->complete(result);
                return;
            }
            if (call->m_staging->write(chunk) != chunk.size()) {
                TransferResult result;
                result.failure = TransferFailure::LocalIo;
                call->complete(result);
                return;
            }
            hash->addData(chunk);
            *received += chunk.size();
        }
    };
    connect(reply, &QIODevice::readyRead, call, drain);
    connect(reply, &QNetworkReply::finished, call,
            [call, reply, drain, hash, received, expectedBytes, expectedSha256] {
                drain();
                if (call->m_done)
                    return;
                auto result = transportResult(reply);
                if (result.failure == TransferFailure::None && result.httpStatus != 200)
                    result.failure = TransferFailure::Http;
                if (result.failure == TransferFailure::None) {
                    bool lengthOk = false;
                    const auto length = reply->rawHeader("Content-Length").toLongLong(&lengthOk);
                    // Même avec un JSON/tar complet, un HTTP tronqué n'est pas
                    // un succès. Qt 6.8 ne signale pas toujours cette coupure.
                    if (reply->hasRawHeader("Content-Length") &&
                        (!lengthOk || length < 0 || length != *received))
                        result.failure = TransferFailure::Network;
                    else if (expectedSha256 && (*received != expectedBytes ||
                                                hash->result().toHex() != *expectedSha256))
                        result.failure = TransferFailure::Integrity;
                    else if (!call->m_staging->flush())
                        result.failure = TransferFailure::LocalIo;
                    else {
                        call->m_staging->close();
                        result.bytes = *received;
                        result.archiveSha256 = hash->result().toHex();
                        result.archive = call->m_staging;
                    }
                }
                call->complete(result);
            });
    return call;
}
} // namespace retrosave::network
