#!/usr/bin/env python3
"""
romconv_galaga.py
Conversione completa delle ROM di Galaga in header C.
Equivalente di conv_galaga.bat, tutto in un unico script.

Le ROM vengono lette direttamente dallo zip in memoria,
senza estrarre nulla su disco.

Struttura attesa:
  ../ROMS/galaga.zip      → zip con le ROM originali
  ../machines/galaga/     → cartella di output per gli header .h

Uso:
  python romconv_galaga.py
"""

import sys
import zipfile
from pathlib import Path

# ---------------------------------------------------------------------------
# Percorsi
# ---------------------------------------------------------------------------
BASE     = Path(__file__).resolve().parent
ZIP_PATH = (BASE / ".." / "ROMS" / "galaga.zip").resolve()
OUT_DIR  = (BASE / ".." / "machines" / "galaga").resolve()


# ===========================================================================
# romconv.py – conversione ROM CPU
# ===========================================================================
PATCHES = {
    "galaga_rom_cpu1": [
        (0x3382, 0x06, 0xC3),
        (0x3383, 0x0A, 0x35),
        (0x3384, 0xD9, 0x34),
        (0x348A, 0x1E, 0x01),
        (0x352B, 0xE5, 0xC9),
    ]
}

def _bit_permute_step(x, m, shift):
    t = ((x >> shift) ^ x) & m
    x = (x ^ t) ^ (t << shift)
    return x

def parse_rom(id_, blobs, outfile, apply_patches=False, decode=False):
    """blobs: lista di bytes/bytearray già letti in memoria."""
    offset = 0
    with open(outfile, "w") as of:
        print(f"const unsigned char {id_}[] = {{\n  ", end="", file=of)
        for name_idx, rom_data in enumerate(blobs):
            rom_data = bytearray(rom_data)

            # frogger audio cpu swap D0/D1
            if rom_data[:8] == bytes([0x05,0x00,0x22,0x00,0x40,0xC3,0x0B,0x02]):
                for i in range(len(rom_data)):
                    rom_data[i] = (rom_data[i] & 0xFC) | ((rom_data[i] & 2) >> 1) | ((rom_data[i] & 1) << 1)

            if apply_patches:
                for pid, patches in PATCHES.items():
                    if pid == id_:
                        for addr, expected, replacement in patches:
                            local = addr - offset
                            if 0 <= local < len(rom_data):
                                if rom_data[local] == expected:
                                    print(f"  Patch {hex(addr)}: {expected} -> {replacement}")
                                    rom_data[local] = replacement
                                else:
                                    raise ValueError(f"Patch mismatch a {hex(addr)}: atteso {expected}, trovato {rom_data[local]}")

            offset += len(rom_data)

            for i, byte in enumerate(rom_data):
                if decode:
                    byte = _bit_permute_step(byte, 8, 2)
                print(f"0x{byte:02X}", end="", file=of)
                if i != len(rom_data) - 1 or name_idx != len(blobs) - 1:
                    print(",", end="", file=of)
                    if i & 15 == 15:
                        print("\n  ", end="", file=of)
                else:
                    print("", file=of)

        print("};", file=of)


# ===========================================================================
# tileconv.py – conversione tilemap
# ===========================================================================
def _parse_chr(data):
    char = []
    for y in range(8):
        row = []
        for x in range(8):
            byte = data[15 - x - 2 * (y & 4)]
            c0 = 1 if byte & (0x08 >> (y & 3)) else 0
            c1 = 2 if byte & (0x80 >> (y & 3)) else 0
            row.append(c0 + c1)
        char.append(row)
    return char

def _dump_chr(data):
    hexs = []
    for y in range(8):
        val = 0
        for x in range(8):
            val = (val >> 2) + (data[y][x] << (16 - 2))
        hexs.append(hex(val))
    return ",".join(hexs)

def parse_charmap(id_, blob, outname):
    """blob: bytes già letti in memoria."""
    if len(blob) != 4096:
        raise ValueError(f"Dimensione tilemap inattesa: {len(blob)}")

    chars = [_parse_chr(blob[16 * c: 16 * (c + 1)]) for c in range(256)]

    with open(outname, "w") as f:
        print(f"const unsigned short {id_}[][8] = {{", file=f)
        chars_str = [" { " + _dump_chr(c) + " }" for c in chars]
        print(",\n".join(chars_str), file=f)
        print("};", file=f)


