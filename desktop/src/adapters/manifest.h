// ─── Un émulateur = un fichier TOML, pas du code C++ ──────────────────────
//
// C'est l'idée directrice de tout ce dossier. Les manifestes `adapters/*.toml`
// décrivent DÉJÀ, de façon déclarative, où vivent les sauvegardes de chaque
// émulateur et comment les identifier. Jusqu'ici personne ne les lisait
// vraiment : Python code chaque adaptateur à la main et n'utilise du manifeste
// que les chemins par défaut.
//
// Ce moteur en fait le contrat réel. Conséquence pratique : ajouter la prise
// en charge d'un nouvel émulateur ne demande, dans le cas courant, AUCUNE
// ligne de C++ — un fichier TOML suffit, et il est partagé avec les autres
// implémentations.
//
// Règle de sûreté : ce lecteur REFUSE tout ce qu'il ne comprend pas — clé
// inconnue, règle d'identité inconnue, type inattendu. Ignorer silencieusement
// une règle serait pire que ne pas la lire : la découverte partirait de
// travers sans que rien ne le signale.
#pragma once

#include <QString>
#include <QStringList>
#include <stdexcept>

namespace retrosave::adapters
{

class ManifestError : public std::runtime_error
{
  public:
    using std::runtime_error::runtime_error;
};

// Vocabulaire fermé des règles d'identité. Ajouter une valeur ici est un
// changement de contrat : il doit être décidé, pas glissé.
namespace unitKeyRule
{
inline constexpr auto Dirname = "dirname";   // le nom du dossier trouvé
inline constexpr auto Filename = "filename"; // le nom du fichier trouvé
// « path_segment:N » : le N-ième segment que le motif a capturé par un
// joker, numéroté à partir de zéro. Pour Azahar, le motif
// `sdmc/Nintendo 3DS/*/*/title/00040000/*/data` capture id0, id1 puis le
// title ID : `path_segment:2` désigne donc bien le title ID.
inline constexpr auto PathSegmentPrefix = "path_segment:";
} // namespace unitKeyRule

namespace gameKeyRule
{
inline constexpr auto Serial9 = "serial9";                 // 9 premiers caractères
inline constexpr auto NormalizedName = "normalized_name";  // nom de jeu normalisé
inline constexpr auto PrefixPrefix = "prefix:";            // littéral + clé d'unité
} // namespace gameKeyRule

struct RootValidationSpec {
    QStringList markersPrimary;
    QStringList markersSecondary;
    int markersSecondaryMin = 0;
};

struct RootsSpec {
    QStringList windows;
    QStringList linux;
    RootValidationSpec validate;
};

struct DiscoverySpec {
    QString unitType; // « file » ou « dir »
    QString pattern;  // motif relatif à la racine ; « * » un segment, « ** » plusieurs
    QStringList exclude;
    int maxDepth = 0;
};

struct IdentitySpec {
    QString unitKey;
    QString gameKey;
};

struct AdapterManifest {
    QString id;
    QString name;
    QStringList processNames;   // pour ne pas capturer pendant qu'un jeu tourne
    QStringList androidPackages; // informatif côté desktop
    RootsSpec roots;
    DiscoverySpec discovery;
    IdentitySpec identity;
};

// Lit le sous-ensemble de TOML qu'utilisent nos manifestes : commentaires,
// tables `[a]` et `[a.b]`, chaînes, entiers et tableaux de chaînes. Rien
// d'autre n'est accepté — voir la règle de sûreté en tête de fichier.
AdapterManifest parseManifest(const QByteArray &toml);

// Charge un manifeste depuis un fichier et vérifie sa cohérence.
AdapterManifest loadManifest(const QString &path);

} // namespace retrosave::adapters
