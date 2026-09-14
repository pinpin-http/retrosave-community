#include "agent/configuration.h"
#include "ipc/protocol.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

namespace retrosave::agent
{
namespace
{
constexpr auto UrlKey = "server/url";
constexpr auto TokenKey = "server/token";
constexpr auto DeviceKey = "server/device_id";
constexpr auto FolderKey = "folder"; // même clé que l'interface : un seul dossier
constexpr auto IntervalKey = "sync/interval_s";
constexpr auto PauseKey = "sync/global_pause";
constexpr auto NotifyConflictsKey = "notify/conflicts";
constexpr auto NotifyErrorsKey = "notify/errors";
constexpr auto NotifyRestoresKey = "notify/restores";
constexpr auto ArtworkKey = "artwork/download";

QString dataDirectory()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
           "/retrosave-community";
}
} // namespace

Configuration loadConfiguration()
{
    QSettings settings(ipc::settingsFile(), QSettings::IniFormat);
    Configuration configuration;
    configuration.serverUrl = settings.value(UrlKey).toString();
    configuration.token = settings.value(TokenKey).toString();
    configuration.deviceId = settings.value(DeviceKey).toString();
    configuration.root = settings.value(FolderKey).toString();
    // Les racines par émulateur vivent sous `roots/<id>`. On lit ce que le
    // fichier contient : un identifiant inconnu du registre sera simplement
    // ignoré au scan, sans faire échouer la lecture.
    settings.beginGroup("roots");
    for (const auto &key : settings.childKeys()) {
        const auto path = settings.value(key).toString();
        if (!path.isEmpty())
            configuration.roots.insert(key, path);
    }
    settings.endGroup();
    configuration.globalPause = settings.value(PauseKey, false).toBool();
    // Par défaut on notifie : une notification qu'on n'attendait pas se coupe
    // en un clic, un conflit qu'on n'a pas vu coûte une partie.
    configuration.notifyConflicts = settings.value(NotifyConflictsKey, true).toBool();
    configuration.notifyErrors = settings.value(NotifyErrorsKey, true).toBool();
    configuration.notifyRestores = settings.value(NotifyRestoresKey, true).toBool();
    configuration.downloadArtwork = settings.value(ArtworkKey, true).toBool();
    const auto interval = settings.value(IntervalKey).toInt();
    // Une valeur absurde lue dans un fichier édité à la main ne doit pas
    // transformer l'agent en marteau-pilon sur le serveur.
    if (interval >= 60 && interval <= 86400)
        configuration.intervalSeconds = interval;
    return configuration;
}

void saveConnection(const QString &url, const QString &token, const QString &deviceId)
{
    const auto path = ipc::settingsFile();
    QDir().mkpath(QFileInfo(path).absolutePath());
    {
        QSettings settings(path, QSettings::IniFormat);
        settings.setValue(UrlKey, url);
        settings.setValue(TokenKey, token);
        settings.setValue(DeviceKey, deviceId);
        settings.sync();
    }
    // Après écriture, jamais avant : QSettings recrée le fichier.
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
}

void saveGlobalPause(bool paused)
{
    const auto path = ipc::settingsFile();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSettings settings(path, QSettings::IniFormat);
    settings.setValue(PauseKey, paused);
    settings.sync();
}

void saveNotificationPreferences(bool conflicts, bool errors, bool restores)
{
    const auto path = ipc::settingsFile();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSettings settings(path, QSettings::IniFormat);
    settings.setValue(NotifyConflictsKey, conflicts);
    settings.setValue(NotifyErrorsKey, errors);
    settings.setValue(NotifyRestoresKey, restores);
    settings.sync();
}

void saveArtworkDownload(bool enabled)
{
    const auto path = ipc::settingsFile();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSettings settings(path, QSettings::IniFormat);
    settings.setValue(ArtworkKey, enabled);
    settings.sync();
}

QString stateDatabasePath()
{
    const auto directory = dataDirectory();
    QDir().mkpath(directory);
    return QDir(directory).filePath("state.db");
}

QString iconCachePath()
{
    const auto directory =
        QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) +
        "/retrosave-community/icons";
    QDir().mkpath(directory);
    return directory;
}

QString iconPathFor(const QString &emulator, const QString &unitKey)
{
    const auto material = emulator.toUtf8() + '\0' + unitKey.toUtf8();
    const auto digest =
        QCryptographicHash::hash(material, QCryptographicHash::Sha256).toHex().left(32);
    return QDir(iconCachePath()).filePath(QString::fromLatin1(digest) + ".png");
}

QString customArtworkPath(const QString &emulator, const QString &unitKey)
{
    const auto directory = QDir(dataDirectory()).filePath("artwork");
    QDir().mkpath(directory);
    const auto material = emulator.toUtf8() + '\0' + unitKey.toUtf8();
    const auto digest =
        QCryptographicHash::hash(material, QCryptographicHash::Sha256).toHex().left(32);
    return QDir(directory).filePath(QString::fromLatin1(digest) + ".png");
}

QString adaptersPath()
{
    // Un banc ou un développeur peut désigner un autre dossier ; rien n'est
    // deviné à partir du répertoire courant, qui n'appartient pas au processus.
    const auto configured = qEnvironmentVariable("RETROSAVE_ADAPTERS_DIR");
    if (!configured.isEmpty())
        return configured;
    const QDir here(QCoreApplication::applicationDirPath());
    for (const auto &candidate :
         {QStringLiteral("../share/retrosave/adapters"), QStringLiteral("adapters"),
          QStringLiteral("../../../adapters")}) {
        const QDir directory(here.filePath(candidate));
        if (directory.exists())
            return directory.absolutePath();
    }
    return {};
}

QString stagingPath()
{
    const auto directory = QDir(dataDirectory()).filePath("staging");
    QDir().mkpath(directory);
    return directory;
}

} // namespace retrosave::agent