# ===========================================================================
# spriteconv.py – conversione sprite
# ===========================================================================
def _decode_sprite_data(data):
    charmap_data = list(data)
    myiter = iter(range(len(charmap_data)))
    for i in myiter:
        swapbuffer = [None] * 8
        for j in range(8):
            index = _bit_permute_step(j, 1, 2)
            swapbuffer[j] = charmap_data[i + index]
        for j in range(8):
            value = _bit_permute_step(swapbuffer[j], 16, 2)
            charmap_data[i + j] = value
        for _ in range(7):
            next(myiter, None)
    return charmap_data

def _parse_sprite(data, pacman_fmt=False, decode=False):
    if decode:
        data = _decode_sprite_data(data)
    sprite = []
    for y in range(16):
        row = []
        for x in range(16):
            idx = ((y & 8) << 1) + (((x & 8) ^ 8) << 2) + (7 - (x & 7)) + 2 * (y & 4)
            c0 = 1 if data[idx] & (0x08 >> (y & 3)) else 0
            c1 = 2 if data[idx] & (0x80 >> (y & 3)) else 0
            row.append(c0 + c1)
        sprite.append(row)
    if pacman_fmt:
        sprite = sprite[4:] + sprite[:4]
    return sprite

def _dump_sprite(data, flip_x, flip_y):
    hexs = []
    for y in (range(16) if not flip_y else reversed(range(16))):
        val = 0
        for x in range(16):
            if not flip_x:
                val = (val >> 2) + (data[y][x] << (32 - 2))
            else:
                val = (val << 2) + data[y][x]
        hexs.append(hex(val))
    return ",".join(hexs)

def _dump_c_source(sprites, flip_x, flip_y, f):
    print(" {", file=f)
    sprites_str = ["  { " + _dump_sprite(s, flip_x, flip_y) + " }" for s in sprites]
    print(",\n".join(sprites_str), file=f)
    if flip_x and flip_y:
        print(" }", file=f)
    else:
        print(" },", file=f)

def parse_spritemap_galaga(id_, blobs, outfile):
    """blobs: lista di bytes già letti in memoria."""
    sprites = []
    for blob in blobs:
        if len(blob) != 4096:
            raise ValueError(f"Dati spritemap di dimensione inattesa: {len(blob)}")
        for sprite in range(64):
            sprites.append(_parse_sprite(blob[64 * sprite: 64 * (sprite + 1)]))

    with open(outfile, "w") as f:
        print(f"const unsigned long {id_}[][{len(sprites)}][16] = {{", file=f)
        _dump_c_source(sprites, False, False, f)
        _dump_c_source(sprites, False, True,  f)
        _dump_c_source(sprites, True,  False, f)
        _dump_c_source(sprites, True,  True,  f)
        print("};", file=f)


# ===========================================================================
# cmapconv.py – conversione colormap e palette
# ===========================================================================
def _parse_palette(blob, offset=0):
    """blob: bytes del PROM palette già letti in memoria."""
    palette = []
    for c in blob:
        b = 31 * ((c >> 6) & 0x3) // 3
        g = 63 * ((c >> 3) & 0x7) // 7
        r = 31 * ((c >> 0) & 0x7) // 7
        rgb  = (r << 11) + (g << 5) + b
        rgbs = ((rgb & 0xFF00) >> 8) + ((rgb & 0xFF) << 8)
        palette.append(rgbs)
    return palette[offset: offset + 16]

def parse_colormap(id_, palette_blob, palette_offset, colormap_blob, outname):
    """Tutti i dati già in memoria come bytes."""
    palette = _parse_palette(palette_blob, palette_offset)

    if len(colormap_blob) != 256:
        raise ValueError("Dati colormap di dimensione inattesa")

    with open(outname, "w") as f:
        print(f"const unsigned short {id_}[][4] = {{", file=f)
        colors = []
        for idx in range(64):
            c = colormap_blob[4 * idx: 4 * (idx + 1)]
            if any(v < 0 or v > 15 for v in c):
                raise ValueError("Indice colore fuori range")
            colors.append("{" + ",".join(hex(palette[v]) for v in c) + "}")
        print(",\n".join(colors), file=f)
        print("};", file=f)


