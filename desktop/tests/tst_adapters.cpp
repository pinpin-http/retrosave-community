// Ces tests utilisent les VRAIS manifestes du dépôt et les VRAIES fixtures.
// C'est ce qui donne son sens à la promesse « ajouter un émulateur = déposer
// un TOML » : si le moteur générique retrouve les quatre adaptateurs existants
// sans une ligne de code par émulateur, la promesse tient.
#include "adapters/registry.h"

#include <QDir>
#include <QTemporaryDir>
#include <QTest>

using namespace retrosave::adapters;

namespace
{
QString adaptersDir()
{
    return QStringLiteral(RETROSAVE_ADAPTERS_DIR);
}
QString fixture(const QString &relative)
{
    return QDir(QStringLiteral(RETROSAVE_ADAPTERS_DIR) + "/fixtures").filePath(relative);
}

std::vector<DiscoveredUnit> discoverWith(const QString &id, const QString &root)
{
    AdapterRegistry registry;
    registry.loadDirectory(adaptersDir());
    const auto *adapter = registry.adapter(id);
    return adapter == nullptr ? std::vector<DiscoveredUnit>{} : adapter->discover(root);
}
} // namespace

class AdaptersTest final : public QObject
{
    Q_OBJECT
  private slots:
    void everyRealManifestLoadsWithoutCode()
    {
        AdapterRegistry registry;
        registry.loadDirectory(adaptersDir());
        QVERIFY2(registry.problems().isEmpty(), qPrintable(registry.problems().join(" | ")));
        QCOMPARE(registry.ids(), QStringList({"azahar", "dolphin", "duckstation", "melonds", "mgba",
                                              "pcsx2", "ppsspp", "retroarch", "snes9x"}));
        // Aucune fabrique n'a été enregistrée : les quatre sont servis par le
        // même adaptateur générique, piloté par leur seul manifeste.
        for (const auto &id : registry.ids())
            QVERIFY(dynamic_cast<const ManifestAdapter *>(registry.adapter(id)) != nullptr);
    }

    void ppssppSeesEachSavedataFolder()
    {
        const auto units = discoverWith("ppsspp", fixture("linux/ppsspp/PSP/SAVEDATA"));
        QCOMPARE(units.size(), size_t(2));
        QCOMPARE(units[0].unitKey, QString("NPJH50043DATA"));
        QCOMPARE(units[0].unitType, QString("dir"));
        // game_key = « serial9 » : les neuf premiers caractères regroupent les
        // sauvegardes d'un même jeu.
        QCOMPARE(units[0].gameKey, QString("NPJH50043"));
        QCOMPARE(units[1].unitKey, QString("ULUS10041SAVE"));
        QCOMPARE(units[1].gameKey, QString("ULUS10041"));
    }

    void ppssppIgnoresTheEmulatorsOwnFoldersIfPointedOneLevelUp()
    {
        // Un utilisateur peut désigner `PSP/` au lieu de `PSP/SAVEDATA`. Sans
        // les exclusions du manifeste, GAME (des exécutables homebrew, interdits
        // par I1), SYSTEM (la configuration) et PPSSPP_STATE (des états de
        // sauvegarde) deviendraient des unités à synchroniser.
        const auto units = discoverWith("ppsspp", fixture("linux/ppsspp/PSP"));
        QStringList keys;
        for (const auto &unit : units)
            keys << unit.unitKey;
        QCOMPARE(keys, QStringList({"SAVEDATA"}));
    }

