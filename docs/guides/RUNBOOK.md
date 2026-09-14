# Runbook de développement

Ce document couvre l'environnement local et les tests. Pour un serveur
accessible aux clients de release, suivre
[le guide d'auto-hébergement](SELF_HOSTING.md).

## Serveur local

Depuis la racine :

```sh
cp .env.example .env
docker compose up -d --build
docker compose ps
curl --fail http://127.0.0.1:8000/healthz
make token LABEL=developpement
```

Les ports PostgreSQL, MinIO et Adminer sont limités à la boucle locale. Le
serveur HTTP local convient au développement desktop. L'APK de release exige
HTTPS ; utiliser le déploiement décrit dans `SELF_HOSTING.md`.

Arrêter sans supprimer les volumes :

```sh
docker compose down
```

Ne pas employer `docker compose down -v` si l'état local doit être conservé.

## Tests serveur

`server/tests` vide toutes les tables de sa base cible. Le point d'entrée sûr
crée ses propres conteneurs et les détruit à la fin :

```sh
make test-server
```

Lancer directement `pytest server/tests` exige une base jetable et
`RETROSAVE_TEST_DB=1`. Cette variable confirme le caractère destructible ;
elle ne rend pas une base existante sûre.

## Tests des clients

```sh
make test
make desktop
make android
```

Les bancs complets employés par la CI sont dans `scripts/testing/`. Ils
utilisent des ports aléatoires sur loopback, des volumes tmpfs et des contenus
synthétiques. Ils ne doivent jamais pointer vers une base, un stockage ou un
dossier de sauvegardes personnel.

## Diagnostic

```sh
docker compose ps
docker compose logs --tail=100 server db minio
curl --fail http://127.0.0.1:8000/healthz
```

Un résultat `db: false` indique un problème PostgreSQL. `s3: false` indique
le stockage ou ses identifiants. Ne publier ni `.env`, ni un jeton, ni une URL
présignée dans un rapport. L'export de diagnostic du client exclut le contenu
des sauvegardes et les secrets.

## Données et récupération

Le développement local ne constitue pas une sauvegarde. La procédure prise en
charge pour un serveur réel se trouve dans
[SELF_HOSTING.md](SELF_HOSTING.md#3-sauvegarder-et-restaurer). Elle capture
PostgreSQL, les objets, les secrets de configuration et le commit source, puis
refuse de restaurer dans des volumes existants.
