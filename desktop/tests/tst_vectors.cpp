// Ce test ne vérifie pas « le C++ est cohérent avec lui-même » : il rejoue les
// MÊMES fichiers JSON que les tests Python et Kotlin. C'est la seule preuve
// qui vaille avant d'écrire quoi que ce soit d'autre dans le moteur — un écart
// d'un octet ici deviendrait un conflit fantôme entre un PC et une console.
#include "core/decisions.h"
#include "core/gameart.h"
#include "core/identity.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

using namespace retrosave::core;

class VectorTest final : public QObject
{
    Q_OBJECT
  private:
    // RETROSAVE_VECTORS_DIR est défini par CMake : le dossier des vecteurs est
    // hors du composant desktop, partagé avec Python et Kotlin.
    // Certains fichiers de vecteurs sont un objet plutôt qu'un tableau : ils
    // regroupent plusieurs familles de cas sous des clés.
    static QJsonObject loadObject(const QString &name)
    {
        QFile file(QStringLiteral(RETROSAVE_VECTORS_DIR) + "/" + name);
        if (!file.open(QIODevice::ReadOnly))
            return {};
        return QJsonDocument::fromJson(file.readAll()).object();
    }

    // JSON ne distingue pas « absent » de « nul » de la même façon que C++ :
    // ce petit passage évite de le retraduire à la main dans chaque cas.
    static std::optional<QString> optionalString(const QJsonValue &value)
    {
        if (value.isNull() || value.isUndefined())
            return std::nullopt;
        return value.toString();
    }

    static QJsonArray load(const QString &name)
    {
        QFile file(QStringLiteral(RETROSAVE_VECTORS_DIR) + "/" + name);
        if (!file.open(QIODevice::ReadOnly))
            return {};
        return QJsonDocument::fromJson(file.readAll()).array();
    }

  private slots:
    // SYN-06 / SYN-07 : la pause et la sélection sont des décisions de noyau,
    // donc elles passent par un vecteur partagé comme les autres. Le Kotlin
    // rejoue le même fichier (`LocalSyncModeVectorsTest`).
    // M8 §7 : le serveur et les clients doivent trancher pareil, sinon la
    // bibliothèque change de nom à chaque passe selon qui a parlé en dernier.
    void labelAdoptionVectors()
    {
        const auto cases = load("label_adoption.json");
        QVERIFY(!cases.isEmpty());
        for (const auto &entry : cases) {
            const auto testCase = entry.toObject();
            const auto input = testCase.value("input").toObject();
            const auto actual = adoptedLabel(input.value("stored_label").toString(),
                                             input.value("stored_source").toString(),
                                             input.value("incoming_label").toString(),
                                             input.value("unit_key").toString());
            const auto expected = optionalString(testCase.value("expected"));
            QCOMPARE(qPrintable(testCase.value("name").toString() + " → "
                                + actual.value_or(QStringLiteral("<inchangé>"))),
                     qPrintable(testCase.value("name").toString() + " → "
                                + expected.value_or(QStringLiteral("<inchangé>"))));
        }
    }

    void localSyncModeVectors()
    {
        const auto cases = load("local_sync_mode.json");
        QVERIFY(!cases.isEmpty());
        for (const auto &entry : cases) {
            const auto testCase = entry.toObject();
            const auto input = testCase.value("input").toObject();
            const auto actual = localSyncDecision(input.value("mode").toString(),
                                                  input.value("global_pause").toBool());
            QCOMPARE(qPrintable(testCase.value("name").toString() + " → " + actual),
                     qPrintable(testCase.value("name").toString() + " → "
                                + testCase.value("expected").toString()));
        }
    }

    // M8 §5 : le libellé affiché est dérivé du nom de fichier. Cette règle
    // vivait uniquement en Kotlin (`DisplayNameVectorsTest`) ; le desktop
    // affichait donc « Golden Sun (FR).sav » là où le téléphone affichait
    // « Golden Sun », et la recherche de jaquette, qui part du libellé, ne
    // trouvait rien. Même fichier, mêmes réponses.
    void displayNameMatchesSharedVectors()
    {
        const auto cases = load("display_name_basic.json");
        QVERIFY(!cases.isEmpty());
        for (const auto &entry : cases) {
            const auto testCase = entry.toObject();
            const auto actual = displayName(testCase.value("input").toString());
            QVERIFY2(actual == testCase.value("expected").toString(),
                     qPrintable(testCase.value("name").toString() + " → " + actual));
        }
    }

