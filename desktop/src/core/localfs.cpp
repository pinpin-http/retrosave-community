#include "core/localfs.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUuid>
#include <algorithm>
#include <filesystem>

namespace retrosave::core
{
namespace
{
constexpr qint64 CopyChunk = 64 * 1024;

// std::filesystem sait renommer en écrasant la cible, ce que QFile::rename ne
// fait pas. Sous Windows les chemins sont en UTF-16, ailleurs en UTF-8 : cette
// conversion évite de perdre les accents d'un nom de sauvegarde.
std::filesystem::path nativePath(const QString &path)
{
#ifdef Q_OS_WIN
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(path.toStdString());
#endif
}

[[noreturn]] void fail(const QString &message)
{
    throw LocalFsError(message.toStdString());
}

QString sha256Of(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        fail("relecture impossible : " + path);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray buffer(CopyChunk, '\0');
    for (;;) {
        const auto got = file.read(buffer.data(), buffer.size());
        if (got < 0)
            fail("relecture interrompue : " + path);
        if (got == 0)
            break;
        hash.addData(QByteArrayView(buffer.constData(), static_cast<qsizetype>(got)));
    }
    return QString::fromLatin1(hash.result().toHex());
}

void copyTree(const QString &from, const QString &to)
{
    std::error_code code;
    std::filesystem::copy(nativePath(from), nativePath(to),
                          std::filesystem::copy_options::recursive |
                              std::filesystem::copy_options::copy_symlinks,
                          code);
    if (code)
        fail("copie impossible de " + from + " : " + QString::fromStdString(code.message()));
}

void renameOver(const QString &from, const QString &to)
{
    std::error_code code;
    std::filesystem::rename(nativePath(from), nativePath(to), code);
    if (code)
        fail("publication impossible de " + to + " : " + QString::fromStdString(code.message()));
}
} // namespace

void writeAtomic(const QString &targetPath, QIODevice &source, const QString &expectedSha256)
{
    const QFileInfo target(targetPath);
    if (!QDir().mkpath(target.absolutePath()))
        fail("dossier parent impossible à créer pour " + targetPath);

    // Le temporaire est VOISIN de la cible, pas dans /tmp : un renommage n'est
    // atomique qu'à l'intérieur d'un même système de fichiers.
    const auto temporary = QDir(target.absolutePath())
                               .filePath(".rsc-tmp-" +
                                         QUuid::createUuid().toString(QUuid::WithoutBraces));
    QCryptographicHash written(QCryptographicHash::Sha256);
    {
        QFile scratch(temporary);
        if (!scratch.open(QIODevice::WriteOnly | QIODevice::NewOnly))
            fail("fichier temporaire impossible à créer près de " + targetPath);
        QByteArray buffer(CopyChunk, '\0');
        for (;;) {
            const auto got = source.read(buffer.data(), buffer.size());
            if (got < 0) {
                QFile::remove(temporary);
                fail("lecture de la source interrompue pour " + targetPath);
            }
            if (got == 0)
                break;
            if (scratch.write(buffer.constData(), got) != got) {
                QFile::remove(temporary);
                fail("écriture incomplète pour " + targetPath);
            }
            written.addData(QByteArrayView(buffer.constData(), static_cast<qsizetype>(got)));
        }
        // Forcer l'écriture réelle avant de publier : sans cela, une coupure
        // de courant pourrait laisser un fichier publié mais vide.
        if (!scratch.flush()) {
            QFile::remove(temporary);
            fail("vidage impossible pour " + targetPath);
        }
    }

    // Q13 : on relit ce que le disque rend vraiment. Comparer seulement ce
    // qu'on a calculé en écrivant ne dirait rien d'un secteur défaillant.
    const auto onDisk = sha256Of(temporary);
    if (QString::fromLatin1(written.result().toHex()) != expectedSha256 ||
        onDisk != expectedSha256) {
        QFile::remove(temporary);
        fail("écriture non conforme pour " + targetPath +
             " : le disque ne rend pas ce qui a été écrit");
    }
    renameOver(temporary, targetPath);
}

void backupWithRotation(const QString &path)
{
    const QFileInfo source(path);
    if (!source.exists())
        return; // rien à protéger
    if (source.isSymLink())
        fail("refus de sauvegarder un lien symbolique : " + path);

    const auto slot0 = path + ".rsc-bak";
    const auto slot1 = slot0 + ".1";
    const auto slot2 = slot0 + ".2";
    // La plus ancienne génération disparaît en premier, sinon le décalage
    // écraserait une copie encore utile.
    removeTarget(slot2);
    if (QFileInfo::exists(slot1))
        renameOver(slot1, slot2);
    if (QFileInfo::exists(slot0))
        renameOver(slot0, slot1);
    if (source.isDir())
        copyTree(path, slot0);
    else if (!QFile::copy(path, slot0))
        fail("copie de sécurité impossible pour " + path);
}

void removeTarget(const QString &path)
{
    const QFileInfo target(path);
    if (!target.exists() && !target.isSymLink())
        return;
    if (target.isSymLink())
        fail("refus de supprimer un lien symbolique : " + path);
    std::error_code code;
    std::filesystem::remove_all(nativePath(path), code);
    if (code)
        fail("suppression impossible de " + path + " : " + QString::fromStdString(code.message()));
}

namespace
{
// Parcours d'une unité-dossier. On ne suit aucun lien symbolique et on ignore
// les fichiers de travail de RetroSave : ni les uns ni les autres ne sont du
// contenu de sauvegarde.
void collect(const QDir &root, const QString &relPath, std::vector<QString> &files)
{
    const auto absolute = relPath.isEmpty() ? root.path() : root.filePath(relPath);
    const auto entries = QDir(absolute).entryInfoList(
        QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden, QDir::Name);
    for (const auto &entry : entries) {
        const auto name = entry.fileName();
        if (entry.isSymLink() || name.startsWith(".rsc-") || name.endsWith(".rsc-bak") ||
            name.endsWith(".rsc-bak.1") || name.endsWith(".rsc-bak.2"))
            continue;
        const auto childRel = relPath.isEmpty() ? name : relPath + "/" + name;
        if (entry.isDir())
            collect(root, childRel, files);
        else if (entry.isFile())
            files.push_back(childRel);
    }
}

std::vector<QString> unitRelativeFiles(const QString &unitType, const QString &targetPath)
{
    if (unitType == "file")
        return QFileInfo::exists(targetPath) ? std::vector<QString>{QFileInfo(targetPath).fileName()}
                                             : std::vector<QString>{};
    std::vector<QString> files;
    if (QFileInfo(targetPath).isDir())
        collect(QDir(targetPath), {}, files);
    std::sort(files.begin(), files.end(),
              [](const QString &a, const QString &b) { return a.toUtf8() < b.toUtf8(); });
    return files;
}
} // namespace

std::vector<NodeMeta> listUnitFiles(const QString &unitType, const QString &targetPath)
{
    std::vector<NodeMeta> nodes;
    for (const auto &relative : unitRelativeFiles(unitType, targetPath)) {
        const QFileInfo info(unitType == "file" ? targetPath
                                                : QDir(targetPath).filePath(relative));
        NodeMeta node;
        node.relPath = relative;
        node.sizeBytes = info.size();
        // La date n'entre pas dans l'identité du contenu : elle sert seulement
        // à repérer qu'un fichier a bougé depuis le scan précédent.
        node.mtimeMs = info.lastModified().toMSecsSinceEpoch();
        nodes.push_back(node);
    }
    return nodes;
}

std::vector<ArchiveSource> unitSources(const QString &unitType, const QString &targetPath)
{
    std::vector<ArchiveSource> sources;
    for (const auto &relative : unitRelativeFiles(unitType, targetPath)) {
        const auto absolute =
            unitType == "file" ? targetPath : QDir(targetPath).filePath(relative);
        sources.push_back({relative, QFileInfo(absolute).size(), [absolute] {
                               auto file = std::make_unique<QFile>(absolute);
                               file->open(QIODevice::ReadOnly);
                               return file;
                           }});
    }
    return sources;
}

ApplyReport applyUnit(const QString &unitType, const QString &targetPath,
                      const std::vector<StagedEntry> &entries)
{
    if (unitType != "file" && unitType != "dir")
        fail("type d'unité inconnu : " + unitType);
    if (unitType == "file" && entries.size() != 1)
        fail("une unité-fichier contient exactement un fichier");

    ApplyReport report;
    report.backedUp = QFileInfo::exists(targetPath);
    // La copie de sécurité passe AVANT toute modification. C'est elle que
    // l'utilisateur retrouvera si la suite se passe mal.
    backupWithRotation(targetPath);

    if (unitType == "file") {
        QFile staged(entries.front().stagedPath);
        if (!staged.open(QIODevice::ReadOnly))
            fail("entrée d'attente illisible : " + entries.front().stagedPath);
        writeAtomic(targetPath, staged, entries.front().sha256Hex);
        report.filesWritten = 1;
        return report;
    }

    // Unité-dossier : le contenu doit correspondre EXACTEMENT à l'archive, donc
    // l'ancien dossier disparaît. La copie de sécurité vient d'être prise, et
    // le journal d'application — écrit par l'appelant — couvre l'interruption.
    removeTarget(targetPath);
    if (!QDir().mkpath(targetPath))
        fail("dossier d'unité impossible à créer : " + targetPath);
    for (const auto &entry : entries) {
        QFile staged(entry.stagedPath);
        if (!staged.open(QIODevice::ReadOnly))
            fail("entrée d'attente illisible : " + entry.stagedPath);
        writeAtomic(QDir(targetPath).filePath(entry.relPath), staged, entry.sha256Hex);
        ++report.filesWritten;
    }
    return report;
}

} // namespace retrosave::core
