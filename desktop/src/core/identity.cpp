#include "core/identity.h"

#include <QCryptographicHash>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <stdexcept>

namespace retrosave::core
{
namespace
{

// Toutes les comparaisons de chemins se font sur les OCTETS UTF-8, jamais sur
// les caractères. C'est le seul ordre que Python, Kotlin et C++ obtiennent à
// l'identique : l'ordre « alphabétique » d'une locale varierait d'une machine
// à l'autre, et l'ordre des UTF-16 de QString diverge de l'UTF-8 hors du plan
// de base. Miroir exact de `utf8_path_sort_key` côté Python.
QByteArray sortKey(const QString &relPath)
{
    return relPath.toUtf8();
}

// QByteArray compare ses octets comme des `char`, dont le signe dépend de la
// plateforme : un octet 0xE6 pourrait passer pour négatif et se classer avant
// 0x41. On compare donc explicitement en non signé.
bool byteOrderLess(const QByteArray &left, const QByteArray &right)
{
    return std::lexicographical_compare(
        reinterpret_cast<const unsigned char *>(left.constData()),
        reinterpret_cast<const unsigned char *>(left.constData() + left.size()),
        reinterpret_cast<const unsigned char *>(right.constData()),
        reinterpret_cast<const unsigned char *>(right.constData() + right.size()));
}

// Un chemin d'unité doit rester à l'intérieur de l'unité. Refuser « .. » et
// les chemins absolus n'est pas de la coquetterie : une archive fabriquée
// pourrait sinon faire écrire ailleurs sur le disque à l'extraction.
void requireSafePaths(const std::vector<QString> &paths)
{
    std::vector<QString> seen;
    for (const auto &path : paths) {
        const auto components = path.split('/');
        const bool unsafe =
            path.isEmpty() || path.startsWith('/') || path.contains('\\') ||
            std::any_of(components.cbegin(), components.cend(), [](const QString &component) {
                return component.isEmpty() || component == "." || component == "..";
            });
        if (unsafe)
            throw std::invalid_argument("chemin relatif dangereux : " + path.toStdString());
        if (std::find(seen.cbegin(), seen.cend(), path) != seen.cend())
            throw std::invalid_argument("chemin relatif en double : " + path.toStdString());
        seen.push_back(path);
    }
}

QString hexSha256(const QByteArray &data)
{
    // toHex() rend des minuscules, ce qu'exigent les vecteurs et le serveur.
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

} // namespace

QString normalizeGameName(const QString &filename)
{
    // 1. Retirer la DERNIÈRE extension seulement : « game.with.dots.sav »
    //    garde ses points internes.
    const auto lastDot = filename.lastIndexOf('.');
    const auto withoutExtension = lastDot >= 0 ? filename.left(lastDot) : filename;

    // 2. Les balises (USA), [b], (En,Fr) décrivent la copie, pas le jeu.
    //    Elles deviennent une espace, jamais rien : « Layton_[EU] (Fr) » ne
    //    doit pas se recoller en un seul mot.
    static const QRegularExpression tag(R"(\([^)]*\)|\[[^\]]*\])");
    auto text = withoutExtension;
    text.replace(tag, " ");

    // 3. Les tirets bas séparent souvent les mots dans les noms de fichiers.
    text.replace('_', ' ');
    text = text.toLower();

    // 4. Compacter les espaces. UseUnicodePropertiesOption aligne `\s` sur ce
    //    que Python entend par espace ; sans elle, PCRE se limiterait à l'ASCII
    //    et une espace insécable survivrait à la normalisation.
    static const QRegularExpression whitespace(
        R"(\s+)", QRegularExpression::UseUnicodePropertiesOption);
    text.replace(whitespace, " ");
    return text.trimmed();
}

QByteArray canonicalQuickFingerprint(std::vector<NodeMeta> nodes)
{
    // Le paramètre est pris PAR VALEUR, donc c'est déjà notre copie : on peut
    // la trier sans surprendre l'appelant. Le passer par référence non const
    // trierait la sienne au passage, ce qui serait un effet de bord caché.
    std::sort(nodes.begin(), nodes.end(), [](const NodeMeta &left, const NodeMeta &right) {
        return byteOrderLess(sortKey(left.relPath), sortKey(right.relPath));
    });

    QByteArray canonical;
    for (const auto &node : nodes) {
        // Une date absente vaut 0 dans la grammaire, mais l'absence et le zéro
        // restent distincts en mémoire — c'est le rôle de std::optional.
        const qint64 mtime = node.mtimeMs.value_or(0);
        // Grammaire Q8, octet pour octet : chemin \0 taille \0 date \n
        canonical += node.relPath.toUtf8();
        canonical += '\0';
        canonical += QByteArray::number(node.sizeBytes);
        canonical += '\0';
        canonical += QByteArray::number(mtime);
        canonical += '\n';
    }
    return canonical;
}

QString quickFingerprintHash(std::vector<NodeMeta> nodes)
{
    return hexSha256(canonicalQuickFingerprint(std::move(nodes)));
}

QString directoryContentSha256(std::vector<ContentDigest> files)
{
    std::vector<QString> paths;
    paths.reserve(files.size());
    for (const auto &file : files)
        paths.push_back(file.relPath);
    requireSafePaths(paths);

    for (const auto &file : files) {
        if (file.sizeBytes < 0)
            throw std::invalid_argument("taille de fichier négative");
        if (file.sha256Hex.size() != 64 || file.sha256Hex != file.sha256Hex.toLower() ||
            QByteArray::fromHex(file.sha256Hex.toLatin1()).size() != 32)
            throw std::invalid_argument("SHA-256 attendu en 64 caractères hexadécimaux minuscules");
    }

    std::sort(files.begin(), files.end(), [](const ContentDigest &left, const ContentDigest &right) {
        return byteOrderLess(sortKey(left.relPath), sortKey(right.relPath));
    });

    // Le manifeste ne contient AUCUNE date : c'est ce qui rend l'identité de
    // contenu stable quand un fichier est copié, restauré ou décompressé.
    QByteArray manifest;
    for (const auto &file : files) {
        manifest += file.relPath.toUtf8();
        manifest += '\0';
        manifest += QByteArray::number(file.sizeBytes);
        manifest += '\0';
        manifest += file.sha256Hex.toLatin1();
        manifest += '\n';
    }
    return hexSha256(manifest);
}

QString contentSha256(const QString &unitType, const std::vector<ContentFile> &files)
{
    std::vector<QString> paths;
    paths.reserve(files.size());
    for (const auto &file : files)
        paths.push_back(file.relPath);
    requireSafePaths(paths);

    if (unitType == "file") {
        // Une unité-fichier est hachée telle quelle : son nom n'entre pas dans
        // l'identité, sinon renommer une sauvegarde en ferait un autre contenu.
        if (files.size() != 1)
            throw std::invalid_argument("une unité-fichier contient exactement un fichier");
        return hexSha256(files.front().content);
    }
    if (unitType != "dir")
        throw std::invalid_argument("type d'unité inconnu : " + unitType.toStdString());

    std::vector<ContentDigest> digests;
    digests.reserve(files.size());
    for (const auto &file : files)
        digests.push_back({file.relPath, static_cast<qint64>(file.content.size()),
                           hexSha256(file.content)});
    return directoryContentSha256(std::move(digests));
}

} // namespace retrosave::core

namespace retrosave::core
{
namespace
{

// Les jeux de nommage courants (No-Intro, GoodTools, TOSEC). Listes identiques
// à celles de `DisplayName.kt` : un écart ferait diverger l'affichage entre le
// PC et le téléphone pour le même fichier.
const QSet<QString> &regionTags()
{
    static const QSet<QString> tags{
        "usa",    "europe", "japan",  "world",  "france",     "germany", "spain",
        "italy",  "australia", "canada", "korea", "china",    "taiwan",  "brazil",
        "netherlands", "sweden", "norway", "denmark", "finland", "russia", "poland",
        "asia",   "uk",     "usa/europe", "japan/usa", "ntsc",  "pal",     "ntsc-u",
        "ntsc-j", "en",     "fr",     "de",     "es",         "it",      "ja",
        "jp",     "nl",     "pt",     "sv",     "no",         "da",      "fi",
        "zh",     "ko",     "pl",     "ru"};
    return tags;
}

const QSet<QString> &editionTags()
{
    static const QSet<QString> tags{"proto", "beta",  "demo",     "sample",
                                    "unl",   "alt",   "kiosk",    "prototype"};
    return tags;
}

bool isKnownPart(const QString &part)
{
    const auto lowered = part.toLower();
    if (regionTags().contains(lowered) || editionTags().contains(lowered))
        return true;
    // Drapeaux de dump GoodTools, traductions, révisions et versions.
    static const QRegularExpression dumpFlag(
        QStringLiteral("^[!abcfhopstux][0-9]*$"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression translation(
        QStringLiteral("^t[+-][a-z]{2,4}[0-9.]*$"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression revision(
        QStringLiteral("^(rev\\s*[0-9a-z]+|v[0-9][0-9a-z.]*)$"),
        QRegularExpression::CaseInsensitiveOption);
    return dumpFlag.match(part).hasMatch() || translation.match(part).hasMatch() ||
           revision.match(part).hasMatch();
}

// Tout le groupe doit être reconnu : « (Europe, Remix) » garde son sens et
// reste affiché, plutôt que de perdre à moitié une information utile.
bool isKnownTag(const QString &inside)
{
    const auto parts = inside.split(',', Qt::SkipEmptyParts);
    bool seen = false;
    for (const auto &part : parts) {
        const auto trimmed = part.trimmed();
        if (trimmed.isEmpty())
            continue;
        seen = true;
        if (!isKnownPart(trimmed))
            return false;
    }
    return seen;
}

QString stripExtension(const QString &filename)
{
    const auto index = filename.lastIndexOf('.');
    // `index > 0` et pas `>= 0` : un fichier comme `.nomedia` n'a pas
    // d'extension, il a un nom qui commence par un point.
    if (index <= 0)
        return filename;
    static const QRegularExpression extension(QStringLiteral("^[A-Za-z0-9]{1,5}$"));
    if (!extension.match(filename.mid(index + 1)).hasMatch())
        return filename;
    return filename.left(index);
}

} // namespace

QString displayName(const QString &filename)
{
    const auto stem = stripExtension(filename);
    static const QRegularExpression group(QStringLiteral("\\(([^()]*)\\)|\\[([^\\[\\]]*)\\]"));
    static const QRegularExpression whitespace(QStringLiteral("\\s+"));

    QString withoutTags;
    qsizetype last = 0;
    auto it = group.globalMatch(stem);
    while (it.hasNext()) {
        const auto match = it.next();
        withoutTags += stem.mid(last, match.capturedStart() - last);
        const auto inside =
            match.captured(1).isEmpty() ? match.captured(2) : match.captured(1);
        withoutTags += isKnownTag(inside) ? QStringLiteral(" ") : match.captured(0);
        last = match.capturedEnd();
    }
    withoutTags += stem.mid(last);

    auto cleaned = withoutTags;
    cleaned.replace('_', ' ');
    cleaned.replace('.', ' ');
    cleaned = cleaned.replace(whitespace, QStringLiteral(" ")).trimmed();
    if (!cleaned.isEmpty())
        return cleaned;
    // Un nom entièrement composé de tags — ça existe — ne doit pas devenir une
    // ligne vide dans la bibliothèque : mieux vaut afficher le nom brut.
    return QString(stem).replace(whitespace, QStringLiteral(" ")).trimmed();
}

} // namespace retrosave::core
