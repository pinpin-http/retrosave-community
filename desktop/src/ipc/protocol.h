#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

namespace retrosave::ipc
{
inline constexpr int ProtocolVersion = 1;
// Borne de sécurité du canal, pas une contrainte de conception. Les 4 Ko
// d'origine suffisaient à un diagnostic ; une liste d'unités et un historique
// ne tiennent pas dedans, et les contorsions pour y entrer coûteraient plus
// cher que le tampon. Le canal reste privé à l'utilisateur et borné.
//
// La borne couvre 300 unités avec appareils et journal d'activité.
inline constexpr qsizetype MaxFrameBytes = 524288;
inline constexpr int MaxConnections = 16;

// QLocalSocket est un flux : un readyRead peut contenir une demi-trame ou
// plusieurs trames. Le '\n' délimite chaque objet JSON compact.
QByteArray encode(const QJsonObject &message);
QJsonObject statusRequest(const QString &id);
QJsonObject shutdownRequest(const QString &id);
QJsonObject showRequest(const QString &id);
// Relier ce poste à un serveur. Le jeton ne transite QUE par ce canal local,
// privé à l'utilisateur : l'interface ne le range jamais elle-même, c'est
// l'agent qui possède la configuration comme il possède les écritures.
QJsonObject connectRequest(const QString &id, const QString &url, const QString &token,
                           const QString &deviceName);
// Demander une passe tout de suite. La réponse dit seulement si elle a été
// acceptée : une passe dure, et le canal ne doit jamais attendre.
QJsonObject syncRequest(const QString &id);
// Aller rechercher les conflits ouverts maintenant, sans attendre la passe
// périodique : l'interface en a besoin dès qu'on ouvre l'écran.
QJsonObject conflictsRequest(const QString &id);
// Trancher. Le perdant reste dans l'historique du serveur — un arbitrage n'est
// pas une suppression (invariant I3).
QJsonObject resolveRequest(const QString &id, const QString &conflictId, int winner);
// Relire le carnet local : ce que cet appareil suit, même hors ligne.
QJsonObject unitsRequest(const QString &id);
// L'historique d'une sauvegarde, lui, vient du serveur.
QJsonObject historyRequest(const QString &id, const QString &unitKey);
// Revenir à une version : le serveur en crée une nouvelle, il n'en réécrit
// aucune.
QJsonObject restoreRequest(const QString &id, const QString &unitKey, int number);

// Réarmer une unité mise de côté après des échecs répétés.
QJsonObject retryRequest(const QString &id, const QString &unitKey);

// Choix strictement local : rien n'est envoyé au serveur, rien
// n'est supprimé nulle part. `mode` vaut « sync », « paused » ou « excluded ».
QJsonObject selectRequest(const QString &id, const QString &unitKey, const QString &mode);
// La pause générale de cet appareil.
QJsonObject pauseAllRequest(const QString &id, bool paused);
// Préférences de notification de ce poste.
QJsonObject notificationsRequest(const QString &id, bool conflicts, bool errors, bool restores);

QJsonObject devicesRequest(const QString &id);
QJsonObject deviceRenameRequest(const QString &id, const QString &deviceId, const QString &name);
// Révoquer refuse les requêtes futures de cet appareil. Rien n'est détruit :
// ses versions restent dans l'historique (invariant I2).
QJsonObject deviceRevokeRequest(const QString &id, const QString &deviceId);

QJsonObject activityRequest(const QString &id);
QJsonObject diagnosticRequest(const QString &id, const QString &path);

QJsonObject exportRequest(const QString &id, const QString &path);

QJsonObject serverCheckRequest(const QString &id);

// Choisir l'image d'une unité, ou la retirer avec un chemin vide. Décoratif :
// aucune décision de synchronisation n'en dépend.
QJsonObject artworkRequest(const QString &id, const QString &unitKey, const QString &imagePath);

// La réponse et sa conséquence sont séparées : le protocole reste une fonction
// pure, et le processus seul décide de rendre la main après l'avoir écrite.
struct Reply {
    QJsonObject message;
    bool stopsService = false;
};

// Deux canaux distincts, deux vocabulaires. L'agent répond au diagnostic et à
// l'arrêt ; la fenêtre ne répond qu'à la demande de se montrer.
Reply reply(const QJsonObject &request);
Reply windowReply(const QJsonObject &request);

// Même endpoint pour l'UI et l'agent d'un utilisateur, indépendamment du
// répertoire de lancement.
QString defaultEndpoint();
// Le canal de la fenêtre dérive de celui de l'agent : `--socket` isole donc un
// test complet, agent et interface compris, sans variable supplémentaire.
QString windowEndpoint(const QString &agentEndpoint);
QString lockFilePath(const QString &endpoint);
// Le fichier de réglages, partagé par l'interface et l'agent — à dessein :
// c'est le même produit, et deux fichiers finiraient par diverger. Il suit
// XDG_CONFIG_HOME, donc un test l'isole entièrement.
QString settingsFile();

} // namespace retrosave::ipc
