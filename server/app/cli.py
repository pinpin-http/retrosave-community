"""Administrative server CLI for one-time invitation-token creation."""

# Utilisé via : `docker compose exec server python -m app.cli new-token --label mon-compte`
# (ou via la cible Makefile `make token LABEL=mon-compte`)
#
# Ce CLI est le seul moyen de créer un utilisateur dans le POC.
# Pas d'interface web, pas d'inscription — intentionnel (AD-07).

from __future__ import annotations

import argparse
import asyncio
from collections.abc import Sequence

from app.db.models import User
from app.db.session import get_session_factory
from app.security import generate_invite_token


async def create_user_token(label: str) -> str:
    """Persist only the token digest and return plaintext exactly once."""

    # generate_invite_token() retourne le tuple (plaintext, hash).
    # On ne stocke que le hash en base — le plaintext n'existe qu'en mémoire
    # le temps de le printer, puis il disparaît (AD-07).
    token, token_hash = generate_invite_token()
    async with get_session_factory()() as db:
        db.add(User(invite_token_hash=token_hash, label=label))
        await db.commit()
    # Retourne le plaintext pour que main() l'affiche à l'admin via print().
    # C'est la SEULE occasion de voir ce token — si l'admin le perd, il doit
    # en générer un nouveau.
    return token


def build_parser() -> argparse.ArgumentParser:
    """Build the single-command administrative interface."""

    parser = argparse.ArgumentParser(prog="python -m app.cli")
    commands = parser.add_subparsers(dest="command", required=True)
    new_token = commands.add_parser("new-token")
    # --label est le nom lisible du compte (ex. 'mon-compte', 'testeur-1').
    # Stocké en clair dans la colonne users.label — pas un secret.
    new_token.add_argument("--label", required=True)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    """Execute one administrative command."""

    args = build_parser().parse_args(argv)
    if args.command == "new-token":
        # print() affiche le token sur stdout. L'admin le copie-colle dans
        # la config du client (Android ou CLI) — jamais dans un fichier versionné.
        print(asyncio.run(create_user_token(args.label)))
        return 0
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
