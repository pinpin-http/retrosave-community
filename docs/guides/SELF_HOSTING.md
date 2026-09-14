# Héberger RetroSave Community

Ce guide installe l'API, PostgreSQL et le stockage des sauvegardes sur votre
serveur. Les clients Android et desktop se connectent avec **la même adresse
HTTPS et le même jeton** pour partager un compte. Aucun abonnement requis.

## 1. Préparer le serveur

Utiliser un serveur Linux avec Docker Engine et le plugin Docker Compose v2,
Git et Python 3. Prévoir environ 4 Go de RAM pour compiler le stockage, puis de
l'espace disque pour les sauvegardes **et toutes leurs versions**. La compilation
initiale télécharge les dépendances ; les démarrages suivants les réutilisent.

Créer deux enregistrements DNS vers le serveur, par exemple :

- `saves.exemple.fr` pour l'application ;
- `storage.exemple.fr` pour les transferts de fichiers.

Les ports TCP 80 et 443 doivent être accessibles pour les certificats HTTPS.
Ne publier ni PostgreSQL, ni la console du stockage. Le fichier Compose ne les
expose pas. Si le serveur est chez vous, prévoir les redirections du routeur ;
un accès sous CGNAT demande une autre solution réseau ou un VPS.

```sh
git clone https://github.com/pinpin-http/retrosave-community.git
cd retrosave-community
git checkout v0.1.0-alpha.1
python3 deploy/configure.py \
  --api-domain saves.exemple.fr \
  --storage-domain storage.exemple.fr \
  --email vous@exemple.fr
cd deploy
docker compose --env-file .env up -d --build
```

Le script génère des secrets aléatoires dans `deploy/.env`, en permissions 600.
Il refuse de remplacer une configuration existante. Ne publier ni ce fichier,
ni la sortie de `docker compose config`, qui contient les secrets interpolés.

Caddy obtient les certificats automatiquement. Vérifier :

```sh
docker compose ps
curl --fail https://saves.exemple.fr/healthz
```

Le résultat doit indiquer `db: true` et `s3: true`. En cas d'échec :

```sh
docker compose logs --tail=80 server proxy
```

