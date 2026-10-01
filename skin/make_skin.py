#!/usr/bin/env python3
"""Builds MiniAmp's default skin, "Graphite", as a classic .wsz file.

Everything is drawn from scratch by this script (no bitmaps from other skins),
so the skin is original artwork. The sheet layouts follow the classic skin
format so any classic-skin player can load it.

usage: skin/make_skin.py OUT.wsz      (needs Pillow)
"""
import io
import sys
import zipfile

from PIL import Image, ImageDraw


def hx(s):
    return (int(s[1:3], 16), int(s[3:5], 16), int(s[5:7], 16))


# ---------------------------------------------------------------- palette
FACE, HI, LO, DEEP = hx("#2c3038"), hx("#4c5360"), hx("#16191e"), hx("#0d0f12")
FACE2 = hx("#23262d")
LCD, GHOST = hx("#0a0c0a"), hx("#2a1d0c")
AMBER, AMBER2, AMBERDK = hx("#ffb347"), hx("#ffd59a"), hx("#7a4a12")
GREEN, TEAL, GREY, ICON = hx("#7dff9a"), hx("#5fd3c4"), hx("#9aa0aa"), hx("#d8dbe2")
DIMLBL = hx("#4a4f58")

# ---------------------------------------------------------------- 3x5 font
G = {
    'A': [".#.", "#.#", "###", "#.#", "#.#"], 'B': ["##.", "#.#", "##.", "#.#", "##."], 'C': [".##", "#..", "#..", "#..", ".##"],
    'D': ["##.", "#.#", "#.#", "#.#", "##."], 'E': ["###", "#..", "##.", "#..", "###"], 'F': ["###", "#..", "##.", "#..", "#.."],
    'G': [".##", "#..", "#.#", "#.#", ".##"], 'H': ["#.#", "#.#", "###", "#.#", "#.#"], 'I': ["###", ".#.", ".#.", ".#.", "###"],
    'J': ["..#", "..#", "..#", "#.#", ".#."], 'K': ["#.#", "#.#", "##.", "#.#", "#.#"], 'L': ["#..", "#..", "#..", "#..", "###"],
    'M': ["#.#", "###", "###", "#.#", "#.#"], 'N': ["##.", "#.#", "#.#", "#.#", "#.#"], 'O': [".#.", "#.#", "#.#", "#.#", ".#."],
    'P': ["##.", "#.#", "##.", "#..", "#.."], 'Q': [".#.", "#.#", "#.#", "##.", ".##"], 'R': ["##.", "#.#", "##.", "#.#", "#.#"],
    'S': [".##", "#..", ".#.", "..#", "##."], 'T': ["###", ".#.", ".#.", ".#.", ".#."], 'U': ["#.#", "#.#", "#.#", "#.#", "###"],
    'V': ["#.#", "#.#", "#.#", "#.#", ".#."], 'W': ["#.#", "#.#", "###", "###", "#.#"], 'X': ["#.#", "#.#", ".#.", "#.#", "#.#"],
    'Y': ["#.#", "#.#", ".#.", ".#.", ".#."], 'Z': ["###", "..#", ".#.", "#..", "###"],
    '0': ["###", "#.#", "#.#", "#.#", "###"], '1': [".#.", "##.", ".#.", ".#.", "###"], '2': ["##.", "..#", ".#.", "#..", "###"],
    '3': ["##.", "..#", ".#.", "..#", "##."], '4': ["#.#", "#.#", "###", "..#", "..#"], '5': ["###", "#..", "##.", "..#", "##."],
    '6': [".##", "#..", "###", "#.#", "###"], '7': ["###", "..#", ".#.", ".#.", ".#."], '8': ["###", "#.#", "###", "#.#", "###"],
    '9': ["###", "#.#", "###", "..#", "##."], ' ': ["..."] * 5, ':': ["...", ".#.", "...", ".#.", "..."],
    '-': ["...", "...", "###", "...", "..."], '.': ["...", "...", "...", "...", ".#."], '(': [".#.", "#..", "#..", "#..", ".#."],
    ')': [".#.", "..#", "..#", "..#", ".#."], '/': ["..#", "..#", ".#.", "#..", "#.."], "'": [".#.", ".#.", "...", "...", "..."],
    '"': ["#.#", "#.#", "...", "...", "..."], '@': [".#.", "#.#", "###", "#..", ".##"], '!': [".#.", ".#.", ".#.", "...", ".#."],
    '_': ["...", "...", "...", "...", "###"], '+': ["...", ".#.", "###", ".#.", "..."], '\\': ["#..", "#..", ".#.", "..#", "..#"],
    '[': ["##.", "#..", "#..", "#..", "##."], ']': [".##", "..#", "..#", "..#", ".##"], '^': [".#.", "#.#", "...", "...", "..."],
    '&': [".#.", "#.#", ".#.", "#.#", ".##"], '%': ["#.#", "..#", ".#.", "#..", "#.#"], ',': ["...", "...", "...", ".#.", "#.."],
    '=': ["...", "###", "...", "###", "..."], '$': [".##", "##.", ".#.", ".##", "##."], '#': ["#.#", "###", "#.#", "###", "#.#"],
    '?': ["##.", "..#", ".#.", "...", ".#."], '*': ["...", "#.#", ".#.", "#.#", "..."], '…': ["...", "...", "...", "...", "#.#"],
    'Å': [".#.", "...", "###", "#.#", "#.#"], 'Ö': ["#.#", "...", "###", "#.#", "###"], 'Ä': ["#.#", "...", "###", "#.#", "#.#"],
}


