#include "network/v0client.h"

#include "network/v0json_p.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QRegularExpression>
#include <cmath>

namespace retrosave::network
{
using namespace json;
namespace
{
std::optional<ConflictHead> head(const QJsonValue &value)
{
    if (!value.isObject())
        return {};
    const auto object = value.toObject();
    const auto number = integer(object.value("number"), 1);
    const auto size = integer(object.value("size_bytes"));
    const auto digest = object.value("content_sha256").toString();
    if (!number || !size || !hash(digest))
        return {};
    return ConflictHead{*number, digest, *size};
}
} // namespace

ApiCall::ApiCall(QObject *parent) : QObject(parent)
{
    m_deadline.setSingleShot(true);
    connect(&m_deadline, &QTimer::timeout, this, [this] {
        ApiResult result;
        result.failure = Failure::Timeout;
        result.mayHaveCommitted = m_write;
        complete(result);
    });
}

ApiCall::~ApiCall()
{
    if (m_reply) {
        disconnect(m_reply, nullptr, this, nullptr);
        m_reply->abort();
        m_reply->deleteLater();
    }
}

void ApiCall::cancel()
{
    ApiResult result;
    result.failure = Failure::Cancelled;
    result.mayHaveCommitted = m_write;
    complete(result);
}

void ApiCall::complete(ApiResult result)
{
    if (m_done)
        return;
    // abort() peut émettre finished immédiatement : désarmer avant l'appel
    // garantit exactement UNE notification, même lors d'un timeout ou cancel.
    m_done = true;
    m_deadline.stop();
    if (m_reply) {
        disconnect(m_reply, nullptr, this, nullptr);
        if (!m_reply->isFinished())
            m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
    }
    emit finished(result);
}

V0Client::V0Client(ApiConfig config, QObject *parent) : QObject(parent), m_config(std::move(config))
{
}

V0Client::~V0Client()
{
    // Les enfants QObject seraient normalement détruits APRÈS les membres
    // C++. Annuler les appels d'abord empêche m_http d'émettre un callback
    // pendant la destruction de ce client partiellement démonté.
    qDeleteAll(findChildren<ApiCall *>(QString(), Qt::FindDirectChildrenOnly));
}

ApiCall *V0Client::prepare(const QString &unitId, const PrepareVersion &version,
                           const QByteArray &key)
{
    return send(Operation::Prepare, unitId, 0,
                {{"base_version", version.baseVersion},
                 {"content_sha256", version.contentSha256},
                 {"archive_sha256", version.archiveSha256},
                 {"size", version.size},
                 {"archive_bytes", version.archiveBytes}},
                key,
                safeInteger(version.baseVersion) && safeInteger(version.size) &&
                    safeInteger(version.archiveBytes) && hash(version.contentSha256) &&
                    hash(version.archiveSha256));
}

ApiCall *V0Client::confirm(const QString &unitId, const ConfirmVersion &version,
                           const QByteArray &key)
{
    return send(Operation::Confirm, unitId, 0,
                {{"base_version", version.baseVersion},
                 {"content_sha256", version.contentSha256},
                 {"archive_sha256", version.archiveSha256},
                 {"object_key", version.objectKey},
                 {"env", version.environment},
                 {"client_mtime", version.clientMtime ? QJsonValue(*version.clientMtime)
                                                      : QJsonValue(QJsonValue::Null)}},
                key,
                safeInteger(version.baseVersion) && !version.objectKey.isEmpty() &&
                    hash(version.contentSha256) && hash(version.archiveSha256) &&
                    (!version.clientMtime || timestamp(*version.clientMtime)));
}

ApiCall *V0Client::downloadTarget(const QString &unitId, qint64 number)
{
    return send(Operation::Download, unitId, number, {}, {}, number > 0 && safeInteger(number));
}

ApiCall *V0Client::send(Operation operation, const QString &unitId, qint64 number, QJsonObject body,
                        const QByteArray &key, bool valid)
{
    auto *call = new ApiCall(this);
    // La route, le verbe et les credentials sont choisis ensemble. Aucun chemin
    // fourni par un utilisateur n'est concaténé ici sans validation d'UUID.
    bool write = false;
    bool authenticated = true;
    bool includeDevice = true;
    bool resource = false;
    QString suffix;
    switch (operation) {
    case Operation::Health:
        suffix = "/healthz";
        authenticated = false;
        includeDevice = false;
        break;
    case Operation::RegisterDevice:
        suffix = "/v0/devices";
        write = true;
        includeDevice = false;
        break;
    case Operation::Devices:
        suffix = "/v0/devices";
        break;
    case Operation::RenameDevice:
        // `resource` fait valider l'identifiant comme UUID avant de le
        // concaténer : aucun segment de chemin n'est repris tel quel.
        suffix = "/v0/devices/" + unitId;
        write = true;
        resource = true;
        break;
    case Operation::RevokeDevice:
        suffix = "/v0/devices/" + unitId + "/revoke";
        write = true;
        resource = true;
        break;
    case Operation::Units:
        suffix = "/v0/units";
        break;
    case Operation::CreateUnit:
        suffix = "/v0/units";
        write = true;
        break;
    case Operation::Conflicts:
        suffix = "/v0/conflicts";
        break;
    case Operation::Resolve:
        suffix = "/v0/conflicts/" + unitId + "/resolve";
        write = true;
        resource = true;
        break;
    default:
        resource = true;
        suffix = "/v0/units/" + unitId;
        switch (operation) {
        case Operation::Prepare:
            suffix += "/versions:prepare";
            write = true;
            break;
        case Operation::Confirm:
            suffix += "/versions:confirm";
            write = true;
            break;
        case Operation::Download:
            suffix += "/versions/" + QString::number(number) + "/download";
            break;
        case Operation::History:
            suffix += "/versions";
            break;
        case Operation::Restore:
            suffix += "/restore";
            write = true;
            break;
        case Operation::Missing:
            suffix += "/missing";
            write = true;
            break;
        case Operation::Rename:
            write = true;
            break;
        default:
            valid = false;
            break;
        }
    }
    const auto &base = m_config.serverUrl;
    if (!valid || (resource && !uuid(unitId)) || (authenticated && !header(m_config.token)) ||
        (includeDevice && !uuid(QString::fromLatin1(m_config.deviceId))) ||
        (write && !header(key)) || !httpUrl(base) || base.hasQuery() ||
        (base.scheme() == "http" && !m_config.allowHttp) || m_config.timeoutMs <= 0) {
        // Même les erreurs locales sont différées : le demandeur peut brancher
        // son slot finished immédiatement après le retour de prepare/confirm.
        QTimer::singleShot(0, call, [call] {
            ApiResult result;
            result.failure = Failure::InvalidRequest;
            call->complete(result);
        });
        return call;
    }
    QString path = base.path();
    while (path.endsWith('/'))
        path.chop(1);
    path += suffix;
    QUrl url = base;
    url.setPath(path);
    if (operation == Operation::Conflicts)
        url.setQuery("open=1");
    QNetworkRequest request(url);
    if (authenticated)
        request.setRawHeader("Authorization", "Bearer " + m_config.token);
    if (includeDevice)
        request.setRawHeader("X-Device-Id", m_config.deviceId);
    request.setRawHeader("Accept", "application/json");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    if (write) {
        request.setRawHeader("Idempotency-Key", key); // clé de l'appelant, jamais régénérée ici
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    }
    call->m_write = write;
    const auto encoded = QJsonDocument(body).toJson(QJsonDocument::Compact);
    auto *reply = operation == Operation::Rename || operation == Operation::RenameDevice
                      ? m_http.sendCustomRequest(request, "PATCH", encoded)
                  : write ? m_http.post(request, encoded)
                          : m_http.get(request);
    call->m_reply = reply;
    reply->setReadBufferSize(64 * 1024);
    call->m_deadline.start(m_config.timeoutMs);
    auto collect = [call, reply] {
        if (call->m_done)
            return;
        call->m_body += reply->read(MaxResponseBytes - call->m_body.size() + 1);
        if (call->m_body.size() > MaxResponseBytes) {
            ApiResult result;
            result.failure = Failure::ResponseTooLarge;
            result.mayHaveCommitted = call->m_write;
            call->complete(result);
        }
    };
    connect(reply, &QIODevice::readyRead, call, collect);
    connect(reply, &QNetworkReply::finished, call, [call, reply, operation, collect] {
        collect();
        if (call->m_done)
            return;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        ApiResult result;
        // Qt signale aussi les 4xx comme erreurs réseau. Il faut d'abord lire
        // leur corps métier (409 = conflit conservé), sans avaler les vraies
        // coupures survenues pendant un corps 200 partiellement reçu.
        const auto error = reply->error();
        const bool brokenTransport =
            error != QNetworkReply::NoError &&
            (status == 0 || status < 400 || error < QNetworkReply::ContentAccessDenied);
        // Qt 6.8 peut terminer sans erreur malgré un Content-Length incomplet.
        // Vérifier les octets collectés quand cet en-tête est disponible. Qt le
        // retire en cas de décompression automatique : ne pas comparer alors
        // une taille compressée à notre JSON décompressé (OriginalContentLength).
        bool lengthOk = false;
        const auto declared = reply->rawHeader("Content-Length").toLongLong(&lengthOk);
        const bool wrongLength = reply->hasRawHeader("Content-Length") &&
                                 (!lengthOk || declared < 0 || declared != call->m_body.size());
        if (brokenTransport || wrongLength) {
            result.failure = Failure::Transport;
            result.httpStatus = status;
        } else {
            result = decode(operation, status, call->m_body);
        }
        result.mayHaveCommitted =
            call->m_write && result.failure != Failure::None &&
            !(result.failure == Failure::Http && status >= 400 && status < 500);
        call->complete(result);
    });
    return call;
}

ApiResult V0Client::decode(Operation operation, int status, const QByteArray &bytes)
{
    ApiResult result;
    result.httpStatus = status;
    result.failure = Failure::InvalidResponse;
    if (status >= 300 && status < 400) {
        result.failure = Failure::Redirect;
        return result; // ne jamais transmettre le Bearer à une destination redirigée
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return result;
    const auto object = document.object();
    if (status < 400 && object.contains("error"))
        return result;
    if (status >= 400) {
        const auto code = object.value("error").toObject().value("code").toString();
        static const QRegularExpression pattern("^[a-z_]{1,64}\\z");
        if (!pattern.match(code).hasMatch())
            return result;
        result.serverCode = code;
        const bool conflictOperation = operation == Operation::Prepare ||
                                       operation == Operation::Confirm ||
                                       operation == Operation::Restore;
        if (status == 409 && conflictOperation &&
            (code == "cas_conflict" || code == "open_conflict")) {
            const bool cas = code == "cas_conflict";
            const auto first = head(object.value(cas ? "head" : "version_a"));
            const auto second = head(object.value(cas ? "yours" : "version_b"));
            const auto id = object.value("conflict_id").toString();
            if (!first || !second || !uuid(id))
                return result;
            result.payload = Conflict{code, id, *first, *second};
            result.failure = Failure::None;
        } else {
            result.failure = Failure::Http;
        }
        return result;
    }
    if (operation == Operation::Prepare && status == 200) {
        if (object.value("duplicate") == QJsonValue(true)) {
            const auto version = integer(object.value("version"), 1);
            if (!version || object.contains("upload"))
                return result;
            result.payload = Duplicate{*version};
        } else {
            const auto upload = object.value("upload").toObject();
            const QUrl url(upload.value("url").toString());
            const auto key = upload.value("object_key").toString();
            const auto expires = upload.value("expires_at").toString();
            if (!httpUrl(url) || key.isEmpty() || !timestamp(expires) ||
                object.contains("duplicate"))
                return result;
            result.payload = UploadTarget{url, key, expires};
        }
    } else if ((operation == Operation::Confirm || operation == Operation::Restore) &&
               (status == 200 || status == 201)) {
        const auto version = integer(object.value("version"), 1);
        if (!version)
            return result;
        result.payload = Published{*version};
    } else if (operation == Operation::Download && status == 200) {
        const QUrl url(object.value("url").toString());
        const auto content = object.value("content_sha256").toString();
        const auto archive = object.value("archive_sha256").toString();
        const auto size = integer(object.value("archive_bytes"));
        const auto expires = object.value("expires_at").toString();
        if (!httpUrl(url) || !hash(content) || !hash(archive) || !size || !timestamp(expires))
            return result;
        result.payload = DownloadTarget{url, content, archive, *size, expires};
    } else {
        return decodeCatalog(operation, status, object);
    }
    result.failure = Failure::None;
    return result;
}

} // namespace retrosave::network
