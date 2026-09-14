"""Génère `tests/vectors/param_sfo_basic.json` (M8 §4).

Les vecteurs sont des **octets SFO réels**, construits ici plutôt qu'écrits à la
main en base64 : un fichier binaire recopié à la main est illisible en revue et
indébogable quand un cas casse. Ce script est la source, le JSON est l'artefact
versionné, et les deux parseurs exécutent le JSON.

Ré-exécuter après toute modification, puis commiter le JSON.
"""

from __future__ import annotations

import base64
import json
import pathlib
import struct

MAGIC = b"\x00PSF"
FMT_UTF8 = 0x0204
FMT_INT32 = 0x0404


def build_sfo(
    entries: list[tuple[str, int, bytes]],
    *,
    magic: bytes = MAGIC,
    version: int = 0x0101,
    key_table_offset: int | None = None,
    data_table_offset: int | None = None,
    entry_count: int | None = None,
    key_offset_override: int | None = None,
    data_offset_override: int | None = None,
) -> bytes:
    """Assembler un SFO. Les surcharges servent à fabriquer les cas malformés."""

    header_size = 20
    index_size = 16 * len(entries)

    keys = bytearray()
    key_offsets: list[int] = []
    for key, _fmt, _value in entries:
        key_offsets.append(len(keys))
        keys += key.encode("utf-8") + b"\x00"
    while len(keys) % 4:
        keys += b"\x00"

    data = bytearray()
    data_offsets: list[int] = []
    for _key, _fmt, value in entries:
        data_offsets.append(len(data))
        data += value
        while len(data) % 4:
            data += b"\x00"

    real_key_table = header_size + index_size
    real_data_table = real_key_table + len(keys)

    header = struct.pack(
        "<4sIIII",
        magic,
        version,
        real_key_table if key_table_offset is None else key_table_offset,
        real_data_table if data_table_offset is None else data_table_offset,
        len(entries) if entry_count is None else entry_count,
    )

    index = bytearray()
    for position, (_key, fmt, value) in enumerate(entries):
        padded = len(value)
        while padded % 4:
            padded += 1
        index += struct.pack(
            "<HHIII",
            key_offsets[position] if key_offset_override is None else key_offset_override,
            fmt,
            len(value),
            padded,
            data_offsets[position] if data_offset_override is None else data_offset_override,
        )

    return header + bytes(index) + bytes(keys) + bytes(data)


def utf8(value: str) -> bytes:
    return value.encode("utf-8") + b"\x00"


CASES: list[dict] = []


def case(name: str, raw: bytes, expected: dict | None) -> None:
    CASES.append(
        {
            "name": name,
            "input_base64": base64.b64encode(raw).decode("ascii"),
            "expected": expected,
        }
    )


# --- cas nominaux -----------------------------------------------------------
case(
    "nominal_ascii_title_and_savedata_title",
    build_sfo(
        [
            ("SAVEDATA_TITLE", FMT_UTF8, utf8("Chapitre 3")),
            ("TITLE", FMT_UTF8, utf8("Final Fantasy Tactics")),
        ]
    ),
    {"title": "Final Fantasy Tactics", "savedata_title": "Chapitre 3"},
)

case(
    "non_ascii_japanese_title_is_not_normalized",
    build_sfo(
        [
            ("SAVEDATA_TITLE", FMT_UTF8, utf8("セーブ１")),
            ("TITLE", FMT_UTF8, utf8("ファイナルファンタジー")),
        ]
    ),
    {"title": "ファイナルファンタジー", "savedata_title": "セーブ１"},
)

case(
    "title_present_without_savedata_title",
    build_sfo([("TITLE", FMT_UTF8, utf8("Wipeout Pure"))]),
    {"title": "Wipeout Pure", "savedata_title": None},
)

case(
    "title_absent_keeps_the_savedata_title",
    build_sfo(
        [
            ("PARENTAL_LEVEL", FMT_INT32, struct.pack("<I", 1)),
            ("SAVEDATA_TITLE", FMT_UTF8, utf8("Slot 1")),
        ]
    ),
    {"title": None, "savedata_title": "Slot 1"},
)

case(
    "integer_fields_are_ignored_not_decoded",
    build_sfo(
        [
            ("CATEGORY", FMT_UTF8, utf8("MS")),
            ("PARENTAL_LEVEL", FMT_INT32, struct.pack("<I", 5)),
            ("TITLE", FMT_UTF8, utf8("Patapon")),
        ]
    ),
    {"title": "Patapon", "savedata_title": None},
)

case(
    "a_title_declared_as_int32_is_refused_rather_than_guessed",
    build_sfo([("TITLE", FMT_INT32, struct.pack("<I", 42))]),
    {"title": None, "savedata_title": None},
)

case(
    "trailing_nulls_are_stripped_from_the_value",
    build_sfo([("TITLE", FMT_UTF8, b"Daxter" + b"\x00\x00\x00")]),
    {"title": "Daxter", "savedata_title": None},
)

