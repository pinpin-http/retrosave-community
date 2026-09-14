// Un sous-dossier direct non vide = une unité. C'est la règle Q9 du client
// dossier générique. L'identité (`emulator`, `unit_key`) doit rester stable
// pour que tous les appareils convergent sur la même unité serveur.
#pragma once

#include "engine/pass.h"

#include <QString>
#include <vector>

namespace retrosave::engine
{

std::vector<ScannedUnit> scanFolderRoot(const QString &root);

} // namespace retrosave::engine
