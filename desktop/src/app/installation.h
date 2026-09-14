#pragma once

#include <QString>

namespace retrosave::installation
{

// Chemin absolu de l'agent installé À CÔTÉ de l'interface. Jamais une
// résolution par PATH ou par dossier courant : un homonyme ne doit pas
// pouvoir se glisser à la place de notre agent.
QString agentExecutable();

// Fichier de réglages de l'interface, sous le dossier de configuration de
// l'utilisateur. Il ne contient aucun secret ni contenu de sauvegarde.
QString settingsFile();

} // namespace retrosave::installation
