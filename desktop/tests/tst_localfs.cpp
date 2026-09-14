// Ces tests portent sur le seul code du projet qui touche aux sauvegardes
// réelles. Ils vérifient moins « ça marche » que « ça ne casse rien quand ça
// se passe mal » : disque menteur, interruption, lien symbolique, rotation.
#include "core/localfs.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using namespace retrosave::core;

namespace
{
void put(const QString &path, const QByteArray &content)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(content), content.size());
}

QByteArray read(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray("<absent>");
}

QString sha(const QByteArray &content)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex());
}

// Prépare un dossier d'attente comme le ferait `extractArchiveTo`.
std::vector<StagedEntry> stage(const QString &directory,
                               const std::vector<std::pair<QString, QByteArray>> &files)
{
    std::vector<StagedEntry> entries;
    int index = 0;
    for (const auto &[relPath, content] : files) {
        const auto staged = QDir(directory).filePath(QString("entry-%1").arg(index++));
        put(staged, content);
        entries.push_back({relPath, staged, sha(content)});
    }
    return entries;
}
} // namespace

class LocalFsTest final : public QObject
{
    Q_OBJECT
  private slots:
    void anAtomicWriteNeverPublishesSomethingElse()
    {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        const auto target = QDir(workspace.path()).filePath("partie.sav");
        put(target, "ancienne");

        QByteArray fresh("nouvelle sauvegarde");
        QBuffer source(&fresh);
        QVERIFY(source.open(QIODevice::ReadOnly));
        writeAtomic(target, source, sha(fresh));
        QCOMPARE(read(target), fresh);

        // Aucun fichier temporaire ne doit survivre à l'opération.
        const auto leftovers = QDir(workspace.path()).entryList({".rsc-tmp-*"}, QDir::Files | QDir::Hidden);
        QVERIFY2(leftovers.isEmpty(), "un fichier temporaire est resté sur le disque");
    }

    void aWriteThatDoesNotMatchIsRefusedAndLeavesTheOriginal()
    {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        const auto target = QDir(workspace.path()).filePath("partie.sav");
        put(target, "précieuse");

        QByteArray fresh("autre chose");
        QBuffer source(&fresh);
        QVERIFY(source.open(QIODevice::ReadOnly));
        bool refused = false;
        try {
            // On annonce une empreinte qui n'est pas celle des octets fournis :
            // c'est ce qui arriverait avec un disque ou une archive menteurs.
            writeAtomic(target, source, sha("encore autre chose"));
        } catch (const LocalFsError &) {
            refused = true;
        }
        QVERIFY(refused);
        // La sauvegarde d'origine est intacte, et rien ne traîne.
        QCOMPARE(read(target), QByteArray("précieuse"));
        QVERIFY(QDir(workspace.path()).entryList({".rsc-tmp-*"}, QDir::Files | QDir::Hidden).isEmpty());
    }

    void backupsRotateOverThreeGenerationsAndStopThere()
    {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        const auto target = QDir(workspace.path()).filePath("partie.sav");

        // Quatre sauvegardes successives : la plus ancienne doit disparaître.
        for (const auto &generation : {"un", "deux", "trois", "quatre"}) {
            put(target, generation);
            backupWithRotation(target);
        }
        QCOMPARE(read(target + ".rsc-bak"), QByteArray("quatre"));
        QCOMPARE(read(target + ".rsc-bak.1"), QByteArray("trois"));
        QCOMPARE(read(target + ".rsc-bak.2"), QByteArray("deux"));
        // « un » est perdu : trois générations, pas quatre. C'est la règle.
        QVERIFY(!QFileInfo::exists(target + ".rsc-bak.3"));
    }

    void backingUpSomethingAbsentIsNotAnError()
    {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        // Première synchronisation d'une unité : il n'y a rien à protéger.
        backupWithRotation(QDir(workspace.path()).filePath("jamais-vue.sav"));
        QVERIFY(!QFileInfo::exists(QDir(workspace.path()).filePath("jamais-vue.sav.rsc-bak")));
    }

    void aFileUnitIsReplacedAfterBeingBackedUp()
    {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        const auto target = QDir(workspace.path()).filePath("zelda.sav");
        put(target, "progression locale");
        QTemporaryDir staging;
        QVERIFY(staging.isValid());

        const auto entries = stage(staging.path(), {{"zelda.sav", "progression du cloud"}});
        const auto report = applyUnit("file", target, entries);
        QCOMPARE(report.filesWritten, 1);
        QVERIFY(report.backedUp);
        QCOMPARE(read(target), QByteArray("progression du cloud"));
        // L'ancienne progression reste récupérable : c'est l'invariant I2.
        QCOMPARE(read(target + ".rsc-bak"), QByteArray("progression locale"));
    }

    void aDirectoryUnitEndsUpExactlyLikeTheArchive()
    {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        const auto target = QDir(workspace.path()).filePath("ULUS10041SAVE");
        put(QDir(target).filePath("SAVE.BIN"), "ancien");
        // Ce fichier n'existe pas dans l'archive : il doit disparaître, sinon
        // l'unité locale ne serait plus celle que le serveur décrit.
        put(QDir(target).filePath("PERIME.DAT"), "à retirer");

        QTemporaryDir staging;
        QVERIFY(staging.isValid());
        const auto entries = stage(staging.path(), {{"SAVE.BIN", "neuf"},
                                                    {"sous/DATA.BIN", "imbriqué"}});
        const auto report = applyUnit("dir", target, entries);
        QCOMPARE(report.filesWritten, 2);
        QCOMPARE(read(QDir(target).filePath("SAVE.BIN")), QByteArray("neuf"));
        QCOMPARE(read(QDir(target).filePath("sous/DATA.BIN")), QByteArray("imbriqué"));
        QVERIFY2(!QFileInfo::exists(QDir(target).filePath("PERIME.DAT")),
                 "un fichier absent de l'archive a survécu au remplacement");
        // Et le dossier d'avant est entièrement récupérable.
        QCOMPARE(read(QDir(target + ".rsc-bak").filePath("PERIME.DAT")), QByteArray("à retirer"));
    }

#ifdef Q_OS_UNIX
    void symbolicLinksAreNeverFollowed()
    {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        const auto outside = QDir(workspace.path()).filePath("precieux-ailleurs.txt");
        put(outside, "ne doit pas bouger");
        const auto link = QDir(workspace.path()).filePath("piege.sav");
        QVERIFY(QFile::link(outside, link));

        bool refusedBackup = false;
        try {
            backupWithRotation(link);
        } catch (const LocalFsError &) {
            refusedBackup = true;
        }
        QVERIFY2(refusedBackup, "un lien symbolique a été sauvegardé");

        bool refusedRemoval = false;
        try {
            removeTarget(link);
        } catch (const LocalFsError &) {
            refusedRemoval = true;
        }
        QVERIFY2(refusedRemoval, "un lien symbolique a été supprimé");
        // Le fichier visé par le lien n'a pas été touché.
        QCOMPARE(read(outside), QByteArray("ne doit pas bouger"));
    }
#endif
};

QTEST_GUILESS_MAIN(LocalFsTest)
#include "tst_localfs.moc"
