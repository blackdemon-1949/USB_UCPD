#!/usr/bin/env python3
"""
Generate oled_font.c / oled_font.h for the 0.96" SSD1306 page.

The glyphs are written here as plain ASCII art so a human can check them by
eye; the script turns each 5x7 cell into 5 column bytes (bit 0 = top row,
bit 6 = bottom row) which is what the SSD1306 page-addressing framebuffer
needs.

Run:  python3 tools/gen_oled_font.py
It rewrites Appli/Core/Src/oled_font.c and Appli/Core/Inc/oled_font.h.
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# ---------------------------------------------------------------------------
# Character set.  Index 0 is the fallback glyph (space) for anything the font
# does not carry - the renderer never indexes out of bounds.
# ---------------------------------------------------------------------------
CHARSET = (
    "0123456789"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    " .,:-_+/%=<>()[]!?*#'"
)

G = {}

def glyph(ch, art):
    assert len(art) == 7, (ch, "need 7 rows")
    for row in art:
        assert len(row) == 5, (ch, "need 5 columns", row)
    G[ch] = art

glyph(' ', [".....", ".....", ".....", ".....", ".....", ".....", "....."])

# --- digits ---------------------------------------------------------------
glyph('0', [".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###."])
glyph('1', ["..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###."])
glyph('2', [".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####"])
glyph('3', ["####.", "....#", "....#", ".###.", "....#", "....#", "####."])
glyph('4', ["...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#."])
glyph('5', ["#####", "#....", "####.", "....#", "....#", "#...#", ".###."])
glyph('6', ["..##.", ".#...", "#....", "####.", "#...#", "#...#", ".###."])
glyph('7', ["#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..."])
glyph('8', [".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###."])
glyph('9', [".###.", "#...#", "#...#", ".####", "....#", "...#.", ".##.."])

# --- letters --------------------------------------------------------------
glyph('A', [".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"])
glyph('B', ["####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."])
glyph('C', [".###.", "#...#", "#....", "#....", "#....", "#...#", ".###."])
glyph('D', ["####.", "#...#", "#...#", "#...#", "#...#", "#...#", "####."])
glyph('E', ["#####", "#....", "#....", "####.", "#....", "#....", "#####"])
glyph('F', ["#####", "#....", "#....", "####.", "#....", "#....", "#...."])
glyph('G', [".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".###."])
glyph('H', ["#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"])
glyph('I', [".###.", "..#..", "..#..", "..#..", "..#..", "..#..", ".###."])
glyph('J', ["..###", "...#.", "...#.", "...#.", "...#.", "#..#.", ".##.."])
glyph('K', ["#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#"])
glyph('L', ["#....", "#....", "#....", "#....", "#....", "#....", "#####"])
glyph('M', ["#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#"])
glyph('N', ["#...#", "##..#", "#.#.#", "#.#.#", "#..##", "#...#", "#...#"])
glyph('O', [".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."])
glyph('P', ["####.", "#...#", "#...#", "####.", "#....", "#....", "#...."])
glyph('Q', [".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#"])
glyph('R', ["####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"])
glyph('S', [".####", "#....", "#....", ".###.", "....#", "....#", "####."])
glyph('T', ["#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."])
glyph('U', ["#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."])
glyph('V', ["#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.."])
glyph('W', ["#...#", "#...#", "#...#", "#.#.#", "#.#.#", "##.##", "#...#"])
glyph('X', ["#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#"])
glyph('Y', ["#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.."])
glyph('Z', ["#####", "....#", "...#.", "..#..", ".#...", "#....", "#####"])

# --- symbols --------------------------------------------------------------
glyph('.', [".....", ".....", ".....", ".....", ".....", ".##..", ".##.."])
glyph(',', [".....", ".....", ".....", ".....", ".##..", ".##..", ".#..."])
glyph(':', [".....", ".##..", ".##..", ".....", ".##..", ".##..", "....."])
glyph('-', [".....", ".....", ".....", "#####", ".....", ".....", "....."])
glyph('_', [".....", ".....", ".....", ".....", ".....", ".....", "#####"])
glyph('+', [".....", "..#..", "..#..", "#####", "..#..", "..#..", "....."])
glyph('/', ["....#", "....#", "...#.", "..#..", ".#...", "#....", "#...."])
glyph('%', ["##..#", "##..#", "...#.", "..#..", ".#...", "#..##", "#..##"])
glyph('=', [".....", ".....", "#####", ".....", "#####", ".....", "....."])
glyph('<', ["...#.", "..#..", ".#...", "#....", ".#...", "..#..", "...#."])
glyph('>', [".#...", "..#..", "...#.", "....#", "...#.", "..#..", ".#..."])
glyph('(', ["..#..", ".#...", "#....", "#....", "#....", ".#...", "..#.."])
glyph(')', ["..#..", "...#.", "....#", "....#", "....#", "...#.", "..#.."])
glyph('[', [".###.", ".#...", ".#...", ".#...", ".#...", ".#...", ".###."])
glyph(']', [".###.", "...#.", "...#.", "...#.", "...#.", "...#.", ".###."])
glyph('!', ["..#..", "..#..", "..#..", "..#..", "..#..", ".....", "..#.."])
glyph('?', [".###.", "#...#", "....#", "...#.", "..#..", ".....", "..#.."])
glyph('*', [".....", "#.#.#", ".###.", "#####", ".###.", "#.#.#", "....."])
glyph('#', [".#.#.", ".#.#.", "#####", ".#.#.", "#####", ".#.#.", ".#.#."])
glyph('\'', ["..#..", "..#..", ".....", ".....", ".....", ".....", "....."])


def columns(art):
    """7 rows x 5 cols -> 5 bytes, bit0 = top row."""
    out = []
    for x in range(5):
        v = 0
        for y in range(7):
            if art[y][x] == '#':
                v |= (1 << y)
        out.append(v)
    return out


def main():
    missing = [c for c in CHARSET if c not in G]
    if missing:
        print("missing glyphs:", missing, file=sys.stderr)
        return 1
    extra = [c for c in G if c not in CHARSET]
    if extra:
        print("warning: glyphs not in charset:", extra, file=sys.stderr)

    lines = []
    lines.append("/* Generated by tools/gen_oled_font.py -- do not edit by hand. */")
    lines.append("")
    lines.append('#include "oled_font.h"')
    lines.append("")
    lines.append("/* 5x7 cell, column major, bit 0 = top row, bit 6 = bottom row. */")
    lines.append("const uint8_t OLED_Font5x7[OLED_FONT_GLYPHS][5] =")
    lines.append("{")
    for c in CHARSET:
        cols = columns(G[c])
        art_comment = "  /* '%s' */" % (c if c != '\\' else '\\\\')
        lines.append(art_comment)
        lines.append("  {" + ", ".join("0x%02X" % b for b in cols) + "},")
    lines.append("};")
    lines.append("")
    # Indices are derived from CHARSET, never hard-coded, so the table and the
    # lookup can never drift apart.
    space_idx = CHARSET.index(' ')
    lines.append("/* Index into OLED_Font5x7.  Unknown characters map to the space")
    lines.append("   glyph (%dU), so the renderer can never index out of bounds. */" % space_idx)
    lines.append("uint8_t OLED_FontIndex(char c)")
    lines.append("{")
    lines.append("  if ((c >= '0') && (c <= '9')) { return (uint8_t)(%dU + (unsigned)(c - '0')); }"
                 % CHARSET.index('0'))
    lines.append("  if ((c >= 'A') && (c <= 'Z')) { return (uint8_t)(%dU + (unsigned)(c - 'A')); }"
                 % CHARSET.index('A'))
    lines.append("  switch (c)")
    lines.append("  {")
    for c in CHARSET:
        if c.isalnum():
            continue
        idx = CHARSET.index(c)
        esc = {"'": "\\'", "\\": "\\\\"}.get(c, c)
        lines.append("    case '%s': return %dU;" % (esc, idx))
    lines.append("    default: return %dU; /* space */" % space_idx)
    lines.append("  }")
    lines.append("}")
    lines.append("")

    src = "\n".join(lines)
    dst_c = os.path.join(ROOT, "Appli", "Core", "Src", "oled_font.c")
    with open(dst_c, "w", encoding="utf-8", newline="\n") as f:
        f.write(src)

    hdr = []
    hdr.append("/* Generated by tools/gen_oled_font.py -- do not edit by hand. */")
    hdr.append("")
    hdr.append("#ifndef OLED_FONT_H")
    hdr.append("#define OLED_FONT_H")
    hdr.append("")
    hdr.append("#ifdef __cplusplus")
    hdr.append('extern "C" {')
    hdr.append("#endif")
    hdr.append("")
    hdr.append("#include <stdint.h>")
    hdr.append("")
    hdr.append("#define OLED_FONT_W          5U")
    hdr.append("#define OLED_FONT_HEIGHT      7U")
    hdr.append("#define OLED_FONT_ADVANCE    6U   /* glyph + 1 px gap */")
    hdr.append("#define OLED_FONT_GLYPHS     %dU" % len(CHARSET))
    hdr.append("")
    hdr.append("extern const uint8_t OLED_Font5x7[OLED_FONT_GLYPHS][5];")
    hdr.append("uint8_t OLED_FontIndex(char c);")
    hdr.append("")
    hdr.append("#ifdef __cplusplus")
    hdr.append("}")
    hdr.append("#endif")
    hdr.append("")
    hdr.append("#endif /* OLED_FONT_H */")
    dst_h = os.path.join(ROOT, "Appli", "Core", "Inc", "oled_font.h")
    with open(dst_h, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(hdr))

    print("wrote %s (%d glyphs)" % (dst_c, len(CHARSET)))
    print("wrote %s" % dst_h)
    return 0


if __name__ == "__main__":
    sys.exit(main())
