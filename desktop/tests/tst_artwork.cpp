// L'image choisie par l'utilisateur est purement décorative — mais son
// RANGEMENT ne l'est pas : si deux sauvegardes différentes se retrouvaient sur
// le même fichier, personnaliser l'une changerait l'autre, et le joueur verrait
// la mauvaise vignette sur une sauvegarde qu'il n'a pas touchée.
#include "agent/configuration.h"
#include "core/gameart.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

using namespace retrosave::agent;

class ArtworkTest final : public QObject
{
    Q_OBJECT
  private:
    QTemporaryDir m_home;

  private slots:
    void initTestCase()
    {
        QVERIFY(m_home.isValid());
        // On déplace les emplacements standards : ce test ne doit rien écrire
        // dans les données réelles du poste.
        QStandardPaths::setTestModeEnabled(true);
        qputenv("XDG_DATA_HOME", m_home.path().toUtf8());
    }

    void twoDifferentUnitsNeverShareAnImage()
    {
        const auto first = customArtworkPath("ppsspp", "ULUS10041GAMEDATA");
        const auto second = customArtworkPath("ppsspp", "ULES00250GAMEDATA");
        QVERIFY(first != second);

        // Même clé, émulateur différent : ce sont deux unités distinctes, et
        // rien ne dit qu'elles représentent le même jeu.
        QVERIFY(customArtworkPath("melonds", "SAVE.SAV")
                != customArtworkPath("mgba", "SAVE.SAV"));
    }

    void theSameUnitAlwaysFindsItsOwnImage()
    {
        // Stable d'un lancement à l'autre : c'est ce qui fait qu'un choix
        // survit au redémarrage sans qu'on ait à l'inscrire dans le carnet.
        QCOMPARE(customArtworkPath("ppsspp", "ULUS10041GAMEDATA"),
                 customArtworkPath("ppsspp", "ULUS10041GAMEDATA"));
    }

    void theImageLivesBesideTheStateNotInTheCache()
    {
        // Un cache se vide : une décision d'utilisateur, non. Le chemin doit
        // donc être sous les DONNÉES, jamais sous le cache.
        const auto path = customArtworkPath("ppsspp", "ULUS10041GAMEDATA");
        QVERIFY2(!path.contains("/cache/"), qPrintable(path));
        QVERIFY2(path.contains("artwork"), qPrintable(path));
    }

    void onlyARealPngIsAccepted()
    {
        // La même validation que pour l'ICON0.PNG d'une sauvegarde : en-tête
        // lu, jamais d'image décodée. Un `.sav` renommé ne passe pas.
        QVERIFY(!retrosave::core::isValidIcon(QByteArray("ceci n'est pas une image")));
        QVERIFY(!retrosave::core::isValidIcon(QByteArray()));
    }
};

QTEST_MAIN(ArtworkTest)
#include "tst_artwork.moc"
