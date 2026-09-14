"""Frozen Pydantic request and response contracts for API v0."""

from __future__ import annotations

import uuid
from datetime import datetime
from typing import Any, Literal

from pydantic import BaseModel, ConfigDict, Field, field_validator

from app.core.emulators import SUPPORTED_EMULATORS

#: Le type d'entrée et de sortie pour un identifiant d'émulateur. Un seul
#: endroit à modifier pour ajouter un adaptateur : `app.core.emulators`.
SupportedEmulator = Literal[SUPPORTED_EMULATORS]  # type: ignore[valid-type]


class StrictModel(BaseModel):
    """Reject fields that are not present in the frozen API table."""

    # extra="forbid" sur les requêtes entrantes : si le client envoie un champ
    # inconnu (ex. un champ prévu pour une future version), Pydantic lève une
    # erreur 422 plutôt que de l'ignorer silencieusement. Cela protège le
    # contrat API gelé et détecte les bugs de sérialisation côté client.
    # Note : sur les RÉPONSES, on n'utilise pas ce mode car les clients doivent
    # tolérer les champs inconnus (AD-31 : ignoreUnknownKeys côté Kotlin).
    model_config = ConfigDict(extra="forbid")


class EmptyRequest(StrictModel):
    """Explicitly empty JSON object used by write endpoint #12."""

    # POST /missing attend un body JSON `{}` (pas de body du tout lèverait une
    # erreur de parsing). Ce modèle valide que le client envoie bien un objet
    # vide et rejette tout champ supplémentaire.


class HealthResponse(StrictModel):
    """Nominal response of GET /healthz."""

    status: str
    db: bool
    s3: bool
    # Q28 : informatif, jamais bloquant. Les lecteurs tolèrent les champs
    # inconnus (AD-31), donc ce champ n'engage aucun client existant.
    version: str | None = None


class DeviceCreate(StrictModel):
    """Registration payload for one client device."""

    name: str
    os: Literal["android", "windows", "linux"]
    app_version: str = ""


class DeviceCreated(StrictModel):
    """Identifier returned exactly once a device is registered."""

    device_id: uuid.UUID


class DeviceRename(StrictModel):
    """New display name for one device (EXP-01)."""

    # Borné : ce nom est réaffiché dans l'historique des versions de tous les
    # autres appareils. Un nom vide priverait l'utilisateur de ce repère.
    name: str = Field(min_length=1, max_length=64)


class DeviceSummary(StrictModel):
    """Stable public device representation."""

    id: uuid.UUID
    name: str
    os: Literal["android", "windows", "linux"]
    last_seen_at: datetime | None
    # EXP-01 : un appareil révoqué reste listé, et c'est voulu — il a publié des
    # versions qui existent toujours, et l'utilisateur doit pouvoir constater
    # que la révocation a bien eu lieu.
    revoked_at: datetime | None = None


class DevicesResponse(StrictModel):
    """Device list envelope."""

    devices: list[DeviceSummary]


class HeadSummary(StrictModel):
    """Current or conflicting version summary embedded in API responses."""

    number: int
    content_sha256: str
    size_bytes: int
    created_at: datetime
    origin_device_name: str | None


class UnitSummary(StrictModel):
    """Frozen unit representation shared by list and write endpoints."""

    id: uuid.UUID
    emulator: SupportedEmulator
    unit_key: str
    unit_type: Literal["file", "dir"]
    game_key: str
    game_label: str
    label_source: Literal["auto", "user"]
    head_version: int
    # Q45 : l'adresse d'une jaquette, quand on a su la résoudre. Le serveur ne
    # STOCKE aucune image et n'en réémet aucune — il rend une adresse, et c'est
    # le client qui va la chercher puis la garde en cache chez lui.
    artwork_url: str | None = None
    state: Literal["active", "missing"]
    updated_at: datetime
    head: HeadSummary | None


class UnitsResponse(StrictModel):
    """Unit list envelope."""

    units: list[UnitSummary]


class UnitResponse(StrictModel):
    """Single unit envelope."""

    unit: UnitSummary


class UnitCreate(StrictModel):
    """Identity fields accepted when declaring a synchronization unit."""

    emulator: SupportedEmulator
    unit_key: str
    unit_type: Literal["file", "dir"]
    game_key: str
    game_label: str


class UnitPatch(StrictModel):
    """The only mutable unit metadata field."""

    game_label: str


