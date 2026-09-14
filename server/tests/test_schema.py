"""Migration contract tests for the frozen PostgreSQL schema."""

from app.db.session import get_engine
from sqlalchemy import inspect


async def test_initial_migration_has_only_frozen_application_tables() -> None:
    """M1 must not add a table outside ARCHITECTURE.md section 7.2."""

    async with get_engine().connect() as connection:
        table_names = await connection.run_sync(
            lambda sync_connection: inspect(sync_connection).get_table_names()
        )

    assert set(table_names) == {
        "alembic_version",
        "conflicts",
        "devices",
        "idempotency_keys",
        "pending_uploads",
        "save_units",
        "save_versions",
        "users",
    }


async def test_initial_migration_columns_match_reference() -> None:
    """Every migration-owned table exposes exactly the frozen columns."""

    expected = {
        "users": {
            "id",
            "invite_token_hash",
            "label",
            "created_at",
            "disabled_at",
            # CLD-01 : l'identifiant du compte chez le fournisseur d'identité.
            # Nul pour l'auto-hébergement, qui reste authentifié par jeton.
            "auth_subject",
        },
        "devices": {
            "id",
            "user_id",
            "name",
            "os",
            "app_version",
            "created_at",
            "last_seen_at",
            # EXP-01 : ajout additif et nullable. Un appareil révoqué garde sa
            # ligne — ses versions existent toujours et l'historique doit
            # pouvoir les attribuer (invariant I2).
            "revoked_at",
        },
        "save_units": {
            "id",
            # M8 §7 : ajout additif, valeur par défaut 'auto' pour l'existant.
            "label_source",
            "user_id",
            "emulator",
            "unit_key",
            "unit_type",
            "game_key",
            "game_label",
            "head_version",
            "state",
            "created_at",
            "updated_at",
        },
        "save_versions": {
            "id",
            "unit_id",
            "number",
            "parent_number",
            "content_sha256",
            "archive_sha256",
            "size_bytes",
            "archive_bytes",
            "object_key",
            "origin_device",
            "env",
            "client_mtime",
            "kind",
            "created_at",
        },
        "conflicts": {
            "id",
            "unit_id",
            "version_a",
            "version_b",
            "status",
            "winner",
            "created_at",
            "resolved_at",
        },
        "pending_uploads": {
            "object_key",
            "unit_id",
            "content_sha256",
            "archive_sha256",
            "size_bytes",
            "archive_bytes",
            "created_at",
        },
        "idempotency_keys": {
            "key",
            "user_id",
            "status_code",
            "response",
            "created_at",
        },
    }

    async with get_engine().connect() as connection:
        actual = await connection.run_sync(
            lambda sync_connection: {
                table: {column["name"] for column in inspect(sync_connection).get_columns(table)}
                for table in expected
            }
        )

    assert actual == expected