case(
    "an_empty_title_reads_as_absent",
    build_sfo([("TITLE", FMT_UTF8, b"\x00")]),
    {"title": None, "savedata_title": None},
)

# --- structure d'un vrai PARAM.SFO ------------------------------------------
# Relevée sur un `PSP/SAVEDATA/` du Thor le 03/08 : 8 entrées, 4912 octets, et
# surtout deux champs au format 0x0004 — UTF-8 **non** terminé par un nul, dont
# `SAVEDATA_PARAMS` qui contient en réalité du binaire.
#
# Les valeurs sont neutralisées : le fichier d'origine porte `SAVEDATA_DETAIL`,
# c'est-à-dire la progression réelle du joueur, qui n'a rien à faire dans un
# dépôt. Seule la forme est reprise, et c'est elle qui compte — aucun de mes
# vecteurs synthétiques n'avait de champ 0x0004 avant celui-ci.
FMT_UTF8_RAW = 0x0004
case(
    "real_world_layout_with_binary_and_unterminated_fields",
    build_sfo(
        [
            ("CATEGORY", FMT_UTF8, utf8("MS")),
            ("PARENTAL_LEVEL", FMT_INT32, struct.pack("<I", 2)),
            ("SAVEDATA_DETAIL", FMT_UTF8, utf8("World 1 - 1")),
            ("SAVEDATA_DIRECTORY", FMT_UTF8, utf8("UCET00357_GameData0")),
            ("SAVEDATA_FILE_LIST", FMT_UTF8_RAW, b"DATA.BIN" + bytes(3160)),
            ("SAVEDATA_PARAMS", FMT_UTF8_RAW, bytes(range(128))),
            ("SAVEDATA_TITLE", FMT_UTF8, utf8("GameData")),
            ("TITLE", FMT_UTF8, utf8("LocoRoco")),
        ]
    ),
    {"title": "LocoRoco", "savedata_title": "GameData"},
)

# --- cas malformés : jamais d'exception, `null` quand le fichier n'est pas un SFO
case("empty_file", b"", None)
case("invalid_magic", b"RIFF" + build_sfo([("TITLE", FMT_UTF8, utf8("X"))])[4:], None)
case("header_truncated", build_sfo([("TITLE", FMT_UTF8, utf8("X"))])[:12], None)

_full = build_sfo([("TITLE", FMT_UTF8, utf8("Killzone Liberation"))])
case("index_table_truncated", _full[:24], None)
case("data_truncated_mid_value", _full[: len(_full) - 6], {"title": None, "savedata_title": None})

case(
    "key_table_offset_out_of_bounds",
    build_sfo([("TITLE", FMT_UTF8, utf8("X"))], key_table_offset=0xFFFF),
    {"title": None, "savedata_title": None},
)
case(
    "data_table_offset_out_of_bounds",
    build_sfo([("TITLE", FMT_UTF8, utf8("X"))], data_table_offset=0xFFFF),
    {"title": None, "savedata_title": None},
)
case(
    "key_offset_out_of_bounds",
    build_sfo([("TITLE", FMT_UTF8, utf8("X"))], key_offset_override=0xFFF0),
    {"title": None, "savedata_title": None},
)
case(
    "data_offset_out_of_bounds",
    build_sfo([("TITLE", FMT_UTF8, utf8("X"))], data_offset_override=0xFFFFFF),
    {"title": None, "savedata_title": None},
)
# `null` et non un SFO vide : quand la table d'index déborde du fichier, aucune
# borne d'entrée n'est fiable — lire les entrées qui « tiennent » reviendrait à
# interpréter des octets pris ailleurs comme des offsets.
case(
    "entry_count_larger_than_the_file",
    build_sfo([("TITLE", FMT_UTF8, utf8("X"))], entry_count=5000),
    None,
)
case(
    "zero_entries_is_a_valid_but_empty_sfo",
    build_sfo([]),
    {"title": None, "savedata_title": None},
)
case(
    "invalid_utf8_in_a_value_does_not_raise",
    build_sfo([("TITLE", FMT_UTF8, b"\xff\xfe\x00")]),
    {"title": None, "savedata_title": None},
)

# Plafond §4 : au-delà de 64 Kio ce n'est plus un PARAM.SFO, on ne le lit pas.
_oversized = build_sfo([("TITLE", FMT_UTF8, utf8("Big"))]) + b"\x00" * (64 * 1024)
case("over_the_64_kib_ceiling_is_refused", _oversized, None)


def main() -> None:
    target = pathlib.Path(__file__).parents[2] / "tests" / "vectors" / "param_sfo_basic.json"
    target.write_text(json.dumps(CASES, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"{target.relative_to(pathlib.Path(__file__).parents[2])}: {len(CASES)} cas")


if __name__ == "__main__":
    main()
