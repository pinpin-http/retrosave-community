// Tests du contrat d'archive. Ils vérifient ce que le contrat exige
// vraiment : déterminisme DE CETTE implémentation, structure tar conforme au
// §5.3, et refus de toute archive dont le contenu ne correspond pas.
// L'égalité binaire avec Python n'est PAS testée ici : la doctrine Q10/Q25
// ne l'exige pas et deux versions de libzstd ne la donneraient pas. La
// compatibilité réelle est prouvée par scripts/testing/test_archive_interop.py.
#include "core/archive.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using namespace retrosave::core;

namespace
{
QString stagedFileName(const QString &relPath)
{
    return QString(relPath).replace('/', '_');
}

std::vector<ContentFile> sampleUnit()
{
    // Un nom non ASCII : c'est lui qui force un en-tête étendu PAX, donc le
    // chemin de code que l'on oublierait le plus facilement de tester.
    return {{"b.dat", QByteArray("beta")}, {"a/é.dat", QByteArray("été")}};
}
#ifdef Q_OS_LINUX
// Pic de mémoire résidente du processus depuis son démarrage, en kibioctets.
// Cette valeur ne redescend jamais : on ne peut mesurer qu'une PROGRESSION, et
// seulement en exécutant le cas économe avant le cas gourmand.
qint64 peakMemoryKiB()
{
    QFile status("/proc/self/status");
    if (!status.open(QIODevice::ReadOnly))
        return -1;
    for (const auto &line : status.readAll().split('\n')) {
        if (line.startsWith("VmHWM:"))
            return line.mid(6).trimmed().split(' ').first().toLongLong();
    }
    return -1;
}

// Écrit un fichier de `megabytes` Mio par tranches, pour que la FABRICATION
// du cas de test ne fausse pas elle-même la mesure.
void writeLargeFile(const QString &path, int megabytes)
{
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QByteArray chunk(1024 * 1024, '\0');
    for (int i = 0; i < chunk.size(); ++i)
        chunk[i] = static_cast<char>(i * 31 + 7);
    for (int i = 0; i < megabytes; ++i)
        QCOMPARE(file.write(chunk), chunk.size());
    file.close();
}
#endif
} // namespace

class ArchiveTest final : public QObject
{
    Q_OBJECT
  private slots:
    // ── Ce cas doit rester le PREMIER du fichier ──────────────────────────
    // Le pic mémoire ne redescend jamais : il ne se mesure qu'en progression,
    // donc l'ordre de déclaration porte du sens. Un cas gourmand exécuté avant
    // rendrait celui-ci vert sans rien prouver.
    void streamingMemoryDoesNotGrowWithTheUnit()
    {
#ifndef Q_OS_LINUX
        QSKIP("La mesure du pic mémoire lit /proc, propre à Linux.");
#else
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());

        // On ne teste PAS un seuil absolu. zstd alloue son propre plan de
        // travail — mesuré à ~25 Mio au niveau 10 — et ce coût est le même
        // pour 8 Mio que pour 8 Gio. Fixer un plafond en mégaoctets casserait
        // au premier changement de version de la bibliothèque, sans rien dire
        // de notre code.
        //
        // Ce qui se teste, c'est la PROPRIÉTÉ qui nous intéresse : la mémoire
        // ne doit pas dépendre de la taille de l'unité.
        const auto archiveOf = [&](const QString &name, int megabytes) {
            const auto sourcePath = QDir(workspace.path()).filePath(name + ".sav");
            writeLargeFile(sourcePath, megabytes);
            std::vector<ArchiveSource> sources{
                {name + ".sav", qint64(megabytes) * 1024 * 1024, [sourcePath] {
                     auto file = std::make_unique<QFile>(sourcePath);
                     file->open(QIODevice::ReadOnly);
                     return file;
                 }}};
            return createArchiveFrom("dir", std::move(sources),
                                     QDir(workspace.path()).filePath(name + ".tar.zst"));
        };

