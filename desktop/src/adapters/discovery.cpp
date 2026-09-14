#include "adapters/discovery.h"

#include "core/identity.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <algorithm>

namespace retrosave::adapters
{
namespace
{

// Un segment de motif devient une expression régulière ancrée. Le joker « * »
// ne traverse jamais un « / » : la traversée de plusieurs dossiers s'écrit
// « ** », et rien d'autre.
QRegularExpression segmentMatcher(const QString &segment)
{
    // Comparaison insensible à la casse : Windows ne distingue pas
    // la casse dans ses noms de fichiers, et un joueur qui a « Sonic.SRM » ne
    // comprendrait pas que sa sauvegarde soit ignorée sous Linux et vue ailleurs.
    return QRegularExpression(
        QRegularExpression::anchoredPattern(QRegularExpression::wildcardToRegularExpression(
            segment, QRegularExpression::UnanchoredWildcardConversion)),
        QRegularExpression::CaseInsensitiveOption);
}

struct MatchResult {
    bool matched = false;
    // Segments capturés par un joker occupant TOUT un segment. Ce sont eux que
    // « path_segment:N » numérote.
    QStringList captures;
};

MatchResult matchSegments(const QStringList &pattern, qsizetype p, const QStringList &path,
                          qsizetype s)
{
    if (p == pattern.size())
        return {s == path.size(), {}};
    if (pattern.at(p) == "**") {
        // « ** » absorbe zéro, un ou plusieurs segments. On essaie du plus
        // court au plus long : le premier accord suffit.
        for (auto consumed = s; consumed <= path.size(); ++consumed) {
            auto rest = matchSegments(pattern, p + 1, path, consumed);
            if (rest.matched)
                return rest;
        }
        return {};
    }
    if (s == path.size())
        return {};
    if (!segmentMatcher(pattern.at(p)).match(path.at(s)).hasMatch())
        return {};
    auto rest = matchSegments(pattern, p + 1, path, s + 1);
    if (!rest.matched)
        return {};
    if (pattern.at(p) == "*")
        rest.captures.prepend(path.at(s));
    return rest;
}

MatchResult matchPattern(const QString &pattern, const QString &relPath)
{
    return matchSegments(pattern.split('/'), 0, relPath.split('/'), 0);
}

bool isExcluded(const QStringList &excludes, const QString &relPath)
{
    return std::any_of(excludes.cbegin(), excludes.cend(), [&](const QString &exclude) {
        return matchPattern(exclude, relPath).matched;
    });
}

// Parcours borné par max_depth. On ne suit JAMAIS un lien symbolique : un lien
// pointant hors de la racine ferait sortir la découverte du périmètre que
// l'utilisateur a désigné.
void walk(const QDir &root, const QString &relPath, int depth, int maxDepth,
          std::vector<QString> &directories, std::vector<QString> &files)
{
    if (depth > maxDepth)
        return;
    const auto absolute = relPath.isEmpty() ? root.path() : root.filePath(relPath);
    QDir directory(absolute);
    const auto entries =
        directory.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden,
                                QDir::Name);
    for (const auto &entry : entries) {
        if (entry.isSymLink() || isRetrosaveInternalName(entry.fileName()))
            continue;
        const auto childRel = relPath.isEmpty() ? entry.fileName() : relPath + "/" + entry.fileName();
        if (entry.isDir()) {
            directories.push_back(childRel);
            walk(root, childRel, depth + 1, maxDepth, directories, files);
        } else if (entry.isFile()) {
            files.push_back(childRel);
        }
    }
}

QString applyUnitKeyRule(const QString &rule, const QString &relPath, const QStringList &captures,
                         QString &problem)
{
    if (rule == unitKeyRule::Dirname || rule == unitKeyRule::Filename)
        return relPath.section('/', -1);
    const auto index = rule.mid(QString(unitKeyRule::PathSegmentPrefix).size()).toInt();
    if (index >= captures.size()) {
        // Le motif n'a pas capturé assez de segments : c'est une incohérence de
        // manifeste, pas une sauvegarde fautive. On le dit sur l'unité plutôt
        // que d'inventer une clé.
        problem = QStringLiteral("le motif ne capture pas le segment %1").arg(index);
        return relPath.section('/', -1);
    }
    return captures.at(index);
}

QString applyGameKeyRule(const QString &rule, const QString &unitKey)
{
    if (rule == gameKeyRule::Serial9)
        return unitKey.left(9);
    if (rule == gameKeyRule::NormalizedName)
        return core::normalizeGameName(unitKey);
    return rule.mid(QString(gameKeyRule::PrefixPrefix).size()) + unitKey;
}

} // namespace