    void melondsSeesSavesButNeitherStatesNorRoms()
    {
        const auto units = discoverWith("melonds", fixture("linux/melonds"));
        QStringList keys;
        for (const auto &unit : units)
            keys << unit.unitKey;
        keys.sort();
        QCOMPARE(keys, QStringList({"Pokémon_Édition_Noire_(France).sav", "orphan.sav"}));
        // La ROM voisine ne doit jamais apparaître (invariant I1), et l'état
        // de sauvegarde `.ml0` est exclu par le manifeste.
        for (const auto &key : keys) {
            QVERIFY(!key.endsWith(".nds"));
            QVERIFY(!key.contains(".ml"));
        }
        // game_key = « normalized_name » : le nom perd son extension et ses
        // balises de région, pour regrouper les copies d'un même jeu.
        for (const auto &unit : units) {
            if (unit.unitKey.startsWith("Pokémon"))
                QCOMPARE(unit.gameKey, QString("pokémon édition noire"));
        }
    }

    void azaharSeesTheNestedDataFolderOnly()
    {
        const auto units = discoverWith("azahar", fixture("linux/azahar"));
        QCOMPARE(units.size(), size_t(1));
        // C'est la preuve que « path_segment:2 » et « prefix:3ds: » sont
        // réellement interprétés : le motif capture id0, id1 puis le title ID,
        // et c'est le troisième qui identifie l'unité.
        QCOMPARE(units[0].unitKey, QString("00abcdef"));
        QCOMPARE(units[0].gameKey, QString("3ds:00abcdef"));
        QVERIFY(units[0].problem.isEmpty());
        // `extdata`, voisin de `data`, n'est pas couvert : le motif
        // s'arrête sur `data` et rien d'autre ne doit passer.
        QVERIFY(units[0].relPath.endsWith("/data"));
    }

    // Chacun est vérifié sur ce qu'il doit voir ET sur ce qu'il doit ignorer :
    // un connecteur qui ramasse trop est plus dangereux qu'un connecteur qui
    // ramasse trop peu — il ferait remonter des fichiers que personne n'a
    // demandé de synchroniser.

    void dolphinSeesEachMemoryCardSaveAndIgnoresTheRawCard()
    {
        const auto units = discoverWith("dolphin", fixture("linux/dolphin"));
        QStringList keys;
        for (const auto &unit : units)
            keys << unit.unitKey;
        keys.sort();
        QCOMPARE(keys, QStringList({"01-GALE-SuperSmashBros.gci", "01-GM4E-MetroidPrime.gci",
                                    "01-GZLP-ZeldaWindWaker.gci"}));
        // L'image brute de la carte n'est pas une unité : elle contient les
        // mêmes sauvegardes, et les versionner deux fois créerait des conflits
        // entre deux représentations du même contenu.
        for (const auto &unit : units)
            QVERIFY(!unit.unitKey.endsWith(".raw"));
        // Rien du dossier de configuration ne remonte.
        for (const auto &unit : units)
            QVERIFY(!unit.relPath.startsWith("Config"));
    }

    void duckstationSeesPerGameAndSharedCards()
    {
        const auto units = discoverWith("duckstation", fixture("linux/duckstation"));
        QCOMPARE(units.size(), size_t(2));
        QStringList keys;
        for (const auto &unit : units)
            keys << unit.unitKey;
        keys.sort();
        // Une carte partagée est une unité comme une autre : elle porte
        // plusieurs jeux, et c'est le fichier entier qui est versionné.
        QCOMPARE(keys, QStringList({"SLUS-00594_FinalFantasyVII.mcd", "shared_card_1.mcd"}));
    }

    void pcsx2SeesMemoryCardsButNeverSaveStates()
    {
        const auto units = discoverWith("pcsx2", fixture("linux/pcsx2"));
        QCOMPARE(units.size(), size_t(2));
        for (const auto &unit : units) {
            QVERIFY(unit.unitKey.endsWith(".ps2"));
            // Un `.p2s` est un instantané de RAM : des dizaines de mégaoctets,
            // périmés au prochain changement de version de l'émulateur.
            QVERIFY(!unit.unitKey.endsWith(".p2s"));
        }
    }

