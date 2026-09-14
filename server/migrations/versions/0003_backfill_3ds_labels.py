"""Name the Azahar units that predate M8 (M8 sections 6 and 10).

Resolution at creation only ever helps future units. The eight Azahar units on
the Thor were declared long before the table existed, so without this they would
display their titleid forever.

Section 10 allows this precisely because commit 3 landed first: the update
touches only rows that are still `label_source='auto'` **and** still carry their
raw identifier. A row a human renamed, or that any client already resolved, is
left alone — that is the whole point of the guard.

The pure selection helpers are imported rather than copied. They read a JSON
file and know nothing about the ORM, so they cannot drift with the schema, and
duplicating the region rule here would let the backfill and the live path
disagree about which name a game has.

Revision ID: 0003_backfill_3ds_labels
Revises: 0002_label_source
Create Date: 2026-08-03
"""

from collections.abc import Sequence

from alembic import op
from app.core.titles_3ds import candidates_for, infer_account_region, pick_name
from sqlalchemy import text

revision: str = "0003_backfill_3ds_labels"
down_revision: str | None = "0002_label_source"
branch_labels: str | Sequence[str] | None = None
depends_on: str | Sequence[str] | None = None


def upgrade() -> None:
    connection = op.get_bind()
    rows = connection.execute(
        text(
            "SELECT id, user_id, unit_key, game_label FROM save_units "
            "WHERE emulator = 'azahar' AND label_source = 'auto'"
        )
    ).all()

    by_user: dict[str, list] = {}
    for row in rows:
        by_user.setdefault(str(row.user_id), []).append(row)

    for user_rows in by_user.values():
        # La région se lit des libellés déjà résolus du compte : un compte
        # européen ne doit pas se retrouver avec un jeu nommé en japonais.
        region = infer_account_region({row.unit_key: row.game_label for row in user_rows})
        for row in user_rows:
            if row.game_label != row.unit_key:
                continue
            name = pick_name(candidates_for(row.unit_key), region)
            if not name or name == row.game_label:
                continue
            connection.execute(
                text("UPDATE save_units SET game_label = :label WHERE id = :id"),
                {"label": name, "id": row.id},
            )


def downgrade() -> None:
    """Irréversible par nature : on ne sait pas quel identifiant remettre.

    Rien n'est perdu pour autant — `unit_key` porte toujours le `titleid_low`,
    et c'est lui qui identifie l'unité.
    """
