// Une unité devient un tar déterministe comprimé avec zstd. L'identité commune
// entre moteurs est `content_sha256` ; les octets zstd peuvent varier selon la
// bibliothèque utilisée.
//
// Ce fichier expose volontairement la couche `tar` seule, en plus de
// l'archive complète : c'est la couche que l'on peut comparer entre
// implémentations sans dépendre de la version de la bibliothèque zstd.
#pragma once

#include "core/identity.h"

#include <QByteArray>
#include <QIODevice>
#include <QString>
#include <functional>
#include <memory>
#include <stdexcept>
#include <vector>

namespace retrosave::core
{

inline constexpr int ZstdLevel = 10;
// Une archive tar se termine par deux blocs nuls de 512, puis un remplissage
// jusqu'à un multiple de 10 240 octets — l'unité d'enregistrement historique.
inline constexpr qsizetype TarBlockSize = 512;
inline constexpr qsizetype TarRecordSize = 10240;

class ArchiveError : public std::runtime_error
{
  public:
    using std::runtime_error::runtime_error;
};

// Tout ce que le serveur doit savoir d'un envoi, figé au moment de la capture.
struct ArchiveBlob {
    QByteArray bytes;       // l'objet .tar.zst lui-même
    QString contentSha256;  // identité du CONTENU : ce qui décide des doublons
    QString archiveSha256;  // intégrité de CET objet : ce qui décide du transfert
    qint64 sizeBytes = 0;   // taille logique décompressée
};

// Les fonctions qui prennent et rendent des QByteArray chargent l'unité
// ENTIÈRE en mémoire. Elles restent pour les tests et les petites unités.
//
// Les fonctions `...From` / `...To` ci-dessous travaillent en FLUX : elles
// lisent, compressent et écrivent par petits morceaux, sans jamais tenir
// l'unité complète. C'est la règle AD-29, et ce n'est pas du confort : une
// unité peut atteindre 256 Mio.

// Une source = un nom, une taille, et le moyen d'ouvrir les octets — jamais un
// chemin. Sur Android, un document n'a pas de chemin de fichier : tout autre
// choix obligerait à recopier chaque unité avant de l'archiver.
struct ArchiveSource {
    QString relPath;
    qint64 sizeBytes = 0;
    std::function<std::unique_ptr<QIODevice>()> open;
};

// Mêmes informations que ArchiveBlob, pour une archive qui vit sur disque.
struct ArchiveInfo {
    QString contentSha256;
    QString archiveSha256;
    qint64 sizeBytes = 0;
    qint64 archiveBytes = 0;
};

// Une entrée déballée : son nom, où elle attend, et son empreinte.
struct StagedEntry {
    QString relPath;
    QString stagedPath;
    QString sha256Hex;
};

// Écrit une archive directement sur disque depuis ses sources.
ArchiveInfo createArchiveFrom(const QString &unitType, std::vector<ArchiveSource> entries,
                              const QString &destination);

// Déballe dans un dossier d'attente, JUGE l'ensemble, et n'appelle `sink`
// qu'ensuite, une fois par entrée. C'est ce qui permet à l'identité de contenu
// de rester le verdict sans rien garder en mémoire : une archive refusée n'a
// touché aucun fichier de l'utilisateur (banc d'acceptation, cas 11).
//
// Le dossier d'attente est effacé au retour, y compris en cas d'exception.
// `sink` doit donc déplacer ou copier ce qui l'intéresse pendant l'appel — les
// chemins reçus ne sont plus valides après. C'est volontairement contraignant :
// une API qui rendrait des chemins survivants ferait fuir des fichiers au
// premier oubli.
std::vector<ContentDigest> extractArchiveTo(
    const QString &archivePath, const QString &unitType, const QString &expectedContentSha256,
    const QString &stagingDirectory, const std::function<void(const StagedEntry &)> &sink);

// ── Couche tar, exposée pour être vérifiable seule ────────────────────────
QByteArray createTar(std::vector<ContentFile> files);
std::vector<ContentFile> readTar(const QByteArray &raw);

// ── Archive complète ──────────────────────────────────────────────────────
ArchiveBlob createArchive(const QString &unitType, std::vector<ContentFile> files);

// L'identité de contenu est le VERDICT final : une archive dont le contenu ne
// correspond pas est refusée, même si sa trame zstd est intacte. C'est ce qui
// protège d'un bit retourné côté stockage (banc d'acceptation, cas 11).
std::vector<ContentFile> extractArchive(const QByteArray &archive, const QString &unitType,
                                        const QString &expectedContentSha256);

} // namespace retrosave::core
