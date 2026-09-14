#include "ipc/localservice.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QTimer>
#include <utility>

namespace retrosave::ipc
{

LocalService::LocalService(Responder responder, QObject *parent)
    : QObject(parent), m_responder(std::move(responder))
{
    // Sur Unix : permissions du socket ; sur Windows : ACL du named pipe.
    // Ce canal ne constitue pas une frontière contre un programme malveillant
    // exécuté sous le même utilisateur. Aucun secret n'y transite.
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    connect(&m_server, &QLocalServer::newConnection, this, &LocalService::acceptConnections);
}

LocalService::~LocalService()
{
    m_server.close();
    m_lock.reset();
}

bool LocalService::listen(const QString &endpoint)
{
    const auto lockPath = lockFilePath(endpoint);
    const auto directory = QFileInfo(lockPath).absolutePath();
    if (!QDir().mkpath(directory)) {
        m_error = tr("Impossible de créer le dossier privé de RetroSave.");
        return false;
    }
#ifdef Q_OS_UNIX
    if (!QFile::setPermissions(directory, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner)) {
        m_error = tr("Impossible de protéger le dossier privé de RetroSave.");
        return false;
    }
#endif
    auto lock = std::make_unique<QLockFile>(lockPath);
    // L'âge seul ne rend pas un verrou invalide : le processus peut rester des
    // jours. QLockFile reconnaît néanmoins un PID mort après un crash.
    lock->setStaleLockTime(0);
    if (!lock->tryLock()) {
        m_error = tr("Ce canal est déjà utilisé, ou son verrou est inaccessible.");
        return false;
    }
    // Seulement APRÈS le verrou : retirer le socket avant pourrait déconnecter
    // un processus vivant. Sur Unix, un crash peut laisser un socket orphelin.
    QLocalServer::removeServer(endpoint);
    if (!m_server.listen(endpoint)) {
        m_error = m_server.errorString();
        return false;
    }
    m_lock = std::move(lock);
    return true;
}

void LocalService::acceptConnections()
{
    while (auto *socket = m_server.nextPendingConnection()) {
        if (m_connections >= MaxConnections) {
            socket->abort();
            socket->deleteLater();
            continue;
        }
        ++m_connections;
        socket->setReadBufferSize(MaxFrameBytes + 1);
        // Le tampon est partagé par les callbacks de cette connexion et libéré
        // avec le dernier d'entre eux.
        auto buffer = std::make_shared<QByteArray>();
        auto *deadline = new QTimer(socket);
        deadline->setSingleShot(true);
        deadline->start(5000);
        connect(deadline, &QTimer::timeout, socket, &QLocalSocket::abort);
        // `this` est le contexte Qt : les connexions sont coupées avant sa
        // destruction. Le socket est supprimé après le callback courant.
        connect(socket, &QLocalSocket::disconnected, this, [this, socket] {
            --m_connections;
            socket->deleteLater();
        });
        connect(socket, &QLocalSocket::readyRead, socket, [this, socket, buffer] {
            buffer->append(socket->readAll());
            // Une requête par connexion : borne mémoire et durée simples à
            // auditer. Les notifications persistantes seront un autre contrat.
            if (buffer->size() > MaxFrameBytes) {
                socket->abort();
                return;
            }
            const auto newline = buffer->indexOf('\n');
            if (newline < 0)
                return;
            QJsonParseError error;
            const auto document = QJsonDocument::fromJson(buffer->left(newline), &error);
            if (error.error != QJsonParseError::NoError || !document.isObject()) {
                socket->abort();
                return;
            }
            // Le service délimite la trame ; le répondeur interprète le message.
            const auto answer = m_responder(document.object());
            socket->write(encode(answer.message));
            if (answer.stopsService && !m_stopping) {
                // Plus aucune connexion nouvelle : le processus ne doit pas
                // accepter une requête qu'il ne pourra plus honorer. Les
                // connexions ouvertes gardent leur échéance de cinq secondes.
                // Le drapeau rend l'arrêt idempotent si deux demandes se croisent.
                m_stopping = true;
                m_server.close();
                // Le signal part à la fermeture de CE socket, donc après
                // l'écriture effective de l'accusé d'arrêt.
                connect(socket, &QLocalSocket::disconnected, this, &LocalService::stopRequested);
            }
            // Qt vide les octets en attente avant de terminer la connexion.
            socket->disconnectFromServer();
        });
    }
}

} // namespace retrosave::ipc
