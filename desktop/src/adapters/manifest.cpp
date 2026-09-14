#include "adapters/manifest.h"

#include <QFile>
#include <QRegularExpression>

namespace retrosave::adapters
{
namespace
{

// Une valeur TOML, telle que nos manifestes en contiennent : une chaîne, un
// entier, ou une liste de chaînes. Rien de plus n'est représentable, donc
// rien de plus ne peut être accepté par erreur.
struct Value {
    enum class Kind { String, Integer, List } kind = Kind::String;
    QString text;
    int number = 0;
    QStringList list;
};

[[noreturn]] void refuse(const QString &reason)
{
    throw ManifestError(reason.toStdString());
}

QString unquote(const QString &raw, const QString &context)
{
    if (raw.size() < 2 || !raw.startsWith('"') || !raw.endsWith('"'))
        refuse(context + " : chaîne entre guillemets attendue, reçu " + raw);
    const auto text = raw.mid(1, raw.size() - 2);
    // Les manifestes contiennent des chemins Windows, où « \\ » représente un
    // seul antislash. C'est la seule séquence d'échappement dont nous ayons
    // besoin ; en accepter d'autres donnerait une fausse impression de
    // complétude. On déroule la chaîne au lieu de la sonder avec une
    // expression régulière : « \\\\Documents » contient des antislashs à la
    // fois échappés et suivis d'une lettre, et seul un parcours de gauche à
    // droite distingue les deux cas.
    QString decoded;
    decoded.reserve(text.size());
    for (qsizetype i = 0; i < text.size(); ++i) {
        if (text.at(i) != '\\') {
            decoded.append(text.at(i));
            continue;
        }
        if (i + 1 >= text.size() || text.at(i + 1) != '\\')
            refuse(context + " : seule la séquence \\\\ est acceptée dans une chaîne");
        decoded.append('\\');
        ++i; // la paire est consommée d'un bloc
    }
    return decoded;
}

Value parseValue(const QString &raw, const QString &context)
{
    const auto trimmed = raw.trimmed();
    if (trimmed.startsWith('[')) {
        if (!trimmed.endsWith(']'))
            refuse(context + " : tableau non refermé sur la même ligne");
        Value value;
        value.kind = Value::Kind::List;
        const auto inner = trimmed.mid(1, trimmed.size() - 2).trimmed();
        if (inner.isEmpty())
            return value;
        // Découpage naïf sur la virgule : nos manifestes ne contiennent aucune
        // virgule à l'intérieur d'une chaîne. On le vérifie plutôt que de le
        // supposer — une chaîne contenant une virgule casserait ce découpage,
        // et il vaut mieux un refus qu'un tableau silencieusement faux.
        for (const auto &part : inner.split(',')) {
            const auto piece = part.trimmed();
            if (piece.isEmpty())
                refuse(context + " : élément vide dans un tableau");
            value.list.append(unquote(piece, context));
        }
        if (value.list.join(",").contains(",,"))
            refuse(context + " : tableau ambigu");
        return value;
    }
    if (trimmed.startsWith('"')) {
        Value value;
        value.kind = Value::Kind::String;
        value.text = unquote(trimmed, context);
        return value;
    }
    bool ok = false;
    const auto number = trimmed.toInt(&ok);
    if (!ok)
        refuse(context + " : valeur non reconnue " + trimmed);
    Value value;
    value.kind = Value::Kind::Integer;
    value.number = number;
    return value;
}

QString requireString(const QHash<QString, Value> &table, const QString &key)
{
    if (!table.contains(key))
        refuse("clé obligatoire absente : " + key);
    if (table.value(key).kind != Value::Kind::String)
        refuse(key + " : chaîne attendue");
    return table.value(key).text;
}

QStringList optionalList(const QHash<QString, Value> &table, const QString &key)
{
    if (!table.contains(key))
        return {};
    if (table.value(key).kind != Value::Kind::List)
        refuse(key + " : tableau attendu");
    return table.value(key).list;
}

int optionalInt(const QHash<QString, Value> &table, const QString &key, int fallback)
{
    if (!table.contains(key))
        return fallback;
    if (table.value(key).kind != Value::Kind::Integer)
        refuse(key + " : entier attendu");
    return table.value(key).number;
}

// Toute clé rencontrée doit figurer dans cette liste. Une clé inconnue est un
// refus : c'est ce qui empêche une faute de frappe de désactiver une règle.
void requireKnownKeys(const QHash<QString, Value> &table, const QString &section,
                      const QStringList &known)
{
    for (const auto &key : table.keys()) {
        if (!known.contains(key))
            refuse("clé inconnue dans [" + section + "] : " + key);
    }
}

} // namespace

AdapterManifest parseManifest(const QByteArray &toml)
{
    // Un dictionnaire par table, la table racine portant le nom vide.
    QHash<QString, QHash<QString, Value>> tables;
    QString current;
    tables.insert(current, {});

    int lineNumber = 0;
    for (const auto &rawLine : QString::fromUtf8(toml).split('\n')) {
        ++lineNumber;
        auto line = rawLine.trimmed();
        // Les commentaires ne peuvent commencer qu'en début de ligne dans nos
        // manifestes ; couper sur un « # » en milieu de ligne casserait un
        // chemin qui en contiendrait un.
        if (line.isEmpty() || line.startsWith('#'))
            continue;
        if (line.startsWith('[')) {
            if (!line.endsWith(']'))
                refuse(QString("ligne %1 : en-tête de table mal formé").arg(lineNumber));
            current = line.mid(1, line.size() - 2).trimmed();
            if (current.isEmpty())
                refuse(QString("ligne %1 : nom de table vide").arg(lineNumber));
            tables.insert(current, {});
            continue;
        }
        const auto equals = line.indexOf('=');
        if (equals < 0)
            refuse(QString("ligne %1 : affectation attendue").arg(lineNumber));
        const auto key = line.left(equals).trimmed();
        if (key.isEmpty())
            refuse(QString("ligne %1 : clé vide").arg(lineNumber));
        tables[current].insert(
            key, parseValue(line.mid(equals + 1),
                            QString("ligne %1 (%2)").arg(lineNumber).arg(key)));
    }

    for (const auto &section : tables.keys()) {
        static const QStringList allowed{"", "roots", "roots.validate", "discovery", "identity"};
        if (!allowed.contains(section))
            refuse("table inconnue : [" + section + "]");
    }

    const auto &root = tables[""];
    requireKnownKeys(root, "racine", {"id", "name", "process_names", "android_packages"});
    AdapterManifest manifest;
    manifest.id = requireString(root, "id");
    manifest.name = requireString(root, "name");
    manifest.processNames = optionalList(root, "process_names");
    manifest.androidPackages = optionalList(root, "android_packages");

    const auto &roots = tables.value("roots");
    requireKnownKeys(roots, "roots", {"windows", "linux"});
    manifest.roots.windows = optionalList(roots, "windows");
    manifest.roots.linux = optionalList(roots, "linux");

    const auto &validate = tables.value("roots.validate");
    requireKnownKeys(validate, "roots.validate",
                     {"markers_primary", "markers_secondary", "markers_secondary_min"});
    manifest.roots.validate.markersPrimary = optionalList(validate, "markers_primary");
    manifest.roots.validate.markersSecondary = optionalList(validate, "markers_secondary");
    manifest.roots.validate.markersSecondaryMin = optionalInt(validate, "markers_secondary_min", 0);

    const auto &discovery = tables.value("discovery");
    if (discovery.isEmpty())
        refuse("section [discovery] obligatoire");
    requireKnownKeys(discovery, "discovery", {"unit_type", "pattern", "exclude", "max_depth"});
    manifest.discovery.unitType = requireString(discovery, "unit_type");
    manifest.discovery.pattern = requireString(discovery, "pattern");
    manifest.discovery.exclude = optionalList(discovery, "exclude");
    manifest.discovery.maxDepth = optionalInt(discovery, "max_depth", 0);
    if (manifest.discovery.unitType != "file" && manifest.discovery.unitType != "dir")
        refuse("unit_type doit valoir « file » ou « dir »");
    if (manifest.discovery.maxDepth <= 0)
        refuse("max_depth doit être strictement positif : sans borne, un lien ou "
               "une arborescence profonde ferait tourner le scan indéfiniment");

    const auto &identity = tables.value("identity");
    if (identity.isEmpty())
        refuse("section [identity] obligatoire");
    requireKnownKeys(identity, "identity", {"unit_key", "game_key"});
    manifest.identity.unitKey = requireString(identity, "unit_key");
    manifest.identity.gameKey = requireString(identity, "game_key");

    // Vocabulaire fermé : une règle inconnue est refusée au chargement, pas
    // découverte au milieu d'un scan chez l'utilisateur.
    const auto &unitKey = manifest.identity.unitKey;
    if (unitKey != unitKeyRule::Dirname && unitKey != unitKeyRule::Filename &&
        !unitKey.startsWith(unitKeyRule::PathSegmentPrefix))
        refuse("règle unit_key inconnue : " + unitKey);
    if (unitKey.startsWith(unitKeyRule::PathSegmentPrefix)) {
        bool ok = false;
        const auto index = unitKey.mid(QString(unitKeyRule::PathSegmentPrefix).size()).toInt(&ok);
        if (!ok || index < 0)
            refuse("path_segment attend un indice positif : " + unitKey);
    }
    const auto &gameKey = manifest.identity.gameKey;
    if (gameKey != gameKeyRule::Serial9 && gameKey != gameKeyRule::NormalizedName &&
        !gameKey.startsWith(gameKeyRule::PrefixPrefix))
        refuse("règle game_key inconnue : " + gameKey);

    if (manifest.id.isEmpty() || manifest.name.isEmpty())
        refuse("id et name sont obligatoires");
    return manifest;
}

AdapterManifest loadManifest(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw ManifestError(("manifeste illisible : " + path).toStdString());
    try {
        return parseManifest(file.readAll());
    } catch (const ManifestError &error) {
        // On rattache le nom du fichier au motif : un message sans fichier
        // serait inutilisable dès qu'il y a quatre manifestes.
        throw ManifestError(path.toStdString() + " — " + error.what());
    }
}

} // namespace retrosave::adapters
