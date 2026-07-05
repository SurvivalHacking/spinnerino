#!/usr/bin/env python3
"""
romconv_gyruss.py  -  Converter ROM Gyruss (set 'gyruss'/'gyrussk') -> header .h per SPINNERINO.

Uso:
    python romconv_gyruss.py [rom_src] [output_dir]

  rom_src    = cartella con i ROM gyrussk.* SCOMPATTATI, OPPURE direttamente gyruss.zip.
               default: <questa cartella>/../ROMS/gyruss.zip
  output_dir = cartella header di destinazione.
               default: <questa cartella>/../machines/gyruss

Genera (nomi array IDENTICI a quelli usati dal sorgente SPINNERINO):
  gyruss_rom_main.h     main Z80 (24KB)        <- gyrussk.1 + .2 + .3
  gyruss_rom_sub.h      M6809 sub (8KB)        <- gyrussk.9 (raw + decrypt Konami-1)
  gyruss_rom_audio.h    audio Z80 (16KB)       <- gyrussk.1a + .2a
  gyruss_rom_i8039.h    i8039 drums (4KB)      <- gyrussk.3a      *** NUOVO (campioni percussioni) ***
  gyruss_tilemap.h      512 tile 8x8 2bpp      <- gyrussk.4
  gyruss_spritemap.h    512 sprite 8x16 4bpp   <- gyrussk.6 + .5 + .8 + .7 (4 varianti flip)
  gyruss_palette.h      palette 32 + colormap  <- gyrussk.pr1 + .pr2 + .pr3

Logica di decodifica (Konami-1, tile/sprite, palette PROM) IDENTICA al converter
galagino originale che ha generato gli header attuali. La sola aggiunta e' l'i8039.
"""

import os
import re
import sys
import zipfile

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
DEF_ROM = os.path.normpath(os.path.join(SCRIPT_DIR, "..", "ROMS", "gyruss.zip"))
DEF_OUT = os.path.normpath(os.path.join(SCRIPT_DIR, "..", "machines", "gyruss"))
PATCH_DIR = os.path.join(SCRIPT_DIR, "gyruss_patches")  # til_patch.table / spr_patch.table


# ============================================================
# Patch grafiche (delta per-posizione su tile/sprite, opzionali)
# ============================================================

def load_patch(name):
    path = os.path.join(PATCH_DIR, name)
    if not os.path.isfile(path):
        print(f"  (patch {name} assente: salto)")
        return None
    return [int(l.strip(), 16) for l in open(path) if l.strip()]


def apply_patch_flat(values, patch, mask):
    """Somma i delta della patch ai valori, in ordine di scrittura."""
    if not patch:
        return values
    out = list(values)
    for i in range(min(len(out), len(patch))):
        if patch[i] != 0:
            out[i] = (out[i] + patch[i]) & mask
    return out


# ============================================================
# Caricamento ROM (da cartella o da .zip)
# ============================================================

class RomSource:
    """Sorgente ROM: cartella scompattata oppure file .zip."""
    def __init__(self, path):
        self.path = path
        self.zip = None
        if os.path.isfile(path) and path.lower().endswith(".zip"):
            self.zip = zipfile.ZipFile(path, "r")
            self.names = self.zip.namelist()
        elif os.path.isdir(path):
            self.names = os.listdir(path)
        else:
            raise FileNotFoundError(f"Sorgente ROM non valida: {path}")

    def _resolve(self, name):
        # match esatto, poi case-insensitive / basename
        for n in self.names:
            if n == name or os.path.basename(n) == name:
                return n
        low = name.lower()
        for n in self.names:
            if os.path.basename(n).lower() == low:
                return n
        return None

    def load(self, name):
        n = self._resolve(name)
        if n is None:
            print(f"ERRORE: '{name}' non trovato in {self.path}")
            return None
        if self.zip:
            return bytearray(self.zip.read(n))
        with open(os.path.join(self.path, n), "rb") as f:
            return bytearray(f.read())


