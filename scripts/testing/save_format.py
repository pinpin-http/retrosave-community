"""Le format d'archive et l'identité de contenu, réimplémentés **pour les bancs**.

Ce module n'est pas le produit. C'est une seconde implémentation, volontairement
courte, des deux contrats que les clients doivent respecter :

- **§5.3 — l'archive** : un `tar` PAX déterministe (entrées triées sur les octets
  UTF-8, `mtime`/`uid`/`gid` à zéro) dans une trame `zstd` niveau 10 avec somme
  de contrôle ;
- **§5.4 — l'identité de contenu** : pour une unité-fichier, le sha256 des
  octets ; pour une unité-dossier, le sha256 du manifeste trié
  `relpath\\0taille\\0sha256\\n`.

**Pourquoi la réécrire plutôt que l'emprunter.** Un banc qui appelle le code
qu'il vérifie ne prouve rien : il constate seulement que le produit est
d'accord avec lui-même. En lisant et en écrivant le format ici, avec `tarfile`
et `zstandard` de la bibliothèque standard, on vérifie que le moteur C++ parle
bien le format **écrit dans l'architecture**, et pas son propre dialecte.

Aucun code de `desktop/` ni du serveur n'est importé ici, et rien de ce module
ne doit être appelé par le produit.
"""

from __future__ import annotations

import hashlib
import io
import tarfile
from dataclasses import dataclass

import zstandard

# Niveau figé par Q25, identique aux deux implémentations du produit.
ZSTD_LEVEL = 10


@dataclass(frozen=True)
class ContentFile:
    rel_path: str
    content: bytes


@dataclass(frozen=True)
class ArchiveBlob:
    bytes: bytes
    content_sha256: str
    archive_sha256: str
    size_bytes: int

    @property
    def archive_bytes(self) -> int:
        return len(self.bytes)


def _sort_key(rel_path: str) -> bytes:
    """Trier sur les OCTETS UTF-8, jamais sur les caractères.

    Trier sur les caractères inverse « é » et « 日本語 » par rapport aux octets,
    ce qui changerait l'empreinte d'un dossier sans que rien n'ait bougé.
    """

    return rel_path.encode("utf-8")


def content_sha256(unit_type: str, files: list[ContentFile]) -> str:
    if unit_type == "file":
        if len(files) != 1:
            raise ValueError("une unité-fichier contient exactement un fichier")
        return hashlib.sha256(files[0].content).hexdigest()
    if unit_type != "dir":
        raise ValueError(f"type d'unité inconnu : {unit_type}")

    manifest = bytearray()
    for entry in sorted(files, key=lambda item: _sort_key(item.rel_path)):
        manifest.extend(entry.rel_path.encode("utf-8"))
        manifest.extend(b"\0")
        manifest.extend(str(len(entry.content)).encode("ascii"))
        manifest.extend(b"\0")
        manifest.extend(hashlib.sha256(entry.content).hexdigest().encode("ascii"))
        manifest.extend(b"\n")
    return hashlib.sha256(manifest).hexdigest()


def create_archive(unit_type: str, files: list[ContentFile]) -> ArchiveBlob:
    """Un tar PAX déterministe dans une trame zstd, comme le produit doit le faire."""

    ordered = sorted(files, key=lambda item: _sort_key(item.rel_path))
    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode="w", format=tarfile.PAX_FORMAT) as archive:
        for entry in ordered:
            info = tarfile.TarInfo(entry.rel_path)
            info.size = len(entry.content)
            # Ce qui rend l'archive déterministe : aucune date, aucun
            # propriétaire. Deux captures du même contenu donnent les mêmes
            # octets de tar, quel que soit le poste.
            info.mtime = 0
            info.uid = 0
            info.gid = 0
            info.uname = ""
            info.gname = ""
            archive.addfile(info, io.BytesIO(entry.content))

    compressed = zstandard.ZstdCompressor(
        level=ZSTD_LEVEL, threads=0, write_checksum=True
    ).compress(raw.getvalue())
    return ArchiveBlob(
        bytes=compressed,
        content_sha256=content_sha256(unit_type, ordered),
        archive_sha256=hashlib.sha256(compressed).hexdigest(),
        size_bytes=sum(len(entry.content) for entry in ordered),
    )


def extract_archive(data: bytes, unit_type: str, expected_content_sha256: str) -> dict[str, bytes]:
    """Déballer et **juger** : une archive dont le contenu ne correspond pas est refusée.

    L'identité de contenu est le verdict, pas l'empreinte de l'archive : deux
    clients peuvent produire des enveloppes différentes du même contenu (Q10),
    et exiger les mêmes octets compressés rejetterait une archive saine.
    """

    raw = zstandard.ZstdDecompressor().decompress(data, max_output_size=512 * 1024 * 1024)
    files: list[ContentFile] = []
    with tarfile.open(fileobj=io.BytesIO(raw), mode="r") as archive:
        for member in archive.getmembers():
            if not member.isfile():
                raise ValueError(f"entrée d'archive inattendue : {member.name}")
            handle = archive.extractfile(member)
            if handle is None:
                raise ValueError(f"entrée illisible : {member.name}")
            files.append(ContentFile(member.name, handle.read()))

    actual = content_sha256(unit_type, files)
    if actual != expected_content_sha256:
        raise ValueError(
            f"identité de contenu incorrecte : {actual} au lieu de {expected_content_sha256}"
        )
    return {entry.rel_path: entry.content for entry in files}
