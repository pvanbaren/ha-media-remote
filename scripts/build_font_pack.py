#!/usr/bin/env python3
"""Generate the font pack: the outline fonts the remote draws, from its
spare flash partition, the scripts its UI faces lack -- Korean, Chinese,
Japanese, Thai, Cyrillic, Greek, the rest of Latin -- and symbols.

The UI faces are Latin (scripts/build_ui_font.py). A character none of them
has is drawn from the first font in the pack that has it, rendered on the
device at the face's own size (src/hardware/outline_fonts.cpp). The pack is
kept on GitHub, as the asset fonts.bin of the pre-release whose tag is
config::kFontPackTag (include/config.h), and the remote downloads it into
its spare flash partition when asked to: Settings > Fonts on the remote, or
Download fonts in the portal (src/services/fonts.cpp).

Each font is a Noto Sans member -- the family the UI faces are cut from -- at
the Regular weight, with no hinting and no layout tables, cut down to:

  Kore  Korean      KS X 1001's 2,350 Hangul syllables and the 94 jamo
                    (Noto Sans KR)
  Hans  Chinese     GB2312: its 6,763 Han characters, the CJK and fullwidth
                    punctuation (Noto Sans SC)
  Jpan  Japanese    JIS X 0208: kana, its 6,355 kanji, the punctuation and
                    fullwidth forms; halfwidth katakana (Noto Sans JP)
  Hant  Traditional the common Traditional characters -- Big5's first level
                    -- that neither of those has (Noto Sans TC)
  Thai  Thai        the Thai block (Noto Sans Thai)
  Latn  Latin &     the rest of Latin -- Vietnamese's letters, Latin
        symbols     Extended-B, IPA, the combining accents a title may spell
                    letters with (an e and a combining acute) -- and the
                    punctuation, currency, letterlike signs and fractions the
                    faces lack: (TM), (P), one quarter, dagger (Noto Sans)
  Grek  Greek       the Greek blocks, monotonic and polytonic (Noto Sans)
  Cyrl  Cyrillic    the Cyrillic block (Noto Sans)
  Zmth  Arrows &    arrows and mathematical operators (Noto Sans Math)
        math
  Zsym  Symbols     the miscellaneous symbols -- suits, weather, smiles,
                    music notes -- circled numbers, technical signs (Noto
                    Sans Symbols)
  Zdng  Dingbats    dingbats -- check marks, hearts, stars -- geometric shapes
                    and more arrows (Noto Sans Symbols 2)

The order on the command line is the order the remote looks in, so a Han
character Chinese and Japanese share is drawn as the first of them draws
it; and the Latin-style fonts go before them, so that an arrow or a star
both have is drawn to match the Latin text. Hant takes only what Hans and
Jpan lack, whatever the order. The script is ISO 15924's code where there is
one.

The sources are Google Fonts' fonts, which are not kept in the repository
but for Noto Sans, assets/fonts/NotoSans-Regular.ttf:
    https://github.com/google/fonts/tree/main/ofl/notosanskr
    https://github.com/google/fonts/tree/main/ofl/notosanssc
    https://github.com/google/fonts/tree/main/ofl/notosansjp
    https://github.com/google/fonts/tree/main/ofl/notosanstc
    https://github.com/google/fonts/tree/main/ofl/notosansthai
    https://github.com/google/fonts/tree/main/ofl/notosansmath
    https://github.com/google/fonts/tree/main/ofl/notosanssymbols
    https://github.com/google/fonts/tree/main/ofl/notosanssymbols2
Their licence, the SIL Open Font License 1.1, goes on the release with the
pack.

Usage:
    python scripts/build_font_pack.py --tag fonts-1 --out data/fonts.bin \\
        Kore="NotoSansKR[wght].ttf" \\
        Latn=assets/fonts/NotoSans-Regular.ttf \\
        Grek=assets/fonts/NotoSans-Regular.ttf \\
        Cyrl=assets/fonts/NotoSans-Regular.ttf \\
        Zmth=NotoSansMath-Regular.ttf Zsym="NotoSansSymbols[wght].ttf" \\
        Zdng=NotoSansSymbols2-Regular.ttf \\
        Hans="NotoSansSC[wght].ttf" Jpan="NotoSansJP[wght].ttf" \\
        Hant="NotoSansTC[wght].ttf" Thai="NotoSansThai[wdth,wght].ttf"

The pack, little endian throughout (include/hardware/font_pack.h):
  0     "HMRFONTS"
  8     u32  format, 1
  12    u32  the pack's length in bytes, this header included
  16    u32  CRC-32 (zlib's) of every byte from 4096 to the end
  20    u32  how many fonts
  24    the tag it was released under, NUL-padded to 24 bytes
  48    per font, 32 bytes: the script (4 letters, ISO 15924), u32 where the
        font starts, u32 its length, its name NUL-padded to 20 bytes
  4096  the fonts, each starting on a 4-byte boundary

Requires fontTools.
"""
import argparse
import io
import struct
import zlib