def glyph(d, x, y, ch, c):
    g = G.get(ch.upper(), G[' '])
    for j, row in enumerate(g):
        for i, v in enumerate(row):
            if v == '#':
                d.point((x + i, y + j), fill=c)


def btext(d, x, y, s, c):
    for ch in s:
        glyph(d, x, y, ch, c)
        x += 4
    return x


def bevel(d, x, y, w, h, up=True, fill=FACE):
    d.rectangle([x, y, x + w - 1, y + h - 1], fill=fill)
    a, b = (HI, LO) if up else (LO, HI)
    d.line([x, y, x + w - 1, y], fill=a)
    d.line([x, y, x, y + h - 1], fill=a)
    d.line([x, y + h - 1, x + w - 1, y + h - 1], fill=b)
    d.line([x + w - 1, y, x + w - 1, y + h - 1], fill=b)


def inset(d, x, y, w, h, fill=LCD):
    d.rectangle([x, y, x + w - 1, y + h - 1], fill=fill)
    d.line([x - 1, y - 1, x + w, y - 1], fill=DEEP)
    d.line([x - 1, y - 1, x - 1, y + h], fill=DEEP)
    d.line([x - 1, y + h, x + w, y + h], fill=HI)
    d.line([x + w, y - 1, x + w, y + h], fill=HI)


def lerp(c1, c2, t):
    return tuple(int(c1[k] + (c2[k] - c1[k]) * t) for k in range(3))


# ---------------------------------------------------------------- sheets
def main_bmp():
    im = Image.new("RGB", (275, 116), FACE)
    d = ImageDraw.Draw(im)
    bevel(d, 0, 0, 275, 116)
    inset(d, 20, 22, 87, 42)                 # time + visualizer display
    for cy in (30, 35):                      # time colon
        d.rectangle([71, cy, 72, cy + 1], fill=AMBER)
    inset(d, 110, 23, 156, 11)               # song title
    inset(d, 110, 41, 17, 9)                 # kbps
    btext(d, 129, 43, "KBPS", GREY)
    inset(d, 155, 41, 12, 9)                 # khz
    btext(d, 169, 43, "KHZ", GREY)
    # cassette badge (the "about" spot)
    d.rounded_rectangle([244, 91, 268, 104], radius=2, outline=hx("#8a909c"), fill=FACE2)
    d.ellipse([248, 95, 253, 100], outline=AMBER)
    d.ellipse([259, 95, 264, 100], outline=AMBER)
    d.line([253, 102, 259, 102], fill=hx("#8a909c"))
    return im


