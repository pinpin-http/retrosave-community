#pragma once

#include "ipc/localservice.h"

#include <QObject>

namespace retrosave
{

// Garantit qu'un second lancement ne crée pas une interface invisible de plus.
// Le premier processus détient le canal ; les suivants lui demandent de se
// montrer, puis s'effacent. Aucune donnée métier ne transite ici.
class InstanceGuard final : public QObject
{
    Q_OBJECT
  public:
    enum class Outcome {
        Owner,       // nous détenons le canal : la fenêtre est la nôtre
        HandedOver,  // une fenêtre existait : elle a été rappelée à l'écran
        Unprotected, // canal indisponible : on continue sans garantie d'unicité
    };
    explicit InstanceGuard(QObject *parent = nullptr);
    Outcome claim(const QString &endpoint);
    QString errorString() const { return m_error; }

  signals:
    void showRequested();

  private:
    ipc::LocalService m_service;
    QString m_error;
};

} // namespace retrosave
