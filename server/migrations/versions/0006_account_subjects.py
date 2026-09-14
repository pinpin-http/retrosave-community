"""Accounts for the hosted offer (CLD-01).

Deux changements, tous deux additifs :

- `users.auth_subject` : l'identifiant stable du compte chez le fournisseur
  d'identité. Nul pour un utilisateur d'auto-hébergement, qui continue de
  s'authentifier par jeton d'invitation ;
- `users.invite_token_hash` devient **nullable** : un compte hébergé n'a pas de
  jeton d'invitation, et lui en fabriquer un faux reviendrait à créer un second
  secret utilisable, donc une seconde surface d'attaque.

L'unicité des deux colonnes est conservée. PostgreSQL autorise plusieurs NULL
dans un index unique : les comptes hébergés cohabitent sans se gêner.

Rien n'est réécrit : les utilisateurs existants gardent leur jeton et n'ont
pas de sujet.

Revision ID: 0006_account_subjects
Revises: 0005_idempotency_per_user
Create Date: 2026-09-12
"""

from collections.abc import Sequence

from alembic import op

revision: str = "0006_account_subjects"
down_revision: str | None = "0005_idempotency_per_user"
branch_labels: str | Sequence[str] | None = None
depends_on: str | Sequence[str] | None = None


def upgrade() -> None:
    op.execute("ALTER TABLE users ADD COLUMN auth_subject text UNIQUE")
    op.execute("ALTER TABLE users ALTER COLUMN invite_token_hash DROP NOT NULL")


def downgrade() -> None:
    # On ne peut pas rendre la colonne obligatoire tant qu'existent des comptes
    # qui n'ont légitimement pas de jeton : ils sont retirés d'abord, avec leurs
    # données, par la cascade déjà déclarée sur `users`.
    op.execute("DELETE FROM users WHERE invite_token_hash IS NULL")
    op.execute("ALTER TABLE users ALTER COLUMN invite_token_hash SET NOT NULL")
    op.execute("ALTER TABLE users DROP COLUMN auth_subject")
