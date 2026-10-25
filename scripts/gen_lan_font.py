#!/usr/bin/env python3
"""Gera runtime/src/win32/lan_font.h: fonte bitmap 8x16 do menu LAN.

Os glifos (ASCII 0x20-0x7E e Latin-1 0xA0-0xFF, para os acentos do português)
são rasterizados da DejaVu Sans Mono (licença Bitstream Vera / domínio público
para as alterações da DejaVu: pode ser embutida). Cada glifo são 16 bytes, um
por linha, bit 7 = coluna da esquerda.

    python3 scripts/gen_lan_font.py [fonte.ttf] [saida.h]
"""
import sys

from PIL import Image, ImageDraw, ImageFont

W, H = 8, 16
CODES = list(range(0x20, 0x7F)) + list(range(0xA0, 0x100))


def main():
    ttf = sys.argv[1] if len(sys.argv) > 1 else "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"
    out = sys.argv[2] if len(sys.argv) > 2 else "runtime/src/win32/lan_font.h"
    font = ImageFont.truetype(ttf, 13)
    asc, _ = font.getmetrics()
    rows = []
    for cp in CODES:
        img = Image.new("L", (W, H), 0)
        d = ImageDraw.Draw(img)
        x0, _, x1, _ = d.textbbox((0, 0), chr(cp), font=font)
        d.text(((W - (x1 - x0)) // 2 - x0, 13 - asc), chr(cp), fill=255, font=font)
        glyph = []
        for y in range(H):
            b = 0
            for x in range(W):
                if img.getpixel((x, y)) >= 110:
                    b |= 0x80 >> x
            glyph.append(b)
        rows.append((cp, glyph))
    with open(out, "w", encoding="utf-8") as f:
        f.write("/* Gerado por scripts/gen_lan_font.py (DejaVu Sans Mono 13 px, 8x16). Não edite à mão. */\n")
        f.write("#ifndef RE5_LAN_FONT_H\n#define RE5_LAN_FONT_H\n\n#include <stdint.h>\n\n")
        f.write(f"#define LAN_FONT_W {W}\n#define LAN_FONT_H {H}\n\n")
        f.write("/* Índice do glifo: cp - 0x20 para 0x20..0x7E; cp - 0xA0 + 95 para 0xA0..0xFF. */\n")
        f.write(f"static const uint8_t lan_font[{len(rows)}][{H}] = {{\n")
        for cp, g in rows:
            f.write("    {" + ", ".join(f"0x{b:02X}" for b in g) + f"}}, /* U+{cp:04X} */\n")
        f.write("};\n\n#endif\n")


if __name__ == "__main__":
    main()