# ============================================================
# Konami-1 opcode decryption (M6809 sub-CPU)
# ============================================================

def konami1_decrypt_opcode(val, addr):
    xor_table = {0x0: 0x22, 0x2: 0x82, 0x8: 0x28, 0xA: 0x88}
    return val ^ xor_table[addr & 0x0A]


# ============================================================
# Assemblaggio ROM CPU
# ============================================================

def assemble_main_rom(src):
    rom = bytearray(0x6000)
    rom[0x0000:0x2000] = src.load("gyrussk.1")
    rom[0x2000:0x4000] = src.load("gyrussk.2")
    rom[0x4000:0x6000] = src.load("gyrussk.3")
    return rom


def assemble_sub_rom(src):
    raw = src.load("gyrussk.9")  # 8KB @ 0xE000-0xFFFF
    decrypted = bytearray(len(raw))
    for i in range(len(raw)):
        decrypted[i] = konami1_decrypt_opcode(raw[i], 0xE000 + i)
    return raw, decrypted


def assemble_audio_rom(src):
    rom = bytearray(0x4000)
    rom[0x0000:0x2000] = src.load("gyrussk.1a")
    rom[0x2000:0x4000] = src.load("gyrussk.2a")
    return rom


# ============================================================
# Tile decode (8x8, 2bpp, 512 tile da gyrussk.4)
# ============================================================

def decode_tile_pixel(data, tile_base, row, pixel):
    if pixel < 4:
        byte_offset = row
        bit = pixel
    else:
        byte_offset = row + 8
        bit = pixel - 4
    b = data[tile_base + byte_offset]
    plane0 = (b >> bit) & 1
    plane1 = (b >> (bit + 4)) & 1
    return plane0 | (plane1 << 1)


def decode_tiles(gfx_data):
    num_tiles = len(gfx_data) // 16
    tiles = []
    for t in range(num_tiles):
        base = t * 16
        tile_rows = []
        for r in range(8):
            row_val = 0
            for p in range(8):
                row_val |= decode_tile_pixel(gfx_data, base, r, p) << (p * 2)
            tile_rows.append(row_val)
        tiles.append(tile_rows)
    return tiles


# ============================================================
# Sprite decode (8x16, 4bpp, 512 sprite, 4 varianti flip)
# ============================================================

def decode_sprite_pixel(data, sprite_base, row, pixel, data_len):
    if row < 8:
        y_bit_offset = row * 8
    else:
        y_bit_offset = 256 + (row - 8) * 8
    if pixel < 4:
        x_bit_offset = pixel
    else:
        x_bit_offset = 64 + (pixel - 4)
    bit_offset = y_bit_offset + x_bit_offset
    byte_idx = bit_offset // 8
    bit_in_byte = bit_offset % 8
    addr0 = sprite_base + byte_idx
    addr1 = addr0 + 0x4000
    if addr0 >= data_len or addr1 >= data_len:
        return 0
    b0 = data[addr0]
    b1 = data[addr1]
    p0 = (b0 >> bit_in_byte) & 1
    p1 = (b0 >> (bit_in_byte + 4)) & 1
    p2 = (b1 >> bit_in_byte) & 1
    p3 = (b1 >> (bit_in_byte + 4)) & 1
    return p0 | (p1 << 1) | (p2 << 2) | (p3 << 3)


def decode_sprites(gfx_data):
    data_len = len(gfx_data)
    num_sprites = 512
    sprites = [[[0] * 16 for _ in range(num_sprites)] for _ in range(4)]
    for s in range(num_sprites):
        base = s * 64
        normal = []
        for r in range(16):
            row_val = 0
            for p in range(8):
                row_val |= decode_sprite_pixel(gfx_data, base, r, p, data_len) << (p * 4)
            normal.append(row_val)
        sprites[0][s] = normal[:]              # normale
        sprites[1][s] = normal[::-1]           # Y flip
        for r in range(16):                    # X flip
            flipped = 0
            for p in range(8):
                px = (normal[r] >> (p * 4)) & 0xF
                flipped |= px << ((7 - p) * 4)
            sprites[2][s][r] = flipped
        for r in range(16):                    # XY flip
            flipped = 0
            src_row = normal[15 - r]
            for p in range(8):
                px = (src_row >> (p * 4)) & 0xF
                flipped |= px << ((7 - p) * 4)
            sprites[3][s][r] = flipped
    return sprites


