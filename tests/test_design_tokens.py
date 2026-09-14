"""L'identité visuelle est un contrat, pas une intention.

Deux clients, deux langages, une seule référence : `design/tokens.json`, relevé
sur les maquettes livrées. Ce test relit les implémentations Qt et Android et
échoue dès qu'une couleur, un rayon, une taille, une durée ou la géométrie de la
marque s'écarte.

C'est la discipline de `tests/vectors/` appliquée à l'apparence. Sans elle,
« identité unifiée » est vrai le jour où on l'écrit et faux trois commits plus
tard : une teinte ajustée d'un côté, jamais reportée de l'autre, et personne ne
le voit avant de poser les deux écrans côte à côte.

Les contrastes sont **recalculés** selon WCAG 2.1 : une valeur écrite à la main
dans un fichier de conception ne prouve rien.
"""

from __future__ import annotations

import json
import re
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
TOKENS = json.loads((ROOT / "design" / "tokens.json").read_text(encoding="utf-8"))
QML_THEME = (ROOT / "desktop" / "qml" / "Theme.qml").read_text(encoding="utf-8")
QML_STAR = (ROOT / "desktop" / "qml" / "StarMark.qml").read_text(encoding="utf-8")
DESKTOP_MAIN = (ROOT / "desktop" / "src" / "app" / "main.cpp").read_text(encoding="utf-8")
KOTLIN_ROOT = ROOT / "android/app/src/main/kotlin/com/retrosave/ui/theme"
KT_THEME = (KOTLIN_ROOT / "RetroSaveTheme.kt").read_text(encoding="utf-8")
KT_STAR = (KOTLIN_ROOT / "RetroSaveStar.kt").read_text(encoding="utf-8")

COLORS = TOKENS["color"]

# QML refuse tout identifiant `on` + majuscule : il y voit un gestionnaire de
# signal, et le fichier ne charge plus — panne silencieuse, constatée deux fois.
# Or TOUS les rôles d'encre de Material 3 commencent par « on ». Voici la
# correspondance, et le test la fait respecter des deux côtés.
QML_ALIASES = {
    "onSurface": "ink",
    "onSurfaceVariant": "inkMuted",
    "onPrimary": "inkInverse",
    "onPrimaryContainer": "inkOnContainer",
    "onSecondaryFixed": "inkAccent",
    "onErrorContainer": "inkAlert",
}


def _channel(value: int) -> float:
    """Composante linéarisée, selon la formule sRGB de WCAG 2.1."""
    ratio = value / 255
    return ratio / 12.92 if ratio <= 0.04045 else ((ratio + 0.055) / 1.055) ** 2.4


def luminance(hex_color: str) -> float:
    raw = hex_color.lstrip("#")
    red, green, blue = (int(raw[index : index + 2], 16) for index in (0, 2, 4))
    return 0.2126 * _channel(red) + 0.7152 * _channel(green) + 0.0722 * _channel(blue)


def contrast(first: str, second: str) -> float:
    light, dark = sorted((luminance(first), luminance(second)), reverse=True)
    return (light + 0.05) / (dark + 0.05)


@pytest.mark.parametrize("name", sorted(COLORS))
def test_qt_uses_the_token_colour(name: str) -> None:
    expected = COLORS[name]
    qml_name = QML_ALIASES.get(name, name)
    match = re.search(rf'readonly property color {qml_name}: "(#[0-9A-Fa-f]{{6}})"', QML_THEME)
    assert match, f"le thème Qt ne déclare pas « {qml_name} » (jeton « {name} »)"
    assert match.group(1).lower() == expected.lower(), (
        f"« {name} » vaut {match.group(1)} côté Qt et {expected} dans les jetons"
    )


@pytest.mark.parametrize("name", sorted(COLORS))
def test_android_uses_the_token_colour(name: str) -> None:
    expected = COLORS[name].lstrip("#")
    match = re.search(rf"val {name} = Color\(0xFF([0-9A-Fa-f]{{6}})\)", KT_THEME)
    assert match, f"le thème Android ne déclare pas « {name} »"
    assert match.group(1).lower() == expected.lower(), (
        f"« {name} » vaut #{match.group(1)} côté Android et #{expected} dans les jetons"
    )