        const auto start = peakMemoryKiB();
        QVERIFY(start > 0);
        archiveOf("petite", 8);
        const auto afterSmall = peakMemoryKiB();
        archiveOf("grande", 64);
        const auto afterLarge = peakMemoryKiB();

        // Huit fois plus de données ne doivent pas coûter un mégaoctet de plus.
        const auto growth = afterLarge - afterSmall;
        QVERIFY2(growth < 4 * 1024,
                 qPrintable(QString("passer de 8 à 64 Mio a coûté %1 Mio de mémoire en plus")
                                .arg(growth / 1024)));

        // Contre-épreuve : le chemin tamponné, lui, paie la taille de l'unité.
        // Sans elle, un instrument cassé rendrait l'assertion ci-dessus verte
        // pour de mauvaises raisons.
        QFile source(QDir(workspace.path()).filePath("grande.sav"));
        QVERIFY(source.open(QIODevice::ReadOnly));
        const auto blob = createArchive("dir", {{"grande.sav", source.readAll()}});
        source.close();
        QVERIFY2(peakMemoryKiB() - afterLarge > 32 * 1024,
                 "la mesure ne distingue pas les deux chemins : l'instrument est en cause");
        QVERIFY(!blob.contentSha256.isEmpty());
#endif
    }

    void tarLayoutFollowsTheFrozenFormat()
    {
        const auto raw = createTar(sampleUnit());
        // Remplissage jusqu'au multiple de 10 240 et deux blocs nuls finaux.
        QCOMPARE(raw.size() % TarRecordSize, 0);
        QCOMPARE(raw.right(2 * TarBlockSize), QByteArray(2 * TarBlockSize, '\0'));
        // Le tri se fait sur les octets UTF-8 : « a/… » avant « b.dat ».
        QVERIFY(raw.indexOf("a/?.dat") < raw.indexOf("b.dat"));
        // Un en-tête étendu précède l'entrée au nom non ASCII, et le vrai nom
        // y voyage en UTF-8 même si le champ historique porte un repli.
        QVERIFY(raw.contains("././@PaxHeader"));
        QVERIFY(raw.contains(QString("path=a/é.dat").toUtf8()));
        // Aucun nom d'utilisateur ne doit fuir dans une archive.
        QVERIFY(!raw.contains(qgetenv("USER")) || qgetenv("USER").isEmpty());
    }

    void archivingTwiceProducesTheSameBytes()
    {
        // C'est LE critère du contrat : déterminisme par implémentation.
        const auto first = createArchive("dir", sampleUnit());
        const auto second = createArchive("dir", sampleUnit());
        QCOMPARE(first.bytes, second.bytes);
        QCOMPARE(first.archiveSha256, second.archiveSha256);
        // L'ordre d'entrée ne doit rien changer : le tri est interne.
        auto reversed = sampleUnit();
        std::reverse(reversed.begin(), reversed.end());
        QCOMPARE(createArchive("dir", reversed).bytes, first.bytes);
    }

    void roundTripPreservesEveryEntry()
    {
        const auto blob = createArchive("dir", sampleUnit());
        const auto files = extractArchive(blob.bytes, "dir", blob.contentSha256);
        QCOMPARE(files.size(), size_t(2));
        // readTar rend les entrées dans l'ordre de l'archive, donc trié.
        QCOMPARE(files[0].relPath, QString("a/é.dat"));
        QCOMPARE(files[0].content, QByteArray("été"));
        QCOMPARE(files[1].relPath, QString("b.dat"));
        QCOMPARE(files[1].content, QByteArray("beta"));
        QCOMPARE(contentSha256("dir", files), blob.contentSha256);
    }

    void contentIdentityIsComputedOnContentNotOnTheArchive()
    {
        // Même contenu, même identité — quel que soit l'emballage.
        const auto blob = createArchive("dir", sampleUnit());
        QCOMPARE(blob.contentSha256, contentSha256("dir", sampleUnit()));
        QVERIFY(blob.contentSha256 != blob.archiveSha256);
        QCOMPARE(blob.sizeBytes, qint64(QByteArray("beta").size() + QByteArray("été").size()));
    }

    void aFileUnitIsOneFileWithoutItsName()
    {
        const std::vector<ContentFile> unit{{"partie.sav", QByteArray("hello")}};
        const auto blob = createArchive("file", unit);
        // Identité d'une unité-fichier = sha256 du contenu brut : renommer la
        // sauvegarde ne doit pas en faire un autre contenu.
        QCOMPARE(blob.contentSha256,
                 QString("2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824"));
        const auto files = extractArchive(blob.bytes, "file", blob.contentSha256);
        QCOMPARE(files.size(), size_t(1));
        QCOMPARE(files[0].content, QByteArray("hello"));
    }

    void aCorruptedArchiveIsRefused()
    {
        auto blob = createArchive("dir", sampleUnit());
        // Un bit retourné au milieu de la trame compressée (banc, cas 11).
        blob.bytes[blob.bytes.size() / 2] = blob.bytes.at(blob.bytes.size() / 2) ^ 0x01;
        bool refused = false;
        try {
            extractArchive(blob.bytes, "dir", blob.contentSha256);
        } catch (const ArchiveError &) {
            refused = true;
        }
        QVERIFY2(refused, "une archive corrompue doit être refusée");
    }

    void anArchiveOfTheWrongContentIsRefused()
    {
        const auto blob = createArchive("dir", sampleUnit());
        bool refused = false;
        try {
            // Trame parfaitement valide, mais ce n'est pas le contenu attendu.
            extractArchive(blob.bytes, "dir",
                           QString("0000000000000000000000000000000000000000000000000000000000000000"));
        } catch (const ArchiveError &) {
            refused = true;
        }
        QVERIFY2(refused, "l'identité de contenu est le verdict final");
    }

    void aTamperedTarHeaderIsRefused()
    {
        auto raw = createTar(sampleUnit());
        // On agrandit une taille annoncée sans toucher au reste : la somme de
        // contrôle de l'en-tête ne colle plus.
        raw[124 + 10] = '7';
        bool refused = false;
        try {
            readTar(raw);
        } catch (const ArchiveError &) {
            refused = true;
        }
        QVERIFY2(refused, "un en-tête tar modifié doit être refusé");
    }

    void bothPathsProduceArchivesTheOtherCanRead()
    {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        std::vector<ArchiveSource> sources;
        for (const auto &file : sampleUnit()) {
            // Le nom du fichier source ne dit rien de son nom dans l'archive :
            // le dossier de travail reste plat, l'archive garde la hiérarchie.
            const auto path = QDir(workspace.path()).filePath(stagedFileName(file.relPath));
            QFile handle(path);
            QVERIFY(handle.open(QIODevice::WriteOnly));
            handle.write(file.content);
            handle.close();
            sources.push_back({file.relPath, qint64(file.content.size()), [path] {
                                   auto opened = std::make_unique<QFile>(path);
                                   opened->open(QIODevice::ReadOnly);
                                   return opened;
                               }});
        }
        const auto archivePath = QDir(workspace.path()).filePath("flux.tar.zst");
        const auto info = createArchiveFrom("dir", sources, archivePath);
        const auto blob = createArchive("dir", sampleUnit());

        // Même identité de contenu par les deux chemins.
        QCOMPARE(info.contentSha256, blob.contentSha256);

        // Le lecteur tamponné lit l'archive écrite en flux.
        QFile written(archivePath);
        QVERIFY(written.open(QIODevice::ReadOnly));
        const auto files = extractArchive(written.readAll(), "dir", info.contentSha256);
        written.close();
        QCOMPARE(files.size(), size_t(2));

        // Et le lecteur en flux lit l'archive écrite en mémoire.
        const auto bufferedPath = QDir(workspace.path()).filePath("tampon.tar.zst");
        QFile bufferedFile(bufferedPath);
        QVERIFY(bufferedFile.open(QIODevice::WriteOnly));
        bufferedFile.write(blob.bytes);
        bufferedFile.close();
        std::vector<QString> seenNames;
        std::vector<QByteArray> seenContents;
        extractArchiveTo(bufferedPath, "dir", blob.contentSha256, workspace.path(),
                         [&](const StagedEntry &entry) {
                             seenNames.push_back(entry.relPath);
                             QFile staged(entry.stagedPath);
                             if (staged.open(QIODevice::ReadOnly))
                                 seenContents.push_back(staged.readAll());
                         });
        QCOMPARE(seenNames.size(), size_t(2));
        QCOMPARE(seenNames[0], QString("a/é.dat"));
        QCOMPARE(seenContents[0], QByteArray("été"));
    }

    void aSourceThatChangesDuringCaptureIsRefused()
    {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        const auto path = QDir(workspace.path()).filePath("bouge.sav");
        QFile handle(path);
        QVERIFY(handle.open(QIODevice::WriteOnly));
        handle.write("trois octets de plus que prevu");
        handle.close();

        // On annonce une taille, on en livre une autre : c'est exactement ce
        // qui arrive si l'émulateur écrit pendant la capture. L'en-tête tar
        // est déjà parti avec l'ancienne taille : l'archive est irrécupérable.
        std::vector<ArchiveSource> sources{{"bouge.sav", 4, [path] {
                                                auto file = std::make_unique<QFile>(path);
                                                file->open(QIODevice::ReadOnly);
                                                return file;
                                            }}};
        bool refused = false;
        try {
            createArchiveFrom("dir", sources, QDir(workspace.path()).filePath("out.tar.zst"));
        } catch (const ArchiveError &) {
            refused = true;
        }
        QVERIFY2(refused, "une source qui change pendant la capture doit être refusée");
    }

    void aRefusedArchiveTouchesNothingAndLeavesNothing()
    {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        const auto archivePath = QDir(workspace.path()).filePath("a.tar.zst");
        QFile file(archivePath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(createArchive("dir", sampleUnit()).bytes);
        file.close();

        bool sinkCalled = false;
        bool refused = false;
        try {
            extractArchiveTo(
                archivePath, "dir",
                QString("0000000000000000000000000000000000000000000000000000000000000000"),
                workspace.path(), [&](const StagedEntry &) { sinkCalled = true; });
        } catch (const ArchiveError &) {
            refused = true;
        }
        QVERIFY(refused);
        // Rien n'a été remis à l'appelant : le verdict tombe avant.
        QVERIFY2(!sinkCalled, "aucune entrée ne doit être livrée si le contenu est refusé");
        // Et le dossier d'attente est nettoyé même après l'exception.
        const auto leftovers =
            QDir(workspace.path()).entryList({".rsc-stage-*"}, QDir::Dirs | QDir::Hidden);
        QVERIFY2(leftovers.isEmpty(), "le dossier d'attente doit disparaître");
    }

    void unsafeEntryNamesAreRefused()
    {
        const auto refused = [](const QString &name) {
            try {
                createTar({{name, QByteArray("x")}});
                return false;
            } catch (const ArchiveError &) {
                return true;
            }
        };
        // Un « .. » dans une archive est le classique zip-slip : à l'extraction
        // il ferait écrire hors du dossier de l'unité.
        QVERIFY(refused("../ailleurs.sav"));
        QVERIFY(refused("/absolu.sav"));
        QVERIFY(refused("dossier\\fichier.sav"));
        QVERIFY(refused(""));
    }
};

QTEST_GUILESS_MAIN(ArchiveTest)
#include "tst_archive.moc"
