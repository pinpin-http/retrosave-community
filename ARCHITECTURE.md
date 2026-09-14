# Architecture de RetroSave Community

Ce document décrit la structure livrée par l'édition Community. Les invariants
de sûreté et le périmètre se trouvent dans [CLAUDE.md](CLAUDE.md).

## Vue d'ensemble

```text
Android Kotlin/Compose ──┐
                        ├── HTTPS ── API FastAPI ── PostgreSQL
Desktop Qt/QML + agent ─┘               └── URL présignées ── stockage S3
```

Les clients échangent les métadonnées avec l'API. Les archives passent
directement entre le client et le stockage objet grâce à des URL PUT/GET
présignées de courte durée. L'API ne transporte donc jamais une sauvegarde.

## Composants

| Chemin | Rôle |
|---|---|
| `desktop/` | interface Qt Quick, agent C++, moteur, SQLite et réseau |
| `android/` | interface Compose, moteur Kotlin, SAF, Room et WorkManager |
| `server/` | API FastAPI, migrations Alembic, PostgreSQL et client S3 |
| `adapters/` | manifestes TOML des émulateurs et fixtures synthétiques |
| `tests/vectors/` | contrats partagés d'identité, décision et archive |
| `deploy/` | installation HTTPS, sauvegarde et restauration du serveur |
| `scripts/testing/` | bancs avec services et données jetables |

## Modèle de synchronisation

Une unité est un fichier de sauvegarde ou un dossier de sauvegarde. Son
identité stable associe l'émulateur et une clé d'unité. Le contenu d'un fichier
est identifié par SHA-256. Pour un dossier, l'empreinte porte sur un manifeste
trié de chemins relatifs, tailles et empreintes de fichiers.

Une capture n'est autorisée qu'après stabilisation sur deux scans. Sur desktop,
l'agent attend aussi l'arrêt du processus d'émulation reconnu. L'archive est un
`tar.zst` déterministe : ordre stable, dates normalisées, aucun propriétaire.

Le client prépare un envoi avec sa version de base. Il transfère ensuite
l'archive par l'URL S3 présignée et confirme l'envoi. Si la tête a changé entre
temps, le serveur crée une branche de conflit immuable. Une résolution choisit
une tête, mais ne supprime aucune branche.

Pour recevoir une version, le client vérifie successivement l'empreinte de
l'archive et celle du contenu extrait. Il crée une copie locale rotative,
prépare le remplacement dans le même système de fichiers, puis utilise un
renommage atomique. Une restauration crée une nouvelle version de tête au lieu
de réécrire l'historique.

## API et données

L'API versionnée sous `/v0` gère :

- les appareils : enregistrement, liste, renommage et révocation ;
- les unités : liste, création, état, libellé et signalement d'absence ;
- les versions : préparation, confirmation, historique, téléchargement et
  restauration ;
- les conflits : liste et résolution explicite ;
- `/healthz` : accès PostgreSQL et bucket S3.

Les écritures acceptent une clé d'idempotence. PostgreSQL conserve les comptes,
appareils, unités, versions, conflits, envois en attente et réponses
idempotentes. Les objets restent privés dans S3 et sont nommés par compte et
identifiants générés par le serveur.

L'auto-hébergement emploie un jeton opaque dont seul le condensat est stocké.
Le serveur sait également vérifier un fournisseur JWT externe optionnel. Une
révocation d'appareil bloque cet appareil sans effacer ses versions.

## Stockage S3

Le serveur utilise deux clients S3 avec les mêmes clés :

- l'endpoint interne exécute les contrôles HEAD, la santé et le nettoyage ;
- l'endpoint public signe localement les URL remises aux clients.

Cette séparation est nécessaire parce que l'hôte fait partie de la signature
SigV4. Le déploiement fourni place le stockage derrière un second nom DNS Caddy
et conserve l'en-tête `Host`. Le bucket est privé et créé de façon idempotente
au démarrage.

## Desktop