from fontTools import subset
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer

MAGIC = b"HMRFONTS"
FORMAT = 1
HEADER_BYTES = 4096
MAX_FONTS = 16


def _double_byte(encoding, rows):
    """Every character `encoding` encodes in the given lead-byte rows."""
    cps = set()
    for hi in rows:
        for lo in range(0xA1, 0xFF):
            try:
                cps.add(ord(bytes([hi, lo]).decode(encoding)))
            except UnicodeDecodeError:
                pass
    return cps


def korean():
    """KS X 1001's 2,350 Hangul syllables (EUC-KR rows 0xB0-0xC8) and the
    compatibility jamo."""
    return _double_byte("euc-kr", range(0xB0, 0xC9)) | set(range(0x3131, 0x318F))


def chinese():
    """GB2312: its punctuation (row 1), fullwidth forms (3) and both levels
    of Han characters (rows 16-87)."""
    return _double_byte("gb2312", [0xA1, 0xA3] + list(range(0xB0, 0xF8)))


def japanese():
    """JIS X 0208: punctuation (row 1), fullwidth forms (3), hiragana (4),
    katakana (5) and both levels of kanji (rows 16-84); and the halfwidth
    katakana. Not its Greek, Cyrillic or box drawing (rows 6-8)."""
    rows = [0xA1, 0xA3, 0xA4, 0xA5] + list(range(0xB0, 0xF5))
    return _double_byte("euc_jp", rows) | set(range(0xFF61, 0xFFA0))


def traditional():
    """Big5's first level, its 5,401 common Han characters, but those GB2312
    or JIS X 0208 already has: the Chinese and Japanese fonts draw those."""
    cps = set()
    for hi in range(0xA4, 0xC7):
        for lo in list(range(0x40, 0x7F)) + list(range(0xA1, 0xFF)):
            try:
                cps.add(ord(bytes([hi, lo]).decode("big5")))
            except UnicodeDecodeError:
                pass
    return cps - chinese() - japanese()


def thai():
    return set(range(0x0E01, 0x0E3B)) | set(range(0x0E3F, 0x0E5C))


def cyrillic():
    return set(range(0x0400, 0x0500))


def greek():
    return set(range(0x0370, 0x0400)) | set(range(0x1F00, 0x2000))


def _blocks(*ranges):
    cps = set()
    for first, last in ranges:
        cps |= set(range(first, last + 1))
    return cps


def latin():
    """Latin-1 to Latin Extended-B, IPA and the modifier letters, the
    combining accents, Latin Extended Additional (Vietnamese among it),
    Extended-C and -D; general punctuation, super- and subscripts,
    currency, letterlike signs and number forms. The faces have much of
    the first blocks, and draw what they have themselves."""
    return _blocks((0x00A0, 0x036F), (0x1E00, 0x1EFF), (0x2C60, 0x2C7F),
                   (0xA720, 0xA7FF), (0x2000, 0x218F))


def arrows_and_math():
    return _blocks((0x2190, 0x22FF), (0x27C0, 0x27FF), (0x2900, 0x2AFF))