def titlebar_bmp():
    im = Image.new("RGB", (344, 87), FACE)
    d = ImageDraw.Draw(im)
    for row, active in ((0, True), (15, False)):
        x0 = 27
        d.rectangle([x0, row, x0 + 274, row + 13], fill=FACE)
        d.line([x0, row, x0 + 274, row], fill=HI)
        d.rectangle([x0 + 2, row + 2, x0 + 272, row + 11], fill=FACE2)
        gc = HI if active else hx("#353a43")
        for gy in (4, 6, 8):
            d.line([x0 + 20, row + gy, x0 + 106, row + gy], fill=gc)
            d.line([x0 + 168, row + gy, x0 + 240, row + gy], fill=gc)
        btext(d, x0 + 114, row + 4, "MINIAMP", AMBER2 if active else GREY)
        for bx in (244, 254, 264):
            bevel(d, x0 + bx, row + 3, 9, 9)
        d.line([x0 + 246, row + 9, x0 + 250, row + 9], fill=ICON)            # minimize
        d.rectangle([x0 + 256, row + 5, x0 + 260, row + 9], outline=ICON)    # shade
        d.line([x0 + 266, row + 5, x0 + 270, row + 9], fill=ICON)            # close
        d.line([x0 + 270, row + 5, x0 + 266, row + 9], fill=ICON)
        # options button at the left
        bevel(d, x0 + 6, row + 3, 9, 9)
        d.line([x0 + 8, row + 7, x0 + 12, row + 7], fill=ICON)
    # stand-alone options button sprites
    for y, up in ((0, True), (9, False)):
        bevel(d, 0, y, 9, 9, up=up)
        d.line([2, y + 4, 6, y + 4], fill=ICON)
    # clutter bar (O A I D V), unpressed
    x, y = 304, 0
    d.rectangle([x, y, x + 7, y + 42], fill=FACE2)
    d.line([x, y, x, y + 42], fill=HI)
    for i, ch in enumerate("OAIDV"):
        glyph(d, x + 3, y + 4 + i * 8, ch, hx("#6a707c"))
    return im


def cbuttons_bmp():
    im = Image.new("RGB", (136, 36), FACE)
    d = ImageDraw.Draw(im)
    defs = [(0, 23), (23, 23), (46, 23), (69, 23), (92, 22)]
    for row, up in ((0, True), (18, False)):
        for i, (bx, bw) in enumerate(defs):
            bevel(d, bx, row, bw, 18, up=up, fill=FACE if up else FACE2)
            cx, cy = bx + bw // 2 + (0 if up else 1), row + 9 + (0 if up else 1)
            ic = ICON
            if i == 0:
                d.rectangle([cx - 6, cy - 4, cx - 5, cy + 4], fill=ic)
                d.polygon([(cx + 4, cy - 4), (cx + 4, cy + 4), (cx - 3, cy)], fill=ic)
            elif i == 1:
                d.polygon([(cx - 3, cy - 5), (cx - 3, cy + 5), (cx + 5, cy)], fill=GREEN)
            elif i == 2:
                d.rectangle([cx - 4, cy - 4, cx - 2, cy + 4], fill=ic)
                d.rectangle([cx + 1, cy - 4, cx + 3, cy + 4], fill=ic)
            elif i == 3:
                d.rectangle([cx - 4, cy - 4, cx + 4, cy + 4], fill=ic)
            else:
                d.polygon([(cx - 5, cy - 4), (cx - 5, cy + 4), (cx + 2, cy)], fill=ic)
                d.rectangle([cx + 4, cy - 4, cx + 5, cy + 4], fill=ic)
    for row, up in ((0, True), (16, False)):
        bevel(d, 114, row, 22, 16, up=up, fill=FACE if up else FACE2)
        o = 0 if up else 1
        d.polygon([(119 + o, 9 + row + o), (130 + o, 9 + row + o), (124 + o, 3 + row + o)], fill=ICON)
        d.rectangle([119 + o, 11 + row + o, 130 + o, 12 + row + o], fill=ICON)
    return im