# ===========================================================================
# audioconv.py – conversione wavetable
# ===========================================================================
def parse_wavetable(name, blobs, outfile):
    """blobs: lista di bytes già letti in memoria."""
    with open(outfile, "w") as of:
        print(f"const signed char {name}[][32] = {{", file=of)
        for blob_idx, wave_data in enumerate(blobs):
            if len(wave_data) != 256:
                raise ValueError("Dati ROM audio di dimensione inattesa")
            for w in range(8):
                print(f"// wave #{w}", file=of)
                for y in range(8):
                    print("//", end="", file=of)
                    for s in range(32):
                        v = wave_data[32 * w + s]
                        if   v == 15 - 2 * y:        print("---", end="", file=of)
                        elif v == 15 - (2 * y + 1):  print("___", end="", file=of)
                        else:                         print("   ", end="", file=of)
                    print("", file=of)

                print(" {", end="", file=of)
                for s in range(32):
                    print(f"{wave_data[32*w+s]-7:2d}", end="", file=of)
                    is_last_sample = (s == 31)
                    is_last_wave   = (w == 7 and blob_idx == len(blobs) - 1)
                    if not is_last_sample:
                        print(",", end="", file=of)
                    elif not is_last_wave:
                        print("},", file=of)
                    else:
                        print("}", file=of)
                print("", file=of)

        print("};", file=of)


# ===========================================================================
# Pipeline principale
# ===========================================================================
def main():
    if not ZIP_PATH.exists():
        print(f"ERRORE: zip non trovato: {ZIP_PATH}")
        sys.exit(1)

    OUT_DIR.mkdir(parents=True, exist_ok=True)

    def out(name):
        return str(OUT_DIR / name)

    print(f"--- Galaga: apertura {ZIP_PATH} ---")
    with zipfile.ZipFile(ZIP_PATH) as zf:
        needed = [
            "gg1_1b.3p", "gg1_2b.3m", "gg1_3.2m", "gg1_4b.2l",
            "gg1_5b.3f", "gg1_7b.2c",
            "gg1_9.4l",
            "gg1_11.4d", "gg1_10.4f",
            "prom-5.5n", "prom-3.1c", "prom-4.2n",
            "prom-1.1d",
        ]
        names = set(zf.namelist())
        missing = [n for n in needed if n not in names]
        if missing:
            print(f"ERRORE: ROM mancanti nello zip: {missing}")
            sys.exit(1)

        # Legge tutto in memoria una volta sola
        roms = {n: zf.read(n) for n in needed}

    # -----------------------------------------------------------------------
    print("--- Galaga: codice CPU ---")
    parse_rom(
        "galaga_rom_cpu1",
        [roms["gg1_1b.3p"], roms["gg1_2b.3m"], roms["gg1_3.2m"], roms["gg1_4b.2l"]],
        out("galaga_rom1.h"),
        apply_patches=True,
    )
    parse_rom("galaga_rom_cpu2", [roms["gg1_5b.3f"]], out("galaga_rom2.h"))
    parse_rom("galaga_rom_cpu3", [roms["gg1_7b.2c"]], out("galaga_rom3.h"))

    # -----------------------------------------------------------------------
    print("--- Galaga: tiles ---")
    parse_charmap("galaga_tilemap", roms["gg1_9.4l"], out("galaga_tilemap.h"))

    # -----------------------------------------------------------------------
    print("--- Galaga: sprite ---")
    parse_spritemap_galaga(
        "galaga_sprites",
        [roms["gg1_11.4d"], roms["gg1_10.4f"]],
        out("galaga_spritemap.h"),
    )

    # -----------------------------------------------------------------------
    print("--- Galaga: colormap ---")
    parse_colormap(
        "galaga_colormap_sprites",
        roms["prom-5.5n"], 0,
        roms["prom-3.1c"],
        out("galaga_cmap_sprites.h"),
    )
    parse_colormap(
        "galaga_colormap_tiles",
        roms["prom-5.5n"], 16,
        roms["prom-4.2n"],
        out("galaga_cmap_tiles.h"),
    )

    # -----------------------------------------------------------------------
    print("--- Galaga: audio ---")
    parse_wavetable("galaga_wavetable", [roms["prom-1.1d"]], out("galaga_wavetable.h"))

    # -----------------------------------------------------------------------
    print("--- Conversione completata con successo! ---")
    print(f"Header generati in: {OUT_DIR}")


if __name__ == "__main__":
    main()
