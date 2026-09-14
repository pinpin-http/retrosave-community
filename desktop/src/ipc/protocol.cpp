#include "ipc/protocol.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QJsonDocument>
#include <QStandardPaths>

namespace retrosave::ipc
{
namespace
{
QString identity(const QString &value)
{
    // On hache uniquement un identifiant de canal, jamais un fichier utilisateur.
    return QString::fromLatin1(
        QCryptographicHash::hash(value.toUtf8(), QCryptographicHash::Sha256).toHex().left(24));
}

// Contrôles communs à tous les canaux, avant toute lecture de la méthode.
QString envelopeProblem(const QJsonObject &request)
{
    const auto id = request.value("id").toString();
    if (id.isEmpty() || id.size() > 64)
        return QStringLiteral("invalid_id");
    if (request.value("protocol") != QJsonValue(ProtocolVersion))
        return QStringLiteral("unsupported_protocol");
    return {};
}

QJsonObject envelope(const QJsonObject &request)
{
    return {{"protocol", ProtocolVersion}, {"id", request.value("id").toString()}};
}
} // namespace

QByteArray encode(const QJsonObject &message)
{
    return QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
}

QJsonObject statusRequest(const QString &id)
{
    return {{"protocol", ProtocolVersion}, {"id", id}, {"method", "status"}};
}

QJsonObject shutdownRequest(const QString &id)
{
    return {{"protocol", ProtocolVersion}, {"id", id}, {"method", "shutdown"}};
}

QJsonObject showRequest(const QString &id)
{
    return {{"protocol", ProtocolVersion}, {"id", id}, {"method", "show"}};
}

Reply reply(const QJsonObject &request)
{
    const auto method = request.value("method").toString();
    const auto problem = envelopeProblem(request);
    QJsonObject response = envelope(request);
    if (!problem.isEmpty()) {
        response.insert("error", problem);
    } else if (method == "status") {
        response.insert("result", QJsonObject{{"version", RETROSAVE_VERSION},
                                              {"pid", QCoreApplication::applicationPid()},
                                              {"state", "prototype"},
                                              {"sync_available", false}});
    } else if (method == "shutdown") {
        // L'arrêt est accusé avant de quitter : l'interface distingue un agent
        // qui a obéi d'un agent devenu injoignable pour une autre raison.
        response.insert(
            "result", QJsonObject{{"accepted", true}, {"pid", QCoreApplication::applicationPid()}});
        return {response, true};
    } else {
        // Pas de commande sync/restore tant que le moteur n'est pas migré.
        response.insert("error", "unknown_method");
    }
    return {response, false};
}

Reply windowReply(const QJsonObject &request)
{
    const auto problem = envelopeProblem(request);
    QJsonObject response = envelope(request);
    if (!problem.isEmpty())
        response.insert("error", problem);
    else if (request.value("method").toString() == "show")
        // La fenêtre déjà lancée se montre : un second lancement ne crée pas
        // une deuxième interface et ne laisse jamais l'utilisateur sans accès.
        response.insert("result",
                        QJsonObject{{"shown", true}, {"pid", QCoreApplication::applicationPid()}});
    else
        response.insert("error", "unknown_method");
    return {response, false};
}

QString defaultEndpoint()
{
    const auto userData = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    return "retrosave-community-v1-" + identity(userData);
}

QString windowEndpoint(const QString &agentEndpoint)
{
    return agentEndpoint + "-ui";
}

QJsonObject connectRequest(const QString &id, const QString &url, const QString &token,
                           const QString &deviceName)
{
    return {{"protocol", ProtocolVersion},
            {"id", id},
            {"method", "connect"},
            {"url", url},
            {"token", token},
            {"device_name", deviceName}};
}

QJsonObject syncRequest(const QString &id)
{
    return {{"protocol", ProtocolVersion}, {"id", id}, {"method", "sync"}};
}

QJsonObject conflictsRequest(const QString &id)
{
    return {{"protocol", ProtocolVersion}, {"id", id}, {"method", "conflicts"}};
}

QJsonObject resolveRequest(const QString &id, const QString &conflictId, int winner)
{
    return {{"protocol", ProtocolVersion},
            {"id", id},
            {"method", "resolve"},
            {"conflict_id", conflictId},
            {"winner", winner}};
}

QJsonObject unitsRequest(const QString &id)
{
    return {{"protocol", ProtocolVersion}, {"id", id}, {"method", "units"}};
}

QJsonObject historyRequest(const QString &id, const QString &unitKey)
{
    return {
        {"protocol", ProtocolVersion}, {"id", id}, {"method", "history"}, {"unit_key", unitKey}};
}

QJsonObject restoreRequest(const QString &id, const QString &unitKey, int number)
{
    return {{"protocol", ProtocolVersion},
            {"id", id},
            {"method", "restore"},
            {"unit_key", unitKey},
            {"version", number}};
}

QJsonObject retryRequest(const QString &id, const QString &unitKey)
{
    return {{"protocol", ProtocolVersion}, {"id", id}, {"method", "retry"}, {"unit_key", unitKey}};
}

QJsonObject selectRequest(const QString &id, const QString &unitKey, const QString &mode)
{
    return {{"protocol", ProtocolVersion},
            {"id", id},
            {"method", "select"},
            {"unit_key", unitKey},
            {"mode", mode}};
}

QJsonObject pauseAllRequest(const QString &id, bool paused)
{
    return {{"protocol", ProtocolVersion},
            {"id", id},
            {"method", "pause_all"},
            {"paused", paused}};
}

QJsonObject notificationsRequest(const QString &id, bool conflicts, bool errors, bool restores)
{
    return {{"protocol", ProtocolVersion}, {"id", id},           {"method", "notifications"},
            {"conflicts", conflicts},      {"errors", errors},   {"restores", restores}};
}

QJsonObject devicesRequest(const QString &id)
{
    return {{"protocol", ProtocolVersion}, {"id", id}, {"method", "devices"}};
}

QJsonObject deviceRenameRequest(const QString &id, const QString &deviceId, const QString &name)
{
    return {{"protocol", ProtocolVersion},
            {"id", id},
            {"method", "device_rename"},
            {"device_id", deviceId},
            {"name", name}};
}

QJsonObject deviceRevokeRequest(const QString &id, const QString &deviceId)
{
    return {{"protocol", ProtocolVersion},
            {"id", id},
            {"method", "device_revoke"},
            {"device_id", deviceId}};
}

QJsonObject activityRequest(const QString &id)
{
    return {{"protocol", ProtocolVersion}, {"id", id}, {"method", "activity"}};
}

QJsonObject diagnosticRequest(const QString &id, const QString &path)
{
    return {{"protocol", ProtocolVersion}, {"id", id}, {"method", "diagnostic"}, {"path", path}};
}

QJsonObject exportRequest(const QString &id, const QString &path)
{
    return {{"protocol", ProtocolVersion}, {"id", id}, {"method", "export"}, {"path", path}};
}

QJsonObject serverCheckRequest(const QString &id)
{
    return {{"protocol", ProtocolVersion}, {"id", id}, {"method", "server_check"}};
}

QJsonObject artworkRequest(const QString &id, const QString &unitKey, const QString &imagePath)
{
    return {{"protocol", ProtocolVersion},
            {"id", id},
            {"method", "artwork"},
            {"unit_key", unitKey},
            {"path", imagePath}};
}

QString settingsFile()
{
    // GenericConfigLocation suit XDG_CONFIG_HOME sous Linux : un test peut donc
    // isoler complètement les réglages sans toucher au poste.
    const auto directory =
        QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) +
        "/retrosave-community";
    return QDir(directory).filePath("retrosave-community.conf");
}

QString lockFilePath(const QString &endpoint)
{
    const auto directory = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
                           "/retrosave-community/ipc";
    // Le demandeur vérifie la création et les permissions avant d'utiliser ce chemin.
    return QDir(directory).filePath(identity(endpoint) + ".lock");
}

} // namespace retrosave::ipc
