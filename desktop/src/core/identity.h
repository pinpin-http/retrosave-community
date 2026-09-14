// Primitives d'identité partagées par les moteurs C++ et Kotlin. Elles restent
// pures et rejouent les vecteurs de tests/vectors/ : une divergence créerait
// des conflits artificiels entre appareils.
#pragma once

#include <QByteArray>
#include <QString>
#include <optional>
#include <vector>

namespace retrosave::core
{

struct NodeMeta {
    QString relPath;
    qint64 sizeBytes = 0;
    std::optional<qint64> mtimeMs;
};

// Un fichier avec son contenu en mémoire. Ne convient qu'aux petites unités et
// aux tests : le moteur réel hachera en flux, sans tout charger (une unité
// peut peser 256 Mo, cf. invariant I1).
struct ContentFile {
    QString relPath;
    QByteArray content;
};

// Une ligne de manifeste, quand le hachage a déjà été fait ailleurs.
struct ContentDigest {
    QString relPath;
    qint64 sizeBytes = 0;
    QString sha256Hex;
};

// Sert de clé de regroupement pour melonDS et RetroArch : deux fichiers de
// sauvegarde nommés différemment doivent tomber sur la même clé s'il s'agit du
// même jeu. Miroir de cli/rsc/core/normalize_name.py.
QString normalizeGameName(const QString &filename);

// Dit « ce dossier a-t-il bougé ? » sans lire le contenu des fichiers. C'est
// ce qui rend un scan périodique acceptable. Miroir de fingerprints.py.
QByteArray canonicalQuickFingerprint(std::vector<NodeMeta> nodes);
QString quickFingerprintHash(std::vector<NodeMeta> nodes);

// Dit « est-ce le même contenu ? », indépendamment des dates et de la
// compression. C'est ce qui permet la déduplication et la détection de
// changement. Miroir de content_hash.py.
//
// `unitType` vaut "file" ou "dir". Ces fonctions LÈVENT une exception
// (std::invalid_argument) sur un chemin dangereux ou dupliqué. L'appelant doit
// la rattraper unité par unité : une seule unité fautive ne doit jamais
// interrompre une passe entière (banc d'acceptation, cas 17).
QString contentSha256(const QString &unitType, const std::vector<ContentFile> &files);
QString directoryContentSha256(std::vector<ContentDigest> files);

// À distinguer de `normalizeGameName`, volontairement : celle-là produit la
// CLÉ d'appariement entre appareils (la toucher casserait l'appariement),
// celle-ci ne sert qu'à l'AFFICHAGE et conserve la casse.
//
// Un seul principe : n'enlever que ce qu'on reconnaît. Retirer tout groupe
// entre parenthèses amputerait « Tony Hawk's Underground 2 (Remix) » ou
// « Zelda [Master Quest] », où le groupe fait partie du titre. Mieux vaut
// laisser un tag de trop que manger un mot du nom.
//
// Miroir de `DisplayName.kt` ; vecteur partagé `display_name_basic.json`.
QString displayName(const QString &filename);

} // namespace retrosave::core