    void mgbaSeesSavesNextToRomsWithoutTouchingThem()
    {
        const auto units = discoverWith("mgba", fixture("linux/mgba"));
        QCOMPARE(units.size(), size_t(2));
        for (const auto &unit : units) {
            QVERIFY(unit.unitKey.endsWith(".sav"));
            // Invariant I1 : la ROM voisine n'est jamais une unité.
            QVERIFY(!unit.unitKey.endsWith(".gba"));
            QVERIFY(!unit.unitKey.endsWith(".ss1"));
        }
        // Deux sauvegardes de jeux différents ne partagent pas de clé de jeu.
        QVERIFY(units[0].gameKey != units[1].gameKey);
    }

    void snes9xSeesOnlyBatterySavesAmongEverythingElse()
    {
        // Le dossier de ROMs d'un joueur Snes9x contient de tout : la
        // sauvegarde de pile, la ROM, les états, l'horloge, les triches et les
        // correctifs. Une seule de ces familles est une progression de jeu.
        const auto units = discoverWith("snes9x", fixture("linux/snes9x/roms"));
        QStringList keys;
        for (const auto &unit : units)
            keys << unit.unitKey;
        keys.sort();
        QCOMPARE(keys, QStringList({"Chrono Trigger (U).srm", "Super Metroid.srm"}));
        for (const auto &unit : units) {
            // Invariant I1 : aucune ROM ne devient une unité, jamais.
            QVERIFY(!unit.unitKey.endsWith(".sfc"));
            QVERIFY(!unit.unitKey.endsWith(".smc"));
        }
        // Deux jeux différents ne partagent pas de clé de jeu : un conflit sur
        // l'un ne doit pas laisser croire que l'autre est concerné.
        QVERIFY(units[0].gameKey != units[1].gameKey);
    }

    void aFileUnitIsLabelledWithoutItsExtension()
    {
        // La clé conserve le nom exact pour l'appariement ; le libellé retire
        // l'extension afin de rester lisible et exploitable par la recherche.
        const auto units = discoverWith("snes9x", fixture("linux/snes9x/roms"));
        QStringList labels;
        for (const auto &unit : units) {
            labels << unit.gameLabel;
            // La clé ne bouge pas : la renommer casserait l'appariement entre
            // appareils déjà synchronisés.
            QVERIFY(unit.unitKey.endsWith(".srm"));
        }
        labels.sort();
        QCOMPARE(labels, QStringList({"Chrono Trigger", "Super Metroid"}));
    }

    void aFolderUnitKeepsItsKeyAsLabel()
    {
        // Un dossier PSP n'est pas un nom de fichier : « ULUS10041GAMEDATA »
        // n'a pas d'extension à retirer, et le vrai titre viendra du serveur ou
        // du PARAM.SFO. Passer ce nom dans la règle d'affichage ne doit rien
        // lui faire perdre.
        const auto units = discoverWith("ppsspp", fixture("linux/ppsspp/PSP/SAVEDATA"));
        QVERIFY(!units.empty());
        for (const auto &unit : units)
            QCOMPARE(unit.gameLabel, unit.unitKey);
    }

    void retroarchAcceptsAnUppercaseExtension()
    {
        const auto units = discoverWith("retroarch", fixture("linux/retroarch/saves"));
        QStringList keys;
        for (const auto &unit : units)
            keys << unit.unitKey;
        keys.sort();
        // `notes.txt` n'est pas une sauvegarde ; `.SRM` en est une, comme sous
        // Windows où la casse ne distingue rien.
        QCOMPARE(keys, QStringList({"Pokemon Emerald (U).srm", "Sonic The Hedgehog 2.SRM"}));
    }

