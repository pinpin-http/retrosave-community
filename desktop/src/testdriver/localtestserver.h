/***
 * Canal de pilotage des tests d'interface — ABSENT du livrable.
 *
 * Ce fichier n'est compilé QUE si `-DRSC_WITH_TEST_DRIVER=ON` a été demandé à
 * CMake, et le binaire de production ne le contient pas. C'est la première des
 * trois barrières ; les deux autres sont dans le `.cpp`.
 ***/
#pragma once

#include <Spix/TestServer.h>

#include <cstdint>
#include <memory>

namespace retrosave::testdriver
{

/**
 * Un serveur XML-RPC minimal qui pilote la fenêtre réelle.
 *
 * Spix fournit `AnyRpcServer`, mais il n'est PAS utilisé ici pour deux raisons,
 * et les deux tiennent à la sûreté :
 *
 * 1. **il écoute sur toutes les interfaces.** AnyRPC lie `INADDR_ANY` par
 *    défaut, et `AnyRpcServer` ne change pas cette adresse. Un canal capable de
 *    cliquer, de saisir du texte et d'appeler des méthodes QML n'a rien à faire
 *    sur le réseau : celui-ci se lie explicitement à `127.0.0.1` ;
 * 2. **il expose tout.** `setStringProperty` et `invokeMethod` permettent
 *    d'appeler n'importe quelle méthode de n'importe quel objet QML. Le
 *    parcours de test n'en a pas besoin : on n'expose que cliquer, donner le
 *    focus, saisir, lire, attendre, capturer et quitter.
 *
 * La classe dérive de `spix::TestServer`, qui exécute `executeTest()` dans son
 * PROPRE FIL. Les commandes qu'elle appelle sont réordonnancées par Spix vers
 * le fil de l'interface : on ne touche jamais un objet QML depuis ce fil-ci.
 */
class LocalTestServer final : public spix::TestServer
{
  public:
    explicit LocalTestServer(std::uint16_t port);
    ~LocalTestServer() override;

    LocalTestServer(const LocalTestServer &) = delete;
    LocalTestServer &operator=(const LocalTestServer &) = delete;

    /** Faux si le port n'a pas pu être ouvert : l'appelant doit alors renoncer. */
    bool listening() const;

  protected:
    void executeTest() override;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace retrosave::testdriver