# ============================================================
# Palette + colormap da PROM (pr1 sprite, pr2 char, pr3 palette)
# ============================================================

def rgb_to_565(r, g, b):
    r5 = (r >> 3) & 0x1F
    g6 = (g >> 2) & 0x3F
    b5 = (b >> 3) & 0x1F
    val = (r5 << 11) | (g6 << 5) | b5
    return ((val & 0xFF) << 8) | ((val >> 8) & 0xFF)   # byte-swap per TFT


def generate_palette_and_colormaps(pr3, pr1, pr2):
    palette_rgb = []
    for i in range(32):
        val = pr3[i]
        r = 0x21 * ((val >> 0) & 1) + 0x47 * ((val >> 1) & 1) + 0x97 * ((val >> 2) & 1)
        g = 0x21 * ((val >> 3) & 1) + 0x47 * ((val >> 4) & 1) + 0x97 * ((val >> 5) & 1)
        b = 0x47 * ((val >> 6) & 1) + 0x97 * ((val >> 7) & 1)
        palette_rgb.append((r, g, b))
    palette_565 = [rgb_to_565(r, g, b) for r, g, b in palette_rgb]

    sprite_colormap = []
    for group in range(16):
        colors = []
        for px in range(16):
            pal_idx = pr1[group * 16 + px] & 0x0F
            r, g, b = palette_rgb[pal_idx]
            colors.append(0 if px == 0 else rgb_to_565(r, g, b))
        sprite_colormap.append(colors)

    char_colormap = []
    for group in range(16):
        colors = []
        for px in range(4):
            pal_idx = (pr2[group * 4 + px] & 0x0F) + 0x10
            if pal_idx >= 32:
                pal_idx = 0
            r, g, b = palette_rgb[pal_idx]
            colors.append(0 if px == 0 else rgb_to_565(r, g, b))
        char_colormap.append(colors)

    return palette_565, sprite_colormap, char_colormap


# ============================================================
# Scrittura header
# ============================================================

def _write_bytes(f, data, per_line=16):
    for i in range(len(data)):
        if i % per_line == 0:
            f.write("  ")
        f.write("0x{:02X}".format(data[i]))
        if i < len(data) - 1:
            f.write(",")
        if i % per_line == per_line - 1 or i == len(data) - 1:
            f.write("\n")


def write_main_rom_h(rom, path):
    with open(path, "w", newline="\n") as f:
        f.write("// Gyruss main Z80 CPU ROM (24KB: 0x0000-0x5FFF)\n")
        f.write("// Generated from gyrussk.1 + gyrussk.2 + gyrussk.3 (romconv_gyruss.py)\n\n")
        f.write("const unsigned char gyruss_rom_main[] PROGMEM = {\n")
        _write_bytes(f, rom)
        f.write("};\n")


def write_sub_rom_h(raw, decrypted, path):
    with open(path, "w", newline="\n") as f:
        f.write("// Gyruss M6809 sub-CPU ROM (8KB: mapped at 0xE000-0xFFFF)\n")
        f.write("// Generated from gyrussk.9 (romconv_gyruss.py)\n")
        f.write("// Raw data for operand reads, decrypted for opcode fetches (Konami-1)\n\n")
        f.write("const unsigned char gyruss_rom_sub_raw[] PROGMEM = {\n")
        _write_bytes(f, raw)
        f.write("};\n\n")
        f.write("// Decrypted opcodes (Konami-1 XOR scheme)\n")
        f.write("const unsigned char gyruss_rom_sub_decrypt[] PROGMEM = {\n")
        _write_bytes(f, decrypted)
        f.write("};\n")


