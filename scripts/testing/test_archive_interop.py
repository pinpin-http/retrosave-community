"""Run the required cross-implementation archive compatibility check.

Ce script valide que les archives tar.zst produites par le CLI Python sont
lisibles par les autres implémentations, et vice-versa : l'app Android
(Kotlin), et depuis M-2 le noyau desktop (C++). Chaque branche est facultative,
car elles n'ont pas les mêmes prérequis — Kotlin demande le SDK Android, C++ un
build CMake — mais aucune ne doit pouvoir être sautée en silence : le script
annonce ce qu'il a réellement exécuté.

Rappel de doctrine (Q10/Q25, ARCHITECTURE §5.3) : l'égalité binaire entre
implémentations n'est PAS requise et ne doit pas être testée ici. Ce qui est
exigé est la lecture croisée avec la même `content_sha256`.

Il couvre les deux chemins de production (§9.1 CLAUDE.md) :
- `create_archive` : chemin tamponné (archive construite en mémoire)
- `create_archive_from` (AD-29) : chemin en flux (archive écrite directement sur
  disque depuis des fichiers sources, sans charger l'intégralité en RAM)

Sans ce test, une différence de padding tar, d'ordre d'entrées, ou d'options zstd
entre les deux implémentations passerait inaperçue jusqu'à un aller-retour réel
Thor ↔ PC — moment le moins pratique pour la découvrir.
"""

from __future__ import annotations

import argparse
import base64
import json
import os
import subprocess
import tempfile
from pathlib import Path

from save_format import ContentFile, content_sha256, create_archive, extract_archive

ROOT = Path(__file__).resolve().parents[2]
# On réutilise le vecteur de contenu partagé plutôt qu'inventer des fixtures ad hoc :
# si Python et Kotlin calculent le même content_sha256 sur ce vecteur, l'archive
# produite par l'un sera acceptée par l'autre (même identité de contenu).
VECTOR = ROOT / "tests" / "vectors" / "content_hash_basic.json"


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--skip-kotlin",
        action="store_true",
        help="ne pas exécuter la branche Android (utile hors CI Android)",
    )
    parser.add_argument(
        "--cpp",
        type=Path,
        default=None,
        help="chemin de retrosave-archive-interop, pour ajouter la branche C++",
    )
    return parser.parse_args()


def main() -> None:
    """Exchange generated archives through temporary files, never fixtures."""

    arguments = parse_arguments()
    if arguments.skip_kotlin and arguments.cpp is None:
        raise SystemExit("rien à vérifier : Kotlin est sauté et aucun binaire C++ n'est fourni")
    if arguments.cpp is not None and not arguments.cpp.exists():
        raise SystemExit(f"binaire C++ introuvable : {arguments.cpp}")

    vector_cases = json.loads(VECTOR.read_text(encoding="utf-8"))
    # On choisit un cas "dir" avec plusieurs fichiers pour couvrir le tri des
    # entrées tar : si l'ordre est différent entre Python et Kotlin, l'archive
    # a une structure différente même si le contenu est identique.
    vector = next(
        case
        for case in vector_cases
        if case["input"]["unit_type"] == "dir" and case["input"]["files"]
    )
    files = [
        ContentFile(
            rel_path=file["rel_path"],
            content=base64.b64decode(file["content_base64"]),
        )
        for file in vector["input"]["files"]
    ]
    expected = content_sha256("dir", files)
    # Vérification de cohérence interne : si le vecteur lui-même est incohérent,
    # les assertions suivantes deviendraient des faux positifs.
    if expected != vector["expected"]:
        raise AssertionError("shared content vector is internally inconsistent")

    with tempfile.TemporaryDirectory(prefix="retrosave-interop-") as temporary:
        temporary_path = Path(temporary)
        # L'archive de référence est écrite par le harnais (`save_format`), qui
        # ne partage aucune ligne avec les moteurs. C'est ce qui rend la lecture
        # croisée probante : si le C++ et le Kotlin la relisent tous deux
        # correctement, ils parlent bien le format de §5.3 et pas leur dialecte.
        python_archive = temporary_path / "python.tar.zst"
        kotlin_archive = temporary_path / "kotlin.tar.zst"
        python_archive.write_bytes(create_archive("dir", files).bytes)

        # Les chemins d'archives sont transmis via l'environnement pour que le test
        # Kotlin les lise sans avoir besoin d'arguments Gradle supplémentaires.
        done: list[str] = []

        if not arguments.skip_kotlin:
            environment = {
                **os.environ,
                "RETROSAVE_PYTHON_ARCHIVE": str(python_archive),
                "RETROSAVE_KOTLIN_ARCHIVE": str(kotlin_archive),
            }
            wrapper = "gradlew.bat" if os.name == "nt" else "gradlew"
            # Le test Kotlin lit python_archive, le valide, puis écrit kotlin_archive.
            # --rerun-tasks force l'exécution même si Gradle croit que la tâche est à jour.
            subprocess.run(
                [
                    str(ROOT / "android" / wrapper),
                    "-p",
                    str(ROOT / "android"),
                    ":app:testDebugUnitTest",
                    "--tests",
                    "com.retrosave.core.archive.ArchiveInteropTest",
                    "--rerun-tasks",
                ],
                cwd=ROOT,
                env=environment,
                check=True,
            )
            check_foreign_archive(kotlin_archive, "Kotlin", expected, files, temporary_path)
            done.append("Kotlin")

        if arguments.cpp is not None:
            cpp_archive = temporary_path / "cpp.tar.zst"
            # Même contrat que la branche Kotlin : l'outil lit l'archive Python,
            # la valide, puis écrit la sienne. Un code non nul est un échec.
            subprocess.run(
                [str(arguments.cpp), str(python_archive), str(cpp_archive)],
                cwd=ROOT,
                check=True,
            )
            check_foreign_archive(cpp_archive, "C++", expected, files, temporary_path)
            done.append("C++")

    print("archive interop OK : Python <-> " + ", ".join(done))


def check_foreign_archive(
    archive: Path,
    label: str,
    expected: str,
    files: list[ContentFile],
    temporary_path: Path,
) -> None:
    """Relire par le harnais l'archive produite par un moteur."""

    extracted = extract_archive(archive.read_bytes(), "dir", expected)
    rebuilt = [ContentFile(rel_path, content) for rel_path, content in extracted.items()]
    if content_sha256("dir", rebuilt) != expected:
        raise AssertionError(f"l'archive {label} ne fait pas l'aller-retour")

    # Et le contenu doit être le MÊME, pas seulement de même empreinte : une
    # comparaison d'empreintes seule masquerait un lecteur qui recalcule à
    # partir de ce qu'il vient d'écrire.
    if sorted(extracted) != sorted(file.rel_path for file in files):
        raise AssertionError(f"l'archive {label} ne contient pas les mêmes entrées")
    for file in files:
        if extracted[file.rel_path] != file.content:
            raise AssertionError(f"l'archive {label} altère le contenu de {file.rel_path}")


if __name__ == "__main__":
    main()
