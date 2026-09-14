#!/usr/bin/env bash
# Restaure uniquement dans de nouveaux volumes ; le proxy reste arrêté pour la recette.
set -euo pipefail
umask 077
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
backup=${1:?Usage: ./restore.sh /chemin/sauvegarde nouveau-projet}
project=${2:?Un nouveau nom de projet Compose est obligatoire}
[[ "$project" =~ ^[a-z0-9][a-z0-9_-]+$ ]] || { echo 'Nom de projet invalide' >&2; exit 2; }
backup=$(cd -- "$backup" && pwd)
(cd "$backup" && sha256sum --check SHA256SUMS.txt)
# Refuser même les volumes arrêtés : aucun état existant ne doit être remplacé.
for suffix in dbdata saves certificates caddyconfig; do
    if docker volume inspect "${project}_${suffix}" >/dev/null 2>&1; then
        echo "Volume existant : ${project}_${suffix}. Choisir un nouveau projet." >&2
        exit 1
    fi
done
export RETROSAVE_ENV_FILE="$backup/config.env"
compose=(docker compose --project-name "$project" --env-file "$RETROSAVE_ENV_FILE" -f compose.yml)
"${compose[@]}" up -d --wait db
"${compose[@]}" exec -T db pg_restore --exit-on-error -U retrosave -d retrosave < "$backup/database.dump"
"${compose[@]}" run --rm --no-deps --entrypoint tar storage -C /data -xzf - < "$backup/saves.tar.gz"
"${compose[@]}" up -d --wait server
printf 'Restauration prête dans %s ; proxy arrêté. Vérifier /healthz avant la bascule.\n' "$project"
printf 'Conserver RETROSAVE_ENV_FILE=%s pour administrer ce projet.\n' "$RETROSAVE_ENV_FILE"