    void vectorsAreReachable()
    {
        // Un fichier de vecteurs introuvable rendrait tous les autres tests
        // verts sans rien vérifier. On le refuse explicitement.
        QVERIFY(!load("normalize_name_basic.json").isEmpty());
        QVERIFY(!load("fingerprint_basic.json").isEmpty());
        QVERIFY(!load("content_hash_basic.json").isEmpty());
        QVERIFY(!load("sync_decision_duplicate.json").isEmpty());
        QVERIFY(!load("sync_decision_pending_branch.json").isEmpty());
        QVERIFY(!load("apply_journal_decision.json").isEmpty());
        QVERIFY(!load("quarantine_decision.json").isEmpty());
        QVERIFY(!load("local_sync_mode.json").isEmpty());
        QVERIFY(!load("label_adoption.json").isEmpty());
        QVERIFY(!load("cloud_ahead_basic.json").isEmpty());
        QVERIFY(!load("display_name_basic.json").isEmpty());
        QVERIFY(!loadObject("sync_guards.json").isEmpty());
        QVERIFY(!load("path_guard_basic.json").isEmpty());
        QVERIFY(!load("write_rename_outcome.json").isEmpty());
        QVERIFY(!loadObject("game_art_basic.json").isEmpty());
    }

    void normalizeGameNameMatchesSharedVectors()
    {
        const auto cases = load("normalize_name_basic.json");
        QCOMPARE(cases.size(), 5);
        for (const auto value : cases) {
            const auto testCase = value.toObject();
            const auto name = testCase.value("name").toString();
            const auto actual = normalizeGameName(testCase.value("input").toString());
            // Le nom du cas apparaît dans l'échec : on sait lequel a cédé.
            QVERIFY2(actual == testCase.value("expected").toString(),
                     qPrintable(name + " → " + actual));
        }
    }

    void quickFingerprintMatchesSharedVectors()
    {
        const auto cases = load("fingerprint_basic.json");
        QCOMPARE(cases.size(), 4);
        for (const auto value : cases) {
            const auto testCase = value.toObject();
            const auto name = testCase.value("name").toString();
            std::vector<NodeMeta> nodes;
            for (const auto node : testCase.value("input").toArray()) {
                const auto entry = node.toObject();
                nodes.push_back({entry.value("rel_path").toString(),
                                 entry.value("size_bytes").toInteger(),
                                 entry.value("mtime_ms").toInteger()});
            }
            const auto expected = testCase.value("expected").toObject();
            // Les octets canoniques ET leur empreinte : comparer seulement
            // l'empreinte cacherait où se situe un écart.
            const auto canonical = canonicalQuickFingerprint(nodes);
            QVERIFY2(QString::fromLatin1(canonical.toHex()) ==
                         expected.value("canonical_hex").toString(),
                     qPrintable(name + " octets → " + QString::fromLatin1(canonical.toHex())));
            const auto digest = quickFingerprintHash(nodes);
            QVERIFY2(digest == expected.value("qf_hash").toString(),
                     qPrintable(name + " empreinte → " + digest));
        }
    }

    void contentIdentityMatchesSharedVectors()
    {
        const auto cases = load("content_hash_basic.json");
        QCOMPARE(cases.size(), 3);
        for (const auto value : cases) {
            const auto testCase = value.toObject();
            const auto name = testCase.value("name").toString();
            const auto input = testCase.value("input").toObject();
            std::vector<ContentFile> files;
            for (const auto entry : input.value("files").toArray()) {
                const auto file = entry.toObject();
                files.push_back(
                    {file.value("rel_path").toString(),
                     QByteArray::fromBase64(file.value("content_base64").toString().toLatin1())});
            }
            const auto actual = contentSha256(input.value("unit_type").toString(), files);
            QVERIFY2(actual == testCase.value("expected").toString(),
                     qPrintable(name + " → " + actual));
        }
    }

    void captureGuardsMatchSharedVectors()
    {
        const auto guards = loadObject("sync_guards.json");
        QVERIFY(!guards.isEmpty());

        const auto stability = guards.value("stability").toArray();
        QCOMPARE(stability.size(), 5);
        for (const auto value : stability) {
            const auto testCase = value.toObject();
            const auto in = testCase.value("input").toObject();
            StabilityInput input;
            input.state = in.value("state").toString();
            input.observedQfHash = optionalString(in.value("observed_qf_hash"));
            input.stableQfHash = optionalString(in.value("stable_qf_hash"));
            input.stableSinceMs =
                in.value("stable_since_ms").isNull()
                    ? std::nullopt
                    : std::optional<qint64>(in.value("stable_since_ms").toInteger());
            input.nowMs = in.value("now_ms").toInteger();
            input.stabilizationMs = in.value("stabilization_ms").toInteger();
            input.emulatorRunning = in.value("emulator_running").toBool();
            QVERIFY2(isStable(input) == testCase.value("expected").toBool(),
                     qPrintable(testCase.value("name").toString()));
        }

        const auto apply = guards.value("apply_emulator_guard").toArray();
        QCOMPARE(apply.size(), 2);
        for (const auto value : apply) {
            const auto testCase = value.toObject();
            QCOMPARE(mayApplyWithEmulator(testCase.value("input").toObject()
                                              .value("emulator_running").toBool()),
                     testCase.value("expected").toBool());
        }

        const auto pause = guards.value("missing_pause").toArray();
        QCOMPARE(pause.size(), 2);
        for (const auto value : pause) {
            const auto testCase = value.toObject();
            const auto in = testCase.value("input").toObject();
            QVERIFY2(shouldPauseForMissing(in.value("missing_count").toInt(),
                                           in.value("threshold").toInt()) ==
                         testCase.value("expected").toBool(),
                     qPrintable(testCase.value("name").toString()));
        }
    }

