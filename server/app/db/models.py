"""SQLAlchemy reflection of the frozen PostgreSQL schema."""

from __future__ import annotations

import uuid
from datetime import datetime
from typing import Any

from sqlalchemy import (
    BigInteger,
    CheckConstraint,
    DateTime,
    ForeignKey,
    Index,
    Integer,
    LargeBinary,
    Text,
    UniqueConstraint,
    text,
)
from sqlalchemy.dialects.postgresql import JSONB, UUID
from sqlalchemy.orm import DeclarativeBase, Mapped, mapped_column

from app.core.emulators import check_constraint


class Base(DeclarativeBase):
    """Declarative metadata used only as a mapping of migration-owned DDL."""


class User(Base):
    """One invitation token and its POC user."""

    __tablename__ = "users"

    id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True),
        primary_key=True,
        # Générée par PostgreSQL via pgcrypto pour éviter les collisions entre
        # appareils qui pourraient générer des UUIDs localement.
        server_default=text("gen_random_uuid()"),
    )
    # SHA-256 du token en clair (AD-07) : jamais le token lui-même.
    # Une fuite de la base ne compromet pas les tokens actifs car SHA-256 est
    # non réversible et les tokens ont 160 bits d'entropie (résistant au bruteforce).
    #
    # Nul pour un compte HÉBERGÉ (migration 0006) : celui-ci s'authentifie par
    # JWT, et lui fabriquer un jeton d'invitation créerait un second secret
    # utilisable — donc une seconde surface d'attaque, pour rien.
    invite_token_hash: Mapped[bytes | None] = mapped_column(
        LargeBinary,
        unique=True,
    )
    # CLD-01 : l'identifiant stable du compte chez le fournisseur d'identité
    # (la revendication `sub` du JWT). Nul pour l'auto-hébergement. C'est LUI
    # qui identifie le compte, jamais l'e-mail : une adresse peut changer.
    auth_subject: Mapped[str | None] = mapped_column(Text, unique=True)
    # Nom lisible pour identifier l'utilisateur dans les logs et l'admin CLI
    # (ex. 'mon-compte', 'testeur-1'). Pas un identifiant fonctionnel.
    label: Mapped[str] = mapped_column(Text, nullable=False)
    created_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True),
        nullable=False,
        server_default=text("now()"),
    )
    # Suppression logique du compte : le token est révoqué mais les données
    # (unités, versions) restent intactes — invariant I2. Un compte avec
    # disabled_at non nul est refusé à l'auth sans message d'erreur différent.
    disabled_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True))


class Device(Base):
    """A registered Android, Windows, or Linux client."""

    __tablename__ = "devices"
    __table_args__ = (
        CheckConstraint("os IN ('android','windows','linux')"),
        # Index pour la requête "lister les appareils d'un utilisateur" (GET /v0/devices).
        Index("devices_user_idx", "user_id"),
    )

    id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True),
        primary_key=True,
        server_default=text("gen_random_uuid()"),
    )
    user_id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True),
        # CASCADE : supprimer un user supprime ses appareils. Non utilisé au POC
        # (pas de suppression de compte) mais protège l'intégrité référentielle.
        ForeignKey("users.id", ondelete="CASCADE"),
        nullable=False,
    )
    # Nom libre choisi par l'utilisateur au premier lancement (ex. 'AYN Thor', 'PC bureau').
    # Affiché dans l'écran de détail d'une version pour identifier l'origine d'un push.
    name: Mapped[str] = mapped_column(Text, nullable=False)
    os: Mapped[str] = mapped_column(Text, nullable=False)
    # Version de l'app au moment de l'enregistrement — utile pour diagnostiquer
    # des divergences de comportement entre clients anciens et récents (AD-31).
    app_version: Mapped[str] = mapped_column(
        Text,
        nullable=False,
        server_default=text("''"),
    )
    created_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True),
        nullable=False,
        server_default=text("now()"),
    )
    # Mis à jour (best effort) à chaque requête authentifiée via X-Device-Id.
    # Sert à l'écran AD-36 "appareils inactifs depuis > 48 h" — informatif uniquement.
    last_seen_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True))
    # EXP-01 : révocation d'un appareil. Suppression LOGIQUE, jamais physique —
    # ses versions, ses conflits et ses archives restent intacts (invariant I2),
    # et l'historique continue de dire quel appareil a publié quoi. Seules ses
    # futures requêtes sont refusées (get_current_device).
    #
    # Limite assumée : l'authentification reste un jeton d'invitation par
    # UTILISATEUR, partagé par tous ses appareils. Révoquer refuse cet
    # enregistrement-là ; cela n'invalide pas le jeton, donc son porteur peut en
    # enregistrer un nouveau. Des identifiants par appareil relèvent des comptes
    # (CLD-01) — voir Q46.
    revoked_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True))