Le stockage intégré compile MinIO depuis le commit
`9e49d5e7a648f00e26f2246f4dc28e6b07f8c84a`, qui contient le
[correctif de sécurité d'octobre 2025](https://github.com/minio/minio/releases/tag/RELEASE.2025-10-15T17-29-55Z).
Le projet amont est désormais archivé : cette option facilite l'alpha, mais
ne constitue pas une promesse de maintenance de MinIO. Pour une exploitation
durable, un service S3 maintenu peut remplacer ce composant (voir ci-dessous).

## 2. Créer votre compte et relier les appareils

Depuis `deploy/` :

```sh
docker compose exec -T server python -m app.cli new-token --label "ma-famille"
```

Copier le jeton affiché une seule fois dans un gestionnaire de mots de passe.
Dans **Settings** sur desktop et les paramètres Android, saisir
`https://saves.exemple.fr` et ce jeton. Ajouter ensuite les dossiers de
**sauvegardes** des émulateurs ; sur Android, utiliser le sélecteur de dossiers.

Un appel à `new-token` crée **un nouveau compte vide**. Ne pas générer un jeton
par appareil si ces appareils doivent synchroniser les mêmes sauvegardes.
La révocation d'un appareil ne révoque pas le jeton partagé. La rotation du
jeton d'un compte existant n'est pas encore fournie par le CLI de cette alpha.

Fermer l'émulateur, synchroniser un premier appareil, puis le second. Commencer
avec une copie de sauvegarde de test. Vérifier l'historique et un téléchargement
avant de confier une partie importante au service.

L'APK de release exige HTTPS avec un certificat reconnu par Android. Une URL
HTTP locale est destinée aux builds de développement ; elle ne remplace pas
le serveur HTTPS de ce guide. L'adresse `storage.exemple.fr` doit également
être joignable depuis chaque appareil, car les fichiers y sont transférés
directement par URL présignée.

## 3. Sauvegarder et restaurer

Le serveur conserve les métadonnées dans PostgreSQL et les archives dans le
stockage objet : sauvegarder seulement PostgreSQL ne suffit pas. Depuis
`deploy/`, choisir un dossier **qui n'existe pas encore** :

```sh
./backup.sh /srv/backups/retrosave-2026-09-14
```

Le script coupe temporairement l'API, l'accès public S3 et le stockage, produit
`database.dump`, `saves.tar.gz`, `config.env`, le commit source et les empreintes,
puis redémarre les services. Une erreur interrompt l'opération : ne considérer
la sauvegarde complète que si le script réussit et `SHA256SUMS.txt` existe.
Copier ce dossier vers un autre disque ou serveur, sous forme chiffrée : il
contient des sauvegardes privées et les secrets du serveur. Aucune rétention
automatique ne supprime ces copies.

Pour une restauration d'essai, utiliser le même commit source que celui du
backup et **un nouveau nom Compose** :

```sh
./restore.sh /srv/backups/retrosave-2026-09-14 retrosave-recovery
export RETROSAVE_ENV_FILE=/srv/backups/retrosave-2026-09-14/config.env
docker compose --project-name retrosave-recovery --env-file "$RETROSAVE_ENV_FILE" \
  exec -T server python -c \
  'import urllib.request; print(urllib.request.urlopen("http://localhost:8000/healthz").read().decode())'
```

Le script refuse tout volume déjà existant. Il lance le serveur restauré sans
proxy pour éviter une collision avec le serveur actuel. Vérifier aussi les
comptes, versions et une archive avant de basculer le trafic. Une restauration
réussie ne supprime pas les anciens volumes. Pour la bascule, arrêter le proxy
de l'ancien projet, puis lancer celui de `retrosave-recovery` avec les mêmes
options `--project-name`, `--env-file` et `RETROSAVE_ENV_FILE`.

## 4. Mettre à jour

Faire une sauvegarde complète et vérifier qu'elle est lisible. Consulter les
notes de la version, récupérer son tag puis reconstruire :

```sh
cd ..
git fetch --tags
git checkout <nouveau-tag>
cd deploy
docker compose --env-file .env up -d --build
```

Les migrations s'exécutent avant le démarrage de l'API. Un retour à une version
plus ancienne peut nécessiter la restauration de la base : ne pas présumer
qu'une migration est réversible. `docker compose down` conserve les volumes ;
**`down -v` les détruit** et ne fait pas partie d'une mise à jour.

## Utiliser un stockage S3 externe

L'API accepte un bucket S3 privé avec `S3_ENDPOINT`, `S3_REGION`, `S3_BUCKET`,
`S3_ACCESS_KEY_ID`, `S3_SECRET_ACCESS_KEY` et `S3_FORCE_PATH_STYLE`. Configurer
les clés limitées au bucket et les opérations GET/PUT/HEAD/LIST/DELETE nécessaires.
Laisser `S3_PUBLIC_ENDPOINT` vide si l'endpoint est déjà accessible aux clients.
Aucune réécriture d'hôte ou de chemin n'est possible après signature.

Dans une copie de `deploy/compose.yml`, supprimer `storage`, `createbucket`,
la dépendance à `createbucket` et le second site Caddy. Créer le bucket privé
chez le fournisseur avant de lancer le serveur. Les scripts `backup.sh` et
`restore.sh` couvrent le stockage intégré ; avec un S3 externe, ajouter une
sauvegarde des objets et une procédure de restauration propres à ce fournisseur.

## Limites de l'alpha

Pas de chiffrement de bout en bout ni de récupération du jeton depuis les
serveurs RetroSave. L'administrateur de votre serveur peut accéder aux données.
La conservation de l'historique n'a pas encore de quota automatique : surveiller
le disque. Les protections contre la perte de sauvegardes sont testées, mais
la recette V1 complète sur appareils réels reste à réaliser.
