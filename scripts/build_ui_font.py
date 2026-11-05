#!/usr/bin/env python3
"""Generate the embedded anti-aliased UI fonts (VLW, TFT_eSPI / LovyanGFX format).

One file per on-screen pixel height the UI actually uses, each rendered natively
at that height. There is deliberately no "master" font that gets scaled at
runtime: LovyanGFX's VLW scaler is nearest-neighbour, with no interpolation, so
a 14 px label taken from a 28 px master is visibly chunkier than one rendered at
14 px. Every size the firmware asks for exists as its own font and is drawn at
setTextSize(1.0).

EM_PX is solved per height so the font's reported height (ascent + descent)
matches the target, because that -- not the point size -- is what LovyanGFX
reports from fontHeight() and what the layout in ui/theme.h is written against.
Only the glyphs LovyanGFX itself measures are allowed to set that height; see
counts_towards_height().

Charset: ASCII, the Latin-1 accented block, Latin Extended-A and Romanian's
comma-below letters, the letters a few more Latin-script languages need, and
a little typography and the common symbols -- enough to set the languages of
western and central Europe, Romanian, Turkish and the Baltic ones, and
Azerbaijani, Welsh, the Sami languages, Uzbek, Hawaiian, Navajo, Guarani,
Yoruba, Igbo, Hausa and Akan. See CHARSET below.

Usage (regenerate everything the firmware embeds):
    python scripts/build_ui_font.py assets/fonts/NotoSans-Regular.ttf \
        --out-dir data --heights 15,17,20,24

Keep the list in step with platformio.ini's board_build.embed_files and with
s_fonts[] in src/hardware/display_font.cpp.

Requires Pillow (PIL) with FreeType support.

Adapted from the same script in ESP32-Plane-Radar, which needed both a scaled
master and a per-size set; this project only wants the latter.
"""
import argparse
import os
import struct

from PIL import Image, ImageDraw, ImageFont

# ASCII, less space. Space (0x20) is left out deliberately: LovyanGFX derives
# it from the advance width.
_ASCII = list(range(33, 127))

# The whole Latin-1 Supplement letter block, U+00C0..U+00FF, less the two
# maths operators sitting in it at U+00D7 and U+00F7.
#
# The block entire rather than the exact union of the four languages: it is
# contiguous, each letter costs about 150 bytes per font, and a set stopping
# at the union would render "Sinead" and "Sinead" differently depending on
# which accent the artist used. Italian, Catalan, Dutch, Nordic and Icelandic
# come along for free.
_LATIN1_LETTERS = [c for c in range(0xC0, 0x100) if c not in (0xD7, 0xF7)]

# Punctuation from the same block these languages actually need: the inverted
# marks Spanish opens with, guillemets for French and Spanish quotation,
# ordinal indicators for Spanish and Portuguese, the Catalan middle dot, and
# the degree sign the UI already had.
_LATIN1_PUNCT = [0xA1, 0xAA, 0xAB, 0xB0, 0xB7, 0xBA, 0xBB, 0xBF]

# Symbols from the same block that turn up in track and artist names: pound,
# yen, copyright, registered, micro (as in "µ-Ziq"), one half, and the
# multiplication sign left out of the letters above.
_LATIN1_SYMBOLS = [0xA3, 0xA5, 0xA9, 0xAE, 0xB5, 0xBD, 0xD7]

# Latin Extended-A, U+0100..U+017F, the block entire for the same reason as
# Latin-1's: Polish, Czech, Slovak, Hungarian, Croatian, Slovenian, Turkish,
# Maltese, the Baltic languages, Romanian's breve and circumflex letters, and
# the French OE ligature. Then what the block leaves out: Romanian's s and t
# with comma below, both cases -- the forms Romanian actually uses, rather
# than the cedilla ones in the block -- and the German capital sharp s.
#
# Most of the block's capitals carry a mark above the cap height, and so does
# U+0178, French capital Y with diaeresis. LovyanGFX would let them set the
# line height, which would make every face solve smaller to fit them; so they
# are left out of the measuring (_OVERHANG), and the firmware puts each face's
# line height back to the header's after loading it (display_font.cpp). They
# overhang the line, as Latin-1's accented capitals always have.
_LATIN_EXT_A = list(range(0x0100, 0x0180))
_LATIN_EXT = [0x0218, 0x0219, 0x021A, 0x021B, 0x1E9E]

