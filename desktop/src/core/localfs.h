// C'est le fichier le plus dangereux du projet : c'est le seul qui touche aux
// sauvegardes réelles. Trois règles y sont absolues (invariant I2).
//
// 1. **Rien n'est remplacé sans copie de sécurité.** `<nom>.rsc-bak`, sur
//    trois générations, avant toute écriture.
// 2. **Aucune écriture partielle n'est visible.** On écrit à côté, on relit,
//    puis on publie d'un seul geste par un renommage atomique.
// 3. **Ce qui compte est ce que le disque rend**, pas ce qu'on croit y avoir
//    mis. Le fichier temporaire est relu avant publication : un secteur qui
//    ment ne devient jamais la sauvegarde de quelqu'un.
//
// Les tests partagés imposent ces mêmes garanties à chaque moteur.
#pragma once

#include "core/archive.h"

#include <QIODevice>
#include <QString>
#include <stdexcept>
#include <vector>

namespace retrosave::core
{

class LocalFsError : public std::runtime_error
{
  public:
    using std::runtime_error::runtime_error;
};

// Écrit le contenu de `source` à `targetPath`. L'écriture passe par un fichier
// temporaire voisin, est relue, puis publiée par renommage. En cas d'échec, la
// cible d'origine est intacte et le temporaire est effacé.
void writeAtomic(const QString &targetPath, QIODevice &source, const QString &expectedSha256);

// Copie `path` en `<nom>.rsc-bak`, en décalant les générations précédentes.
// Fonctionne pour un fichier comme pour un dossier. Ne fait rien si la cible
// n'existe pas : il n'y a alors rien à protéger.
void backupWithRotation(const QString &path);

// Supprime un fichier ou un dossier. Refuse un lien symbolique : le suivre
// détruirait quelque chose hors du périmètre désigné par l'utilisateur.
void removeTarget(const QString &path);

// Liste les fichiers d'une unité, triés par octets UTF-8, avec taille et date
// de modification. Ne lit AUCUN contenu : c'est ce qui rend un scan périodique
// supportable sur trois cents unités.
std::vector<NodeMeta> listUnitFiles(const QString &unitType, const QString &targetPath);

// Les mêmes fichiers, prêts à être archivés en flux. Chaque source n'est
// ouverte qu'au moment où l'archive la lit.
std::vector<ArchiveSource> unitSources(const QString &unitType, const QString &targetPath);

struct ApplyReport {
    int filesWritten = 0;
    bool backedUp = false;
};

// Remplace une unité par le contenu déjà validé d'une archive.
//
// L'appelant a DÉJÀ rendu le verdict de contenu (`extractArchiveTo` l'a fait) :
// cette fonction n'a plus rien à juger, elle écrit. Elle n'est jamais appelée
// sur une archive douteuse.
//
// Pour une unité-dossier, le dossier cible est remplacé dans son ensemble. Un
// arrêt brutal au milieu laisse donc un dossier incomplet : c'est le journal
// d'application (AD-27) qui, à la passe suivante, le fait ré-appliquer. C'est
// pour cela que ce journal doit être écrit AVANT d'appeler ici.
ApplyReport applyUnit(const QString &unitType, const QString &targetPath,
                      const std::vector<StagedEntry> &entries);

} // namespace retrosave::core
