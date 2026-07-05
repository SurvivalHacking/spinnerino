#!/usr/bin/env python3
"""
romconv_gigas.py — converte gigasb.zip (set bootleg di Gigas, NO MC-8123)
in header C++ per il porting SPINNERINO.

Set: gigasb (Gigas bootleg, ROM già decifrate)
Driver MAME: misc/freekick.cpp (linea 1709 ROM_START gigasb)

ROM da estrarre:
  g-7   32 KB  → maincpu Z80 part 1 (con ROM_CONTINUE bizzarro)
  g-8   64 KB  → maincpu Z80 part 2
  g-4   16 KB  → tiles plane 0
  g-5   16 KB  → tiles plane 1
  g-6   16 KB  → tiles plane 2
  g-1   16 KB  → sprites plane 0 (MSB, da gfx_layout RGN_FRAC(0,3))
  g-2   16 KB  → sprites plane 1 (RGN_FRAC(2,3) = g-2 al offset 0x8000)
  g-3   16 KB  → sprites plane 2 (LSB, RGN_FRAC(1,3) = g-3 al offset 0x4000)
  1.pr  256 B  → palette R[0..255]
  6.pr  256 B  → palette R[256..511]
  5.pr  256 B  → palette G[0..255]
  4.pr  256 B  → palette G[256..511]
  2.pr  256 B  → palette B[0..255]
  3.pr  256 B  → palette B[256..511]

Output (in arkanoid/machines/gigas/):
  gigas_rom_cpu.h          (96 KB Z80 ricostruito da g-7+g-8 con ROM_CONTINUE)
  gigas_gfx_tile_p0..p2.h  (3 × 16 KB tile plane)
  gigas_gfx_spr_p0..p2.h   (3 × 16 KB sprite plane, ORDINE: g-1, g-2, g-3)
  gigas_palette.h          (512 colori RGB565 byte-swap, formato pal4bit
                            con weights MAME 0x0e/0x1f/0x43/0x8f)

Layout tile (gfx_8x8x3_planar, standard MAME):
  8x8 px, 3 plane separati nelle 3 ROM tile.
  byte_offset(y) = y (0..7), bit_pos = 7 - x (MSB-first).

Layout sprite (custom freekick):
  16x16 px, 3 plane separati. Plane order { 0, 2, 1 } (RGN_FRAC).
  x bits: 0..7 nel byte 0 (riga di 8 px sx), 0..7 nel byte 16 (riga 8 px dx)
  y bits: 0..7 * 8 (8 byte per riga complessivamente)
  Total: 16*16 / 8 = 32 byte per tile per plane.

Palette decode (palette_init_rgb_444_proms):
  Per i in 0..511:
    R = bit weights di prom[i + 0x000]    (4-bit nibble)
    G = bit weights di prom[i + 0x200]
    B = bit weights di prom[i + 0x400]
  Bit weight: 0x0e * b0 + 0x1f * b1 + 0x43 * b2 + 0x8f * b3
  → output RGB888 → RGB565 byte-swap.

ROM_CONTINUE per CPU (MAME ROM_START gigasb):
  ROM_LOAD( "g-7", 0x0c000, 0x4000, ... )  ; primi 16KB di g-7 → ROM[0x0c000]
  ROM_CONTINUE(    0x00000, 0x4000 )       ; secondi 16KB di g-7 → ROM[0x00000]
  ROM_LOAD( "g-8", 0x10000, 0x8000, ... )  ; primi 32KB di g-8 → ROM[0x10000]
  ROM_CONTINUE(    0x04000, 0x8000 )       ; secondi 32KB di g-8 → ROM[0x04000]

FIX MACRO OpZ80 in Z80.c (CRITICO!):
  Marat Fayzullin Z80.c di default ha:
    #define OpZ80(A) RdZ80(A)
  Cioè operandi delle istruzioni leggono via RdZ80 (data section).
  Per Gigas (gigasb bootleg) DATA e OP section differiscono in 48899/49152
  byte. Quindi le costanti immediate (es. LD SP,$5C06) hanno valori diversi
  in op vs data section. Senza fix: programma legge operandi data = valori
  random → CPU crash o test mode.
  Fix in Z80.c: cambiato a:
    #define OpZ80(A) OpZ80_INL(A)
  → tutti gli accessi PC-based (operandi) e SP-based (stack pop) passano via
  dispatcher opZ80() della machine. Per Arkanoid 1 (no dual-mapping)
  opZ80==rdZ80 = no effetto. Per Gigas: operandi leggono OP section corretta.

SP INITIAL (CRITICO):
  Z80 ResetZ80 (Marat Fayzullin) setta SP=0xF000 di default. In Gigas 0xF000 è
  IO address (DSW1 read), NON RAM. Il programma a $BF44 esegue immediato
  RST 20H (E7) che è PUSH PC + JP $0020. Senza SP valido, la PUSH va in IO
  ignorata → POP successivo legge spazzatura → CPU crash o stato indefinito.
  Fix: in Gigas::reset() forziamo cpu[0].SP.W = 0xCFFF (= top di main RAM
  0xC000-0xCFFF, area writable).

INTERRUPT (freekick.cpp:1100):
  Gigas usa DUE tipi di interrupt:
    - IRQ (RST 38h) periodico a 120 Hz (= 2 per frame, set_periodic_int)
    - NMI a vblank 60 Hz, abilitato da bit 4 outlatch (0xE004)
  run_frame() in gigas.cpp esegue: half_step + IRQ + half_step + IRQ + NMI.

DUAL-MAPPING CRITICO (init_gigasb in freekick.cpp:1892):
  Il bootleg gigasb ha CODICE in 2 zone diverse della ROM region 96KB:
    - DATA fetch (Z80 mreq normale)  → ROM[0x0000 .. 0x0BFFF]
    - OPCODE fetch (Z80 M1 cycle)    → ROM[0x0C000 .. 0x17FFF]
  MAME usa AS_OPCODES (decrypted_opcodes_map) per leggere opcode da bank
  diverso. Il mio porting deve quindi:
    - Gigas::rdZ80(addr)  → pgm_read_byte(rom[addr])              per addr<0xC000
    - Gigas::opZ80(addr)  → pgm_read_byte(rom[addr + 0xC000])     per addr<0xC000
  Il reset vector PC=0 fetcha opcode = ROM[0xC000] = 0xAF (XOR A) — codice
  Z80 valido. Senza questo fix il PC=0 leggeva 0xC9 (RET) e crashava in HALT.
FIX SESSIONE 2026-05-09 (gigas.cpp, NON tocca il .py output):
  1) Test screen lock al boot: physical FIRE (EC11 SW) era letto come BUTTON1
     pressed durante init → game entrava in service test screen.
     Fix: maschera BUTTON1 per primi 90 frame dopo reset (boot_button1_mask).
  2) BG garbled: plane order tile sbagliato. gfx_8x8x3_planar STANDARD MAME ha
     plane[0]=LSB, plane[2]=MSB. Fix in blit_tile: pix = (p2<<2)|(p1<<1)|p0
     (era invertito). p0/p1/p2 da romconv = file order = MAME plane[] order.
  3) Audio mancante: framework SN76489 esteso da 2 a 4 chip (Gigas ne ha 4 a
     0xFC00-0xFC03). Aggiunto parser PSG byte-stream in gigas.cpp wrZ80, e
     dispatch MCH_GIGAS in audio.cpp transmit() → sn76489_render_buffer.
  4) Sprite mancanti (drago in 2 pezzi, paddle pezzi): limit 32 → 64 in
     prepare_frame (MAME itera tutti i 64 slot). Y formula 240-b2 (era b2 raw).
  5) Paddle non raggiungeva muro sx: per ROT270 + 180° SW flip, gli assi del
     buffer sono ruotati. buffer X (sp.x) = display Y vertical, buffer Y
     (sp.y) = display X horizontal. Game ROM clampa b2 a sub-range del
     playfield → paddle corto sul muro sx. Fix: sp.y = 240 - b2 - 16
     (offset -16 estende sprite verso sx user view).
  6) Mapping pulsanti: physical FIRE (EC11 SW) = BUTTON1 (FIRE in game) +
     START1 (start) — BUTTON1 mascherato boot 90 frame; physical START =
     START1 alternativo; physical COIN = COIN1.
  7) Spinner full range: aggiunto ec11_dial_counter free-running uint8 in
     input.cpp (wrappa mod 256), Gigas legge questo invece di ec11_paddle_x
     che e' clamped 0..255 per Arkanoid. Read inverted: -(int8_t)dial per
     PORT_REVERSE MAME.
"""
import zipfile, os, sys
from pathlib import Path

