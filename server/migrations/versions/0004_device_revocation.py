"""Add devices.revoked_at (EXP-01).

Additive and non-destructive: the column is nullable, so every existing device
stays exactly as it was — active. Revoking a device never touches its versions,
its conflicts or its archives (invariant I2): it only stops that device_id from
being accepted on new requests.

What this does NOT do, and it matters: authentication is still one invitation
token per user, shared by every device. Revoking a device refuses that
registration; it does not invalidate the token, so whoever holds the token can
register a new device. Full per-device credentials belong to the accounts work
(CLD-01) — see Q46.

Revision ID: 0004_device_revocation
Revises: 0003_backfill_3ds_labels
Create Date: 2026-09-12
"""

from collections.abc import Sequence

from alembic import op

revision: str = "0004_device_revocation"
down_revision: str | None = "0003_backfill_3ds_labels"
branch_labels: str | Sequence[str] | None = None
depends_on: str | Sequence[str] | None = None


def upgrade() -> None:
    op.execute("ALTER TABLE devices ADD COLUMN revoked_at timestamptz")


def downgrade() -> None:
    op.execute("ALTER TABLE devices DROP COLUMN revoked_at")