def numbers_bmp(ex):
    SEG = {'a': (2, 0, 5, 2), 'b': (7, 2, 2, 4), 'c': (7, 7, 2, 4), 'd': (2, 11, 5, 2),
           'e': (0, 7, 2, 4), 'f': (0, 2, 2, 4), 'g': (2, 6, 5, 1)}
    DIG = {'0': "abcdef", '1': "bc", '2': "abged", '3': "abgcd", '4': "fgbc", '5': "afgcd",
           '6': "afgedc", '7': "abc", '8': "abcdefg", '9': "abcdfg", ' ': "", '-': "g"}
    chars = "0123456789 -" if ex else "0123456789 "
    im = Image.new("RGB", (9 * len(chars), 13), LCD)
    d = ImageDraw.Draw(im)
    for n, ch in enumerate(chars):
        for k, (sx, sy, w, h) in SEG.items():
            d.rectangle([n * 9 + sx, sy, n * 9 + sx + w - 1, sy + h - 1], fill=AMBER if k in DIG[ch] else GHOST)
    if not ex:
        # numbers.bmp carries the minus sign inside digit slots (x 20, y 6)
        d.rectangle([20, 6, 24, 6], fill=AMBER)
        d.rectangle([9, 6, 13, 6], fill=GHOST)
    return im


def text_bmp():
    im = Image.new("RGB", (155, 18), LCD)
    d = ImageDraw.Draw(im)
    rows = ["abcdefghijklmnopqrstuvwxyz\"@   ", "0123456789….:()-'!_+\\/[]^&%,=$#", "ÅÖÄ?*"]
    for r, s in enumerate(rows):
        for c, ch in enumerate(s):
            glyph(d, c * 5 + 1, r * 6, ch, AMBER)
    return im


def playpaus_bmp():
    im = Image.new("RGB", (42, 9), LCD)
    d = ImageDraw.Draw(im)
    d.polygon([(2, 1), (2, 7), (6, 4)], fill=GREEN)
    d.rectangle([10, 1, 11, 7], fill=AMBER)
    d.rectangle([14, 1, 15, 7], fill=AMBER)
    d.rectangle([20, 2, 24, 6], fill=AMBER)
    d.rectangle([36, 0, 38, 8], fill=LCD)
    d.rectangle([37, 2, 37, 6], fill=hx("#2c5a36"))
    d.rectangle([40, 2, 40, 6], fill=GREEN)
    return im


def monoster_bmp():
    im = Image.new("RGB", (56, 24), FACE)
    d = ImageDraw.Draw(im)
    for y, on in ((0, True), (12, False)):
        btext(d, 2, y + 4, "STEREO", GREEN if on else DIMLBL)
        btext(d, 31, y + 4, "MONO", GREEN if on else DIMLBL)
    return im


def posbar_bmp():
    im = Image.new("RGB", (307, 10), FACE)
    d = ImageDraw.Draw(im)
    inset(d, 1, 2, 246, 6, fill=DEEP)
    for x in range(1, 247, 10):
        d.point((x, 7), fill=hx("#20242a"))
    for x0, sel in ((248, False), (278, True)):
        bevel(d, x0, 0, 29, 10, up=not sel)
        for yy in (3, 6):
            d.line([x0 + 10, yy, x0 + 18, yy], fill=AMBER if not sel else AMBER2)
    return im