def test_no_qml_property_starts_with_on() -> None:
    """Le piège qui a coûté deux pannes silencieuses, transformé en test.

    Un identifiant QML commençant par `on` suivi d'une majuscule est lu comme un
    gestionnaire de signal. Le fichier ne charge plus, et Qt n'en dit rien sur
    la sortie d'erreur quand il journalise vers systemd.
    """
    offenders = re.findall(r"readonly property \w+ (on[A-Z]\w*)", QML_THEME)
    assert offenders == [], (
        f"noms interdits en QML : {offenders} — QML y voit des gestionnaires de signal"
    )


@pytest.mark.parametrize("ink,ground,minimum", TOKENS["contrast"]["pairs"])
def test_text_pairs_stay_readable(ink: str, ground: str, minimum: float) -> None:
    """Seuil AA pour du texte normal, recalculé — jamais recopié."""
    measured = contrast(COLORS[ink], COLORS[ground])
    assert measured >= minimum, (
        f"« {ink} » sur « {ground} » ne contraste qu'à {measured:.2f}, minimum {minimum}"
    )


def test_radii_match_on_both_clients() -> None:
    radius = TOKENS["radius"]
    for name, value in (("radiusChip", radius["chip"]), ("radiusControl", radius["control"]),
                        ("radiusCard", radius["card"]), ("radiusPod", radius["pod"])):
        assert re.search(rf"readonly property int {name}: {value}\b", QML_THEME), (
            f"le rayon « {name} » n'est pas {value} côté Qt"
        )
    assert f"extraLarge = RoundedCornerShape({radius['pod']}.dp)" in KT_THEME
    assert f"large = RoundedCornerShape({radius['card']}.dp)" in KT_THEME
    assert f"medium = RoundedCornerShape({radius['control']}.dp)" in KT_THEME


def test_type_scale_matches_on_both_clients() -> None:
    scale = {
        "sizeHero": TOKENS["type"]["hero"],
        "sizeHeadlineLg": TOKENS["type"]["headlineLg"],
        "sizeHeadlineMd": TOKENS["type"]["headlineMd"],
        "sizeTitleMd": TOKENS["type"]["titleMd"],
        "sizeBodyLg": TOKENS["type"]["bodyLg"],
        "sizeBodySm": TOKENS["type"]["bodySm"],
        "sizeCaption": TOKENS["type"]["caption"],
    }
    for name, value in scale.items():
        assert re.search(rf"readonly property int {name}: {value}\b", QML_THEME), (
            f"la taille « {name} » n'est pas {value} côté Qt"
        )
    for value in scale.values():
        assert f"fontSize = {value}.sp" in KT_THEME, (
            f"aucun style Android n'utilise la taille {value}"
        )


def test_weight_never_exceeds_semibold() -> None:
    """La DA l'interdit explicitement : pas de noir gras sur les grands titres."""
    assert "readonly property int weightSemi: Font.DemiBold" in QML_THEME
    assert "FontWeight.Bold" not in KT_THEME, "graisse trop lourde côté Android"
    assert "FontWeight.Black" not in KT_THEME


def test_motion_durations_match_on_both_clients() -> None:
    motion = TOKENS["motion"]
    for name, key in (("hoverMs", "hoverMs"), ("enterMs", "enterMs"),
                      ("staggerMs", "staggerMs"), ("pulseMs", "pulseMs")):
        assert re.search(rf"readonly property int {name}: {motion[key]}\b", QML_THEME), (
            f"la durée « {name} » n'est pas {motion[key]} côté Qt"
        )
    for constant, key in (("HOVER_MS", "hoverMs"), ("ENTER_MS", "enterMs"),
                          ("STAGGER_MS", "staggerMs"), ("PULSE_MS", "pulseMs")):
        assert f"const val {constant} = {motion[key]}" in KT_THEME, (
            f"la durée « {constant} » ne correspond plus aux jetons"
        )


def test_the_desktop_declares_the_font_stack() -> None:
    """La pile vit dans main.cpp : le type `font` de QML n'accepte qu'une famille."""
    expected = ", ".join(f'"{name}"' for name in TOKENS["type"]["families"])
    assert f"setFamilies({{{expected}}})" in DESKTOP_MAIN, (
        "la pile de polices du desktop ne correspond plus aux jetons"
    )


def test_the_mark_has_the_same_geometry_on_both_clients() -> None:
    points = TOKENS["star"]["points"]
    ratio = TOKENS["star"]["innerRatio"]
    assert f"property int points: {points}" in QML_STAR
    assert f"property real innerRatio: {ratio}" in QML_STAR
    assert f"points: Int = {points}" in KT_STAR
    assert f"innerRatio: Float = {ratio}f" in KT_STAR