    void ambiguousMatchesMatchSharedVectors()
    {
        const auto cases = load("sync_decision_duplicate.json");
        QCOMPARE(cases.size(), 5);
        for (const auto value : cases) {
            const auto testCase = value.toObject();
            std::vector<QString> paths;
            for (const auto path : testCase.value("input").toObject().value("rel_paths").toArray())
                paths.push_back(path.toString());
            QVERIFY2(quarantineState(paths) == testCase.value("expected").toString(),
                     qPrintable(testCase.value("name").toString()));
        }
    }

    void quarantineDecisionsMatchSharedVectors()
    {
        // AD-30. Le compteur vit dans le carnet ; la DÉCISION qu'on en tire est
        // une fonction pure, et c'est elle que Python rejoue sur ce fichier.
        const auto cases = load("quarantine_decision.json");
        QVERIFY(!cases.isEmpty());
        for (const auto &entry : cases) {
            const auto object = entry.toObject();
            const auto input = object.value("input").toObject();
            const auto optionalInt = [](const QJsonValue &value) -> std::optional<int> {
                if (value.isNull() || value.isUndefined())
                    return std::nullopt;
                return value.toInt();
            };
            const auto actual =
                quarantineDecision(input.value("consecutive_failures").toInt(),
                                   optionalInt(input.value("last_failure_head")),
                                   optionalInt(input.value("head_version")));
            QCOMPARE(actual, object.value("expected").toString());
        }
    }

    void pendingBranchDecisionsMatchSharedVectors()
    {
        const auto cases = load("sync_decision_pending_branch.json");
        QCOMPARE(cases.size(), 2);
        for (const auto value : cases) {
            const auto testCase = value.toObject();
            const auto in = testCase.value("input").toObject();
            const auto branch = in.value("pending_branch");
            const auto pending = branch.isNull() || branch.isUndefined()
                                     ? std::nullopt
                                     : optionalString(branch.toObject().value("content_sha256"));
            const auto decision =
                pendingBranchDecision(in.value("open_conflict").toBool(),
                                      in.value("local_content_sha256").toString(), pending);
            QVERIFY2(decision.has_value() && *decision == testCase.value("expected").toString(),
                     qPrintable(testCase.value("name").toString()));
        }
    }

    void interruptedApplyDecisionsMatchSharedVectors()
    {
        const auto cases = load("apply_journal_decision.json");
        QCOMPARE(cases.size(), 10);
        for (const auto value : cases) {
            const auto testCase = value.toObject();
            const auto in = testCase.value("input").toObject();
            const auto marker = in.value("journal");
            // Le vecteur encode les trois états du marqueur : absent (null),
            // incomplet (la chaîne « malformed »), ou complet (une empreinte).
            ApplyJournal journal;
            if (marker.isNull() || marker.isUndefined())
                journal = readApplyJournal(std::nullopt, std::nullopt, std::nullopt);
            else if (marker.toString() == "malformed")
                journal = readApplyJournal(3, std::nullopt, 1.0);
            else
                journal = readApplyJournal(3, marker.toString(), 1.0);
            QVERIFY2(
                applyJournalDecision(journal, optionalString(in.value("local_content_sha256"))) ==
                    testCase.value("expected").toString(),
                qPrintable(testCase.value("name").toString()));
        }
    }

