#include "app/installation.h"

#include "ipc/protocol.h"

#include <QCoreApplication>
#include <QDir>
#include <QStandardPaths>

namespace retrosave::installation
{

QString agentExecutable()
{
    QString name = "retrosave-agent";
#ifdef Q_OS_WIN
    name += ".exe";
#endif
    return QDir(QCoreApplication::applicationDirPath()).filePath(name);
}

QString settingsFile()
{
    // Une seule définition, partagée avec l'agent : deux calculs du même chemin
    // finiraient par désigner deux fichiers différents.
    return ipc::settingsFile();
}

} // namespace retrosave::installation