class SaveUnit(Base):
    """Server identity and current head of one synchronization unit."""

    __tablename__ = "save_units"
    __table_args__ = (
        # Contraintes CHECK pour garantir les valeurs valides sans couche applicative.
        # La liste vit dans `app.core.emulators` : un seul endroit à
        # modifier pour le prochain adaptateur (puis une migration).
        CheckConstraint(check_constraint(), name="save_units_emulator_check"),
        CheckConstraint("unit_type IN ('file','dir')"),
        # Au POC, le serveur ne connaît que 'active' et 'missing'. Les états
        # 'paused', 'error', 'duplicate' sont purement côté client (§6.1).
        CheckConstraint("state IN ('active','missing')"),
        CheckConstraint("label_source IN ('auto','user')"),
        # Contrainte d'unicité métier : un même jeu sur le même émulateur ne peut
        # exister qu'une fois par compte, quelle que soit la machine d'origine.
        # C'est ce qui permet à deux appareils de partager la même unité serveur.
        UniqueConstraint("user_id", "emulator", "unit_key"),
        # Index utilisé par GET /v0/units qui trie par updated_at DESC.
        Index("save_units_user_updated_idx", "user_id", "updated_at"),
    )

    id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True),
        primary_key=True,
        server_default=text("gen_random_uuid()"),
    )
    user_id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True),
        ForeignKey("users.id", ondelete="CASCADE"),
        nullable=False,
    )
    # Identifiant de l'émulateur qui gère cette sauvegarde (ppsspp, melonds, etc.).
    emulator: Mapped[str] = mapped_column(Text, nullable=False)
    # Clé stable de l'unité au sein de l'émulateur : nom du dossier SAVEDATA
    # pour PPSSPP, nom de fichier .sav pour melonDS, titleid_low pour Azahar.
    # Ne change pas si l'utilisateur déplace la racine — c'est le point d'ancrage.
    unit_key: Mapped[str] = mapped_column(Text, nullable=False)
    # 'file' pour un .sav unique (melonDS), 'dir' pour un dossier entier (PPSSPP, Azahar).
    # Détermine comment l'archive tar.zst est construite et appliquée côté client.
    unit_type: Mapped[str] = mapped_column(Text, nullable=False)
    # Clé de regroupement inter-appareils : serial PSP ('ULES01234'), nom normalisé
    # pour melonDS, '3ds:<titleid>' pour Azahar. Permet d'afficher "même jeu" même
    # si unit_key diffère légèrement entre plateformes.
    game_key: Mapped[str] = mapped_column(Text, nullable=False)
    # Libellé affiché à l'utilisateur dans les listes — peut être renommé manuellement.
    game_label: Mapped[str] = mapped_column(Text, nullable=False)
    # M8 §7 : 'user' verrouille le libellé contre toute résolution automatique.
    # Sans ce drapeau, le renommage manuel serait effacé au prochain scan — le
    # pire défaut possible pour une fonctionnalité de nommage.
    label_source: Mapped[str] = mapped_column(
        Text,
        nullable=False,
        server_default=text("'auto'"),
    )
    # Numéro de la version actuellement en tête (la "version courante").
    # 0 = aucune version poussée encore (sentinelle, pas une FK vers save_versions).
    # On évite une FK circulaire (save_units ↔ save_versions) en gérant la cohérence
    # applicativement dans la transaction CAS de core/sync.py.
    head_version: Mapped[int] = mapped_column(
        Integer,
        nullable=False,
        server_default=text("0"),
    )
    # 'active' = état normal. 'missing' = le client a signalé que le fichier a
    # disparu localement (POST /missing). La sauvegarde reste téléchargeable.
    state: Mapped[str] = mapped_column(
        Text,
        nullable=False,
        server_default=text("'active'"),
    )
    created_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True),
        nullable=False,
        server_default=text("now()"),
    )
    # Mis à jour à chaque confirm, patch ou missing pour permettre un tri chronologique
    # dans GET /v0/units et détecter les unités récemment modifiées.
    updated_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True),
        nullable=False,
        server_default=text("now()"),
    )