def write_audio_rom_h(rom, path):
    with open(path, "w", newline="\n") as f:
        f.write("// Gyruss audio Z80 CPU ROM (16KB: 0x0000-0x3FFF)\n")
        f.write("// Generated from gyrussk.1a + gyrussk.2a (romconv_gyruss.py)\n\n")
        f.write("const unsigned char gyruss_rom_audio[] PROGMEM = {\n")
        _write_bytes(f, rom)
        f.write("};\n")


def write_i8039_rom_h(rom, path):
    """NUOVO: ROM del MCU i8039 (campioni/drum). gyrussk.3a (4KB)."""
    assert len(rom) == 4096, f"gyrussk.3a deve essere 4096 byte, trovati {len(rom)}"
    with open(path, "w", newline="\n") as f:
        f.write("// Gyruss i8039 sample/drum MCU ROM (4KB: gyrussk.3a)\n")
        f.write("// 8039 @ 8MHz: riceve comando via soundlatch2 (Z80 port 0x18) + IRQ (port 0x14),\n")
        f.write("// riproduce il campione scrivendo P1 -> DAC. Generato da romconv_gyruss.py.\n")
        f.write("const unsigned char gyruss_rom_i8039[4096] = {\n")
        _write_bytes(f, rom)
        f.write("};\n")


def write_tilemap_h(tiles, path, patch=None):
    flat = apply_patch_flat([v for tile in tiles for v in tile], patch, 0xFFFF)
    rows = [flat[i * 8:(i + 1) * 8] for i in range(len(tiles))]
    with open(path, "w", newline="\n") as f:
        f.write("// Gyruss character tiles (512 tiles, 8x8, 2bpp)\n")
        f.write("// Generated from gyrussk.4 + til_patch.table (romconv_gyruss.py)\n\n")
        f.write("const unsigned short gyruss_tilemap[][8] PROGMEM = {\n")
        for t_idx, tile in enumerate(rows):
            f.write("  { " + ",".join("0x{:04x}".format(v) for v in tile) + " }")
            f.write(",\n" if t_idx < len(rows) - 1 else "\n")
        f.write("};\n")


def write_spritemap_h(sprites, path, patch=None):
    flat = apply_patch_flat([r for v in range(4) for s in range(512) for r in sprites[v][s]],
                            patch, 0xFFFFFFFF)
    g = lambda v, s, r: flat[((v * 512 + s) * 16) + r]
    with open(path, "w", newline="\n") as f:
        f.write("// Gyruss sprites (512 sprites, 8x16, 4bpp, 4 flip variants)\n")
        f.write("// Generated from gyrussk.6 + gyrussk.5 + gyrussk.8 + gyrussk.7 + spr_patch.table\n")
        f.write("// Variant 0=normal, 1=Y-flip, 2=X-flip, 3=XY-flip\n")
        f.write("// NOTA: il decode base sprite della tua build differisce ~15% da questo\n")
        f.write("// riferimento galagino; tieni lo spritemap esistente se gia' corretto.\n\n")
        f.write("const unsigned long gyruss_sprites[][512][16] PROGMEM = {\n")
        for v in range(4):
            f.write("  {\n")
            for s in range(512):
                f.write("    { " + ",".join("0x{:08x}".format(g(v, s, r)) for r in range(16)) + " }")
                f.write(",\n" if s < 511 else "\n")
            f.write("  }")
            f.write(",\n" if v < 3 else "\n")
        f.write("};\n")


