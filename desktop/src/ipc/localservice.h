#pragma once

#include "ipc/protocol.h"

#include <QLocalServer>
#include <QLockFile>
#include <QObject>
#include <functional>
#include <memory>

namespace retrosave::ipc
{

// Canal local générique : verrou d'instance, découpage des trames et bornes
// d'usage. Le sens des messages reste au-dessus, dans le répondeur fourni ;
// ce service ne connaît ni agent, ni fenêtre, ni sauvegarde.
class LocalService final : public QObject
{
    Q_OBJECT
  public:
    using Responder = std::function<Reply(const QJsonObject &)>;
    explicit LocalService(Responder responder, QObject *parent = nullptr);
    ~LocalService() override;
    bool listen(const QString &endpoint);
    QString errorString() const { return m_error; }

  signals:
    // Émis quand la réponse d'arrêt est réellement partie, jamais avant :
    // le processus ne doit pas disparaître pendant que son client attend.
    void stopRequested();

  private:
    void acceptConnections();
    Responder m_responder;
    QLocalServer m_server;
    // Le destructeur ferme le socket avant de rendre le verrou, pour éviter
    // qu'un autre processus démarre pendant la fermeture du premier.
    std::unique_ptr<QLockFile> m_lock;
    QString m_error;
    int m_connections = 0;
    bool m_stopping = false;
};

} // namespace retrosave::ipc
