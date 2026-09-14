"""One-line JSON logging with mandatory invitation-token redaction."""

from __future__ import annotations

import json
import logging
from datetime import UTC, datetime
from typing import Any

from app.security import redact_tokens

# Champs structurés ajoutés au payload JSON si présents dans le LogRecord.
# Passés via `logger.info("event", extra={"user_id": user.id})`.
# On liste explicitement les champs autorisés plutôt que d'inclure tout extra
# pour éviter de logguer accidentellement des données sensibles.
OPTIONAL_FIELDS = (
    "user_id",
    "device_id",
    "unit_id",
    "duration_ms",
    "error_code",
)


class TokenRedactionFilter(logging.Filter):
    """Redact invitation tokens before any handler formats a record."""

    def filter(self, record: logging.LogRecord) -> bool:
        # On remplace record.msg par le message formaté + redacté AVANT que
        # le handler ne le formate à nouveau. Ensuite record.args est vidé pour
        # empêcher le handler de reconstruire le message original avec % formatting.
        # Sans record.args = (), l'handler appliquerait record.msg % record.args
        # et le token redacté dans msg serait remplacé par l'original dans args.
        record.msg = redact_tokens(record.getMessage())
        record.args = ()
        return True  # True = continuer à traiter l'enregistrement.


class JsonFormatter(logging.Formatter):
    """Render the frozen log envelope as one compact JSON object."""

    def format(self, record: logging.LogRecord) -> str:
        payload: dict[str, Any] = {
            "ts": datetime.now(UTC).isoformat().replace("+00:00", "Z"),
            "level": record.levelname.lower(),
            # redact_tokens() appelé une deuxième fois par sécurité, car certains
            # handlers (ex. Uvicorn) peuvent contourner le filtre.
            "event": redact_tokens(record.getMessage()),
        }
        # Inclut uniquement les champs structurés connus — ignore tous les autres
        # attributs du record pour ne pas exposer de données internes inattendues.
        for field in OPTIONAL_FIELDS:
            value = getattr(record, field, None)
            if value is not None:
                payload[field] = str(value)
        # separators=(",", ":") : JSON compact sans espaces — réduit la taille
        # des logs. ensure_ascii=False : conserve les caractères UTF-8 (noms de jeux).
        return json.dumps(payload, separators=(",", ":"), ensure_ascii=False)


def configure_logging(level: str) -> None:
    """Configure the root logger once for API and scheduler events."""

    handler = logging.StreamHandler()
    handler.addFilter(TokenRedactionFilter())
    handler.setFormatter(JsonFormatter())

    # On remplace les handlers existants du root logger (dont celui de uvicorn
    # par défaut) pour que TOUS les logs passent par notre JSON formatter.
    root = logging.getLogger()
    root.handlers.clear()
    root.addHandler(handler)
    root.setLevel(level.upper())

    # Redirige les loggers uvicorn vers le root logger (propagate=True) pour
    # qu'ils passent par notre JSON formatter au lieu de leur handler texte natif.
    # handlers.clear() empêche le double log (uvicorn + root).
    for logger_name in ("uvicorn", "uvicorn.error", "uvicorn.access"):
        framework_logger = logging.getLogger(logger_name)
        framework_logger.handlers.clear()
        framework_logger.propagate = True

    # boto3/botocore/urllib3 sont très verbeux en DEBUG (une ligne par requête S3,
    # headers inclus). On les maintient à WARNING pour ne voir que les erreurs.
    for logger_name in ("boto3", "botocore", "urllib3"):
        logging.getLogger(logger_name).setLevel(logging.WARNING)
