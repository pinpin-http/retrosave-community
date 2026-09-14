"""Check local Markdown document links without opening their target files."""

from __future__ import annotations

import re
from pathlib import Path
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parents[2]
LINK = re.compile(r"\]\(([^\s)]+)\)")


def main() -> int:
    documents = sorted(ROOT.glob("*.md"))
    documents += sorted((ROOT / "docs").rglob("*.md"))
    documents += sorted((ROOT / "scripts").rglob("*.md"))
    # Les builds Qt embarquent des copies générées : ne parcourir que les
    # documents de source à la racine du composant desktop.
    documents += sorted((ROOT / "desktop").glob("*.md"))
    errors = []
    checked = 0
    for document in documents:
        for match in LINK.finditer(document.read_text(encoding="utf-8")):
            target = urlsplit(match.group(1))
            if target.scheme or target.netloc or not target.path:
                continue
            checked += 1
            if not (document.parent / unquote(target.path)).exists():
                errors.append(f"{document.relative_to(ROOT)}: {match.group(1)}")
    for error in errors:
        print(f"Lien local absent : {error}")
    print(f"{checked} liens locaux contrôlés, {len(errors)} erreur(s).")
    return bool(errors)


if __name__ == "__main__":
    raise SystemExit(main())
