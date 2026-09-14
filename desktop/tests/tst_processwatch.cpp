// Ce test ne simule rien : il cherche le processus de test LUI-MÊME dans la
// liste des processus vivants. Une détection qui ne se voit pas elle-même ne
// protégerait aucune sauvegarde.
#include "agent/processwatch.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QTest>

using namespace retrosave::agent;

class ProcessWatchTest final : public QObject
{
    Q_OBJECT
  private slots:
    void theWatchSeesItsOwnProcess()
    {
        const auto running = runningProcessNames();
        QVERIFY(!running.isEmpty());
        // runningProcessNames() normalise les noms en retirant ".exe" sous
        // Windows ; comparer la même représentation des deux côtés.
        const auto own =
            QFileInfo(QCoreApplication::applicationFilePath()).completeBaseName().toLower();
        QVERIFY2(running.contains(own), qPrintable("le test ne se voit pas lui-même : " + own));
    }

    void aManifestNameMatchesWhateverTheSystem()
    {
        const auto running = runningProcessNames();
        const auto own = QFileInfo(QCoreApplication::applicationFilePath()).completeBaseName();
        // Un manifeste annonce « PPSSPPWindows64.exe » ET « ppsspp » : le même
        // fichier sert aux deux systèmes. La comparaison ignore donc la casse
        // et l'extension, sinon la protection dépendrait du poste sur lequel le
        // manifeste a été écrit.
        QVERIFY(anyProcessRunning({own.toUpper() + ".EXE"}, running));
        QVERIFY(anyProcessRunning({own}, running));
    }

    void anEmulatorThatIsNotRunningIsNotDetected()
    {
        const auto running = runningProcessNames();
        QVERIFY(!anyProcessRunning({"un-emulateur-qui-n-existe-pas-42"}, running));
        // Une liste vide — le cas d'un manifeste sans `process_names` — ne
        // bloque jamais la capture : elle ne prétend rien surveiller.
        QVERIFY(!anyProcessRunning({}, running));
    }
};

QTEST_MAIN(ProcessWatchTest)
#include "tst_processwatch.moc"
