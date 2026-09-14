#include "app/instanceguard.h"

#include <QJsonDocument>
#include <QLocalSocket>
#include <QUuid>

namespace retrosave
{

// m_service est détruit avant l'InstanceGuard ; la capture de `this` reste donc
// valide pendant toute la vie du répondeur.
InstanceGuard::InstanceGuard(QObject *parent)
    : QObject(parent), m_service([this](const QJsonObject &request) {
          const auto answer = ipc::windowReply(request);
          if (answer.message.contains("result"))
              emit showRequested();
          return answer;
      })
{
}

InstanceGuard::Outcome InstanceGuard::claim(const QString &endpoint)
{
    if (m_service.listen(endpoint))
        return Outcome::Owner;
    m_error = m_service.errorString();

    // Ici, et seulement ici, on utilise les `waitFor...` bloquants : nous
    // sommes au démarrage, aucune fenêtre n'existe encore, donc il n'y a pas
    // d'interface à figer. Ailleurs dans l'application ce serait une faute.
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QLocalSocket socket;
    socket.setReadBufferSize(ipc::MaxFrameBytes + 1);
    socket.connectToServer(endpoint);
    if (!socket.waitForConnected(1000))
        return Outcome::Unprotected;
    const auto request = ipc::encode(ipc::showRequest(id));
    if (socket.write(request) != request.size())
        return Outcome::Unprotected;
    socket.flush();
    QByteArray response;
    while (!response.contains('\n') && response.size() <= ipc::MaxFrameBytes) {
        // Sous Windows, les octets peuvent être écrits ou reçus avant l'appel
        // bloquant. Dans ce cas waitForBytesWritten/waitForReadyRead rend faux
        // puisqu'il n'a plus de nouveau signal à attendre, alors que l'échange
        // a réussi. L'état du tampon est donc toujours vérifié en premier.
        if (!socket.bytesAvailable() && !socket.waitForReadyRead(1000))
            return Outcome::Unprotected;
        response += socket.readAll();
    }
    const auto message = QJsonDocument::fromJson(response.left(response.indexOf('\n'))).object();
    // Sans réponse corrélée et positive, mieux vaut ouvrir notre propre fenêtre
    // que laisser l'utilisateur devant un lancement sans effet visible.
    if (message.value("id").toString() != id ||
        message.value("result").toObject().value("shown") != QJsonValue(true))
        return Outcome::Unprotected;
    return Outcome::HandedOver;
}

} // namespace retrosave
