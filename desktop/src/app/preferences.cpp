#include "app/preferences.h"
#include "app/installation.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>

namespace retrosave
{
namespace
{
constexpr auto FolderKey = "folder";
constexpr auto ServiceName = "retrosave-community-agent.service";
constexpr auto TaskName = "RetroSaveCommunityAgent";

QString userConfigDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
}
} // namespace

QString Preferences::unitPath()
{
    return QDir(userConfigDir() + "/systemd/user").filePath(ServiceName);
}

QString Preferences::unitLinkPath()
{
    // C'est le lien que `systemctl --user enable` crée pour un WantedBy=. On
    // l'écrit nous-mêmes : l'activation devient une opération de fichiers,
    // vérifiable par un test et indépendante d'un systemd joignable.
    return QDir(userConfigDir() + "/systemd/user/default.target.wants").filePath(ServiceName);
}

QString Preferences::unitText()
{
    return QStringLiteral("[Unit]\n"
                          "Description=RetroSave — agent local\n"
                          "\n"
                          "[Service]\n"
                          "Type=simple\n"
                          "ExecStart=%1\n"
                          // Un arrêt demandé depuis l'interface rend 0 : il doit
                          // rester un arrêt, pas être défait par un redémarrage.
                          "Restart=on-failure\n"
                          "RestartSec=30\n"
                          // 2 = le canal est déjà servi par un autre agent. Du
                          // point de vue de la session, le but est atteint ;
                          // sans cela systemd relancerait toutes les 30 s.
                          "SuccessExitStatus=2\n"
                          "\n"
                          "[Install]\n"
                          "WantedBy=default.target\n")
        .arg(installation::agentExecutable());
}

Preferences::Preferences(QObject *parent) : QObject(parent)
{
    QSettings settings(installation::settingsFile(), QSettings::IniFormat);
    m_folder = settings.value(FolderKey).toString();
    m_roots.clear();
    settings.beginGroup("roots");
    for (const auto &key : settings.childKeys()) {
        const auto path = settings.value(key).toString();
        if (!path.isEmpty())
            m_roots.insert(key, path);
    }
    settings.endGroup();
    checkFolder();
    refreshAutostart();
}

#ifdef Q_OS_WIN
QProcess *Preferences::runPowerShell(const QString &script)
{
    // Le processus doit survivre à cette méthode ; son parent Qt et les
    // callbacks assurent sa destruction différée.
    auto *process = new QProcess(this);
    process->setProgram("powershell");
    process->setArguments(
        {"-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-Command", script});
    connect(process, &QProcess::errorOccurred, this, [this, process] {
        // PowerShell absent ou refusé : on le dit, on n'invente pas d'état.
        m_autostart = "unsupported";
        m_problem = tr("PowerShell n'a pas pu être lancé pour lire ou modifier "
                       "le démarrage de session.");
        m_busy = false;
        process->deleteLater();
        emit changed();
    });
    m_busy = true;
    emit changed();
    process->start();
    return process;
}
#endif

void Preferences::refreshAutostart()
{
#if defined(Q_OS_WIN)
    // Interroger la tâche prend le temps d'un processus : l'interface affiche
    // « unknown » en attendant, plutôt qu'un état supposé.
    auto *process = runPowerShell(
        QStringLiteral("$t = Get-ScheduledTask -TaskName '%1' -ErrorAction SilentlyContinue; "
                       "if ($t) { $t.State } else { 'absent' }")
            .arg(TaskName));
    connect(process, &QProcess::finished, this, [this, process](int code) {
        const auto state = QString::fromLocal8Bit(process->readAllStandardOutput()).trimmed();
        m_autostart = (code == 0 && !state.isEmpty() && state != "absent") ? "on" : "off";
        m_busy = false;
        process->deleteLater();
        emit changed();
    });
#elif defined(Q_OS_UNIX)
    // Le lien fait foi, comme côté Python où c'est la disparition du fichier
    // qui tranche : un systemd injoignable ne doit pas rendre l'état illisible.
    m_autostart = QFileInfo(unitLinkPath()).isSymLink() ? "on" : "off";
    m_busy = false;
    emit changed();
#else
    m_autostart = "unsupported";
    m_busy = false;
    emit changed();
#endif
}

