#!/usr/bin/env python3
# romconv_galaxian.py
# Converte le ROM Galaxian (set "galmidw", Namco/Midway 1979) negli header .h
# PROGMEM usati dal porting SPINNERINO P4.

import os
import sys
import zipfile
import tempfile
from pathlib import Path


def load_file(rom_dir, name):
    path = os.path.join(rom_dir, name)
    if not os.path.exists(path):
        for alt in [name.lower(), name.upper()]:
            alt_path = os.path.join(rom_dir, alt)
            if os.path.exists(alt_path):
                path = alt_path
                break
    if not os.path.exists(path):
        raise FileNotFoundError(f"File '{name}' non trovato in {os.path.abspath(rom_dir)}")
    with open(path, "rb") as f:
        return bytearray(f.read())


def hex8(v):
    return "0x{:02X}".format(v & 0xFF)


def hex16(v):
    return "0x{:04X}".format(v & 0xFFFF)


def hex32(v):
    return "0x{:08X}".format(v & 0xFFFFFFFF)


def convert_colors(prom):
    rgb565 = []
    for i in range(32):
        b = prom[i]
        r = 0x21 * ((b >> 0) & 1) + 0x47 * ((b >> 1) & 1) + 0x97 * ((b >> 2) & 1)
        g = 0x21 * ((b >> 3) & 1) + 0x47 * ((b >> 4) & 1) + 0x97 * ((b >> 5) & 1)
        bl = 0x4F * ((b >> 6) & 1) + 0xA8 * ((b >> 7) & 1)
        r5 = (r >> 3) & 0x1F
        g6 = (g >> 2) & 0x3F
        b5 = (bl >> 3) & 0x1F
        val = (r5 << 11) | (g6 << 5) | b5
        rgb565.append(((val & 0xFF) << 8) | ((val >> 8) & 0xFF))
    return rgb565


def parse_chr_2(data0, data1):
    char = []
    for y in range(8):
        row = []
        for x in range(8):
            c0 = 1 if data0[7 - x] & (0x80 >> y) else 0
            c1 = 2 if data1[7 - x] & (0x80 >> y) else 0
            row.append(c0 + c1)
        char.append(row)
    return char


def dump_chr(data):
    vals = []
    for y in range(8):
        val = 0
        for x in range(8):
            val = (val >> 2) + (data[y][x] << (16 - 2))
        vals.append(val)
    return vals


def convert_tiles(plane0, plane1):
    num_tiles = len(plane0) // 8
    return [
        dump_chr(parse_chr_2(plane0[t * 8:t * 8 + 8], plane1[t * 8:t * 8 + 8]))
        for t in range(num_tiles)
    ]


def parse_sprite_galaxian(data0, data1):
    sprite = []
    for y in range(16):
        row = []
        for x in range(16):
            ym = (y & 7) | ((x & 8) ^ 8)
            xm = (x & 7) | (y & 8)
            byte_idx = (xm ^ 7) + ((ym & 8) << 1)
            bit_mask = 0x80 >> (ym & 7)
            c0 = 1 if data0[byte_idx] & bit_mask else 0
            c1 = 2 if data1[byte_idx] & bit_mask else 0
            row.append(c0 + c1)
        sprite.append(row)
    return sprite


def dump_sprite(data, flip_x, flip_y):
    vals = []
    y_range = range(16) if not flip_y else reversed(range(16))
    for y in y_range:
        val = 0
        for x in range(16):
            if not flip_x:
                val = (val >> 2) + (data[y][x] << (32 - 2))
            else:
                val = (val << 2) + data[y][x]
        vals.append(val)
    return vals


def convert_sprites(plane0, plane1):
    num_sprites = len(plane0) // 32
    sprites = [
        parse_sprite_galaxian(plane0[32 * s:32 * (s + 1)], plane1[32 * s:32 * (s + 1)])
        for s in range(num_sprites)
    ]
    all_orientations = []
    for flip_x, flip_y in [(False, False), (False, True), (True, False), (True, True)]:
        all_orientations.append([dump_sprite(s, flip_x, flip_y) for s in sprites])
    return all_orientations


