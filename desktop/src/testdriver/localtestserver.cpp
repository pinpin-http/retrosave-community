#include "localtestserver.h"

#include <anyrpc/anyrpc.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <netinet/in.h>
#include <cstddef>
#include <string>

namespace retrosave::testdriver
{
namespace
{
// AnyRPC enregistre des FONCTIONS LIBRES : sa signature de rappel ne porte
// aucun contexte. Le serveur est unique par processus, donc un pointeur de
// fichier suffit — et le rendre `static` empêche toute autre unité de
// compilation d'y toucher.
LocalTestServer *g_server = nullptr;

std::string argumentAt(anyrpc::Value &params, std::size_t index)
{
    if (!params.IsArray() || params.Size() <= index || !params[index].IsString())
        return {};
    return params[index].GetString();
}

int intArgumentAt(anyrpc::Value &params, std::size_t index, int fallback)
{
    if (!params.IsArray() || params.Size() <= index)
        return fallback;
    if (params[index].IsInt())
        return params[index].GetInt();
    return fallback;
}
} // namespace

struct LocalTestServer::Impl {
    std::unique_ptr<anyrpc::Server> rpc;
    std::mutex access;
    std::atomic<bool> keepRunning {true};
    bool listening = false;
};

LocalTestServer::LocalTestServer(std::uint16_t port)
    : m_impl(std::make_unique<Impl>())
{
    g_server = this;
    m_impl->rpc = std::make_unique<anyrpc::XmlHttpServer>();
    auto *methods = m_impl->rpc->GetMethodManager();

    // ─── La surface exposée, et rien d'autre ──────────────────────────────
    // Surface fixe. Ni `setStringProperty` ni `invokeMethod` générique : le
    // parcours n'en a pas besoin, et une commande absente ne peut pas être
    // détournée. `focus` ne peut appeler que forceActiveFocus(), car le greffon
    // offscreen de certains runners ne donne pas le focus après un clic.
    methods->AddFunction(
        [](anyrpc::Value &params, anyrpc::Value &result) {
            g_server->mouseClick(argumentAt(params, 0));
            result.SetNull();
        },
        "click", "Cliquer sur l'objet désigné | click(chemin)");

    methods->AddFunction(
        [](anyrpc::Value &params, anyrpc::Value &result) {
            g_server->inputText(argumentAt(params, 0), argumentAt(params, 1));
            result.SetNull();
        },
        "inputText", "Saisir du texte dans l'objet désigné | inputText(chemin, texte)");

    methods->AddFunction(
        [](anyrpc::Value &params, anyrpc::Value &result) {
            g_server->invokeMethod(argumentAt(params, 0), "forceActiveFocus", {});
            result.SetNull();
        },
        "focus", "Donner le focus clavier à l'objet désigné | focus(chemin)");

    methods->AddFunction(
        [](anyrpc::Value &params, anyrpc::Value &result) {
            const auto value = g_server->getStringProperty(argumentAt(params, 0),
                                                           argumentAt(params, 1));
            result.SetString(value.c_str(), value.size());
        },
        "getString", "Lire une propriété texte | getString(chemin, propriété)");

    methods->AddFunction(
        [](anyrpc::Value &params, anyrpc::Value &result) {
            result = g_server->existsAndVisible(argumentAt(params, 0));
        },
        "exists", "L'objet existe-t-il et est-il visible | exists(chemin)");

    methods->AddFunction(
        [](anyrpc::Value &params, anyrpc::Value &result) {
            // Attente sur une CONDITION observable, jamais sur une durée
            // arbitraire : c'est ce qui distingue un test stable d'un test qui
            // passe sur cette machine-ci.
            const auto found = g_server->waitForItem(
                argumentAt(params, 0),
                std::chrono::milliseconds(intArgumentAt(params, 1, 10000)));
            result = found;
        },
        "waitForItem", "Attendre qu'un objet apparaisse | waitForItem(chemin, msMax)");

    methods->AddFunction(
        [](anyrpc::Value &params, anyrpc::Value &result) {
            g_server->takeScreenshot(argumentAt(params, 0), argumentAt(params, 1));
            result.SetNull();
        },
        "screenshot", "Capturer un objet dans un fichier | screenshot(chemin, fichier)");

    methods->AddFunction(
        [](anyrpc::Value &, anyrpc::Value &result) {
            g_server->quit();
            result.SetNull();
        },
        "quit", "Fermer l'application pilotée | quit()");

    // ─── La barrière réseau ───────────────────────────────────────────────
    // AnyRPC lie `INADDR_ANY` par défaut. On impose la boucle locale AVANT
    // d'ouvrir le port : ce canal ne doit jamais être joignable depuis une
    // autre machine.
    m_impl->rpc->SetBindAddress(htonl(INADDR_LOOPBACK));
    m_impl->listening = m_impl->rpc->BindAndListen(port);
}

LocalTestServer::~LocalTestServer()
{
    m_impl->keepRunning.store(false);
    // On attend que `executeTest` ait rendu le verrou : détruire le serveur
    // pendant qu'il traite une requête planterait le processus.
    std::lock_guard<std::mutex> lock(m_impl->access);
    g_server = nullptr;
}

bool LocalTestServer::listening() const
{
    return m_impl->listening;
}

void LocalTestServer::executeTest()
{
    std::lock_guard<std::mutex> lock(m_impl->access);
    while (m_impl->keepRunning.load())
        m_impl->rpc->Work(200);
}

} // namespace retrosave::testdriver
