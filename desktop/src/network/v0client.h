#pragma once

#include "network/v0catalog.h"

#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <optional>
#include <variant>

namespace retrosave::network
{

struct PrepareVersion {
    qint64 baseVersion = 0;
    QString contentSha256;
    QString archiveSha256;
    qint64 size = 0;
    qint64 archiveBytes = 0;
};
struct ConfirmVersion {
    qint64 baseVersion = 0;
    QString objectKey;
    QString contentSha256;
    QString archiveSha256;
    QJsonObject environment;
    std::optional<QString> clientMtime;
};

struct Duplicate {
    qint64 version;
};
struct Published {
    qint64 version;
};
struct UploadTarget {
    QUrl url;
    QString objectKey;
    QString expiresAt;
};
struct DownloadTarget {
    QUrl url;
    QString contentSha256;
    QString archiveSha256;
    qint64 archiveBytes;
    QString expiresAt;
};
struct ConflictHead {
    qint64 number;
    QString contentSha256;
    qint64 sizeBytes;
};
struct Conflict {
    QString code; // cas_conflict : head/yours ; open_conflict : version_a/version_b
    QString id;
    ConflictHead first;
    ConflictHead second;
};

enum class Failure {
    None,
    InvalidRequest,
    Transport,
    Timeout,
    Cancelled,
    Http,
    InvalidResponse,
    Redirect,
    ResponseTooLarge
};
using Payload = std::variant<std::monostate, Duplicate, Published, UploadTarget, DownloadTarget,
                             Conflict, RegisteredDevice, Devices, RemoteUnit, Units, History,
                             Conflicts, ResolvedConflict, Missing, Health, RemoteDevice>;
struct ApiResult {
    Failure failure = Failure::None;
    int httpStatus = 0;
    QString serverCode; // code machine ; jamais le message brut potentiellement sensible
    Payload payload;
    // Une coupure APRÈS un POST/PATCH ne prouve pas que le serveur n'a rien fait.
    // L'orchestrateur doit réconcilier/rejouer avec la même clé, pas créer une
    // nouvelle opération aveuglément. Aucune relance automatique dans cet adaptateur.
    bool mayHaveCommitted = false;
};

class V0Client;

// Une opération = un QObject. L'appelant le conserve jusqu'à finished, puis
// appelle deleteLater(). Le client le possède par défaut et le nettoie à sa mort.
// Détruire le client/l'appel annule les I/O ; aucun callback vers un objet mort.
class ApiCall final : public QObject
{
    Q_OBJECT
  public:
    ~ApiCall() override;
    void cancel();
  signals:
    void finished(const retrosave::network::ApiResult &result);

  private:
    friend class V0Client;
    explicit ApiCall(QObject *parent);
    void complete(ApiResult result);
    QPointer<QNetworkReply> m_reply;
    QTimer m_deadline;
    QByteArray m_body;
    bool m_done = false;
    bool m_write = false;
};

struct ApiConfig {
    QUrl serverUrl;
    QByteArray token;
    QByteArray deviceId;
    int timeoutMs = 30000;
    // Les environnements locaux et les tests peuvent autoriser HTTP
    // explicitement. Un échec TLS ne provoque jamais de repli automatique.
    bool allowHttp = false;
};

// Adaptateur HTTP hors du noyau pur. Il ne lit aucun fichier et ne télécharge
// pas les archives S3 : ce transport appartient à S3Transfer, séparément.
// Utiliser cette classe dans son thread Qt d'origine, avec une boucle active.
class V0Client final : public QObject
{
    Q_OBJECT
  public:
    explicit V0Client(ApiConfig config, QObject *parent = nullptr);
    ~V0Client() override;
    ApiCall *health();
    // Ne modifie pas m_config : après succès, persister l'identifiant, puis
    // créer un client configuré avec lui. Un échec n'invente aucun appareil.
    ApiCall *registerDevice(const DeviceRegistration &device, const QByteArray &idempotencyKey);
    ApiCall *listDevices();
    // EXP-01. Renommer est cosmétique ; révoquer refuse les requêtes futures de
    // cet appareil et ne détruit rien (invariant I2).
    ApiCall *renameDevice(const QString &deviceId, const QString &name,
                          const QByteArray &idempotencyKey);
    ApiCall *revokeDevice(const QString &deviceId, const QByteArray &idempotencyKey);
    ApiCall *listUnits();
    ApiCall *createUnit(const UnitDeclaration &unit, const QByteArray &idempotencyKey);
    ApiCall *renameUnit(const QString &unitId, const QString &label,
                        const QByteArray &idempotencyKey);
    ApiCall *history(const QString &unitId);
    ApiCall *restore(const QString &unitId, qint64 version, const QByteArray &idempotencyKey);
    ApiCall *markMissing(const QString &unitId, const QByteArray &idempotencyKey);
    ApiCall *listConflicts();
    // Exécute un choix explicite de l'appelant ; ne choisit jamais A ou B.
    ApiCall *resolveConflict(const QString &conflictId, qint64 winner,
                             const QByteArray &idempotencyKey);
    ApiCall *prepare(const QString &unitId, const PrepareVersion &version,
                     const QByteArray &idempotencyKey);
    ApiCall *confirm(const QString &unitId, const ConfirmVersion &version,
                     const QByteArray &idempotencyKey);
    ApiCall *downloadTarget(const QString &unitId, qint64 number);

    static constexpr qsizetype MaxResponseBytes = 1024 * 1024;

  private:
    enum class Operation {
        Prepare,
        Confirm,
        Download,
        Health,
        RegisterDevice,
        Devices,
        RenameDevice,
        RevokeDevice,
        Units,
        CreateUnit,
        Rename,
        History,
        Restore,
        Missing,
        Conflicts,
        Resolve
    };
    ApiCall *send(Operation operation, const QString &unitId, qint64 number, QJsonObject body,
                  const QByteArray &key, bool valid);
    static ApiResult decode(Operation operation, int status, const QByteArray &body);
    static ApiResult decodeCatalog(Operation operation, int status, const QJsonObject &object);
    ApiConfig m_config;
    QNetworkAccessManager m_http;
};

} // namespace retrosave::network

Q_DECLARE_METATYPE(retrosave::network::ApiResult)
