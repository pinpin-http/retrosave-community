#pragma once

#include <QList>
#include <QString>
#include <optional>

namespace retrosave::network
{
// DTO du contrat serveur : données copiables, sans QObject ni accès disque.
// Les dates restent informatives (I4) ; aucun ordre de sync n'en est déduit.
struct DeviceRegistration {
    QString name;
    QString os;
    QString appVersion;
};
struct RegisteredDevice {
    QString id;
};
struct RemoteDevice {
    QString id;
    QString name;
    QString os;
    std::optional<QString> lastSeenAt;
    // EXP-01. Champ ADDITIF : un serveur antérieur ne l'envoie pas, et son
    // absence doit rester lisible — pas de révocation, donc pas de date.
    std::optional<QString> revokedAt;
};
struct Devices {
    QList<RemoteDevice> items;
};
struct UnitDeclaration {
    QString emulator;
    QString unitKey;
    QString unitType;
    QString gameKey;
    QString gameLabel;
};
struct RemoteHead {
    qint64 number;
    QString contentSha256;
    qint64 sizeBytes;
    QString createdAt;
    std::optional<QString> originDeviceName;
};
struct RemoteUnit {
    QString id;
    UnitDeclaration identity;
    QString labelSource;
    qint64 headVersion;
    QString state; // active/missing côté serveur ; jamais la pause locale V1
    QString updatedAt;
    std::optional<RemoteHead> head; // absent seulement quand headVersion == 0
    // Q45 : l'adresse d'une jaquette, quand le serveur a su la résoudre. Champ
    // ADDITIF : un serveur antérieur ne l'envoie pas, et son absence signifie
    // simplement « pas d'image », jamais une erreur.
    QString artworkUrl;
};
struct Units {
    QList<RemoteUnit> items;
};
struct RemoteVersion {
    RemoteHead summary;
    QString kind;
    std::optional<QString> clientMtime;
    std::optional<qint64> parentNumber;
};
struct History {
    QList<RemoteVersion> versions;
    qint64 headVersion; // n'est PAS forcément le plus grand numéro (Q7)
};
struct RemoteConflict {
    QString id;
    QString unitId;
    QString unitLabel;
    RemoteHead first;
    RemoteHead second;
    QString createdAt;
};
struct Conflicts {
    QList<RemoteConflict> items;
};
struct ResolvedConflict {
    QString unitId;
    qint64 headVersion;
};
struct Missing {
}; // simple accusé ; aucun effacement distant ou local
struct Health {
    QString status;
    bool database;
    bool storage;
    std::optional<QString> version; // champ additif, jamais requis pour se connecter
};
} // namespace retrosave::network