class SaveVersion(Base):
    """Immutable version metadata for one uploaded archive."""

    __tablename__ = "save_versions"
    __table_args__ = (
        CheckConstraint("size_bytes >= 0"),
        CheckConstraint("archive_bytes >= 0"),
        # Trois types de versions :
        # 'normal'          → push standard, devient la nouvelle tête si CAS réussit.
        # 'restore'         → créée par POST /restore ; copie l'object_key d'une version
        #                     ancienne sans re-uploader l'objet (AD-10).
        # 'conflict_branch' → créée quand le CAS échoue ; ne devient jamais tête
        #                     automatiquement, en attente de résolution manuelle.
        CheckConstraint("kind IN ('normal','restore','conflict_branch')"),
        UniqueConstraint("unit_id", "number"),
        # Index pour la requête de dédup dans prepare : "est-ce que ce content_sha256
        # est déjà la tête ?" (AD-10 : adressage par contenu).
        Index("save_versions_unit_content_idx", "unit_id", "content_sha256"),
        # Index pour que le GC puisse vérifier rapidement si un objet S3 est référencé
        # par au moins une version avant de le supprimer.
        Index("save_versions_object_key_idx", "object_key"),
    )

    id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True),
        primary_key=True,
        server_default=text("gen_random_uuid()"),
    )
    unit_id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True),
        ForeignKey("save_units.id", ondelete="CASCADE"),
        nullable=False,
    )
    # Numéro séquentiel attribué par le serveur dans la transaction CAS.
    # Toujours croissant par unité (1, 2, 3...) — c'est lui qui détermine l'ordre,
    # jamais l'horloge client (invariant I4).
    number: Mapped[int] = mapped_column(Integer, nullable=False)
    # Numéro de la version dont cette version est issue. Pour 'restore', c'est
    # l'ancienne tête. Pour 'conflict_branch', c'est la version de base du client.
    # Null pour la première version d'une unité.
    parent_number: Mapped[int | None] = mapped_column(Integer)
    # Empreinte du CONTENU de la sauvegarde, indépendante de la compression et des
    # mtimes (§5.4). C'est l'identité stable utilisée pour la dédup (AD-10) et
    # pour vérifier l'intégrité après téléchargement côté client.
    content_sha256: Mapped[bytes] = mapped_column(LargeBinary, nullable=False)
    # SHA-256 de l'objet tar.zst dans S3 — intégrité de transport. Différent de
    # content_sha256 : deux archives du même contenu ont le même content_sha256
    # mais peuvent avoir des archive_sha256 différents (compression non déterministe
    # entre Python et Kotlin, cf. §5.3).
    archive_sha256: Mapped[bytes] = mapped_column(LargeBinary, nullable=False)
    # Taille du contenu décompressé en octets — affichée à l'utilisateur.
    size_bytes: Mapped[int] = mapped_column(BigInteger, nullable=False)
    # Taille de l'objet tar.zst dans S3 — vérifiée par HEAD avant le commit (§7.4).
    archive_bytes: Mapped[int] = mapped_column(BigInteger, nullable=False)
    # Chemin dans le bucket S3, adressé par contenu : 'u/<user_id>/<unit_id>/<content_sha256>.tar.zst'
    # (AD-10). Un restore réutilise le même object_key sans copier l'objet.
    object_key: Mapped[str] = mapped_column(Text, nullable=False)
    # Appareil qui a pushé cette version. SET NULL si l'appareil est supprimé —
    # l'historique des versions reste intact même si le device disparaît.
    origin_device: Mapped[uuid.UUID | None] = mapped_column(
        UUID(as_uuid=True),
        ForeignKey("devices.id", ondelete="SET NULL"),
    )
    # Contexte d'exécution au moment du push : {"os": "android", "app": "0.1.0",
    # "emulator_version": "2.3.1"}. Informatif, utile pour diagnostiquer des
    # incompatibilités de format entre versions d'émulateur.
    env: Mapped[dict[str, Any]] = mapped_column(
        JSONB,
        nullable=False,
        server_default=text("'{}'"),
    )
    # Heure de modification du fichier source selon l'horloge du client.
    # Affiché dans l'UI pour aider l'utilisateur à choisir en cas de conflit,
    # mais jamais utilisé pour décider de l'ordre des versions (invariant I4).
    client_mtime: Mapped[datetime | None] = mapped_column(DateTime(timezone=True))
    kind: Mapped[str] = mapped_column(
        Text,
        nullable=False,
        server_default=text("'normal'"),
    )
    created_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True),
        nullable=False,
        server_default=text("now()"),
    )


