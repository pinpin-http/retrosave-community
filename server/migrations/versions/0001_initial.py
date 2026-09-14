"""Create the frozen RetroSave POC schema.

Revision ID: 0001_initial
Revises:
Create Date: 2026-07-30
"""

from collections.abc import Sequence

from alembic import op

revision: str = "0001_initial"
down_revision: str | None = None
branch_labels: str | Sequence[str] | None = None
depends_on: str | Sequence[str] | None = None


CREATE_STATEMENTS = (
    "CREATE EXTENSION IF NOT EXISTS pgcrypto",
    """
    CREATE TABLE users (
      id uuid PRIMARY KEY DEFAULT gen_random_uuid(),
      invite_token_hash bytea UNIQUE NOT NULL,
      label text NOT NULL,
      created_at timestamptz NOT NULL DEFAULT now(),
      disabled_at timestamptz
    )
    """,
    """
    CREATE TABLE devices (
      id uuid PRIMARY KEY DEFAULT gen_random_uuid(),
      user_id uuid NOT NULL REFERENCES users(id) ON DELETE CASCADE,
      name text NOT NULL,
      os text NOT NULL CHECK (os IN ('android','windows','linux')),
      app_version text NOT NULL DEFAULT '',
      created_at timestamptz NOT NULL DEFAULT now(),
      last_seen_at timestamptz
    )
    """,
    "CREATE INDEX devices_user_idx ON devices(user_id)",
    """
    CREATE TABLE save_units (
      id uuid PRIMARY KEY DEFAULT gen_random_uuid(),
      user_id uuid NOT NULL REFERENCES users(id) ON DELETE CASCADE,
      emulator text NOT NULL
        CHECK (emulator IN ('ppsspp','melonds','azahar','retroarch','folder')),
      unit_key text NOT NULL,
      unit_type text NOT NULL CHECK (unit_type IN ('file','dir')),
      game_key text NOT NULL,
      game_label text NOT NULL,
      head_version int NOT NULL DEFAULT 0,
      state text NOT NULL DEFAULT 'active'
        CHECK (state IN ('active','missing')),
      created_at timestamptz NOT NULL DEFAULT now(),
      updated_at timestamptz NOT NULL DEFAULT now(),
      UNIQUE (user_id, emulator, unit_key)
    )
    """,
    """
    CREATE INDEX save_units_user_updated_idx
    ON save_units(user_id, updated_at)
    """,
    """
    CREATE TABLE save_versions (
      id uuid PRIMARY KEY DEFAULT gen_random_uuid(),
      unit_id uuid NOT NULL REFERENCES save_units(id) ON DELETE CASCADE,
      number int NOT NULL,
      parent_number int,
      content_sha256 bytea NOT NULL,
      archive_sha256 bytea NOT NULL,
      size_bytes bigint NOT NULL CHECK (size_bytes >= 0),
      archive_bytes bigint NOT NULL CHECK (archive_bytes >= 0),
      object_key text NOT NULL,
      origin_device uuid REFERENCES devices(id) ON DELETE SET NULL,
      env jsonb NOT NULL DEFAULT '{}',
      client_mtime timestamptz,
      kind text NOT NULL DEFAULT 'normal'
        CHECK (kind IN ('normal','restore','conflict_branch')),
      created_at timestamptz NOT NULL DEFAULT now(),
      UNIQUE (unit_id, number)
    )
    """,
    """
    CREATE INDEX save_versions_unit_content_idx
    ON save_versions(unit_id, content_sha256)
    """,
    "CREATE INDEX save_versions_object_key_idx ON save_versions(object_key)",
    """
    CREATE TABLE conflicts (
      id uuid PRIMARY KEY DEFAULT gen_random_uuid(),
      unit_id uuid NOT NULL REFERENCES save_units(id) ON DELETE CASCADE,
      version_a int NOT NULL,
      version_b int NOT NULL,
      status text NOT NULL DEFAULT 'open'
        CHECK (status IN ('open','resolved')),
      winner int,
      created_at timestamptz NOT NULL DEFAULT now(),
      resolved_at timestamptz
    )
    """,
    """
    CREATE INDEX conflicts_unit_open_idx
    ON conflicts(unit_id) WHERE status = 'open'
    """,
    """
    CREATE TABLE pending_uploads (
      object_key text PRIMARY KEY,
      unit_id uuid NOT NULL REFERENCES save_units(id) ON DELETE CASCADE,
      content_sha256 bytea NOT NULL,
      archive_sha256 bytea NOT NULL,
      size_bytes bigint NOT NULL,
      archive_bytes bigint NOT NULL,
      created_at timestamptz NOT NULL DEFAULT now()
    )
    """,
    """
    CREATE TABLE idempotency_keys (
      key text PRIMARY KEY,
      user_id uuid NOT NULL,
      status_code int NOT NULL,
      response jsonb NOT NULL,
      created_at timestamptz NOT NULL DEFAULT now()
    )
    """,
)


def upgrade() -> None:
    """Create exactly the schema frozen in ARCHITECTURE.md section 7.2."""

    for statement in CREATE_STATEMENTS:
        op.execute(statement)


def downgrade() -> None:
    """Remove all application tables in dependency order."""

    for table in (
        "idempotency_keys",
        "pending_uploads",
        "conflicts",
        "save_versions",
        "save_units",
        "devices",
        "users",
    ):
        op.execute(f"DROP TABLE {table}")
