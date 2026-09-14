"""Opaque invitation-token generation, hashing, and redaction."""

from __future__ import annotations

import hashlib
import re
import secrets

# Regex de validation du format fil : le préfixe 'rsc_' identifie le type de
# token pour éviter les confusions avec d'autres secrets, suivi de 40 hex
# (= 20 octets aléatoires, soit 160 bits d'entropie — impossible à bruteforcer).
# Utilisée à la réception pour rejeter rapidement les requêtes mal formées
# avant même d'interroger la base.
TOKEN_RE = re.compile(r"^rsc_[0-9a-f]{40}$")

# Regex plus permissive (majuscules tolérées) utilisée uniquement pour la
# redaction dans les logs — on veut attraper les tokens même mal copiés.
TOKEN_IN_TEXT_RE = re.compile(r"rsc_[0-9a-fA-F]{40}")


def generate_invite_token() -> tuple[str, bytes]:
    """Create the one-time plaintext token and its stored SHA-256 digest."""

    # secrets.token_hex(20) génère 20 octets cryptographiquement aléatoires
    # convertis en 40 caractères hexadécimaux — résistant aux attaques par
    # timing et au bruteforce.
    token = f"rsc_{secrets.token_hex(20)}"
    # On retourne les deux : le plaintext à afficher UNE SEULE FOIS à l'admin,
    # et le hash à stocker en base (AD-07 : jamais de plaintext en base).
    return token, hash_token(token)


def hash_token(token: str) -> bytes:
    """Hash the complete opaque token as mandated by AD-07."""

    # SHA-256 suffisant ici : les tokens sont longs et aléatoires, donc sans
    # risque de collision par dictionnaire. Un sel par token serait du over-
    # engineering pour un POC avec < 20 utilisateurs.
    return hashlib.sha256(token.encode("ascii")).digest()


def is_valid_token_format(token: str) -> bool:
    """Return whether a token has the only accepted POC wire format."""

    # Rejet immédiat si le format est invalide, avant toute requête SQL.
    # Protège aussi contre les injections de caractères spéciaux dans le hash.
    return TOKEN_RE.fullmatch(token) is not None


def redact_tokens(value: str) -> str:
    """Remove invitation tokens from arbitrary log text."""

    # Remplace toute occurrence d'un token dans une chaîne quelconque.
    # Appelé systématiquement par le filtre de logging pour que les tokens
    # ne fuient jamais dans les fichiers de log, même par erreur de code.
    return TOKEN_IN_TEXT_RE.sub("[REDACTED_TOKEN]", value)


# ─── Comptes hébergés (CLD-01) ────────────────────────────────────────────
# Le service hébergé n'a ni URL ni jeton à coller : l'application se connecte
# auprès du fournisseur d'identité et présente le JWT obtenu. Ce module ne
# parle à personne — il vérifie une signature et des bornes, rien d'autre.


class AccountClaims:
    """The only two claims this product needs from a verified token."""

    __slots__ = ("email", "subject")

    def __init__(self, subject: str, email: str | None) -> None:
        # Le SUJET identifie le compte, jamais l'e-mail : une adresse change,
        # un sujet non. Les rattacher à l'e-mail donnerait le compte de
        # quelqu'un à celui qui récupère son adresse.
        self.subject = subject
        self.email = email


def account_token_kid(credential: str) -> str | None:
    """Read the key identifier from an UNVERIFIED header.

    Lire un en-tête non vérifié sert uniquement à savoir **quelle clé publique
    demander** ; la signature est contrôlée juste après, avec cette clé. C'est
    l'usage normal de `kid`, et il n'y a rien à en tirer d'autre : aucune
    décision d'autorisation ne dépend de cette lecture.
    """

    import jwt

    try:
        header = jwt.get_unverified_header(credential)
    except Exception:  # noqa: BLE001 — un en-tête illisible vaut « pas de kid »
        return None
    kid = header.get("kid")
    return kid if isinstance(kid, str) and kid.strip() else None


def verify_account_token(
    credential: str,
    *,
    secret: object,
    issuer: str | None,
    audience: str,
    algorithms: tuple[str, ...],
) -> AccountClaims | None:
    """Return the claims of a token that holds up, or None.

    `secret` est soit un secret partagé (HS256, auto-hébergement historique),
    soit une **clé publique** récupérée sur le JWKS du fournisseur (ES256 ou
    RS256). Dans le second cas, le serveur ne détient aucun secret : une fuite
    de sa configuration ne permet de fabriquer aucun jeton.

    Aucune exception ne sort d'ici : l'appelant répond `invalid_token` de la
    même façon pour un jeton expiré, mal signé ou destiné à un autre service.
    Distinguer les cas dans la réponse aiderait surtout celui qui essaie.
    """

    import jwt  # importé ici : un serveur auto-hébergé ne s'en sert jamais

    try:
        payload = jwt.decode(
            credential,
            secret,
            # Liste FERMÉE, fournie par la configuration : accepter
            # l'algorithme annoncé dans l'en-tête du jeton — `none` compris —
            # est la faille historique de JWT.
            algorithms=list(algorithms),  # type: ignore[arg-type]
            audience=audience,
            issuer=issuer,
            options={
                "require": ["exp", "sub"],
                "verify_signature": True,
                "verify_exp": True,
                "verify_aud": True,
                "verify_iss": issuer is not None,
            },
        )
    except Exception:  # noqa: BLE001 — toute défaillance vaut « refusé »
        return None

    subject = payload.get("sub")
    if not isinstance(subject, str) or not subject.strip():
        return None
    email = payload.get("email")
    return AccountClaims(
        subject=subject.strip(),
        email=email.strip() if isinstance(email, str) and email.strip() else None,
    )


def looks_like_account_token(credential: str) -> bool:
    """Tell a JWT apart from an invitation token, before touching the database.

    Un JWT compact porte exactement deux points ; un jeton d'invitation n'en
    contient aucun. Ce tri n'est pas une validation — il choisit seulement
    quelle vérification appliquer.
    """

    return credential.count(".") == 2 and all(part for part in credential.split("."))