class Conflict(Base):
    """An explicit preserved CAS branch awaiting a winner."""

    __tablename__ = "conflicts"
    __table_args__ = (
        CheckConstraint("status IN ('open','resolved')"),
        # Index partiel sur les conflits ouverts seulement : la quasi-totalité des
        # requêtes portent sur les conflits 'open' (liste + vérification avant push).
        # Un index partiel est plus petit et plus rapide qu'un index complet.
        Index(
            "conflicts_unit_open_idx",
            "unit_id",
            postgresql_where=text("status = 'open'"),
        ),
    )

    id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True),
        primary_key=True,
        server_default=text("gen_random_uuid()"),
    )
    unit_id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True),
        ForeignKey("save_units.id", ondelete="CASCADE"),
        nullable=False,
    )
    # version_a = la tête au moment où le conflit a été créé (la version "déjà là").
    # version_b = la branche conflict_branch (la version du client qui a perdu le CAS).
    # Les deux versions sont conservées dans save_versions (invariant I3).
    version_a: Mapped[int] = mapped_column(Integer, nullable=False)
    version_b: Mapped[int] = mapped_column(Integer, nullable=False)
    status: Mapped[str] = mapped_column(
        Text,
        nullable=False,
        server_default=text("'open'"),
    )
    # Numéro de la version choisie comme gagnante par l'utilisateur.
    # NULL tant que le conflit est ouvert.
    winner: Mapped[int | None] = mapped_column(Integer)
    created_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True),
        nullable=False,
        server_default=text("now()"),
    )
    resolved_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True))


class PendingUpload(Base):
    """Object uploaded but not yet attached to a version."""

    # Table de liaison entre prepare et confirm : quand le client appelle prepare,
    # on réserve un slot ici avec les métadonnées attendues de l'archive.
    # Quand confirm arrive, on vérifie que l'objet S3 correspond à ce slot, puis
    # on le supprime (le slot est "consommé"). Si confirm n'arrive jamais, le GC
    # horaire nettoie les slots de plus de 24 h.
    __tablename__ = "pending_uploads"

    # La clé primaire est l'object_key S3 — pas un UUID — pour permettre un upsert
    # efficace : si le client retente un prepare pour le même contenu, on écrase le
    # slot existant plutôt que d'en créer un deuxième.
    object_key: Mapped[str] = mapped_column(Text, primary_key=True)
    unit_id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True),
        ForeignKey("save_units.id", ondelete="CASCADE"),
        nullable=False,
    )
    # Les checksums et tailles sont enregistrés au prepare pour être comparés au
    # confirm — on s'assure que le client n'a pas substitué un objet différent
    # entre les deux appels (protection contre les modifications accidentelles ou
    # les bugs de cache).
    content_sha256: Mapped[bytes] = mapped_column(LargeBinary, nullable=False)
    archive_sha256: Mapped[bytes] = mapped_column(LargeBinary, nullable=False)
    size_bytes: Mapped[int] = mapped_column(BigInteger, nullable=False)
    archive_bytes: Mapped[int] = mapped_column(BigInteger, nullable=False)
    # Date de création utilisée par le GC pour identifier les slots orphelins
    # (prepare sans confirm depuis plus de pending_upload_ttl_h heures).
    created_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True),
        nullable=False,
        server_default=text("now()"),
    )


class IdempotencyKey(Base):
    """Cached response for one optional write idempotency key."""

    # Si le client envoie un header 'Idempotency-Key: <uuid>', la réponse d'une
    # écriture (POST/PATCH) est mise en cache ici pendant 24 h. Si le client
    # retente la même requête avec la même clé (ex. après un timeout réseau),
    # il reçoit exactement la même réponse sans que l'opération soit rejouée.
    # Essentiel pour les uploads : un confirm rejoué sans cette table créerait
    # une deuxième version du même contenu.
    __tablename__ = "idempotency_keys"

    # La clé est fournie par le client — opaque pour le serveur. Elle n'est
    # PAS unique à elle seule : les clients la dérivent de l'identité de
    # l'opération, et une `unit_key` comme `ULJM05800DATA00` est la même chez
    # tous ceux qui jouent à ce jeu.
    key: Mapped[str] = mapped_column(Text, primary_key=True)
    # Le compte fait partie de la clé primaire (migration 0005). Deux comptes
    # peuvent donc porter la même clé sans se voir ; c'est ce qui garantit à la
    # fois l'isolation et le fait qu'aucun compte n'en bloque un autre.
    user_id: Mapped[uuid.UUID] = mapped_column(
        UUID(as_uuid=True),
        primary_key=True,
        nullable=False,
    )
    # Le statut HTTP original est conservé : un 201 doit retourner 201 au replay,
    # pas 200, pour que le client sache que c'était une création.
    status_code: Mapped[int] = mapped_column(Integer, nullable=False)
    # La réponse JSON complète sérialisée, rejouée telle quelle.
    response: Mapped[dict[str, Any]] = mapped_column(JSONB, nullable=False)
    # Utilisé par le job de purge horaire pour supprimer les clés expirées.
    created_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True),
        nullable=False,
        server_default=text("now()"),
    )
