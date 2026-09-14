#include "app/agentclient.h"

#include <QUrl>
#include "app/installation.h"
#include "ipc/protocol.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QUuid>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace retrosave
{

AgentClient::AgentClient(QString endpoint, QObject *parent)
    : QObject(parent), m_endpoint(std::move(endpoint)), m_message(tr("Agent unchecked."))
{
    m_timeout.setSingleShot(true);
    m_socket.setReadBufferSize(ipc::MaxFrameBytes + 1);
    connect(&m_timeout, &QTimer::timeout, this,
            [this] { fail(tr("The agent is not responding.")); });
    connect(&m_socket, &QLocalSocket::connected, this, [this] {
        QJsonObject request;
        if (m_method == "shutdown")
            request = ipc::shutdownRequest(m_requestId);
        else if (m_method == "sync")
            request = ipc::syncRequest(m_requestId);
        else if (m_method == "conflicts")
            request = ipc::conflictsRequest(m_requestId);
        else if (m_method == "resolve")
            request = ipc::resolveRequest(m_requestId, m_extra.value("conflict_id").toString(),
                                          m_extra.value("winner").toInt());
        else if (m_method == "units")
            request = ipc::unitsRequest(m_requestId);
        else if (m_method == "history")
            request = ipc::historyRequest(m_requestId, m_extra.value("unit_key").toString());
        else if (m_method == "restore")
            request = ipc::restoreRequest(m_requestId, m_extra.value("unit_key").toString(),
                                          m_extra.value("version").toInt());
        else if (m_method == "connect")
            request = ipc::connectRequest(m_requestId, m_extra.value("url").toString(),
                                          m_extra.value("token").toString(),
                                          m_extra.value("device_name").toString());
        else if (m_method == "retry")
            request = ipc::retryRequest(m_requestId, m_extra.value("unit_key").toString());
        else if (m_method == "artwork")
            request = ipc::artworkRequest(m_requestId, m_extra.value("unit_key").toString(),
                                          m_extra.value("path").toString());
        else if (m_method == "select")
            request = ipc::selectRequest(m_requestId, m_extra.value("unit_key").toString(),
                                         m_extra.value("mode").toString());
        else if (m_method == "pause_all")
            request = ipc::pauseAllRequest(m_requestId, m_extra.value("paused").toBool());
        else if (m_method == "notifications")
            request = ipc::notificationsRequest(m_requestId, m_extra.value("conflicts").toBool(),
                                                m_extra.value("errors").toBool(),
                                                m_extra.value("restores").toBool());
        else if (m_method == "devices")
            request = ipc::devicesRequest(m_requestId);
        else if (m_method == "device_rename")
            request = ipc::deviceRenameRequest(m_requestId, m_extra.value("device_id").toString(),
                                               m_extra.value("name").toString());
        else if (m_method == "device_revoke")
            request = ipc::deviceRevokeRequest(m_requestId, m_extra.value("device_id").toString());
        else if (m_method == "activity")
            request = ipc::activityRequest(m_requestId);
        else if (m_method == "diagnostic")
            request = ipc::diagnosticRequest(m_requestId, m_extra.value("path").toString());
        else if (m_method == "export")
            request = ipc::exportRequest(m_requestId, m_extra.value("path").toString());
        else if (m_method == "server_check")
            request = ipc::serverCheckRequest(m_requestId);
        else if (m_method == "status")
            request = ipc::statusRequest(m_requestId);
        else {
            // Une commande inconnue ne doit jamais devenir un sondage : cela
            // masquerait l'erreur et perdrait l'action utilisateur.
            qCritical().noquote() << "Méthode non construite par le client :" << m_method;
            fail(tr("Not wired to the agent yet."));
            return;
        }
        m_socket.write(ipc::encode(request));
    });
    connect(&m_socket, &QLocalSocket::readyRead, this, &AgentClient::receive);
    connect(&m_socket, &QLocalSocket::errorOccurred, this, [this] {
        if (m_pending)
            fail(unavailableMessage());
    });
    connect(&m_socket, &QLocalSocket::disconnected, this, [this] {
        if (m_pending)
            fail(tr("The connection closed before the agent replied."));
    });
}

namespace
{
// Le rapport, dit en français. Un utilisateur n'a pas à décoder « pulled=2 » :
// il veut savoir si sa partie est en sécurité et s'il doit faire quelque chose.
QString describe(const QJsonObject &last)
{
    if (!last.value("ran").toBool())
        return {};
    QStringList parts;
    const auto count = [&](const char *key) { return last.value(key).toInt(); };
    if (count("pushed") > 0)
        parts << AgentClient::tr("%n sauvegarde(s) envoyée(s)", nullptr, count("pushed"));
    if (count("pulled") > 0)
        parts << AgentClient::tr("%n reçue(s)", nullptr, count("pulled"));
    if (count("duplicates") > 0)
        parts << AgentClient::tr("%n déjà connue(s) du serveur", nullptr, count("duplicates"));
    if (count("reapplied") > 0)
        parts << AgentClient::tr("%n écriture(s) reprise(s)", nullptr, count("reapplied"));
    if (parts.isEmpty())
        parts << AgentClient::tr("nothing to transfer");
    auto text = AgentClient::tr("%1 save(s) checked: %2.")
                    .arg(count("scanned"))
                    .arg(parts.join(", "));
    // Ce qui demande une action de l'utilisateur vient APRÈS le résumé, jamais
    // à la place : il doit voir que le reste a fonctionné.
    if (last.value("paused").toBool())
        text += AgentClient::tr(" Sync suspended: many saves vanished at once, "
                                "nothing was propagated.");
    if (count("conflicts") > 0)
        text += AgentClient::tr(" %n conflict(s) to resolve; both versions are kept.",
                                nullptr, count("conflicts"));
    if (count("errors") > 0)
        text += AgentClient::tr(" %n save(s) in error; the rest went through.", nullptr,
                                count("errors"));
    return text;
}
} // namespace

QString AgentClient::unavailableMessage() const
{
    return m_stoppedByUser
               ? tr("Agent stopped. Start it again to resume.")
               : tr("Agent unavailable.");
}

void AgentClient::refresh()
{
    send("status");
}

void AgentClient::stopAgent()
{
    // L'arrêt passe par le protocole, jamais par un signal envoyé au PID :
    // l'agent range son canal et rend son verrou avant de disparaître.
    if (m_state != "online")
        return;
    send("shutdown");
}

void AgentClient::connectAccount(const QString &url, const QString &token,
                                 const QString &deviceName)
{
    if (m_state != "online")
        return;
    m_syncProblem.clear();
    send("connect", {{"url", url}, {"token", token}, {"device_name", deviceName}});
}

void AgentClient::synchronize()
{
    if (m_state != "online")
        return;
    m_syncProblem.clear();
    send("sync");
}

void AgentClient::refreshConflicts()
{
    if (m_state != "online")
        return;
    send("conflicts");
}

void AgentClient::resolveConflict(const QString &conflictId, int winner)
{
    if (m_state != "online" || conflictId.isEmpty() || winner <= 0)
        return;
    m_syncProblem.clear();
    send("resolve", {{"conflict_id", conflictId}, {"winner", winner}});
}

void AgentClient::refreshUnits()
{
    if (m_state != "online")
        return;
    send("units");
}

void AgentClient::openHistory(const QString &unitKey)
{
    if (m_state != "online" || unitKey.isEmpty())
        return;
    m_syncProblem.clear();
    send("history", {{"unit_key", unitKey}});
}

void AgentClient::restoreVersion(const QString &unitKey, int version)
{
    if (m_state != "online" || unitKey.isEmpty() || version <= 0)
        return;
    m_syncProblem.clear();
    send("restore", {{"unit_key", unitKey}, {"version", version}});
}

void AgentClient::retryUnit(const QString &unitKey)
{
    if (m_state != "online" || unitKey.isEmpty())
        return;
    m_syncProblem.clear();
    send("retry", {{"unit_key", unitKey}});
}

void AgentClient::chooseArtwork(const QString &unitKey, const QString &imagePath)
{
    if (m_state != "online" || unitKey.isEmpty())
        return;
    m_syncProblem.clear();
    // `QUrl::toLocalFile` : le sélecteur de fichiers de QML rend une URL
    // `file://`, l'agent attend un chemin. Traduire ici évite que chaque
    // appelant s'en souvienne.
    const auto path = imagePath.startsWith("file:") ? QUrl(imagePath).toLocalFile() : imagePath;
    send("artwork", {{"unit_key", unitKey}, {"path", path}});
}

void AgentClient::setLocalMode(const QString &unitKey, const QString &mode)
{
    if (m_state != "online" || unitKey.isEmpty())
        return;
    m_syncProblem.clear();
    send("select", {{"unit_key", unitKey}, {"mode", mode}});
}

void AgentClient::setGlobalPause(bool paused)
{
    if (m_state != "online")
        return;
    m_syncProblem.clear();
    send("pause_all", {{"paused", paused}});
}

void AgentClient::setNotifications(bool conflicts, bool errors, bool restores)
{
    if (m_state != "online")
        return;
    m_syncProblem.clear();
    send("notifications", {{"conflicts", conflicts}, {"errors", errors}, {"restores", restores}});
}

void AgentClient::refreshDevices()
{
    if (m_state != "online")
        return;
    send("devices");
}

void AgentClient::renameDevice(const QString &deviceId, const QString &name)
{
    if (m_state != "online" || deviceId.isEmpty() || name.trimmed().isEmpty())
        return;
    m_syncProblem.clear();
    send("device_rename", {{"device_id", deviceId}, {"name", name.trimmed()}});
}

void AgentClient::revokeDevice(const QString &deviceId)
{
    if (m_state != "online" || deviceId.isEmpty())
        return;
    m_syncProblem.clear();
    send("device_revoke", {{"device_id", deviceId}});
}

void AgentClient::refreshActivity()
{
    if (m_state != "online")
        return;
    send("activity");
}

void AgentClient::exportDiagnostic(const QString &path)
{
    if (m_state != "online" || path.isEmpty())
        return;
    m_syncProblem.clear();
    // Le sélecteur QML rend une URL `file://` ; l'agent attend un chemin.
    const auto local = path.startsWith("file:") ? QUrl(path).toLocalFile() : path;
    send("diagnostic", {{"path", local}});
}

void AgentClient::exportData(const QString &path)
{
    if (m_state != "online" || path.isEmpty())
        return;
    m_syncProblem.clear();
    const auto local = path.startsWith("file:") ? QUrl(path).toLocalFile() : path;
    send("export", {{"path", local}});
}

void AgentClient::checkServer()
{
    if (m_state != "online")
        return;
    send("server_check");
}

void AgentClient::send(const QString &method, const QJsonObject &extra)
{
    // Pas de waitForConnected() ici : attendre bloquerait le thread de l'UI.
    // Les signaux ci-dessus poursuivent l'opération quand le système répond.
    if (m_pending) {
        // Une action utilisateur est prioritaire sur le sondage périodique.
        if (method != QLatin1String("status")) {
            m_queued = true;
            m_queuedMethod = method;
            m_queuedExtra = extra;
        }
        return;
    }
    m_socket.abort();
    m_buffer.clear();
    m_method = method;
    m_extra = extra;
    m_requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_pending = true;
    m_timeout.start(3000);
    emit changed();
    m_socket.connectToServer(m_endpoint);
}

void AgentClient::receive()
{
    if (!m_pending)
        return;
    m_buffer.append(m_socket.readAll());
    if (m_buffer.size() > ipc::MaxFrameBytes) {
        fail(tr("The agent's reply was too large."));
        return;
    }
    const auto newline = m_buffer.indexOf('\n');
    if (newline < 0)
        return;
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(m_buffer.left(newline), &error);
    const auto response = document.object();
    const auto result = response.value("result").toObject();
    const auto mismatch =
        tr("Incompatible reply. Align the interface and agent versions.");
    if (error.error != QJsonParseError::NoError || !document.isObject() ||
        response.value("protocol") != QJsonValue(ipc::ProtocolVersion) ||
        response.value("id") != QJsonValue(m_requestId)) {
        fail(mismatch);
        return;
    }
    if (response.contains("error")) {
        // Un agent antérieur à ce lot ne connaît pas l'arrêt : le dire avec le
        // geste de repli, plutôt que de laisser croire à une panne du canal.
        fail(m_method == "shutdown" && response.value("error").toString() == "unknown_method"
                 ? tr("This agent cannot stop on request. End process %1.")
                       .arg(m_pid)
                 : mismatch);
        return;
    }
    if (m_method == "shutdown") {
        if (result.value("accepted") != QJsonValue(true)) {
            fail(mismatch);
            return;
        }
        m_state = "offline";
        m_stoppedByUser = true;
        m_version.clear();
        m_pid.clear();
        m_message = unavailableMessage();
        // L'accusé d'arrêt est un contact réussi : la date reste honnête.
        m_lastChecked = QDateTime::currentDateTime().toString("HH:mm:ss");
        finish();
        return;
    }
    // Seul `status` met à jour l'instantané. Toute autre méthode confirme une
    // action dont l'effet sera observé au prochain sondage.
    if (m_method != QLatin1String("status")) {
        // La demande a été transmise ; l'état réel viendra du prochain status.
        // Une réponse « acceptée » n'est PAS une réussite de synchronisation,
        // et l'interface ne doit jamais laisser croire le contraire.
        if (!result.value("accepted").toBool(false))
            m_syncProblem = result.value("reason").toString(tr("Refused by the agent."));
        finish();
        QTimer::singleShot(400, this, &AgentClient::refresh);
        return;
    }
    const auto state = result.value("state").toString();
    if ((state != "idle" && state != "syncing") ||
        result.value("sync_available") != QJsonValue(true) ||
        result.value("version").toString().isEmpty() || !result.value("pid").isDouble() ||
        result.value("pid").toDouble() <= 0) {
        fail(mismatch);
        return;
    }
    m_state = "online";
    m_stoppedByUser = false;
    m_linked = result.value("connected").toBool();
    m_ready = result.value("ready").toBool();
    m_syncing = state == "syncing";
    const auto problem = result.value("problem").toString();
    if (!problem.isEmpty())
        m_syncProblem = problem;
    const auto last = result.value("last").toObject();
    m_lastSummary = describe(last);
    // La liste arrive telle que l'agent l'a bornée ; l'interface ne la
    // recompose pas, elle l'affiche.
    m_conflicts = result.value("conflicts").toArray().toVariantList();
    m_units = result.value("units").toArray().toVariantList();
    m_adapters = result.value("adapters").toArray().toVariantList();
    m_devices = result.value("devices").toArray().toVariantList();
    m_activity = result.value("activity").toArray().toVariantList();
    m_globalPause = result.value("global_pause").toBool();
    m_notifyConflicts = result.value("notify_conflicts").toBool(true);
    m_notifyErrors = result.value("notify_errors").toBool(true);
    m_notifyRestores = result.value("notify_restores").toBool(true);
    m_serverVersion = result.value("server_version").toString();
    m_serverCompatibility = result.value("server_compatibility").toString();
    m_outcome = result.value("outcome").toString();
    const auto history = result.value("history").toObject();
    m_historyUnit = history.value("unit").toString();
    m_historyHead = history.value("head").toInt();
    m_versions = history.value("versions").toArray().toVariantList();
    // Les compteurs restent des chaînes : l'interface les affiche, elle ne
    // calcule rien avec. Un rapport absent vaut zéro, pas « inconnu ».
    for (const auto &key : {"pushed", "pulled", "conflicts", "errors"})
        m_stats.insert(QString::fromLatin1(key),
                       QString::number(last.value(QLatin1String(key)).toInt()));
    m_message = m_linked ? (m_ready ? tr("Agent running, server linked.")
                                    : tr("Server linked. Choose a folder to sync."))
                         : tr("Agent running. Connect a server to sync.");
    m_version = result.value("version").toString();
    m_pid = QString::number(result.value("pid").toInteger());
    m_lastChecked = QDateTime::currentDateTime().toString("HH:mm:ss");
    // Une passe en cours mérite un contrôle rapproché : l'utilisateur vient de
    // cliquer et attend de voir quelque chose bouger.
    if (m_syncing)
        QTimer::singleShot(1000, this, &AgentClient::refresh);
    finish();
}

void AgentClient::fail(const QString &message)
{
    m_state = "offline";
    m_message = message;
    m_version.clear();
    m_pid.clear();
    // La date du dernier SUCCÈS reste visible même si le contrôle courant échoue.
    finish();
}

void AgentClient::finish()
{
    m_pending = false; // avant abort(), qui peut émettre disconnected immédiatement
    m_timeout.stop();
    m_socket.abort();
    emit changed();
    if (!m_queued)
        return;
    // On vide la réserve APRÈS avoir signalé la fin : l'interface voit d'abord
    // le résultat de la requête précédente, puis le départ de la suivante.
    m_queued = false;
    const auto method = m_queuedMethod;
    const auto extra = m_queuedExtra;
    m_queuedMethod.clear();
    m_queuedExtra = {};
    send(method, extra);
}

void AgentClient::startAgent()
{
    if (m_pending || m_state == "online")
        return;
    m_stoppedByUser = false;
    // Le processus détaché survit à l'objet QProcess local et à l'interface.
    QProcess process;
    process.setProgram(installation::agentExecutable());
    process.setArguments({"--socket", m_endpoint});
    process.setStandardInputFile(QProcess::nullDevice());
    process.setStandardOutputFile(QProcess::nullDevice());
    process.setStandardErrorFile(QProcess::nullDevice());
#ifdef Q_OS_WIN
    process.setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments *args) { args->flags |= CREATE_NO_WINDOW; });
#endif
    // Détaché = la fermeture de la fenêtre ne tue pas l'agent. Son verrou
    // protège contre deux clics/instances lançant le même agent en parallèle.
    if (!process.startDetached()) {
        fail(
            tr("Could not start the agent. Check that it sits next to the interface."));
        return;
    }
    m_message = tr("Starting…");
    emit changed();
    QTimer::singleShot(500, this, &AgentClient::refresh);
}

} // namespace retrosave
