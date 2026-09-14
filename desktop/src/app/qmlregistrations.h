#pragma once

#include "app/agentclient.h"
#include "app/preferences.h"
#include "app/trayshell.h"
#include <QtQml/qqmlregistration.h>

// Comment une classe C++ devient un type QML.
//
// `Q_GADGET` est un `Q_OBJECT` allégé, pour un type sans signaux. Ces structs
// ne contiennent rien : elles servent uniquement de FICHE DESCRIPTIVE lue par
// `moc` et par les outils QML.
//   QML_FOREIGN(X)        décrit X sans le modifier — X n'a donc pas besoin de
//                         connaître QtQuick, et la bibliothèque IPC reste
//                         indépendante de l'interface ;
//   QML_NAMED_ELEMENT(N)  le nom sous lequel QML voit le type ;
//   QML_UNCREATABLE(...)  QML ne peut pas en fabriquer une instance.
//
// L'instance est créée dans main.cpp et injectée : QML ne peut pas en créer
// une deuxième, ce qui garantit un seul propriétaire de l'état.
struct AgentClientRegistration {
    Q_GADGET
    QML_FOREIGN(retrosave::AgentClient)
    QML_NAMED_ELEMENT(AgentConnection)
    QML_UNCREATABLE("L'instance est fournie par le point d'entrée C++.")
};

struct TrayShellRegistration {
    Q_GADGET
    QML_FOREIGN(retrosave::TrayShell)
    QML_NAMED_ELEMENT(DesktopTray)
    QML_UNCREATABLE("L'instance est fournie par le point d'entrée C++.")
};

struct PreferencesRegistration {
    Q_GADGET
    QML_FOREIGN(retrosave::Preferences)
    QML_NAMED_ELEMENT(DesktopPreferences)
    QML_UNCREATABLE("L'instance est fournie par le point d'entrée C++.")
};
