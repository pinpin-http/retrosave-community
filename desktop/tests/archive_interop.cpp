// Point d'échange C++ du test d'interopérabilité piloté par Python
// (scripts/testing/test_archive_interop.py). Même contrat que le test Kotlin :
// deux chemins de fichiers, l'un à lire, l'autre à écrire.
#include "core/archive.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <cstdio>

using namespace retrosave::core;

int main(int argc, char **argv)
{
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <archive-python-a-lire> <archive-cpp-a-ecrire>\n", argv[0]);
        return 2;
    }
    // On réutilise le vecteur de contenu partagé plutôt qu'une fixture binaire
    // commitée : Q10 interdit les secondes, et le vecteur est déjà la
    // référence commune aux trois langages.
    QFile vectors(QStringLiteral(RETROSAVE_VECTORS_DIR) + "/content_hash_basic.json");
    if (!vectors.open(QIODevice::ReadOnly)) {
        std::fprintf(stderr, "vecteurs introuvables\n");
        return 3;
    }
    std::vector<ContentFile> files;
    for (const auto value : QJsonDocument::fromJson(vectors.readAll()).array()) {
        const auto input = value.toObject().value("input").toObject();
        if (input.value("unit_type").toString() != "dir" || input.value("files").toArray().isEmpty())
            continue;
        for (const auto entry : input.value("files").toArray()) {
            const auto file = entry.toObject();
            files.push_back(
                {file.value("rel_path").toString(),
                 QByteArray::fromBase64(file.value("content_base64").toString().toLatin1())});
        }
        break;
    }
    if (files.empty()) {
        std::fprintf(stderr, "aucun cas dossier dans le vecteur partagé\n");
        return 3;
    }

    try {
        const auto expected = contentSha256("dir", files);

        QFile incoming(QString::fromLocal8Bit(argv[1]));
        if (!incoming.open(QIODevice::ReadOnly)) {
            std::fprintf(stderr, "archive Python illisible\n");
            return 3;
        }
        const auto extracted = extractArchive(incoming.readAll(), "dir", expected);
        if (contentSha256("dir", extracted) != expected) {
            std::fprintf(stderr, "l'archive Python ne redonne pas la même identité\n");
            return 1;
        }

        // On écrit par le chemin EN FLUX, pas par le chemin tamponné : c'est
        // celui que la production utilisera, donc le seul qu'il soit utile de
        // soumettre à l'autre implémentation.
        QTemporaryDir workspace;
        if (!workspace.isValid()) {
            std::fprintf(stderr, "dossier de travail indisponible\n");
            return 3;
        }
        std::vector<ArchiveSource> sources;
        for (const auto &file : files) {
            const auto path =
                QDir(workspace.path()).filePath(QString(file.relPath).replace('/', '_'));
            QFile handle(path);
            if (!handle.open(QIODevice::WriteOnly)) {
                std::fprintf(stderr, "source temporaire non écrivable\n");
                return 3;
            }
            handle.write(file.content);
            handle.close();
            sources.push_back({file.relPath, static_cast<qint64>(file.content.size()), [path] {
                                   auto opened = std::make_unique<QFile>(path);
                                   opened->open(QIODevice::ReadOnly);
                                   return opened;
                               }});
        }
        const auto info = createArchiveFrom("dir", std::move(sources),
                                            QString::fromLocal8Bit(argv[2]));
        if (info.contentSha256 != expected) {
            std::fprintf(stderr, "le producteur en flux diverge sur l'identité de contenu\n");
            return 1;
        }
        std::printf("archive interop C++ : lecture Python OK, archive C++ écrite (chemin en flux)\n");
        return 0;
    } catch (const ArchiveError &error) {
        std::fprintf(stderr, "archive refusée : %s\n", error.what());
        return 1;
    }
}
