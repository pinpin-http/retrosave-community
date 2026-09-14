#include "processwatch.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#ifdef Q_OS_WIN
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace retrosave::agent
{
namespace
{
QString bareName(const QString &name)
{
    // « PPSSPPWindows64.exe » et « ppsspp » doivent se comparer de la même
    // façon des deux côtés : on retire l'extension et la casse.
    auto trimmed = name.trimmed();
    if (trimmed.endsWith(".exe", Qt::CaseInsensitive))
        trimmed.chop(4);
    return trimmed.toLower();
}
} // namespace

QSet<QString> runningProcessNames()
{
    QSet<QString> names;
#ifdef Q_OS_WIN
    // Windows : l'instantané Toolhelp32 est l'API portable entre versions.
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return names;
    PROCESSENTRY32W entry {};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            names.insert(bareName(QString::fromWCharArray(entry.szExeFile)));
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
#else
    // Linux : `/proc/<pid>/comm` porte le nom du programme, tronqué à quinze
    // caractères par le noyau. On lit donc AUSSI `cmdline`, dont le premier
    // champ est le chemin complet : sans lui, « PPSSPPSDL » passerait mais un
    // nom plus long serait coupé et la protection sauterait en silence.
    QDir proc("/proc");
    const auto entries = proc.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const auto &entry : entries) {
        bool numeric = false;
        entry.toInt(&numeric);
        if (!numeric)
            continue;
        QFile comm(proc.filePath(entry + "/comm"));
        if (comm.open(QIODevice::ReadOnly))
            names.insert(bareName(QString::fromUtf8(comm.readAll()).trimmed()));
        QFile cmdline(proc.filePath(entry + "/cmdline"));
        if (cmdline.open(QIODevice::ReadOnly)) {
            const auto raw = cmdline.readAll();
            const auto first = raw.split('\0').value(0);
            if (!first.isEmpty())
                names.insert(bareName(QFileInfo(QString::fromUtf8(first)).fileName()));
        }
    }
#endif
    names.remove(QString());
    return names;
}

bool anyProcessRunning(const QStringList &processNames, const QSet<QString> &running)
{
    for (const auto &candidate : processNames) {
        const auto bare = bareName(candidate);
        if (!bare.isEmpty() && running.contains(bare))
            return true;
    }
    return false;
}

} // namespace retrosave::agent