# The letters that other Latin-script languages need beyond those blocks,
# each in both cases where it has two: schwa for Azerbaijani and Tatar; the
# w and y with grave, acute and diaeresis Welsh uses now and then; the g with
# stroke, g and k with caron, ezh and ezh with caron of Skolt and Inari Sami;
# the okina of Hawaiian, Samoan and Tongan, and the apostrophe Uzbek and
# Hausa write as a letter; Navajo's o with ogonek; Guarani's nasal e and y;
# the dotted letters of Yoruba and Igbo (the u, o and i serve Vietnamese
# too); Hausa's hooked b, d, k and y; and the open e and open o of Akan, Ewe,
# Ga and Lingala.
_LATIN_MORE = [
    0x018F, 0x0259,                                  # Ə ə
    0x1E80, 0x1E81, 0x1E82, 0x1E83, 0x1E84, 0x1E85,  # Ẁ ẁ Ẃ ẃ Ẅ ẅ
    0x1EF2, 0x1EF3,                                  # Ỳ ỳ
    0x01E4, 0x01E5, 0x01E6, 0x01E7, 0x01E8, 0x01E9,  # Ǥ ǥ Ǧ ǧ Ǩ ǩ
    0x01B7, 0x0292, 0x01EE, 0x01EF,                  # Ʒ ʒ Ǯ ǯ
    0x02BB, 0x02BC,                                  # ʻ ʼ
    0x01EA, 0x01EB,                                  # Ǫ ǫ
    0x1EBC, 0x1EBD, 0x1EF8, 0x1EF9,                  # Ẽ ẽ Ỹ ỹ
    0x1EB8, 0x1EB9, 0x1ECC, 0x1ECD, 0x1E62, 0x1E63,  # Ẹ ẹ Ọ ọ Ṣ ṣ
    0x1ECA, 0x1ECB, 0x1EE4, 0x1EE5, 0x1E44, 0x1E45,  # Ị ị Ụ ụ Ṅ ṅ
    0x0181, 0x0253, 0x018A, 0x0257,                  # Ɓ ɓ Ɗ ɗ
    0x0198, 0x0199, 0x01B3, 0x01B4,                  # Ƙ ƙ Ƴ ƴ
    0x0190, 0x025B, 0x0186, 0x0254,                  # Ɛ ɛ Ɔ ɔ
]

# General punctuation. Not language coverage as such, but track and artist
# names arrive from Music Assistant full of curly quotes, en dashes and real
# ellipses, and a codepoint the font does not have renders as a blank space.
_PUNCT = [0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x201E, 0x2026]

# German's low single quote, the bullet, the single guillemets of French and
# Swiss German, and the euro sign.
_SYMBOLS = [0x201A, 0x2022, 0x2039, 0x203A, 0x20AC]

# Sorted, and that is load-bearing rather than tidy: LovyanGFX looks glyphs up
# with std::lower_bound over the table in file order (VLWfont::getUnicodeIndex),
# so an unsorted table silently finds the wrong glyph or none at all.
CHARSET = sorted(set(_ASCII + _LATIN1_LETTERS + _LATIN1_PUNCT +
                     _LATIN1_SYMBOLS + _LATIN_EXT_A + _LATIN_EXT + _LATIN_MORE +
                     _PUNCT + _SYMBOLS))

# Glyphs that may reach above or below the line without moving it: the ones
# above with marks over a capital or under the baseline. The firmware resets
# each face's line height to the header's, which counts_towards_height() keeps
# them out of.
_OVERHANG = (set(_LATIN_EXT_A) | {0x0218, 0x0219, 0x021A, 0x021B} |
             set(_LATIN_MORE))

# gUnicode[] is uint16_t, so nothing outside the Basic Multilingual Plane.
assert all(0 < cp <= 0xFFFF for cp in CHARSET)


def counts_towards_height(cp: int) -> bool:
    """Whether LovyanGFX lets this glyph set the font's ascent and descent.

    Its VLW loader computes yAdvance from the glyph extents, but skips
    U+00A0..U+00FF while doing it (lgfx_fonts.cpp, VLWfont::loadFont).
    Accented capitals are taller than any ASCII letter, and letting them set
    the line height would push every layout on the panel around.

    The generator has to use the same rule. Measuring the full charset would
    solve EM_PX against a height the device is never going to report, and
    every ASCII label would come out a size small to make room for an accent
    that is not on it.

    _OVERHANG is left out too. LovyanGFX would count it, but the firmware
    puts each face's line height back to the header's once it is loaded, so
    the header -- measured here -- is what the device reports.
    """
    if cp in _OVERHANG:
        return False
    return cp > 0xFF or (0x20 < cp < 0xA0 and cp != 0x7F)


def check_metric_drivers(glyphs, hdr_ascent: int, hdr_descent: int) -> None:
    """Complain if a non-ASCII glyph is what set the line height.

    The layout in ui/theme.h is written against the height ASCII needs. A
    glyph that reaches higher and is not in the range LovyanGFX skips drags
    the whole font down a size to make room for itself, which is how adding
    one rare accented capital silently shrinks every label on the panel.
    Loud, because the symptom -- everything a bit small -- does not point at
    its cause.
    """
    ascii_glyphs = [g for g in glyphs
                    if 0x20 < g[0] < 0xA0 and g[0] != 0x7F and g[1] > 0]
    ascii_ascent = max(g[4] for g in ascii_glyphs)
    ascii_descent = max(g[1] - g[4] for g in ascii_glyphs)

    for g in glyphs:
        if not counts_towards_height(g[0]) or g[0] < 0xA0:
            continue
        if g[4] > ascii_ascent or (g[1] - g[4]) > ascii_descent:
            raise SystemExit(
                f"U+{g[0]:04X} {chr(g[0])!r} is taller than ASCII "
                f"(ascent {g[4]} vs {ascii_ascent}, descent {g[1] - g[4]} vs "
                f"{ascii_descent}) and is counted towards the line height, so "
                f"every font would solve smaller to fit it. Either drop it "
                f"from CHARSET or accept the shrink deliberately.")


