// Configuration possédée par l'agent, seul processus autorisé à déclencher des
// écritures locales. L'interface lui transmet les changements par IPC.
#pragma once

#include <QHash>
#include <QString>

namespace retrosave::agent
{

struct Configuration {
    QString serverUrl;
    QString token;
    QString deviceId;
    QString root; // dossier générique désigné explicitement par l'utilisateur
    // Une racine par émulateur, désignée séparément : le dossier de PPSSPP
    // n'est pas celui de Dolphin, et les confondre ferait scanner l'un avec
    // les règles de l'autre. Clé = identifiant du manifeste.
    QHash<QString, QString> roots;
    int intervalSeconds = 900; // 15 min, comme la passe périodique Android
    // La pause générale est un réglage d'appareil, donc elle vit ici et
    // non dans le carnet ; il ne quitte jamais ce poste. Une pause n'arrête
    // pas l'agent : elle arrête les captures et les réceptions, pour que
    // l'interface reste consultable et que la reprise soit immédiate.
    bool globalPause = false;
    // Aucune notification
    // ne porte de contenu de sauvegarde, seulement des faits de synchronisation.
    bool notifyConflicts = true;
    bool notifyErrors = true;
    bool notifyRestores = true;
    // Récupération des jaquettes proposées par le serveur. Activée par
    // défaut — c'est ce qui rend la bibliothèque lisible — mais désactivable :
    // l'adresse contient le nom du jeu, et tout le monde n'a pas envie que son
    // poste interroge un service tiers.
    bool downloadArtwork = true;

    // Une configuration incomplète n'est pas une erreur : c'est l'état normal
    // avant que l'utilisateur ait relié son compte. L'agent doit le dire
    // clairement plutôt que d'échouer à chaque passe.
    bool connected() const
    {
        return !serverUrl.isEmpty() && !token.isEmpty() && !deviceId.isEmpty();
    }
    bool complete() const { return connected() && !(root.isEmpty() && roots.isEmpty()); }
};

Configuration loadConfiguration();

// Écrit l'identité du compte, et **restreint le fichier au seul utilisateur**
// (0600), comme le fait `cli/rsc/config.py`. Un jeton lisible par tout le
// poste ne vaudrait guère mieux qu'un jeton public.
void saveConnection(const QString &url, const QString &token, const QString &deviceId);

// Séparée de `saveConnection` parce que la pause générale
// n'est pas un secret et change beaucoup plus souvent.
void saveGlobalPause(bool paused);

void saveNotificationPreferences(bool conflicts, bool errors, bool restores);

void saveArtworkDownload(bool enabled);

// Le carnet de l'agent est distinct des données d'autres éditions du produit.
QString stateDatabasePath();
// Dossier de travail des archives en transit. Vidé de ses restes au démarrage :
// un plantage pendant un transfert n'a pas à coûter de l'espace disque à vie.
QString stagingPath();

// Cache des vignettes, jetable par construction : une entrée perdue coûte
// une relecture de quelques kilo-octets, une entrée corrompue est écartée à la
// validation. C'est pourquoi il vit dans le dossier de CACHE du système et non
// à côté de l'état persistant.
QString iconCachePath();
// Chemin de la vignette d'une unité. Le nom d'unité peut contenir n'importe
// quoi : on ne le met jamais tel quel dans un chemin, un condensé est court,
// stable, et ne peut pas remonter d'un dossier.
QString iconPathFor(const QString &emulator, const QString &unitKey);

// Où trouver les manifestes `adapters/*.toml`. Cherché à côté de l'exécutable
// pour une installation, puis dans le dépôt pour le développement — et une
// variable d'environnement permet aux bancs de désigner un autre dossier sans
// toucher au poste.
// Image choisie par l'utilisateur pour une unité. Elle vit à côté de l'ÉTAT et
// non dans le cache : c'est une décision d'utilisateur, pas une donnée
// recalculable — vider le cache ne doit pas l'effacer.
//
// Le nom du fichier dérive de l'identité de l'UNITÉ (`emulator` + `unit_key`),
// jamais de la clé de jeu : deux sauvegardes homonymes appartenant au même jeu
// doivent pouvoir porter deux images différentes, et surtout ne jamais hériter
// l'une de l'autre.
QString customArtworkPath(const QString &emulator, const QString &unitKey);

QString adaptersPath();

} // namespace retrosave::agent
