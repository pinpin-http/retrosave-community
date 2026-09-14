"""Environment-backed server configuration."""

from functools import lru_cache
from typing import Literal

from pydantic_settings import BaseSettings, SettingsConfigDict


class Settings(BaseSettings):
    """Configuration defined by ARCHITECTURE.md section 16.1."""

    # Contrôle les comportements spécifiques à l'environnement : en 'dev', les
    # erreurs sont plus verbeuses et certaines validations allégées.
    app_env: Literal["dev", "prod"] = "dev"

    # URL de connexion asyncpg pour SQLAlchemy ; contient hôte, port, user,
    # password et nom de la base. En dev, pointe vers le conteneur Docker 'db'.
    database_url: str

    # --- Fournisseur d'identité JWT optionnel ---
    # Deux façons de vérifier un jeton de compte, et une seule à configurer.
    #
    # 1. `account_jwks_url` — méthode recommandée : les jetons sont signés par
    #    une clé privée que le
    #    fournisseur garde, et ce serveur vérifie avec la clé PUBLIQUE publiée.
    #    Il ne détient alors aucun secret : une fuite de sa configuration ne
    #    permet de fabriquer aucun jeton.
    # 2. `account_jwt_secret` — un secret partagé (HS256), pour un fournisseur
    #    qui ne publie pas de JWKS. Le serveur peut alors forger des jetons :
    #    ce secret se protège comme un mot de passe.
    #
    # Les deux laissés VIDES, ce serveur n'accepte que les jetons d'invitation :
    # c'est le mode auto-hébergé, et c'est le défaut. Personne ne doit avoir
    # besoin d'un fournisseur d'identité pour héberger RetroSave chez lui.
    account_jwks_url: str | None = None
    account_jwt_secret: str | None = None
    # L'émetteur attendu, vérifié à chaque requête : un jeton parfaitement
    # signé par un AUTRE projet ne doit pas ouvrir ce serveur.
    account_issuer: str | None = None
    # L'audience attendue est propre au fournisseur choisi.
    account_audience: str = "authenticated"
    # Les algorithmes admis. Liste FERMÉE, et sans `none` : accepter l'algorithme
    # annoncé par le jeton lui-même est la faille historique de JWT. Le jeu
    # dépend du mode : asymétrique par JWKS, ou secret partagé.
    account_jwks_algorithms: tuple[str, ...] = ("ES256", "RS256")
    account_algorithms: tuple[str, ...] = ("HS256",)
    # Durée de mise en cache du JWKS. Le fournisseur fait tourner ses clés :
    # un cache éternel finirait par refuser tout le monde, un cache absent
    # ferait un appel réseau à chaque requête.
    account_jwks_ttl_s: int = 600

    # --- Stockage objet S3-compatible ---
    # AD-25 : deux endpoints distincts pour résoudre un problème de signature.
    # SigV4 (le protocole d'auth S3) intègre l'hôte dans la signature de l'URL.
    # Si le serveur signait avec l'adresse interne ('http://minio:9000'), les
    # clients Android (hors Docker) recevraient des URLs avec un hôte injoignable.
    # Solution : s3_endpoint pour les opérations internes (HEAD, healthcheck, GC),
    # s3_public_endpoint pour les URLs remises aux clients.
    s3_endpoint: str
    # En dev sur le réseau local : IP de la machine (ex. http://192.168.1.20:9000).
    # Avec un endpoint déjà public : laisser vide.
    s3_public_endpoint: str | None = None
    s3_region: str = "auto"
    s3_bucket: str
    s3_access_key_id: str
    s3_secret_access_key: str
    # Force le style 'path' (ex. http://minio:9000/bucket/key) plutôt que
    # 'virtual-hosted' (ex. http://bucket.minio:9000/key) — requis pour MinIO.
    s3_force_path_style: bool = True

    # Durée de validité des URLs présignées : 900 s = 15 min, suffisant pour
    # uploader 256 Mo sur une connexion lente (environ 2 Mo/s minimum).
    presign_expires_s: int = 900
    # Taille maximale d'une unité : 268_435_456 octets = exactement 256 Mio (I1).
    max_unit_bytes: int = 268_435_456
    # Durée pendant laquelle une réponse idempotente est rejouable (TTL).
    # Après 24 h, la clé est purgée et un nouvel appel crée une nouvelle version.
    idempotency_ttl_h: int = 24
    # Durée après laquelle un upload non confirmé est considéré orphelin et
    # éligible au GC (le client a commencé un upload mais n'a jamais appelé confirm).
    pending_upload_ttl_h: int = 24

    log_level: str = "INFO"
    # Domaine de production uniquement, utilisé par le déploiement Caddy.
    retrosave_domain: str | None = None

    model_config = SettingsConfigDict(
        env_file=".env",
        env_file_encoding="utf-8",
        # Les variables d'env inconnues sont ignorées plutôt que rejetées —
        # pratique pour les envs Docker qui injectent des variables système.
        extra="ignore",
    )

    @property
    def s3_signing_endpoint(self) -> str:
        """Return the exact host embedded in client-facing SigV4 URLs."""

        # Si aucun endpoint public n'est configuré (ex. production R2 où l'endpoint
        # est déjà la même URL publique), on réutilise l'endpoint interne.
        return self.s3_public_endpoint or self.s3_endpoint


@lru_cache
def get_settings() -> Settings:
    """Return one immutable configuration instance per process."""

    # lru_cache garantit qu'une seule instance Settings est créée par process.
    # Sans ça, chaque appel lirait le .env depuis le disque — coûteux et
    # risqué si le fichier change pendant que le serveur tourne.
    return Settings()