def build(ttf_path: str, em_px: int) -> tuple[bytes, int]:
    """Return (vlw_bytes, font_height) for the font rendered at em_px."""
    font = ImageFont.truetype(ttf_path, em_px)
    ascent, descent = font.getmetrics()
    pad = em_px
    canvas_w, canvas_h = em_px * 3, ascent + descent + 2 * pad
    baseline_y, pen_x = pad + ascent, pad

    glyphs = []  # (codepoint, height, width, xAdvance, dY, gdX, bitmap)
    for cp in CHARSET:
        img = Image.new("L", (canvas_w, canvas_h), 0)
        ImageDraw.Draw(img).text((pen_x, baseline_y), chr(cp), fill=255,
                                 font=font, anchor="ls")
        advance = round(font.getlength(chr(cp)))
        bbox = img.getbbox()
        if bbox is None:  # blank glyph
            glyphs.append((cp, 0, 0, advance, 0, 0, b""))
            continue
        x0, y0, x1, y1 = bbox
        w, h = x1 - x0, y1 - y0
        glyphs.append((cp, h, w, advance, baseline_y - y0, x0 - pen_x,
                       img.crop(bbox).tobytes()))

    # Field limits enforced by the VLW readers.
    assert all(g[2] < 256 and g[1] < 256 and g[3] < 256 for g in glyphs)
    assert all(-128 <= g[5] < 128 for g in glyphs)

    measured = [g for g in glyphs if counts_towards_height(g[0])]
    hdr_ascent = max(g[4] for g in measured)
    hdr_descent = max(g[1] - g[4] for g in measured)
    check_metric_drivers(glyphs, hdr_ascent, hdr_descent)

    out = bytearray()
    # Header (big-endian): count, version, fontSize, mboxY(unused), ascent, descent
    out += struct.pack(">IIIIII", len(glyphs), 11, em_px, 0, hdr_ascent, hdr_descent)
    # Per-glyph metrics: unicode, height, width, xAdvance, dY, gdX, pad
    for cp, h, w, adv, dY, gdX, _ in glyphs:
        out += struct.pack(">iiiiiii", cp, h, w, adv, dY, gdX, 0)
    # Bitmaps (8-bit alpha, row-major), in glyph order
    for *_, bmp in glyphs:
        out += bmp
    # LovyanGFX reports fontHeight() as ascent + descent (the header values).
    return bytes(out), hdr_ascent + hdr_descent


def solve_em_for_height(ttf_path: str, target_h: int) -> tuple[int, bytes, int]:
    """Find the EM_PX whose rendered font-height is closest to target_h.

    Height grows monotonically with EM_PX -- ~1.03x for CHARSET, ~0.73x for
    digits alone -- so one render at target_h estimates the ratio, and a small
    window around that estimate is scanned, preferring the smallest EM_PX
    whose height is >= target."""
    best = None
    _, h0 = build(ttf_path, target_h)
    guess = round(target_h * target_h / max(h0, 1))
    lo = max(6, guess - 8)
    for em in range(lo, guess + 9):
        data, h = build(ttf_path, em)
        key = (abs(h - target_h), 0 if h >= target_h else 1)
        if best is None or key < best[0]:
            best = (key, em, data, h)
    _, em, data, h = best
    return em, data, h


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("ttf", help="path to a TrueType font")
    ap.add_argument("--out-dir", default="data", help="output directory")
    ap.add_argument("--heights", required=True,
                    help="comma-separated on-screen heights, e.g. 14,17,19,28")
    ap.add_argument("--chars",
                    help="only these characters instead of CHARSET, e.g. "
                         "'0123456789:' for the clock face's digits")
    ap.add_argument("--prefix", default="ui_font",
                    help="file name before the height, e.g. clock_font")
    args = ap.parse_args()
    if args.chars:
        # The height is then measured over these alone, as the firmware
        # measures a face over the glyphs it has.
        global CHARSET
        CHARSET = sorted({ord(c) for c in args.chars})

    os.makedirs(args.out_dir, exist_ok=True)
    total = 0
    for target in [int(x) for x in args.heights.split(",") if x.strip()]:
        em, data, h = solve_em_for_height(args.ttf, target)
        path = os.path.join(args.out_dir, f"{args.prefix}_{target}.vlw")
        with open(path, "wb") as f:
            f.write(data)
        total += len(data)
        status = "exact" if h == target else f"height {h}"
        print(f"wrote {path}: {len(data):6d} bytes @ EM_PX={em:2d} ({status})")
    print(f"{total} bytes total")


if __name__ == "__main__":
    main()