    void aManifestThatSaysSomethingUnknownIsRefused()
    {
        const auto base = QByteArray(
            "id = \"x\"\nname = \"X\"\n[discovery]\nunit_type = \"file\"\npattern = \"*.sav\"\n"
            "max_depth = 2\n[identity]\nunit_key = \"filename\"\ngame_key = \"normalized_name\"\n");
        // Le manifeste de référence doit passer, sinon les refus ci-dessous ne
        // prouveraient rien.
        QCOMPARE(parseManifest(base).id, QString("x"));

        const auto refused = [](const QByteArray &toml) {
            try {
                parseManifest(toml);
                return false;
            } catch (const ManifestError &) {
                return true;
            }
        };
        // Une clé mal orthographiée doit être un refus, jamais un silence :
        // sinon une exclusion oubliée laisserait passer une ROM.
        QVERIFY2(refused(base + "exclud = [\"a\"]\n"), "clé inconnue acceptée");
        QVERIFY2(refused(QByteArray(base).replace("\"filename\"", "\"inventé\"")),
                 "règle unit_key inconnue acceptée");
        QVERIFY2(refused(QByteArray(base).replace("\"normalized_name\"", "\"inventé\"")),
                 "règle game_key inconnue acceptée");
        QVERIFY2(refused(QByteArray(base).replace("max_depth = 2", "max_depth = 0")),
                 "profondeur nulle acceptée");
        QVERIFY2(refused(QByteArray(base).replace("unit_type = \"file\"", "unit_type = \"lien\"")),
                 "type d'unité inconnu accepté");
        QVERIFY2(refused(base + "[inconnue]\nx = \"y\"\n"), "table inconnue acceptée");
    }

    void aRomExtensionIsNeverAUnitEvenIfTheManifestAsksForIt()
    {
        // Un manifeste ne doit PAS pouvoir lever l'invariant I1. On en écrit un
        // qui réclame explicitement des ROMs, et le moteur doit refuser.
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        QFile manifest(QDir(folder.path()).filePath("gourmand.toml"));
        QVERIFY(manifest.open(QIODevice::WriteOnly));
        manifest.write("id = \"gourmand\"\nname = \"Gourmand\"\n[discovery]\n"
                       "unit_type = \"file\"\npattern = \"**/*.nds\"\nmax_depth = 3\n"
                       "[identity]\nunit_key = \"filename\"\ngame_key = \"normalized_name\"\n");
        manifest.close();

        AdapterRegistry registry;
        registry.loadDirectory(folder.path());
        QVERIFY(registry.problems().isEmpty());
        const auto units = registry.adapter("gourmand")->discover(fixture("linux/melonds"));
        QVERIFY2(units.empty(), "une ROM a été proposée comme unité malgré I1");
    }

    void aCustomFactoryCanReplaceTheGenericAdapter()
    {
        // Le point d'extension, pour l'émulateur qui sortirait un jour du
        // vocabulaire déclaratif. Rien ne l'utilise aujourd'hui, et c'est
        // volontaire : on vérifie seulement qu'il est là et qu'il fonctionne.
        struct Special final : Adapter {
            explicit Special(AdapterManifest declared) : m_manifest(std::move(declared)) {}
            QString id() const override { return m_manifest.id; }
            const AdapterManifest &manifest() const override { return m_manifest; }
            std::vector<DiscoveredUnit> discover(const QString &) const override
            {
                return {DiscoveredUnit{m_manifest.id, "sur-mesure", "dir", "k", "l", "p", {}}};
            }
            AdapterManifest m_manifest;
        };

        AdapterRegistry registry;
        registry.registerFactory("ppsspp", [](const AdapterManifest &declared) {
            return std::unique_ptr<Adapter>(new Special(declared));
        });
        registry.loadDirectory(adaptersDir());
        QCOMPARE(registry.ids().size(), 9);
        const auto units = registry.adapter("ppsspp")->discover("/peu importe");
        QCOMPARE(units.size(), size_t(1));
        QCOMPARE(units[0].unitKey, QString("sur-mesure"));
        // Les autres restent servis par le moteur générique.
        QVERIFY(dynamic_cast<const ManifestAdapter *>(registry.adapter("melonds")) != nullptr);
    }
};

QTEST_GUILESS_MAIN(AdaptersTest)
#include "tst_adapters.moc"