ZIP = str((Path.cwd() / ".." / "ROMS" / "gigasb.zip").resolve())
OUT  = str((Path.cwd() / ".." / "machines" / "gigas").resolve())


def write_byte_array(path, var_name, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w') as f:
        f.write(f"// Auto-generated by romconv_gigas.py\n")
        f.write(f"// {os.path.basename(path)}  size={len(data)} bytes\n")
        f.write(f"#include <stdint.h>\n\n")
        f.write(f"const uint8_t {var_name}[{len(data)}] PROGMEM = {{\n")
        for i in range(0, len(data), 16):
            chunk = data[i:i+16]
            f.write("  " + ", ".join(f"0x{b:02X}" for b in chunk) + ",\n")
        f.write("};\n")
    print(f"  wrote {path}  ({len(data)} bytes)")

def write_palette(path, var_name, palette_rgb565):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    n = len(palette_rgb565)
    with open(path, 'w') as f:
        f.write(f"// Auto-generated by romconv_gigas.py\n")
        f.write(f"// {n} colors RGB565 byte-swap (ESP32 SPI BE)\n")
        f.write(f"const unsigned short {var_name}[{n}] PROGMEM = {{\n")
        for i in range(0, n, 8):
            chunk = palette_rgb565[i:i+8]
            f.write("  " + ", ".join(f"0x{p:04X}" for p in chunk) + ",\n")
        f.write("};\n")
    print(f"  wrote {path}  ({n} colors)")

def prom_4bit_to_8bit(nibble):
    """Bit weight conversion da MAME palette_init_rgb_444_proms:
       bit0=0x0e, bit1=0x1f, bit2=0x43, bit3=0x8f. Total max = 0xFF.
    """
    return (0x0e * ((nibble >> 0) & 1)
          | 0x1f * ((nibble >> 1) & 1)
          | 0x43 * ((nibble >> 2) & 1)
          | 0x8f * ((nibble >> 3) & 1))

def build_palette(prom_r, prom_g, prom_b):
    """ prom_r, prom_g, prom_b: ognuna 512 byte (256 lo + 256 hi concatenati).
        Ritorna 512 colori RGB565 byte-swap.
    """
    out = []
    for i in range(512):
        r = prom_4bit_to_8bit(prom_r[i] & 0x0F)
        g = prom_4bit_to_8bit(prom_g[i] & 0x0F)
        b = prom_4bit_to_8bit(prom_b[i] & 0x0F)
        rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        bs = ((rgb565 >> 8) & 0xFF) | ((rgb565 & 0xFF) << 8)
        out.append(bs)
    return out

def build_cpu_rom(g7, g8):
    """ ROM region maincpu è 0x18000 (96 KB), assemblata da g-7 (32KB) + g-8 (64KB)
        usando ROM_CONTINUE come MAME.
    """
    rom = bytearray(0x18000)
    # g-7 (32KB): primi 16KB → 0x0C000, secondi 16KB → 0x00000
    rom[0x0C000:0x10000] = g7[0x0000:0x4000]
    rom[0x00000:0x04000] = g7[0x4000:0x8000]
    # g-8 (64KB): primi 32KB → 0x10000, secondi 32KB → 0x04000
    rom[0x10000:0x18000] = g8[0x0000:0x8000]
    rom[0x04000:0x0C000] = g8[0x8000:0x10000]
    return bytes(rom)

def main():
    if not os.path.exists(ZIP):
        print(f"!! ZIP non trovato: {ZIP}", file=sys.stderr); sys.exit(1)
    print(f"[romconv_gigas] {ZIP}")
    zf = zipfile.ZipFile(ZIP)
    names = set(zf.namelist())

    needed = ["g-7","g-8","g-4","g-5","g-6","g-1","g-2","g-3",
              "1.pr","2.pr","3.pr","4.pr","5.pr","6.pr"]
    for n in needed:
        if n not in names:
            print(f"!! ROM mancante: {n}", file=sys.stderr); sys.exit(1)

    g7 = zf.read("g-7"); g8 = zf.read("g-8")
    if len(g7) != 0x8000 or len(g8) != 0x10000:
        print(f"!! size CPU ROM inattese g7={len(g7)} g8={len(g8)}", file=sys.stderr)
        sys.exit(1)
    cpu_rom = build_cpu_rom(g7, g8)
    write_byte_array(os.path.join(OUT, "gigas_rom_cpu.h"), "gigas_rom_cpu", cpu_rom)

    # Tiles plane 0..2 (g-4, g-5, g-6)
    write_byte_array(os.path.join(OUT, "gigas_gfx_tile_p0.h"), "gigas_gfx_tile_p0", zf.read("g-4"))
    write_byte_array(os.path.join(OUT, "gigas_gfx_tile_p1.h"), "gigas_gfx_tile_p1", zf.read("g-5"))
    write_byte_array(os.path.join(OUT, "gigas_gfx_tile_p2.h"), "gigas_gfx_tile_p2", zf.read("g-6"))

    # Sprites plane 0..2 (RGN_FRAC order: g-1=plane0, g-2=plane1, g-3=plane2)
    # MAME: { RGN_FRAC(0,3), RGN_FRAC(2,3), RGN_FRAC(1,3) } → plane[0]=g-1,
    # plane[1]=g-2 (offset 0x8000), plane[2]=g-3 (offset 0x4000).
    write_byte_array(os.path.join(OUT, "gigas_gfx_spr_p0.h"), "gigas_gfx_spr_p0", zf.read("g-1"))
    write_byte_array(os.path.join(OUT, "gigas_gfx_spr_p1.h"), "gigas_gfx_spr_p1", zf.read("g-2"))
    write_byte_array(os.path.join(OUT, "gigas_gfx_spr_p2.h"), "gigas_gfx_spr_p2", zf.read("g-3"))

    # Palette PROMs (512 colori)
    # Layout MAME ROM_LOAD: 1.pr,6.pr,5.pr,4.pr,2.pr,3.pr a offset 0x000..0x500
    # palette_init_rgb_444_proms legge: R=prom[i], G=prom[i+0x200], B=prom[i+0x400]
    prom_R = zf.read("1.pr") + zf.read("6.pr")  # 512B (R[0..511])
    prom_G = zf.read("5.pr") + zf.read("4.pr")  # 512B (G[0..511])
    prom_B = zf.read("2.pr") + zf.read("3.pr")  # 512B (B[0..511])
    palette = build_palette(prom_R, prom_G, prom_B)
    write_palette(os.path.join(OUT, "gigas_palette.h"), "gigas_palette", palette)

    print("Done!")

if __name__ == '__main__':
    main()