def symbols():
    """Technical signs, circled and parenthesised numbers, and the
    miscellaneous symbols: suits, weather, smiles, music notes."""
    return _blocks((0x2300, 0x23FF), (0x2460, 0x24FF), (0x2600, 0x26FF))


def dingbats():
    """Geometric shapes, the miscellaneous symbols Noto Sans Symbols lacks,
    dingbats, and the miscellaneous symbols and arrows."""
    return _blocks((0x2190, 0x21FF), (0x25A0, 0x26FF), (0x2700, 0x27BF),
                   (0x2B00, 0x2BFF))


RECIPES = {
    "Kore": ("Korean", korean),
    "Hans": ("Chinese", chinese),
    "Jpan": ("Japanese", japanese),
    "Hant": ("Traditional Chinese", traditional),
    "Thai": ("Thai", thai),
    "Cyrl": ("Cyrillic", cyrillic),
    "Grek": ("Greek", greek),
    "Latn": ("Latin & symbols", latin),
    "Zmth": ("Arrows & math", arrows_and_math),
    "Zsym": ("Symbols", symbols),
    "Zdng": ("Dingbats", dingbats),
}


def cut(source, wanted):
    """The source at the Regular weight, every other axis at its default,
    cut down to the characters in `wanted` it has."""
    font = TTFont(source)
    if "fvar" in font:
        limits = {axis.axisTag: (400 if axis.axisTag == "wght"
                                 else axis.defaultValue)
                  for axis in font["fvar"].axes}
        font = instancer.instantiateVariableFont(font, limits)
    have = set(font.getBestCmap())
    options = subset.Options()
    options.hinting = False
    options.layout_features = []
    options.notdef_outline = True
    options.name_IDs = []
    options.drop_tables += ["DSIG", "GSUB", "GPOS", "GDEF", "STAT", "vhea",
                            "vmtx", "meta"]
    subsetter = subset.Subsetter(options)
    subsetter.populate(unicodes=sorted(wanted & have))
    subsetter.subset(font)
    out = io.BytesIO()
    font.save(out)
    return out.getvalue(), len(wanted & have), len(wanted - have)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--tag", required=True,
                        help="the release the pack goes on: fonts-1")
    parser.add_argument("--out", required=True, help="data/fonts.bin")
    parser.add_argument("fonts", nargs="+", metavar="SCRIPT=SOURCE",
                        help="Kore, Hans, Jpan or Thai, and its source font")
    args = parser.parse_args()

    tag = args.tag.encode("ascii")
    if len(tag) >= 24:
        parser.error("the tag is longer than 23 characters")
    if len(args.fonts) > MAX_FONTS:
        parser.error(f"at most {MAX_FONTS} fonts")

    entries = []
    body = bytearray()
    for item in args.fonts:
        script, _, source = item.partition("=")
        if script not in RECIPES or not source:
            parser.error(f"{item}: want SCRIPT=SOURCE, SCRIPT one of "
                         f"{', '.join(RECIPES)}")
        name, charset = RECIPES[script]
        data, count, missing = cut(source, charset())
        while len(body) % 4:
            body.append(0)
        entries.append((script, HEADER_BYTES + len(body), len(data), name))
        body += data
        note = f", {missing} it lacks" if missing else ""
        print(f"{script} {name}: {count} characters{note}, "
              f"{len(data) / 1024:,.0f} KB")

    total = HEADER_BYTES + len(body)
    header = bytearray(HEADER_BYTES)
    struct.pack_into("<8sIIII24s", header, 0, MAGIC, FORMAT, total,
                     zlib.crc32(body), len(entries), tag)
    for i, (script, offset, length, name) in enumerate(entries):
        struct.pack_into("<4sII20s", header, 48 + 32 * i,
                         script.encode("ascii"), offset, length,
                         name.encode("ascii"))
    with open(args.out, "wb") as f:
        f.write(header)
        f.write(body)
    print(f"{args.out}: {len(entries)} fonts, {total / 1024:,.0f} KB, "
          f"tag {args.tag}")


if __name__ == "__main__":
    main()
