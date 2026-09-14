// Reproduction minimale : quelles primitives d'interaction Qt Quick répondent
// aux événements synthétisés par Spix ? Trois cibles, un seul clic chacune.
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <Spix/AnyRpcServer.h>
#include <Spix/QtQmlBot.h>

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    QQmlApplicationEngine engine;
    engine.load(QUrl("qrc:/probe.qml"));
    if (engine.rootObjects().isEmpty()) return 1;
    spix::AnyRpcServer server(9111);
    auto *bot = new spix::QtQmlBot(&app);
    bot->runTestServer(server);
    return app.exec();
}