void Preferences::applyAutostart(bool enabled)
{
#if defined(Q_OS_WIN)
    // Register-ScheduledTask permet une tâche utilisateur sans élévation.
    const auto quote = [](const QString &value) {
        return "'" + QString(value).replace("'", "''") + "'";
    };
    const auto script =
        enabled ? QStringLiteral("$a = New-ScheduledTaskAction -Execute %1; "
                                 "$t = New-ScheduledTaskTrigger -AtLogOn -User $env:USERNAME; "
                                 "$s = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries "
                                 "-DontStopIfGoingOnBatteries; "
                                 "Register-ScheduledTask -TaskName %2 -Action $a -Trigger $t "
                                 "-Settings $s -Force | Out-Null")
                      .arg(quote(QDir::toNativeSeparators(installation::agentExecutable())),
                           quote(TaskName))
                // Retirer ce qui n'existe pas est un succès : on interroge
                // avant, sinon PowerShell rend un code non nul pour rien.
                : QStringLiteral("if (Get-ScheduledTask -TaskName %1 -ErrorAction "
                                 "SilentlyContinue) { Unregister-ScheduledTask -TaskName %1 "
                                 "-Confirm:$false }")
                      .arg(quote(TaskName));
    auto *process = runPowerShell(script);
    connect(process, &QProcess::finished, this, [this, process, enabled](int code) {
        if (code != 0) {
            const auto error = QString::fromLocal8Bit(process->readAllStandardError()).trimmed();
            m_problem =
                enabled ? tr("Windows refused to register the startup task. %1").arg(error)
                        : tr("Windows refused to remove the startup task. %1").arg(error);
        }
        process->deleteLater();
        // Dans tous les cas on relit le système : l'affichage suit le réel.
        refreshAutostart();
    });
#elif defined(Q_OS_UNIX)
    const auto unit = unitPath();
    const auto link = unitLinkPath();
    if (enabled) {
        if (!QDir().mkpath(QFileInfo(unit).absolutePath()) ||
            !QDir().mkpath(QFileInfo(link).absolutePath())) {
            m_problem = tr("Could not create the user units folder.");
            refreshAutostart();
            return;
        }
        // Écriture atomique : une unité tronquée gênerait l'ouverture de
        // session entière, pas seulement RetroSave.
        QSaveFile file(unit);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            m_problem = tr("Could not write the user unit %1.").arg(unit);
            refreshAutostart();
            return;
        }
        file.write(unitText().toUtf8());
        if (!file.commit()) {
            m_problem = tr("Could not write the user unit %1.").arg(unit);
            refreshAutostart();
            return;
        }
        QFile::remove(link);
        if (!QFile::link(unit, link)) {
            m_problem = tr("Could not enable the user unit %1.").arg(link);
            QFile::remove(unit);
            refreshAutostart();
            return;
        }
    } else {
        // Retirer ce qui n'existe pas est un succès : `remove` échoue alors,
        // et c'est l'absence finale qui compte, pas son code de retour.
        QFile::remove(link);
        QFile::remove(unit);
    }
    // Au mieux : systemd prend l'unité en compte sans attendre la prochaine
    // ouverture de session. Son absence ne change pas ce qui est inscrit.
    QProcess::startDetached("systemctl", {"--user", "daemon-reload"});
    refreshAutostart();
#else
    Q_UNUSED(enabled)
    m_problem = tr("Start at login is not supported on this system.");
    refreshAutostart();
#endif
}

void Preferences::setStartWithSession(bool enabled)
{
    if (m_busy || m_autostart == "unsupported")
        return;
    m_problem.clear();
    applyAutostart(enabled);
}

void Preferences::chooseFolder(const QUrl &url)
{
    // Un dossier distant ou virtuel n'a pas de chemin local exploitable.
    if (!url.isLocalFile()) {
        m_folderProblem = tr("This folder is not a local path.");
        emit changed();
        return;
    }
    m_folder = QDir::toNativeSeparators(url.toLocalFile());
    QSettings settings(installation::settingsFile(), QSettings::IniFormat);
    settings.setValue(FolderKey, m_folder);
    settings.sync();
    checkFolder();
    emit changed();
}

void Preferences::forgetFolder()
{
    m_folder.clear();
    m_folderProblem.clear();
    QSettings settings(installation::settingsFile(), QSettings::IniFormat);
    settings.remove(FolderKey);
    settings.sync();
    emit changed();
}

void Preferences::chooseRoot(const QString &emulator, const QUrl &url)
{
    // Un identifiant vide viendrait d'un bug d'interface ; l'écrire créerait
    // une clé orpheline que l'agent ne saurait rattacher à aucun connecteur.
    if (emulator.isEmpty() || !url.isLocalFile())
        return;
    const auto path = url.toLocalFile();
    m_roots.insert(emulator, path);
    QSettings settings(installation::settingsFile(), QSettings::IniFormat);
    settings.setValue("roots/" + emulator, path);
    settings.sync();
    emit changed();
}

void Preferences::forgetRoot(const QString &emulator)
{
    // Retirer une racine arrête sa surveillance sans toucher aux fichiers ni
    // aux versions déjà publiées.
    m_roots.remove(emulator);
    QSettings settings(installation::settingsFile(), QSettings::IniFormat);
    settings.remove("roots/" + emulator);
    settings.sync();
    emit changed();
}

void Preferences::checkFolder()
{
    m_folderProblem.clear();
    if (m_folder.isEmpty())
        return;
    // Contrôle de métadonnées uniquement : le contenu n'est pas énuméré ici.
    const QFileInfo info(m_folder);
    if (!info.exists())
        m_folderProblem = tr("This folder no longer exists.");
    else if (!info.isDir())
        m_folderProblem = tr("This path is not a folder.");
    else if (!info.isReadable())
        m_folderProblem = tr("This folder is not readable.");
}

} // namespace retrosave
