#!/usr/bin/env python3
"""
romconv_pang.py - genera machines/pang/pang_rom_assets.h da ROMS/pang.zip
per il porting di PANG (Mitchell 1989) su SPINNERINO.

Set MAME: `pang` (World). File usati:
  programma Z80 (KABUKI): pang6.bin (fisso 0x0000-0x7FFF) + pang7.bin (8 banchi x 0x4000)
  gfx char  (512K): pang_09 + bb3 + pang_11 + bb5  (4 x 128K, concatenati)
  gfx sprite(256K): bb10 + bb9                     (2 x 128K, concatenati)
  OKI6295   (128K): bb1
Output array: pang_op_fixed/pang_data_fixed (0x8000), pang_op_banks/pang_data_banks
(0x20000), pang_chars (0x80000), pang_sprites (0x40000), pang_oki (0x20000).

KABUKI: porting fedele di src/mame/capcom/kabuki.cpp (bytedecode + mitchell_decode).
Chiavi pang: swap_key1=0x01234567 swap_key2=0x76543210 addr_key=0x6548 xor_key=0x24.
Verificato byte-identico contro l'header pre-generato.
Path portabili (eseguire da ROM_CONVERTER/). Solo stdlib (zipfile).
"""
import os, zipfile
from pathlib import Path

ZIP = str((Path.cwd() / ".." / "ROMS" / "pang.zip").resolve())
OUT = str((Path.cwd() / ".." / "machines" / "pang" / "pang_rom_assets.h").resolve())

# chiavi KABUKI pang (kabuki.cpp::pang_decode -> mitchell_decode)
SK1, SK2, ADDR_KEY, XOR_KEY = 0x01234567, 0x76543210, 0x6548, 0x24


def bitswap1(src, key, select):
    if select & (1 << ((key >> 0) & 7)):  src = (src & 0xfc) | ((src & 0x01) << 1) | ((src & 0x02) >> 1)
    if select & (1 << ((key >> 4) & 7)):  src = (src & 0xf3) | ((src & 0x04) << 1) | ((src & 0x08) >> 1)
    if select & (1 << ((key >> 8) & 7)):  src = (src & 0xcf) | ((src & 0x10) << 1) | ((src & 0x20) >> 1)
    if select & (1 << ((key >> 12) & 7)): src = (src & 0x3f) | ((src & 0x40) << 1) | ((src & 0x80) >> 1)
    return src


def bitswap2(src, key, select):
    if select & (1 << ((key >> 12) & 7)): src = (src & 0xfc) | ((src & 0x01) << 1) | ((src & 0x02) >> 1)
    if select & (1 << ((key >> 8) & 7)):  src = (src & 0xf3) | ((src & 0x04) << 1) | ((src & 0x08) >> 1)
    if select & (1 << ((key >> 4) & 7)):  src = (src & 0xcf) | ((src & 0x10) << 1) | ((src & 0x20) >> 1)
    if select & (1 << ((key >> 0) & 7)):  src = (src & 0x3f) | ((src & 0x40) << 1) | ((src & 0x80) >> 1)
    return src


def _rol(src):
    return ((src & 0x7f) << 1) | ((src & 0x80) >> 7)


def bytedecode(src, sk1, sk2, xor_key, select):
    src = bitswap1(src, sk1 & 0xffff, select & 0xff)
    src = _rol(src)
    src = bitswap2(src, sk1 >> 16, select & 0xff)
    src ^= xor_key
    src = _rol(src)
    src = bitswap2(src, sk2 & 0xffff, select >> 8)
    src = _rol(src)
    src = bitswap1(src, sk2 >> 16, select >> 8)
    return src & 0xff


def kabuki_decode(src, dst_op, dst_data, so, doo, ddo, base, length):
    """Porting di kabuki_decode: opcodes->dst_op, data->dst_data (qui dst_data==src,
    decodifica in-place come fa mitchell_decode)."""
    for A in range(length):
        sel = (A + base) + ADDR_KEY
        dst_op[doo + A] = bytedecode(src[so + A], SK1, SK2, XOR_KEY, sel)
        sel = ((A + base) ^ 0x1fc0) + ADDR_KEY + 1
        dst_data[ddo + A] = bytedecode(src[so + A], SK1, SK2, XOR_KEY, sel)


