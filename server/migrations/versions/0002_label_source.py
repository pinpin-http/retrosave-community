"""Add save_units.label_source (M8 section 7).

Additive by construction: the column has a default, so every existing row keeps
its label and becomes 'auto'. That is the honest value — those labels were
posed by adapters, never typed by anyone.

Revision ID: 0002_label_source
Revises: 0001_initial
Create Date: 2026-08-03
"""

from collections.abc import Sequence

from alembic import op

revision: str = "0002_label_source"
down_revision: str | None = "0001_initial"
branch_labels: str | Sequence[str] | None = None
depends_on: str | Sequence[str] | None = None


def upgrade() -> None:
    op.execute(
        "ALTER TABLE save_units "
        "ADD COLUMN label_source text NOT NULL DEFAULT 'auto' "
        "CHECK (label_source IN ('auto','user'))"
    )


def downgrade() -> None:
    op.execute("ALTER TABLE save_units DROP COLUMN label_source")