def write_rom(filename, name, data):
    with open(filename, "w", newline="\n") as f:
        f.write("// Galaxian program ROM ({} bytes)\n".format(len(data)))
        f.write("const unsigned char {}[] PROGMEM = {{\n".format(name))
        for i in range(0, len(data), 16):
            line = ", ".join(hex8(data[j]) for j in range(i, min(i + 16, len(data))))
            f.write(" " + line + ("," if i + 16 < len(data) else "") + "\n")
        f.write("};\n")
    print("Written: {} ({} bytes)".format(filename, len(data)))


def write_tilemap(filename, tiles):
    with open(filename, "w", newline="\n") as f:
        f.write("// Galaxian tilemap: {} tiles, 8x8, 2bpp\n".format(len(tiles)))
        f.write("const unsigned short galaxian_tilemap[][8] PROGMEM = {\n")
        for t, rows in enumerate(tiles):
            f.write(" { " + ", ".join(hex16(r) for r in rows) + " }" +
                    ("," if t < len(tiles) - 1 else "") + "\n")
        f.write("};\n")
    print("Written: {} ({} tiles)".format(filename, len(tiles)))


def write_spritemap(filename, all_orientations):
    num_sprites = len(all_orientations[0])
    with open(filename, "w", newline="\n") as f:
        f.write("// Galaxian spritemap: {} sprites, 16x16, 2bpp, 4 orientations\n".format(num_sprites))
        f.write("const unsigned long galaxian_spritemap[][%d][16] PROGMEM = {\n" % num_sprites)
        for o, sprites in enumerate(all_orientations):
            f.write(" { // orientation %d\n" % o)
            for s, rows in enumerate(sprites):
                f.write(" { " + ", ".join(hex32(r) for r in rows) + " }" +
                        ("," if s < len(sprites) - 1 else "") + "\n")
            f.write(" }" + ("," if o < 3 else "") + "\n")
        f.write("};\n")
    print("Written: {} ({} sprites x 4 orientations)".format(filename, num_sprites))


def write_colormap(filename, rgb565):
    with open(filename, "w", newline="\n") as f:
        f.write("// Galaxian colormap: 8 palettes x 4 colors, RGB565\n")
        f.write("const unsigned short galaxian_colormap[][4] PROGMEM = {\n")
        for pal in range(8):
            colors = rgb565[pal * 4:pal * 4 + 4]
            f.write(" { " + ", ".join(hex16(c) for c in colors) + " }" +
                    ("," if pal < 7 else "") + " // palette {}\n".format(pal))
        f.write("};\n")
    print("Written: {} (8 palettes)".format(filename))


def main():
    zip_path = (Path.cwd() / ".." / "ROMS" / "galaxian.zip").resolve()
    out_dir = (Path.cwd() / ".." / "machines" / "galaxian").resolve()

    os.makedirs(out_dir, exist_ok=True)

    if not zip_path.exists():
        print(f"[ERRORE] ZIP non trovato: {zip_path}", file=sys.stderr)
        sys.exit(1)

    with tempfile.TemporaryDirectory(prefix="galaxian_") as temp_dir:
        with zipfile.ZipFile(zip_path, "r") as zf:
            zf.extractall(temp_dir)

        rom_dir = temp_dir

        rom_u = load_file(rom_dir, "galmidw.u")
        rom_v = load_file(rom_dir, "galmidw.v")
        rom_w = load_file(rom_dir, "galmidw.w")
        rom_y = load_file(rom_dir, "galmidw.y")
        rom_7l = load_file(rom_dir, "7l")
        gfx_1h = load_file(rom_dir, "1h.bin")
        gfx_1k = load_file(rom_dir, "1k.bin")
        prom = load_file(rom_dir, "6l.bpr")

        program = rom_u + rom_v + rom_w + rom_y + rom_7l
        program += bytearray([0xFF] * (0x4000 - len(program)))

        write_rom(os.path.join(out_dir, "galaxian_rom.h"), "galaxian_rom", program)
        write_tilemap(os.path.join(out_dir, "galaxian_tilemap.h"), convert_tiles(gfx_1h, gfx_1k))
        write_spritemap(os.path.join(out_dir, "galaxian_spritemap.h"), convert_sprites(gfx_1h, gfx_1k))
        write_colormap(os.path.join(out_dir, "galaxian_cmap.h"), convert_colors(prom))

    print("\n--- OK: header generati in {} ---".format(os.path.abspath(out_dir)))


if __name__ == "__main__":
    main()