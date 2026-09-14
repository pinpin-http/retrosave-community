"""M0 contract tests for the public health endpoint."""

import pytest
from app.api import health
from httpx import AsyncClient


async def test_healthz_reports_database_and_storage(
    client: AsyncClient,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """The nominal response must match the frozen API contract exactly."""

    async def available() -> bool:
        return True

    monkeypatch.setattr(health, "check_database", available)
    monkeypatch.setattr(health, "check_storage", available)

    response = await client.get("/healthz")

    assert response.status_code == 200
    from app.version import app_version

    assert response.json() == {
        "status": "ok",
        "db": True,
        "s3": True,
        "version": app_version(),
    }


async def test_browser_documentation_endpoints_are_not_exposed(
    client: AsyncClient,
) -> None:
    """The frozen API contains no browser or OpenAPI endpoints."""

    for path in ("/docs", "/redoc", "/openapi.json"):
        response = await client.get(path)
        assert response.status_code == 404
        assert response.json()["error"]["code"] == "not_found"


async def test_unexpected_failure_uses_server_error_envelope(
    client: AsyncClient,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """Unexpected failures never expose a raw stacktrace to API clients."""

    async def unavailable() -> bool:
        raise RuntimeError("private detail")

    monkeypatch.setattr(health, "check_database", unavailable)
    monkeypatch.setattr(health, "check_storage", unavailable)

    response = await client.get("/healthz")

    assert response.status_code == 500
    assert response.json() == {
        "error": {
            "code": "server_error",
            "message": "Erreur interne du serveur.",
            "details": {},
        }
    }
    assert "private detail" not in response.text


async def test_healthz_announces_its_version(client: AsyncClient) -> None:
    """Q28 : informatif, et lu depuis la source unique du dépôt.

    Le champ est optionnel côté schéma pour que les clients antérieurs à Q28 ne
    voient aucune différence, et les lecteurs tolèrent les champs inconnus
    (AD-31) — c'est cette tolérance, pas l'optionalité, qui rend l'ajout sûr.
    """

    from app.version import app_version

    response = await client.get("/healthz")

    assert response.status_code == 200
    assert response.json()["version"] == app_version()
    assert app_version() != "inconnue", "le fichier VERSION doit être lisible"


def test_the_version_source_is_reported_alongside_the_version() -> None:
    """AD-32 : dire d'où vient la version, pas seulement laquelle.

    L'écart dev/image de Q30 n'était invisible que faute d'être annoncé. Sans
    la source, deux déploiements qui divergent se ressemblent.
    """

    from app.version import app_version, version_source

    assert version_source() != "aucune source"
    assert app_version() != "0.0.0", "0.0.0 signale une source introuvable"


def test_an_unreachable_version_file_degrades_honestly(monkeypatch, tmp_path) -> None:
    """Une version absente doit se voir, pas se deviner."""

    from app import version as version_module

    version_module.app_version.cache_clear()
    monkeypatch.setenv("RETROSAVE_VERSION_FILE", str(tmp_path / "absent"))
    try:
        assert version_module.app_version() == "0.0.0"
        assert version_module.version_source() == "aucune source"
    finally:
        version_module.app_version.cache_clear()
