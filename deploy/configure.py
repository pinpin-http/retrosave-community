#!/usr/bin/env python3
"""Create a private deployment configuration without overwriting existing secrets."""
from __future__ import annotations

import argparse
import os
import re
import secrets
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--api-domain', required=True)
parser.add_argument('--storage-domain', required=True)
parser.add_argument('--email', required=True)
args = parser.parse_args()
for domain in (args.api_domain, args.storage_domain):
    if not re.fullmatch(r'[a-zA-Z0-9](?:[a-zA-Z0-9.-]*[a-zA-Z0-9])?', domain) or '.' not in domain:
        parser.error('Utiliser un nom DNS sans protocole, port ou chemin.')
if args.api_domain == args.storage_domain:
    parser.error('Deux noms DNS distincts sont nécessaires.')
if not re.fullmatch(r'[^\s=@]+@[^\s=@]+\.[^\s=@]+', args.email):
    parser.error('Adresse email invalide.')
password, secret = secrets.token_hex(32), secrets.token_hex(32)
values = {
    'API_DOMAIN': args.api_domain, 'STORAGE_DOMAIN': args.storage_domain,
    'ACME_EMAIL': args.email, 'POSTGRES_PASSWORD': password, 'APP_ENV': 'prod',
    'DATABASE_URL': f'postgresql+asyncpg://retrosave:{password}@db:5432/retrosave',
    'S3_ENDPOINT': 'http://storage:9000', 'S3_PUBLIC_ENDPOINT': f'https://{args.storage_domain}',
    'S3_REGION': 'us-east-1', 'S3_BUCKET': 'retrosave', 'S3_ACCESS_KEY_ID': 'retrosave',
    'S3_SECRET_ACCESS_KEY': secret, 'S3_FORCE_PATH_STYLE': 'true', 'LOG_LEVEL': 'INFO',
}
path = Path(__file__).resolve().parent / '.env'
try:
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
except FileExistsError:
    parser.exit(1, '.env existe déjà : configuration conservée, aucun secret remplacé.\n')
with os.fdopen(descriptor, 'w') as stream:
    stream.write(''.join(f'{key}={value}\n' for key, value in values.items()))
print(f'Configuration créée : {path} (secrets non affichés).')