def write_palette_h(palette_565, sprite_cmap, char_cmap, path):
    with open(path, "w", newline="\n") as f:
        f.write("// Gyruss color data\n")
        f.write("// Generated from gyrussk.pr1 + gyrussk.pr2 + gyrussk.pr3 (romconv_gyruss.py)\n\n")
        f.write("// Master palette (32 entries, RGB565 byte-swapped)\n")
        f.write("const unsigned short gyruss_palette[] PROGMEM = {\n  ")
        f.write(",".join("0x{:04x}".format(c) for c in palette_565))
        f.write("\n};\n\n")
        f.write("// Sprite color lookup (16 groups x 16 colors, RGB565)\n")
        f.write("const unsigned short gyruss_sprite_colormap[][16] PROGMEM = {\n")
        for i, colors in enumerate(sprite_cmap):
            f.write("  { " + ",".join("0x{:04x}".format(c) for c in colors) + " }")
            f.write(",\n" if i < len(sprite_cmap) - 1 else "\n")
        f.write("};\n\n")
        f.write("// Character color lookup (16 groups x 4 colors, RGB565)\n")
        f.write("const unsigned short gyruss_char_colormap[][4] PROGMEM = {\n")
        for i, colors in enumerate(char_cmap):
            f.write("  { " + ",".join("0x{:04x}".format(c) for c in colors) + " }")
            f.write(",\n" if i < len(char_cmap) - 1 else "\n")
        f.write("};\n")


# ============================================================
# Main
# ============================================================

def main():
    rom_path = sys.argv[1] if len(sys.argv) > 1 else DEF_ROM
    out_dir = sys.argv[2] if len(sys.argv) > 2 else DEF_OUT

    print("romconv_gyruss.py")
    print(f"  ROM src : {rom_path}")
    print(f"  Output  : {out_dir}")
    print()

    src = RomSource(rom_path)
    os.makedirs(out_dir, exist_ok=True)

    # 1) main Z80
    print("Main Z80 ROM (24KB)...")
    write_main_rom_h(assemble_main_rom(src), os.path.join(out_dir, "gyruss_rom_main.h"))

    # 2) M6809 sub (raw + decrypt Konami-1)
    print("M6809 sub ROM (8KB + decrypt)...")
    raw, dec = assemble_sub_rom(src)
    write_sub_rom_h(raw, dec, os.path.join(out_dir, "gyruss_rom_sub.h"))

    # 3) audio Z80
    print("Audio Z80 ROM (16KB)...")
    write_audio_rom_h(assemble_audio_rom(src), os.path.join(out_dir, "gyruss_rom_audio.h"))

    # 4) i8039 drums (NUOVO)
    print("i8039 sample/drum ROM (4KB, gyrussk.3a)...")
    write_i8039_rom_h(src.load("gyrussk.3a"), os.path.join(out_dir, "gyruss_rom_i8039.h"))

    # 5) tiles (+ til_patch -> esatto)
    print("Tilemap (512 tile 8x8 2bpp + patch)...")
    write_tilemap_h(decode_tiles(src.load("gyrussk.4")),
                    os.path.join(out_dir, "gyruss_tilemap.h"),
                    load_patch("til_patch.table"))

    # 6) sprites (+ spr_patch)
    print("Spritemap (512 sprite 8x16 4bpp, 4 varianti + patch)...")
    spr = bytearray(0x8000)
    spr[0x0000:0x2000] = src.load("gyrussk.6")
    spr[0x2000:0x4000] = src.load("gyrussk.5")
    spr[0x4000:0x6000] = src.load("gyrussk.8")
    spr[0x6000:0x8000] = src.load("gyrussk.7")
    write_spritemap_h(decode_sprites(spr),
                      os.path.join(out_dir, "gyruss_spritemap.h"),
                      load_patch("spr_patch.table"))

    # 7) palette + colormap
    print("Palette + colormap (PROM pr1/pr2/pr3)...")
    pr3 = src.load("gyrussk.pr3")
    pr1 = src.load("gyrussk.pr1")
    pr2 = src.load("gyrussk.pr2")
    pal, scm, ccm = generate_palette_and_colormaps(pr3, pr1, pr2)
    write_palette_h(pal, scm, ccm, os.path.join(out_dir, "gyruss_palette.h"))

    print("\nFatto. Header generati (incluso gyruss_rom_i8039.h per i drums).")


if __name__ == "__main__":
    main()
