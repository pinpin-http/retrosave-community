"""Génère `tests/vectors/display_name_basic.json` (M8 §5).

`display_name` est **distincte** de `normalize_game_name`, qui produit une clé
d'appariement : la modifier casserait des unités existantes. Celle-ci produit un
libellé présentable, et conserve la casse.

Ré-exécuter après toute modification, puis commiter le JSON.
"""

from __future__ import annotations

import json
import pathlib

CASES: list[dict[str, str]] = []


def case(name: str, value: str, expected: str) -> None:
    CASES.append({"name": name, "input": value, "expected": expected})


# --- cas nominaux : ce que M8 §5 promet -------------------------------------
case(
    "region_and_dump_tags_removed_case_preserved",
    "Pokemon_Platinum_Version_(USA)_[!].sav",
    "Pokemon Platinum Version",
)
case("europe_tag_and_languages", "Professor_Layton_[Europe] (En,Fr).sav", "Professor Layton")
case("underscores_become_spaces", "Final_Fantasy_Tactics.srm", "Final Fantasy Tactics")
case("dots_inside_the_name_become_spaces", "Mario.Kart.DS.sav", "Mario Kart DS")
case(
    "mixed_whitespace_is_compacted",
    "  The__World  Ends With_You (USA).SAV",
    "The World Ends With You",
)
case("no_extension_at_all", "Chrono Trigger", "Chrono Trigger")

# --- la casse est conservée : c'est toute la différence avec normalize_name --
case("uppercase_is_kept", "METAL GEAR SOLID (USA).sav", "METAL GEAR SOLID")
case("accents_are_kept_untouched", "Pokémon_Édition_Noire_(France).sav", "Pokémon Édition Noire")
case("japanese_title_is_left_alone", "ファイナルファンタジー.sav", "ファイナルファンタジー")

# --- tags connus seulement : un parenthésé porteur de sens doit survivre -----
case(
    "an_unknown_parenthetical_is_part_of_the_title",
    "Tony Hawk's Underground 2 (Remix).sav",
    "Tony Hawk's Underground 2 (Remix)",
)
case(
    "a_subtitle_in_brackets_is_kept_when_it_is_not_a_dump_tag",
    "Zelda [Master Quest].sav",
    "Zelda [Master Quest]",
)
case("dump_flag_alone", "Sonic [!].srm", "Sonic")
case("bad_dump_flag", "Sonic [b].srm", "Sonic")
case("fixed_and_translated_flags", "Sonic [f1][T+Fre].srm", "Sonic")
case("revision_tag", "Street Fighter (Rev 1).srm", "Street Fighter")
case("multi_region_tag", "Tekken (Europe, Australia).srm", "Tekken")
case("proto_and_beta", "Starfox (Proto) (Beta).srm", "Starfox")

# --- dégradations : ne jamais rendre une chaîne vide -------------------------
case(
    "a_name_made_only_of_tags_falls_back_to_the_raw_stem",
    "(USA) [!].sav",
    "(USA) [!]",
)
case("empty_input_stays_empty", "", "")
# Ce qui compte ici n'est pas le point de tête mais le fait qu'un nom commençant
# par un point ne soit pas *amputé* comme s'il n'était qu'une extension : il
# resterait une ligne vide dans la bibliothèque. Le point lui-même disparaît
# avec la règle « point = séparateur de mots », et « sav » est un libellé
# parfaitement lisible.
case("a_leading_dot_is_not_an_extension_to_strip", ".sav", "sav")
case("dotfile_keeps_its_name", ".nomedia", "nomedia")
case("trailing_separators_are_trimmed", "Metroid_-_Zero_Mission_.sav", "Metroid - Zero Mission")


def main() -> None:
    target = pathlib.Path(__file__).parents[2] / "tests" / "vectors" / "display_name_basic.json"
    target.write_text(json.dumps(CASES, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"{target.relative_to(pathlib.Path(__file__).parents[2])}: {len(CASES)} cas")


if __name__ == "__main__":
    main()
