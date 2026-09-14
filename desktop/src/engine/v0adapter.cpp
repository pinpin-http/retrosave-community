#include "engine/v0adapter.h"

#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QScopedPointer>
#include <QSysInfo>
#include <QUuid>

namespace retrosave::engine
{
namespace
{
using namespace retrosave::network;

// Borne du téléchargement (Q10). L'archive reçue n'est PAS forcément celle que
// nos métadonnées décrivent : un autre client a pu produire une autre enveloppe
// du même contenu. On borne donc sur ce que le produit autorise — 256 Mo
// d'unité (MAX_UNIT_BYTES, invariant I1), plus la place d'un en-tête tar et
// d'une trame zstd sur un contenu incompressible — et non sur une taille
// annoncée, qui rejetterait un objet parfaitement sain.
constexpr qint64 MaxArchiveBytes = 268435456LL + 1048576LL;

// Attend la fin d'un appel asynchrone. `QScopedPointer` garantit la libération
// même si la traduction qui suit lève : un appel oublié retiendrait sa réponse
// en mémoire pour toute la vie du client.
template <typename CallT, typename ResultT> ResultT await(CallT *call)
{
    QScopedPointer<CallT, QScopedPointerDeleteLater> owned(call);
    ResultT result;
    QEventLoop loop;
    // Le rappel n'est branché qu'APRÈS le retour de l'appel, et c'est sans
    // risque : le client diffère même ses erreurs locales d'un tour de boucle,
    // précisément pour que ce branchement arrive à temps.
    QObject::connect(call, &CallT::finished, &loop, [&](const ResultT &value) {
        result = value;
        loop.quit();
    });
    loop.exec();
    return result;
}

QString describe(Failure failure)
{
    switch (failure) {
    case Failure::None:
        return QStringLiteral("unexpected reply");
    case Failure::InvalidRequest:
        return QStringLiteral("request refused before sending");
    case Failure::Transport:
        return QStringLiteral("network failure");
    case Failure::Timeout:
        return QStringLiteral("timed out");
    case Failure::Cancelled:
        return QStringLiteral("cancelled");
    case Failure::Http:
        return QStringLiteral("refus du serveur");
    case Failure::InvalidResponse:
        return QStringLiteral("unreadable reply");
    case Failure::Redirect:
        return QStringLiteral("redirect refused");
    case Failure::ResponseTooLarge:
        return QStringLiteral("reply too large");
    }
    return QStringLiteral("erreur inconnue");
}

// Ni le corps de la réponse, ni l'URL présignée, ni le token ne remontent
// jamais dans un message : le code machine du serveur suffit à diagnostiquer,
// et ces valeurs finiraient dans un journal ou une capture d'écran.
[[noreturn]] void fail(const QString &what, const ApiResult &result)
{
    auto message = what + " : " + describe(result.failure);
    if (result.httpStatus > 0)
        message += QStringLiteral(" (HTTP %1)").arg(result.httpStatus);
    if (!result.serverCode.isEmpty())
        message += " [" + result.serverCode + "]";
    if (result.mayHaveCommitted)
        message += QStringLiteral(" — outcome unknown, replay with the same key");
    throw ApiError(message.toStdString());
}

[[noreturn]] void failTransfer(const QString &what, const TransferResult &result)
{
    auto message = what;
    switch (result.failure) {
    case TransferFailure::Integrity:
        message += QStringLiteral(" : integrity check failed");
        break;
    case TransferFailure::LocalIo:
        message += QStringLiteral(" : local write failed");
        break;
    case TransferFailure::Timeout:
        message += QStringLiteral(" : timed out");
        break;
    default:
        message += QStringLiteral(" : transfer failed");
        break;
    }
    if (result.httpStatus > 0)
        message += QStringLiteral(" (HTTP %1)").arg(result.httpStatus);
    throw ApiError(message.toStdString());
}

// Une clé d'opération STABLE, jamais tirée au hasard. C'est ce qui rend une
// reprise sûre : rejouer la même publication après une coupure rend la même
// réponse du serveur au lieu de créer une seconde version. Elle est donc
// dérivée de l'identité de l'opération — cette unité, depuis cette base, avec
// ce contenu — et de rien d'autre.
QByteArray operationKey(const QString &scope, const QStringList &parts)
{
    const auto material = (scope + "|" + parts.join('|')).toUtf8();
    return QCryptographicHash::hash(material, QCryptographicHash::Sha256).toHex();
}

} // namespace

V0SyncApi::V0SyncApi(network::ApiConfig config) : m_client(config), m_s3(config.allowHttp) {}

V0SyncApi::~V0SyncApi() = default;

std::vector<RemoteUnit> V0SyncApi::listUnits()
{
    const auto result = await<ApiCall, ApiResult>(m_client.listUnits());
    const auto *units = std::get_if<Units>(&result.payload);
    if (result.failure != Failure::None || units == nullptr)
        fail(QStringLiteral("listing saves"), result);
    std::vector<RemoteUnit> remote;
    for (const auto &item : units->items)
        remote.push_back({item.id, item.identity.emulator, item.identity.unitKey,
                          item.identity.unitType, item.identity.gameKey,
                          item.identity.gameLabel, int(item.headVersion), item.state,
                          item.artworkUrl});
    return remote;
}

QString V0SyncApi::declareUnit(const QString &emulator, const QString &unitKey,
                               const QString &unitType, const QString &gameKey,
                               const QString &gameLabel)
{
    const UnitDeclaration declaration{emulator, unitKey, unitType, gameKey, gameLabel};
    const auto result = await<ApiCall, ApiResult>(
        m_client.createUnit(declaration, operationKey("unit", {emulator, unitKey})));
    const auto *unit = std::get_if<network::RemoteUnit>(&result.payload);
    if (result.failure != Failure::None || unit == nullptr)
        fail(QStringLiteral("declaring the save"), result);
    return unit->id;
}

PrepareOutcome V0SyncApi::prepare(const QString &serverId, int baseVersion,
                                  const QString &contentSha256, const QString &archiveSha256,
                                  qint64 sizeBytes, qint64 archiveBytes)
{
    const PrepareVersion version{baseVersion, contentSha256, archiveSha256, sizeBytes,
                                 archiveBytes};
    const auto key =
        operationKey("prepare", {serverId, QString::number(baseVersion), contentSha256});
    const auto result = await<ApiCall, ApiResult>(m_client.prepare(serverId, version, key));
    if (const auto *duplicate = std::get_if<Duplicate>(&result.payload))
        return {true, int(duplicate->version), {}, {}};
    if (const auto *target = std::get_if<UploadTarget>(&result.payload))
        return {false, 0, target->url.toString(), target->objectKey};
    // Un conflit déjà ouvert au moment du prepare est une course : la passe a
    // vérifié les conflits ouverts avant de pousser. On échoue proprement pour
    // cette unité ; la passe suivante lira le conflit et la bloquera comme il
    // faut, plutôt que d'insister ici.
    fail(QStringLiteral("preparing the version"), result);
}

void V0SyncApi::upload(const QString &url, const QString &archivePath)
{
    auto source = std::make_shared<QFile>(archivePath);
    if (!source->open(QIODevice::ReadOnly))
        throw ApiError("prepared archive unreadable");
    const auto bytes = source->size();
    const auto result = await<TransferCall, TransferResult>(m_s3.upload(QUrl(url), source, bytes));
    if (result.failure != TransferFailure::None)
        failTransfer(QStringLiteral("uploading the archive"), result);
}

ConfirmOutcome V0SyncApi::confirm(const QString &serverId, const QString &objectKey,
                                  const QString &contentSha256, const QString &archiveSha256,
                                  int baseVersion)
{
    ConfirmVersion version;
    version.baseVersion = baseVersion;
    version.objectKey = objectKey;
    version.contentSha256 = contentSha256;
    version.archiveSha256 = archiveSha256;
    // L'environnement est informatif (§7.2). La date locale, elle, n'est pas
    // envoyée : aucune horloge cliente ne doit peser sur l'ordre des versions
    // (invariant I4), et une valeur absente vaut mieux qu'une valeur trompeuse.
    version.environment = QJsonObject{{"os", QSysInfo::productType()}};

    // Même clé que le prepare correspondant, au préfixe près : c'est la même
    // opération métier, vue en deux temps.
    const auto key =
        operationKey("confirm", {serverId, QString::number(baseVersion), contentSha256});
    const auto result = await<ApiCall, ApiResult>(m_client.confirm(serverId, version, key));
    if (const auto *published = std::get_if<Published>(&result.payload))
        return {false, int(published->version), {}, 0};
    if (const auto *conflict = std::get_if<Conflict>(&result.payload)) {
        // `cas_conflict` : la tête d'abord, notre branche ensuite. Les deux
        // contenus existent côté serveur — c'est l'invariant I3, et c'est ce
        // qui permet à l'utilisateur de trancher sans avoir rien perdu.
        return {true, int(conflict->second.number), conflict->id, int(conflict->first.number)};
    }
    fail(QStringLiteral("publication de la version"), result);
}

DownloadedVersion V0SyncApi::downloadVersion(const QString &serverId, int number,
                                             const QString &stagingDirectory)
{
    const auto target = await<ApiCall, ApiResult>(m_client.downloadTarget(serverId, number));
    const auto *location = std::get_if<DownloadTarget>(&target.payload);
    if (target.failure != Failure::None || location == nullptr)
        fail(QStringLiteral("download address"), target);

    QDir().mkpath(stagingDirectory);
    // `downloadCandidate`, jamais `downloadExact` : les octets d'une version
    // peuvent être une autre enveloppe du même contenu (Q10). Le verdict tombe
    // plus loin, à l'extraction, sur `content_sha256`.
    const auto received = await<TransferCall, TransferResult>(
        m_s3.downloadCandidate(location->url, MaxArchiveBytes, stagingDirectory));
    if (received.failure != TransferFailure::None || !received.archive)
        failTransfer(QStringLiteral("downloading the version"), received);

    // Le fichier temporaire s'effacerait à la mort du `shared_ptr`. La passe,
    // elle, attend un chemin qu'elle possède et qu'elle effacera elle-même :
    // on le sort donc de la garde de Qt par un renommage dans le même dossier.
    received.archive->setAutoRemove(false);
    const auto held =
        QDir(stagingDirectory)
            .filePath("version-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
    received.archive->close();
    // Demander le renommage à l'objet QTemporaryFile lui-même lui permet de
    // mettre à jour son état interne. C'est nécessaire sous Windows, où un
    // QFile::rename séparé peut encore rencontrer le handle que Qt associe au
    // temporaire, même après close().
    if (!received.archive->rename(held)) {
        received.archive->setAutoRemove(true);
        throw ApiError("downloaded archive not kept");
    }
    return {held, location->contentSha256};
}

UnitHistory V0SyncApi::history(const QString &serverId)
{
    const auto result = await<ApiCall, ApiResult>(m_client.history(serverId));
    const auto *history = std::get_if<History>(&result.payload);
    if (result.failure != Failure::None || history == nullptr)
        fail(QStringLiteral("save history"), result);

    UnitHistory unit;
    unit.headVersion = int(history->headVersion);
    for (const auto &version : history->versions)
        unit.versions.push_back({int(version.summary.number), version.kind,
                                 version.summary.contentSha256, version.summary.sizeBytes,
                                 version.summary.createdAt,
                                 version.summary.originDeviceName.value_or(QString())});
    return unit;
}

void V0SyncApi::restore(const QString &serverId, int number)
{
    // Clé dérivée de l'unité ET du numéro visé : rejouer la même restauration
    // après une coupure rend la même réponse, tandis que restaurer une AUTRE
    // version reste une opération distincte.
    const auto key = operationKey("restore", {serverId, QString::number(number)});
    const auto result = await<ApiCall, ApiResult>(m_client.restore(serverId, number, key));
    if (std::holds_alternative<Published>(result.payload))
        return;
    // Un conflit ouvert interdit la restauration : le serveur refuse tant que
    // l'utilisateur n'a pas tranché, et c'est la bonne réponse — restaurer
    // par-dessus un désaccord non résolu reviendrait à choisir à sa place.
    fail(QStringLiteral("restauration de la version"), result);
}

void V0SyncApi::markMissing(const QString &serverId)
{
    const auto result = await<ApiCall, ApiResult>(
        m_client.markMissing(serverId, operationKey("missing", {serverId})));
    if (result.failure != Failure::None || !std::holds_alternative<Missing>(result.payload))
        fail(QStringLiteral("signalement de disparition"), result);
}

std::vector<OpenConflict> V0SyncApi::openConflicts()
{
    const auto result = await<ApiCall, ApiResult>(m_client.listConflicts());
    const auto *conflicts = std::get_if<Conflicts>(&result.payload);
    if (result.failure != Failure::None || conflicts == nullptr)
        fail(QStringLiteral("liste des conflits"), result);

    // `origin_device_name` peut légitimement être absent — un appareil supprimé,
    // une version restaurée. On ne fabrique pas de nom : l'interface dira
    // « appareil inconnu » plutôt que d'affirmer une provenance fausse.
    const auto side = [](const RemoteHead &head) {
        return ConflictSide{int(head.number), head.contentSha256, head.sizeBytes, head.createdAt,
                            head.originDeviceName.value_or(QString())};
    };
    std::vector<OpenConflict> open;
    for (const auto &conflict : conflicts->items)
        open.push_back({conflict.id, conflict.unitId, conflict.unitLabel, side(conflict.first),
                        side(conflict.second)});
    return open;
}

void V0SyncApi::resolveConflict(const QString &conflictId, int winner)
{
    // Clé dérivée du conflit ET du gagnant : rejouer la même décision après une
    // coupure rend la même réponse, et une décision DIFFÉRENTE n'est jamais
    // confondue avec un rejeu — ce serait trancher à la place de l'utilisateur.
    const auto key = operationKey("resolve", {conflictId, QString::number(winner)});
    const auto result =
        await<ApiCall, ApiResult>(m_client.resolveConflict(conflictId, winner, key));
    if (result.failure != Failure::None ||
        !std::holds_alternative<ResolvedConflict>(result.payload))
        fail(QStringLiteral("resolving the conflict"), result);
}

} // namespace retrosave::engine
