#!/usr/bin/env python3
"""Generate data/korean.ttf, the outline font Korean is drawn from.

The VLW faces are Latin; Korean is drawn from this instead, rendered on the
device at each face's size (src/hardware/korean_font.cpp). It is Noto Sans KR,
the Korean member of the family the Latin faces are cut from, at the Regular
weight, cut down to what KS X 1001 encodes: the 2,350 common Hangul syllables
and the 94 compatibility jamo -- what a K-pop artist's name or a song title is
written in -- with no hinting and no layout tables. About 415 KB.

The source is Google Fonts' variable Noto Sans KR, which is not kept in the
repository (10 MB):
    https://github.com/google/fonts/tree/main/ofl/notosanskr
Its licence, the SIL Open Font License 1.1, is assets/fonts/NotoSansKR-OFL.txt.

Usage:
    python scripts/build_korean_font.py "NotoSansKR[wght].ttf" data/korean.ttf

Requires fontTools.
"""
import argparse

from fontTools import subset
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer


def ksx1001_hangul():
    """Every syllable EUC-KR encodes (rows 0xB0-0xC8), and the compatibility
    jamo, U+3131-U+318E."""
    cps = set()
    for hi in range(0xB0, 0xC9):
        for lo in range(0xA1, 0xFF):
            try:
                cps.add(ord(bytes([hi, lo]).decode("euc-kr")))
            except UnicodeDecodeError:
                pass
    cps |= set(range(0x3131, 0x318F))
    return sorted(cps)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("source", help="Google Fonts' NotoSansKR[wght].ttf")
    parser.add_argument("out", help="where to write it: data/korean.ttf")
    args = parser.parse_args()

    font = instancer.instantiateVariableFont(TTFont(args.source),
                                             {"wght": 400})
    options = subset.Options()
    options.hinting = False
    options.layout_features = []
    options.notdef_outline = True
    options.name_IDs = [0, 1, 2, 3, 4, 5, 6, 13, 14]
    options.drop_tables += ["DSIG", "GSUB", "GPOS", "GDEF", "STAT", "vhea",
                            "vmtx"]
    subsetter = subset.Subsetter(options)
    subsetter.populate(unicodes=ksx1001_hangul())
    subsetter.subset(font)

    # The instance keeps the variable font's default names, which are the
    # Thin's: say what it is.
    names = font["name"]
    for record in list(names.names):
        if record.nameID == 1:
            names.setName("Noto Sans KR", 1, record.platformID,
                          record.platEncID, record.langID)
        elif record.nameID == 2:
            names.setName("Regular", 2, record.platformID, record.platEncID,
                          record.langID)
        elif record.nameID in (3, 4, 6):
            names.removeNames(nameID=record.nameID)
    names.setName("Noto Sans KR Regular", 4, 3, 1, 0x409)
    names.setName("NotoSansKR-Regular", 6, 3, 1, 0x409)
    names.setName("NotoSansKR-Regular;KS X 1001 Hangul", 3, 3, 1, 0x409)
    font["OS/2"].usWeightClass = 400

    font.save(args.out)
    print(f"{args.out}: {len(font.getGlyphOrder())} glyphs")


if __name__ == "__main__":
    main()
