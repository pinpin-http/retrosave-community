"""Accepter les cinq adaptateurs livrés mais jamais déclarés.

mGBA, Snes9x, Dolphin, DuckStation et PCSX2 étaient installés, détectés et
affichés côté clients, mais la contrainte `CHECK` de `save_units` — écrite au
tout premier jour du POC — n'avait jamais bougé. Une sauvegarde Golden Sun
apparaissait dans la bibliothèque et n'était jamais poussée.

Rien à convertir : la contrainte est ÉLARGIE, donc aucune ligne existante ne
peut la violer. Le retour arrière est le seul moment délicat, et il est traité
ci-dessous.

Revision ID: 0007_supported_emulators
Revises: 0006_account_subjects
Create Date: 2026-09-13
"""

from collections.abc import Sequence

from alembic import op
from app.core.emulators import check_constraint

revision: str = "0007_supported_emulators"
down_revision: str | None = "0006_account_subjects"
branch_labels: str | Sequence[str] | None = None
depends_on: str | Sequence[str] | None = None

#: Le nom donné par PostgreSQL à la contrainte en ligne de `0001_initial`.
NAME = "save_units_emulator_check"

LEGACY = "emulator IN ('ppsspp','melonds','azahar','retroarch','folder')"


def upgrade() -> None:
    op.execute(f"ALTER TABLE save_units DROP CONSTRAINT IF EXISTS {NAME}")
    op.execute(f"ALTER TABLE save_units ADD CONSTRAINT {NAME} CHECK ({check_constraint()})")


def downgrade() -> None:
    # Restreindre une contrainte, c'est refuser des lignes qui existent. On ne
    # les EFFACE pas — invariant I2 : rien de ce qu'un joueur a synchronisé ne
    # disparaît sur un retour arrière de schéma. La contrainte est donc reposée
    # `NOT VALID` : elle s'applique aux écritures futures et laisse l'existant
    # en place, lisible et téléchargeable.
    op.execute(f"ALTER TABLE save_units DROP CONSTRAINT IF EXISTS {NAME}")
    op.execute(f"ALTER TABLE save_units ADD CONSTRAINT {NAME} CHECK ({LEGACY}) NOT VALID")
