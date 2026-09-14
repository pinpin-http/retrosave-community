#!/usr/bin/env bash
# Sauvegarde cohérente : couper API et accès S3 avant de copier les deux magasins.
set -euo pipefail
umask 077
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
destination=${1:?Usage: ./backup.sh /chemin/vers/un-nouveau-dossier}
mkdir -- "$destination"
destination=$(cd -- "$destination" && pwd)
compose=(docker compose --project-name "${RSC_COMPOSE_PROJECT:-retrosave-community}" \
    --env-file .env -f compose.yml)
# Arrêter le proxy bloque aussi les URL S3 présignées encore valides.
restart() { "${compose[@]}" up -d storage server proxy; }
trap restart EXIT
"${compose[@]}" stop proxy server storage
"${compose[@]}" exec -T db pg_dump -U retrosave -d retrosave -Fc > "$destination/database.dump.part"
"${compose[@]}" run --rm --no-deps --entrypoint tar storage -C /data -czf - . > "$destination/saves.tar.gz.part"
cp .env "$destination/config.env"
git -C .. rev-parse HEAD > "$destination/source-commit.txt"
mv "$destination/database.dump.part" "$destination/database.dump"
mv "$destination/saves.tar.gz.part" "$destination/saves.tar.gz"
(cd "$destination" && sha256sum database.dump saves.tar.gz config.env source-commit.txt > SHA256SUMS.txt)
printf 'Sauvegarde terminée : %s\n' "$destination"