    void sessionStartWarningsMatchSharedVectors()
    {
        const auto cases = load("cloud_ahead_basic.json");
        QCOMPARE(cases.size(), 8);
        for (const auto value : cases) {
            const auto testCase = value.toObject();
            const auto in = testCase.value("input").toObject();
            std::vector<RemoteUnitView> remote;
            for (const auto entry : in.value("remote").toArray()) {
                const auto unit = entry.toObject();
                remote.push_back(
                    {unit.value("emulator").toString(), unit.value("unit_key").toString(),
                     unit.value("head_version").toInt(), unit.value("state").toString()});
            }
            std::vector<LocalUnitView> local;
            for (const auto entry : in.value("local").toArray()) {
                const auto unit = entry.toObject();
                local.push_back({unit.value("emulator").toString(),
                                 unit.value("unit_key").toString(),
                                 unit.value("last_synced_version").toInt()});
            }
            std::vector<QString> expected;
            for (const auto name : testCase.value("expected").toArray())
                expected.push_back(name.toString());
            QVERIFY2(unitsBehindCloud(in.value("emulator").toString(), remote, local) == expected,
                     qPrintable(testCase.value("name").toString()));
        }
    }

    void writeGuardsMatchSharedVectors()
    {
        const auto paths = load("path_guard_basic.json");
        QCOMPARE(paths.size(), 14);
        for (const auto value : paths) {
            const auto testCase = value.toObject();
            const auto actual =
                pathGuard(testCase.value("input").toObject().value("rel_path").toString());
            QVERIFY2(actual == testCase.value("expected").toString(),
                     qPrintable(testCase.value("name").toString() + " → " + actual));
        }

        const auto renames = load("write_rename_outcome.json");
        QCOMPARE(renames.size(), 7);
        for (const auto value : renames) {
            const auto testCase = value.toObject();
            const auto in = testCase.value("input").toObject();
            const auto actual = renameOutcome(in.value("requested").toString(),
                                              optionalString(in.value("obtained")));
            QVERIFY2(actual == testCase.value("expected").toString(),
                     qPrintable(testCase.value("name").toString() + " → " + actual));
        }
    }

    void unsafeOrDuplicatePathsAreRefused()
    {
        // Pas encore de vecteur partagé pour ces cas : ils protègent l'écriture
        // sur disque, qui n'existe dans aucun des trois moteurs sous cette
        // forme. À transformer en vecteur commun le jour où Python et Kotlin
        // exposeront le même contrat d'erreur.
        const auto refused = [](const QString &path) {
            try {
                contentSha256("dir", {{path, QByteArray()}});
                return false;
            } catch (const std::invalid_argument &) {
                return true;
            }
        };
        QVERIFY2(refused("../ailleurs.sav"), "une remontée de dossier doit être refusée");
        QVERIFY2(refused("/absolu.sav"), "un chemin absolu doit être refusé");
        QVERIFY2(refused("dossier\\fichier.sav"), "un séparateur Windows doit être refusé");
        QVERIFY2(refused(""), "un chemin vide doit être refusé");

        bool duplicateRefused = false;
        try {
            contentSha256("dir", {{"a.sav", QByteArray()}, {"a.sav", QByteArray()}});
        } catch (const std::invalid_argument &) {
            duplicateRefused = true;
        }
        QVERIFY2(duplicateRefused, "deux fois le même chemin doit être refusé");
    }

    void aFileUnitHoldsExactlyOneFile()
    {
        bool refused = false;
        try {
            contentSha256("file", {{"a.sav", QByteArray()}, {"b.sav", QByteArray()}});
        } catch (const std::invalid_argument &) {
            refused = true;
        }
        QVERIFY(refused);
    }
    // ── La vignette d'une sauvegarde ──────────────────────────────────────
    // Même teinte, mêmes initiales, même verdict sur un PNG que Python et
    // Kotlin : sinon la même sauvegarde change d'aspect selon l'appareil.
    void theArtworkHueComesFromTheGameKey()
    {
        const auto cases = loadObject("game_art_basic.json").value("hues").toArray();
        QVERIFY(!cases.isEmpty());
        for (const auto &entry : cases) {
            const auto item = entry.toObject();
            const auto key = item.value("game_key").toString();
            QCOMPARE(placeholderHue(key), item.value("expected").toInt());
        }
    }

    void theArtworkInitialsSkipStopWordsWithoutEmptyingTheLabel()
    {
        const auto cases = loadObject("game_art_basic.json").value("initials").toArray();
        QVERIFY(!cases.isEmpty());
        for (const auto &entry : cases) {
            const auto item = entry.toObject();
            const auto label = item.value("label").toString();
            QCOMPARE(placeholderInitials(label), item.value("expected").toString());
        }
    }

    void onlyARealPngIsAcceptedAsAnIcon()
    {
        const auto cases = loadObject("game_art_basic.json").value("icons").toArray();
        QVERIFY(!cases.isEmpty());
        for (const auto &entry : cases) {
            const auto item = entry.toObject();
            const auto data =
                QByteArray::fromBase64(item.value("input_base64").toString().toLatin1());
            QCOMPARE(isValidIcon(data), item.value("expected_valid").toBool());
        }
    }
};

QTEST_GUILESS_MAIN(VectorTest)
#include "tst_vectors.moc"
