#include "engine/folderscan.h"

#include <QDir>
#include <QFileInfo>
#include <algorithm>

namespace retrosave::engine
{
namespace
{
// Même règle que `is_retrosave_internal_name` côté Python et que la découverte
// pilotée par manifeste. Sans cette exclusion, chaque réception créerait une
// unité de plus — la copie de sécurité qu'on vient d'écrire — et l'agent
// téléverserait ses propres sauvegardes de sauvegarde, indéfiniment.
bool internalName(const QString &name)
{
    return name.startsWith(".rsc-") || name.endsWith(".rsc-bak") || name.endsWith(".rsc-bak.1") ||
           name.endsWith(".rsc-bak.2");
}
} // namespace

std::vector<ScannedUnit> scanFolderRoot(const QString &root)
{
    std::vector<ScannedUnit> units;
    QDir directory(root);
    if (!directory.exists())
        return units;
    auto names = directory.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    // Tri sur les octets UTF-8, comme partout ailleurs dans le noyau : trier
    // sur les unités UTF-16 de QString classerait « é » et « 日本語 » dans un
    // autre ordre que Python et Kotlin.
    std::sort(names.begin(), names.end(),
              [](const QString &a, const QString &b) { return a.toUtf8() < b.toUtf8(); });
    for (const auto &name : names) {
        const QFileInfo info(directory.filePath(name));
        if (internalName(name) || info.isSymLink())
            continue;
        // Un dossier vide n'est pas une sauvegarde : le déclarer créerait une
        // unité que rien ne peut jamais remplir.
        if (QDir(info.absoluteFilePath())
                .entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot)
                .isEmpty())
            continue;
        units.push_back({"folder", name, "dir", "folder:" + name, name, "folder", name});
    }
    return units;
}

} // namespace retrosave::engine
