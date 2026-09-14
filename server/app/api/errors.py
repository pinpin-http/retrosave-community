"""Frozen API error envelope and FastAPI handlers."""

from __future__ import annotations

import logging
from typing import Any

from fastapi import FastAPI, Request
from fastapi.encoders import jsonable_encoder
from fastapi.exceptions import RequestValidationError
from fastapi.responses import JSONResponse
from starlette.exceptions import HTTPException as StarletteHTTPException

logger = logging.getLogger(__name__)


class ApiError(Exception):
    """Expected API failure carrying a stable machine-readable code."""

    # ApiError est une exception "attendue" (auth échouée, ressource absente,
    # CAS raté). Elle est interceptée par handle_api_error() et convertie en
    # réponse JSON sans logger de stacktrace — ce n'est pas un bug.
    # Contrairement à Exception générique, elle porte un code machine stable
    # que les clients peuvent tester de façon fiable (ex. "cas_conflict").

    def __init__(
        self,
        status_code: int,
        code: str,  # Code machine stable (ex. "not_found", "cas_conflict").
        message: str,  # Message lisible (ex. "Unité introuvable.").
        *,
        # details : informations structurées supplémentaires (ex. erreurs de validation).
        details: dict[str, Any] | None = None,
        # extra : champs ajoutés au niveau racine de la réponse, en dehors de l'enveloppe
        # "error". Utilisé pour les conflits : on veut {error:{...}, conflict_id:...,
        # head:{...}, yours:{...}} plutôt que tout dans details.
        extra: dict[str, Any] | None = None,
    ) -> None:
        super().__init__(message)
        self.status_code = status_code
        self.code = code
        self.message = message
        self.details = details or {}
        self.extra = extra or {}

    def payload(self) -> dict[str, Any]:
        """Return the exact common error envelope plus endpoint-specific data."""

        # Format de toutes les erreurs API (§16.2) :
        # {"error": {"code": "...", "message": "...", "details": {...}}, ...extra}
        return {
            "error": {
                "code": self.code,
                "message": self.message,
                "details": self.details,
            },
            # **extra fusionne les champs additionnels (conflict_id, head, yours)
            # à la racine du JSON, pas dans l'enveloppe "error".
            **self.extra,
        }


def install_error_handlers(app: FastAPI) -> None:
    """Install stable handlers for expected and validation failures."""

    @app.exception_handler(ApiError)
    async def handle_api_error(
        _request: Request,
        error: ApiError,
    ) -> JSONResponse:
        # Erreur attendue → réponse JSON propre, pas de log de stacktrace.
        return JSONResponse(error.payload(), status_code=error.status_code)

    @app.exception_handler(RequestValidationError)
    async def handle_validation_error(
        _request: Request,
        error: RequestValidationError,
    ) -> JSONResponse:
        # Pydantic a rejeté le corps de la requête (champ manquant, type invalide...).
        # On normalise dans le format d'erreur API standard avec les détails Pydantic.
        payload = ApiError(
            422,
            "validation_error",
            "Requête invalide.",
            details={"errors": error.errors()},
        ).payload()
        return JSONResponse(jsonable_encoder(payload), status_code=422)

    @app.exception_handler(StarletteHTTPException)
    async def handle_not_found(
        _request: Request,
        error: StarletteHTTPException,
    ) -> JSONResponse:
        # Intercepte les 404 levés par FastAPI lui-même (route inconnue) pour les
        # convertir au format API standard. Les autres codes (405, etc.) sont re-levés
        # et gérés par Starlette avec son format par défaut.
        if error.status_code == 404:
            payload = ApiError(
                404,
                "not_found",
                "Ressource introuvable.",
            ).payload()
            return JSONResponse(payload, status_code=404)
        raise error

    @app.exception_handler(Exception)
    async def handle_server_error(
        _request: Request,
        error: Exception,
    ) -> JSONResponse:
        # Dernier filet : toute exception non prévue (bug, assertion, import raté...).
        # On logue la stacktrace complète (pour les alertes et le debug) mais on
        # retourne un message générique au client — les détails internes ne doivent
        # pas fuiter (sécurité + UX).
        logger.exception(
            "server_error",
            exc_info=error,
            extra={"error_code": "server_error"},
        )
        payload = ApiError(
            500,
            "server_error",
            "Erreur interne du serveur.",
        ).payload()
        return JSONResponse(payload, status_code=500)
