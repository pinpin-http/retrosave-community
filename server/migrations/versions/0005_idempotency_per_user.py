"""Scope idempotency keys to their account.

Défaut constaté le 12/09/2026. Les clients dérivent leur `Idempotency-Key` de
l'identité de l'opération — pour une déclaration d'unité, de l'émulateur et de
la `unit_key`. Or une `unit_key` n'est unique que dans un compte :
`ULJM05800DATA00` désigne le même dossier de sauvegarde chez tous ceux qui
jouent à ce jeu. Deux comptes produisaient donc la même clé, et le second
recevait `403 not_owner` pendant vingt-quatre heures.

La clé primaire devient `(user_id, key)`. Deux comptes peuvent porter la même
clé sans se voir ; à l'intérieur d'un compte, l'idempotence est inchangée.

**Sans perte.** Les lignes existantes portent déjà leur `user_id` : la clé
primaire est simplement élargie, aucune donnée n'est réécrite ni supprimée. Ces
lignes expirent de toute façon en 24 h (job `purge_idempotency_keys`).

Revision ID: 0005_idempotency_per_user
Revises: 0004_device_revocation
Create Date: 2026-09-12
"""

from collections.abc import Sequence

from alembic import op

revision: str = "0005_idempotency_per_user"
down_revision: str | None = "0004_device_revocation"
branch_labels: str | Sequence[str] | None = None
depends_on: str | Sequence[str] | None = None


def upgrade() -> None:
    op.execute("ALTER TABLE idempotency_keys DROP CONSTRAINT idempotency_keys_pkey")
    op.execute(
        "ALTER TABLE idempotency_keys "
        "ADD CONSTRAINT idempotency_keys_pkey PRIMARY KEY (user_id, key)"
    )


def downgrade() -> None:
    # Le retour arrière ne peut pas inventer un arbitrage entre deux comptes
    # portant la même clé : on garde la plus ancienne, la seule qu'un serveur
    # d'avant cette migration aurait pu servir.
    op.execute(
        "DELETE FROM idempotency_keys a USING idempotency_keys b "
        "WHERE a.key = b.key AND a.created_at > b.created_at"
    )
    op.execute("ALTER TABLE idempotency_keys DROP CONSTRAINT idempotency_keys_pkey")
    op.execute(
        "ALTER TABLE idempotency_keys ADD CONSTRAINT idempotency_keys_pkey PRIMARY KEY (key)"
    )
