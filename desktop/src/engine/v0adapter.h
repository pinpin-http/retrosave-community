// `engine/ports.h` décrit ce que la passe attend d'un serveur, en style
// SYNCHRONE : « donne-moi les unités », « publie ceci ». Qt, lui, travaille en
// asynchrone. Ce fichier est l'endroit — le seul — où les deux se rencontrent.
//
// Chaque appel démarre l'opération Qt puis fait tourner une petite
// boucle d'événements jusqu'à sa fin. Le fil d'exécution attend donc ici, et
// c'est voulu : la passe est une suite d'étapes qui se lisent de haut en bas.
//
// Cet objet ne doit pas vivre dans le
// fil de l'interface : la fenêtre y resterait figée pendant chaque transfert.
// Il appartient au fil de travail de l'agent, qui n'affiche rien.
//
// Ce que cet adaptateur ne fait pas, volontairement : il ne réessaie rien tout
// seul. Une panne remonte à la passe, qui traite l'unité suivante et laisse la
// passe d'après reprendre celle-ci — avec la MÊME clé d'opération.
#pragma once

#include "engine/ports.h"
#include "network/s3transfer.h"
#include "network/v0client.h"

#include <QString>

namespace retrosave::engine
{

class V0SyncApi final : public SyncApi
{
  public:
    explicit V0SyncApi(network::ApiConfig config);
    ~V0SyncApi() override;

    std::vector<RemoteUnit> listUnits() override;
    QString declareUnit(const QString &emulator, const QString &unitKey, const QString &unitType,
                        const QString &gameKey, const QString &gameLabel) override;
    PrepareOutcome prepare(const QString &serverId, int baseVersion, const QString &contentSha256,
                           const QString &archiveSha256, qint64 sizeBytes,
                           qint64 archiveBytes) override;
    void upload(const QString &url, const QString &archivePath) override;
    ConfirmOutcome confirm(const QString &serverId, const QString &objectKey,
                           const QString &contentSha256, const QString &archiveSha256,
                           int baseVersion) override;
    DownloadedVersion downloadVersion(const QString &serverId, int number,
                                      const QString &stagingDirectory) override;
    void markMissing(const QString &serverId) override;
    std::vector<OpenConflict> openConflicts() override;
    void resolveConflict(const QString &conflictId, int winner) override;
    UnitHistory history(const QString &serverId) override;
    void restore(const QString &serverId, int number) override;

  private:
    network::V0Client m_client;
    network::S3Transfer m_s3;
};

} // namespace retrosave::engine