def slider_sheet(width, x0, frame_w, centered):
    im = Image.new("RGB", (width, 433), FACE)
    d = ImageDraw.Draw(im)
    for n in range(28):
        y = n * 15
        inset(d, x0 + 1, y + 4, frame_w - 2, 5, fill=DEEP)
        t = n / 27
        if centered:
            mid = x0 + frame_w // 2
            half = int((frame_w // 2 - 2) * t)
            for xx in range(mid - half, mid + half + 1):
                d.line([xx, y + 4, xx, y + 8], fill=lerp(hx("#2a5a54"), TEAL, abs(xx - mid) / max(1, frame_w // 2)))
        else:
            fw = int((frame_w - 2) * t)
            for i in range(fw):
                d.line([x0 + 1 + i, y + 4, x0 + 1 + i, y + 8], fill=lerp(AMBERDK, AMBER, i / max(1, frame_w - 3)))
    for tx, sel in ((0, True), (15, False)):
        bevel(d, tx, 422, 14, 11, up=not sel)
        d.line([tx + 4, 426, tx + 9, 426], fill=AMBER2 if sel else HI)
        d.line([tx + 4, 428, tx + 9, 428], fill=AMBER2 if sel else HI)
    return im


def shufrep_bmp():
    im = Image.new("RGB", (92, 85), FACE)
    d = ImageDraw.Draw(im)
    for i, (on, pressed) in enumerate(((False, False), (False, True), (True, False), (True, True))):
        y = i * 15
        bevel(d, 0, y, 28, 15, up=not pressed, fill=FACE if not pressed else FACE2)
        btext(d, 6, y + 5, "REPT", TEAL if on else GREY)
        bevel(d, 28, y, 47, 15, up=not pressed, fill=FACE if not pressed else FACE2)
        btext(d, 36, y + 5, "SHUFFLE", TEAL if on else GREY)
    for col, lab in ((0, "EQ"), (23, "PL")):
        for y, on in ((61, False), (73, True)):
            for dx, pressed in ((0, False), (46, True)):
                bevel(d, col + dx, y, 23, 12, up=not pressed)
                btext(d, col + dx + 8, y + 4, lab, TEAL if on else GREY)
    return im


def viscolor_txt():
    cols = [LCD, hx("#1c1810")]
    top, mid, bot = hx("#ff5a3c"), AMBER, AMBERDK
    for i in range(16):  # 2 (top) .. 17 (bottom)
        t = i / 15
        cols.append(lerp(top, mid, t * 2) if t < 0.5 else lerp(mid, bot, (t - 0.5) * 2))
    cols += [AMBER2, AMBER, hx("#e09a3a"), hx("#b07428"), AMBERDK]  # oscilloscope
    cols.append(AMBER2)  # peaks
    return "".join("%d,%d,%d,\r\n" % c for c in cols)


PLEDIT_TXT = "[Text]\r\nNormal=#C9CED8\r\nCurrent=#FFB347\r\nNormalBG=#0B0D10\r\nSelectedBG=#1F2A48\r\nFont=Rubik\r\n"

README = """Graphite - the default MiniAmp skin.
Original artwork generated by skin/make_skin.py in the MiniAmp source.
Dedicated to the public domain (CC0 1.0).
"""


def bmp_bytes(im):
    b = io.BytesIO()
    im.convert("RGB").save(b, "BMP")
    return b.getvalue()


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "Graphite.wsz"
    sheets = {
        "main.bmp": main_bmp(), "titlebar.bmp": titlebar_bmp(), "cbuttons.bmp": cbuttons_bmp(),
        "numbers.bmp": numbers_bmp(False), "nums_ex.bmp": numbers_bmp(True), "text.bmp": text_bmp(),
        "playpaus.bmp": playpaus_bmp(), "monoster.bmp": monoster_bmp(), "posbar.bmp": posbar_bmp(),
        "volume.bmp": slider_sheet(68, 0, 68, False), "balance.bmp": slider_sheet(47, 9, 38, True),
        "shufrep.bmp": shufrep_bmp(),
    }
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for name, im in sheets.items():
            z.writestr(name, bmp_bytes(im))
        z.writestr("viscolor.txt", viscolor_txt())
        z.writestr("pledit.txt", PLEDIT_TXT)
        z.writestr("readme.txt", README)
    print("wrote", out)


if __name__ == "__main__":
    main()
