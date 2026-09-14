#pragma once

#include <QIODevice>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QTemporaryFile>
#include <QTimer>
#include <memory>
#include <optional>

namespace retrosave::network
{

enum class TransferFailure {
    None,
    InvalidRequest,
    Network,
    Http,
    Redirect,
    Timeout,
    Cancelled,
    LocalIo,
    Integrity
};
struct TransferResult {
    TransferFailure failure = TransferFailure::None;
    int httpStatus = 0;
    qint64 bytes = 0;
    QByteArray archiveSha256;
    // Candidat temporaire, jamais applicable directement. downloadCandidate
    // impose encore l'extraction et la validation de content_sha256. La
    // dernière copie du shared_ptr supprime le fichier de transit.
    std::shared_ptr<QTemporaryFile> archive;
    bool objectMayExist = false; // PUT interrompu : ne prouve pas l'absence distante
};

class S3Transfer;
class TransferCall final : public QObject
{
    Q_OBJECT
  public:
    ~TransferCall() override;
    void cancel();
  signals:
    void finished(const retrosave::network::TransferResult &result);

  private:
    friend class S3Transfer;
    explicit TransferCall(QObject *parent);
    void complete(TransferResult result);
    QPointer<QNetworkReply> m_reply;
    QTimer m_deadline;
    std::shared_ptr<QIODevice> m_source;
    std::shared_ptr<QTemporaryFile> m_staging;
    bool m_done = false;
    bool m_upload = false;
};

// Transport S3 séparé du client API : ne possède AUCUN token, identifiant
// d'appareil, cookie ou clé d'idempotence API. Seule l'URL présignée autorise
// l'opération. Ne jamais la journaliser : sa signature est un secret temporaire.
class S3Transfer final : public QObject
{
    Q_OBJECT
  public:
    explicit S3Transfer(bool allowHttp = false, int timeoutMs = 300000, QObject *parent = nullptr);
    ~S3Transfer() override;
    // Source déjà ouverte, position zéro, archive de staging immuable. Le
    // shared_ptr garantit qu'elle survit jusqu'à la fin des lectures de Qt.
    TransferCall *upload(const QUrl &url, std::shared_ptr<QIODevice> source, qint64 bytes);
    // ⚠ NE JAMAIS utiliser pour télécharger une VERSION de sauvegarde.
    // Exige des octets d'archive exactement identiques à ceux annoncés. C'est
    // valide uniquement pour un objet dont on a soi-même produit les octets —
    // vérifier son propre envoi, par exemple. Pour une version, deux clients
    // peuvent avoir écrit des enveloppes différentes du même contenu (Q10,
    // cas 14 du banc) : exiger l'empreinte enregistrée rejetterait un objet
    // parfaitement sain. Le nom porte la contrainte pour qu'on ne s'y trompe pas.
    TransferCall *downloadExact(const QUrl &url, qint64 expectedBytes,
                                const QByteArray &expectedSha256,
                                const QString &stagingDirectory);

    // LE chemin des versions. Q10 : les métadonnées d'une version peuvent
    // décrire une autre enveloppe du même contenu. Borne explicite de taille,
    // empreinte d'archive seulement observée, et verdict rendu à l'extraction
    // par `content_sha256`.
    TransferCall *downloadCandidate(const QUrl &url, qint64 maximumBytes,
                                    const QString &stagingDirectory);

  private:
    TransferCall *receive(const QUrl &url, qint64 maximumBytes,
                          std::optional<QByteArray> exactSha256, const QString &stagingDirectory);
    QNetworkRequest request(const QUrl &url) const;
    bool validUrl(const QUrl &url) const;
    TransferCall *invalid();
    bool m_allowHttp;
    int m_timeoutMs;
    QNetworkAccessManager m_http;
};

} // namespace retrosave::network
Q_DECLARE_METATYPE(retrosave::network::TransferResult)