QStringList blockedUnitExtensions()
{
    return {".nds", ".3ds", ".cci", ".cxi", ".app", ".iso",
            ".cso", ".chd", ".pbp", ".zip", ".7z",  ".rar"};
}

bool isRetrosaveInternalName(const QString &name)
{
    return name.startsWith(".rsc-") || name.endsWith(".rsc-bak") ||
           name.endsWith(".rsc-bak.1") || name.endsWith(".rsc-bak.2");
}

ManifestAdapter::ManifestAdapter(AdapterManifest manifest) : m_manifest(std::move(manifest)) {}

std::vector<DiscoveredUnit> ManifestAdapter::discover(const QString &rootPath) const
{
    std::vector<DiscoveredUnit> units;
    const QDir root(rootPath);
    if (!root.exists())
        return units;

    std::vector<QString> directories;
    std::vector<QString> files;
    walk(root, {}, 1, m_manifest.discovery.maxDepth, directories, files);

    const bool wantsDirectories = m_manifest.discovery.unitType == "dir";
    auto &candidates = wantsDirectories ? directories : files;
    // Ordre stable par octets UTF-8 : deux machines doivent découvrir les
    // mêmes unités dans le même ordre, indépendamment de la locale.
    std::sort(candidates.begin(), candidates.end(), [](const QString &a, const QString &b) {
        return a.toUtf8() < b.toUtf8();
    });

    for (const auto &relPath : candidates) {
        const auto match = matchPattern(m_manifest.discovery.pattern, relPath);
        if (!match.matched || isExcluded(m_manifest.discovery.exclude, relPath))
            continue;

        DiscoveredUnit unit;
        unit.emulator = m_manifest.id;
        unit.unitType = m_manifest.discovery.unitType;
        unit.relPath = relPath;
        unit.unitKey =
            applyUnitKeyRule(m_manifest.identity.unitKey, relPath, match.captures, unit.problem);
        unit.gameKey = applyGameKeyRule(m_manifest.identity.gameKey, unit.unitKey);
        // Le libellé de départ est dérivé de la clé, et un libellé n'a jamais
        // le droit de faire échouer une passe : le vrai titre viendra plus
        // tard, du serveur ou du PARAM.SFO.
        //
        // La clé garde le nom de fichier EXACT — c'est elle qui relie la
        // sauvegarde à son émulateur — mais l'afficher tel quel donnait
        // « Chrono Trigger (U).srm » dans la bibliothèque, et empêchait de
        // trouver la jaquette, cherchée à partir du libellé. `displayName`
        // n'enlève que ce qu'elle reconnaît, et rend le nom brut si le
        // nettoyage ne laisse rien ; un nom de dossier PSP en ressort intact.
        unit.gameLabel = core::displayName(unit.unitKey);

        if (!wantsDirectories) {
            // Invariant I1 : une unité-fichier dont l'extension est celle d'une
            // ROM ou d'une image disque n'est jamais prise, quoi que dise le
            // motif. Cette barrière est volontairement hors du manifeste : un
            // fichier TOML ne doit pas pouvoir la lever.
            const auto lowered = relPath.section('/', -1).toLower();
            for (const auto &extension : blockedUnitExtensions()) {
                if (lowered.endsWith(extension)) {
                    unit.problem = QStringLiteral("extension interdite par I1 : ") + extension;
                    break;
                }
            }
            if (!unit.problem.isEmpty())
                continue; // on ne la propose même pas
        }
        units.push_back(unit);
    }
    return units;
}

} // namespace retrosave::adapters