L'interface et l'agent sont deux processus. Ils communiquent uniquement sur la
boucle locale avec un jeton de session et un nom d'endpoint propre à l'édition
Community. L'interface lance l'agent lorsque nécessaire ; l'agent reste la
seule autorité pour les passes de synchronisation et l'état SQLite.

Qt possède les objets parents/enfants. Les travaux réseau et les transitions
de moteur sont asynchrones afin de ne pas bloquer la boucle d'interface. Le
démarrage automatique utilise une unité utilisateur systemd sous Linux et une
tâche planifiée sous Windows.

Les dépendances vont dans un seul sens :

```text
app/QML ──IPC──> agent ──> adapters ──> engine ──> core
                    │                      │
                    ├──> state/SQLite      ├──> network/API + S3
                    └──> processwatch      └──> localfs
```

- `core/` contient les fonctions pures, l'archive et les écritures atomiques ;
- `engine/` orchestre une passe à travers les interfaces de `ports.h` ;
- `network/` implémente ces ports pour l'API v0 et les URL S3 présignées ;
- `state/` possède le carnet SQLite et le journal des applications interrompues ;
- `adapters/` transforme les manifestes et les arbres de fichiers en unités ;
- `agent/` possède le worker, la planification et la configuration sensible ;
- `app/` adapte l'IPC en propriétés QML, sans règle métier.

Une nouvelle règle de décision appartient à `core/decisions.*` et commence par
un vecteur partagé. Une nouvelle étape de passe appartient à `engine/pass.*`.
Une route serveur ou un format JSON appartient à `network/`. Le QML ne doit
jamais ouvrir une sauvegarde ni écrire directement dans SQLite.

Une requête de l'interface suit ce trajet : QML appelle `AgentClient`, qui
écrit une trame JSON délimitée par un saut de ligne. `LocalService` borne et
découpe la trame, puis `agent/main.cpp` la traduit en appel de `SyncService`.
Les opérations longues partent dans le worker ; la réponse IPC confirme leur
acceptation et l'interface observe ensuite leur résultat par sondage.

## Android

Le cœur Kotlin ne dépend pas du framework Android et partage les vecteurs du
moteur C++. SAF fournit l'accès aux dossiers choisis par l'utilisateur. Room
conserve le miroir local. WorkManager programme les passes en arrière-plan ;
une tuile et l'application permettent aussi un déclenchement explicite.

L'application de release refuse le trafic HTTP en clair. Le serveur et
l'endpoint S3 public doivent donc présenter des certificats HTTPS reconnus.

`core/` reste indépendant d'Android et porte les décisions partagées. `data/`
adapte Room, SAF et le réseau ; `ui/` consomme des états préparés par les view
models. Un URI SAF reste un URI : le moteur ouvre un flux via le port fourni et
ne suppose jamais qu'un document possède un chemin de fichier classique.

## Déploiement et reprise

`deploy/compose.yml` lance PostgreSQL, le stockage, l'initialisation du bucket,
l'API et Caddy. Seuls les ports 80 et 443 sont publiés. `configure.py` crée un
fichier `.env` privé sans jamais remplacer des secrets existants.

Une sauvegarde cohérente interrompt temporairement l'API et le stockage, puis
capture PostgreSQL, les objets, la configuration et le commit source avec des
empreintes SHA-256. La restauration refuse tout volume existant et démarre sans
proxy afin de permettre une recette avant bascule. La procédure complète est
dans [docs/guides/SELF_HOSTING.md](docs/guides/SELF_HOSTING.md).

## Tests et livraison

Les tests serveur utilisent PostgreSQL et un stockage jetables. Les bancs
d'intégration font converger deux moteurs et parcourent chaque adaptateur. Le
parcours GUI pilote la vraie fenêtre avec l'agent et le serveur réels de test.

La matrice desktop compile et teste Linux et Windows, puis déploie les
bibliothèques Qt dans un arbre déplaçable. La release produit un installateur et
une archive portable Windows, une archive Linux et un APK Android signé. Les
empreintes de tous les artefacts accompagnent la release.