def mitchell_decode(prog_fixed, prog_banks):
    """Replica mitchell_decode: regione maincpu = [fisso 0x8000][gap][banchi @0x10000].
    Ritorna (op_fixed, data_fixed, op_banks, data_banks)."""
    size = 0x10000 + len(prog_banks)
    src = bytearray(size)
    src[0x0000:0x8000] = prog_fixed
    src[0x10000:0x10000 + len(prog_banks)] = prog_banks
    dst = bytearray(size)
    # blocco fisso: base 0x0000, len 0x8000 (data in-place su src)
    kabuki_decode(src, dst, src, 0, 0, 0, 0x0000, 0x8000)
    # banchi: ognuno base 0x8000 (mappa CPU 0x8000-0xBFFF), len 0x4000
    numbanks = (size - 0x10000) // 0x4000
    for i in range(numbanks):
        off = 0x10000 + i * 0x4000
        kabuki_decode(src, dst, src, off, off, off, 0x8000, 0x4000)
    return (bytes(dst[0:0x8000]), bytes(src[0:0x8000]),
            bytes(dst[0x10000:size]), bytes(src[0x10000:size]))


def write_array(f, name, data, per_line=20):
    f.write(f"const unsigned char {name}[] PROGMEM = {{\n")
    for i in range(0, len(data), per_line):
        f.write("  " + ",".join(f"0x{b:02X}" for b in data[i:i+per_line]) + ",\n")
    f.write("};\n")


def main():
    print(f"[pang] zip = {ZIP}")
    z = zipfile.ZipFile(ZIP)
    r = {n: z.read(n) for n in z.namelist()}

    op_fixed, data_fixed, op_banks, data_banks = mitchell_decode(r['pang6.bin'], r['pang7.bin'])
    chars   = r['pang_09.bin'] + r['bb3.bin'] + r['pang_11.bin'] + r['bb5.bin']   # 4 x 128K
    sprites = r['bb10.bin'] + r['bb9.bin']                                        # 2 x 128K
    oki     = r['bb1.bin']                                                        # 128K

    assert len(op_fixed) == 0x8000 and len(op_banks) == 0x20000
    assert len(chars) == 0x80000 and len(sprites) == 0x40000 and len(oki) == 0x20000

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, 'w') as f:
        f.write("// pang_rom_assets.h - asset PROGMEM per Pang (Mitchell 1989)\n")
        f.write("// Generato da romconv_pang.py - NON modificare a mano.\n")
        f.write("// KABUKI gia' decifrato: opcodes (fetch) e data (read) separati.\n")
        f.write("#ifndef PANG_ROM_ASSETS_H\n#define PANG_ROM_ASSETS_H\n")
        f.write("#include <pgmspace.h>\n\n")
        f.write("// --- CPU Z80 KABUKI: blocco fisso 0x0000-0x7FFF ---\n")
        write_array(f, "pang_op_fixed", op_fixed)
        write_array(f, "pang_data_fixed", data_fixed)
        f.write("\n// --- CPU Z80 KABUKI: banchi 0x8000-0xBFFF (8 x 0x4000) ---\n")
        write_array(f, "pang_op_banks", op_banks)
        write_array(f, "pang_data_banks", data_banks)
        f.write("\n// --- GFX char 8x8 4bpp (pang_09 + bb3 + pang_11 + bb5) ---\n")
        f.write("#define PANG_CHARS_HALF 0x40000\n")
        write_array(f, "pang_chars", chars)
        f.write("\n// --- GFX sprite 16x16 4bpp (bb10 + bb9) ---\n")
        f.write("#define PANG_SPRITES_HALF 0x20000\n")
        write_array(f, "pang_sprites", sprites)
        f.write("\n// --- OKI6295 ADPCM samples (bb1) ---\n")
        write_array(f, "pang_oki", oki)
        f.write("\n#endif // PANG_ROM_ASSETS_H\n")
    print(f"  wrote {OUT}")
    print(f"  op_fixed=0x{len(op_fixed):X} op_banks=0x{len(op_banks):X} "
          f"chars=0x{len(chars):X} sprites=0x{len(sprites):X} oki=0x{len(oki):X}")
    print("Done!")


if __name__ == '__main__':
    main()