class VersionPrepare(StrictModel):
    """Metadata needed before direct object upload."""

    # base_version = numéro de version que le client croit être la tête actuelle.
    # Transmis ici pour être comparé lors du confirm (CAS). 0 = première version.
    base_version: int = Field(ge=0)
    # SHA-256 du CONTENU (§5.4) : identité stable indépendante de la compression.
    # Envoyé au prepare pour permettre la déduplication avant l'upload.
    content_sha256: str
    # SHA-256 de l'archive tar.zst : intégrité de transport.
    # Envoyé au prepare pour être stocké dans pending_uploads et comparé au confirm.
    archive_sha256: str
    # Taille du contenu décompressé — affiché dans l'UI.
    size: int = Field(ge=0)
    # Taille de l'archive tar.zst — vérifiée par HEAD S3 au confirm.
    archive_bytes: int = Field(ge=0)

    @field_validator("content_sha256", "archive_sha256")
    @classmethod
    def validate_hash(cls, value: str) -> str:
        # Validation au niveau du schéma pour détecter les bugs de sérialisation
        # côté client le plus tôt possible (avant d'ouvrir une transaction).
        return validate_sha256(value)


class DuplicateVersion(StrictModel):
    """Prepare response when the current head already has this content."""

    duplicate: Literal[True]
    version: int


class UploadTarget(StrictModel):
    """Presigned direct-upload target."""

    url: str
    object_key: str
    expires_at: datetime


class UploadPrepared(StrictModel):
    """Prepare response for content not already at the current head."""

    upload: UploadTarget


class VersionConfirm(StrictModel):
    """Frozen confirm body; sizes come from the prepared upload reservation."""

    object_key: str
    content_sha256: str
    archive_sha256: str
    base_version: int = Field(ge=0)
    env: dict[str, Any]
    client_mtime: datetime | None

    @field_validator("content_sha256", "archive_sha256")
    @classmethod
    def validate_hash(cls, value: str) -> str:
        return validate_sha256(value)


class VersionCreated(StrictModel):
    """Version number created by confirm or restore."""

    version: int


class VersionSummary(StrictModel):
    """One immutable history entry."""

    number: int
    kind: Literal["normal", "restore", "conflict_branch"]
    size_bytes: int
    content_sha256: str
    origin_device_name: str | None
    client_mtime: datetime | None
    created_at: datetime
    parent_number: int | None


class VersionsResponse(StrictModel):
    """Descending version history and current head."""

    versions: list[VersionSummary]
    head_version: int


class DownloadResponse(StrictModel):
    """Presigned download and integrity metadata."""

    url: str
    archive_sha256: str
    content_sha256: str
    archive_bytes: int
    expires_at: datetime


class RestoreRequest(StrictModel):
    """Historical version selected for non-destructive restoration."""

    version: int = Field(ge=1)


class MissingResponse(StrictModel):
    """Unit state returned after a non-destructive missing marker."""

    state: Literal["missing"]


class ConflictSummary(StrictModel):
    """Open conflict representation."""

    id: uuid.UUID
    unit_id: uuid.UUID
    unit_label: str
    version_a: HeadSummary
    version_b: HeadSummary
    created_at: datetime


class ConflictsResponse(StrictModel):
    """Open conflict list envelope."""

    conflicts: list[ConflictSummary]


class ResolveRequest(StrictModel):
    """Version number selected as conflict winner."""

    winner: int = Field(ge=1)


class ResolveResponse(StrictModel):
    """New unit head after resolution."""

    unit_id: uuid.UUID
    head_version: int


def validate_sha256(value: str) -> str:
    """Validate the lowercase, 32-byte hexadecimal wire representation."""

    # 64 caractères hex = 32 octets = SHA-256. Toute autre longueur = bug client.
    if len(value) != 64:
        raise ValueError("must contain exactly 64 hexadecimal characters")
    try:
        bytes.fromhex(value)
    except ValueError as error:
        raise ValueError("must contain exactly 64 hexadecimal characters") from error
    # On impose les minuscules pour garantir qu'un même contenu produit toujours
    # le même object_key S3 (u/.../9f2ab3... pas 9F2AB3...). Sans cette contrainte,
    # la dédup (AD-10) échouerait si un client envoie des majuscules.
    if value != value.lower():
        raise ValueError("must use lowercase hexadecimal characters")
    return value
