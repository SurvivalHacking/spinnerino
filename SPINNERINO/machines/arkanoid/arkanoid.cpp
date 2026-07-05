// ============================================================================
// SPINNERINO SH - machines/arkanoid/arkanoid.cpp
//
// Target ROM set: arkangc (Arkanoid bootleg Game Corporation, NO MCU 68705)
// Hardware reale (sintesi MAME taito/arkanoid.cpp + arkanoid_v.cpp):
//   Z80 @ 6 MHz, 1x AY-3-8910, 256x224 ROT270
//   Tile 8x8 3bpp (4096 char), sprite 16x8 = 2 char 8x8 affiancati
//   Palette 512 colori RGB444 (3 PROM)
//
// Memory map (bootleg arkangc - bootleg_map MAME, NO MCU 68705):
//   0x0000-0xBFFF  ROM Z80 (48 KB)
//   0xC000-0xC7FF  RAM lavoro (2 KB)
//   0xD000         AY-3-8910 address_w
//   0xD001         AY-3-8910 data_r/w
//   0xD008         control: bit5=gfx_bank, bit6=palette_bank, flip
//   0xD00C         SYSTEM (COIN1, COIN2, SERVICE, TILT)
//   0xD010         BUTTONS read + watchdog write (START1, START2, BUTTON1)
//   0xD018         input_mux_r (paddle 0..255 via DIAL 8-bit)
//   0xE000-0xE7FF  Video RAM (paired bytes: byte0=color/code-hi, byte1=code-lo)
//   0xE800-0xE83F  Sprite RAM (4 byte per sprite, 16 sprite max)
//   0xE840-0xEFFF  RAM extra
//   0xF000-0xFFFF  NOP read (open bus)
//
// Tile decode:
//   offs = (row*32 + col) * 2
//   videoram[offs]   : bits 0-2 = code high (3 bit),  bits 3-7 = color (5 bit)
//   videoram[offs+1] : code low (8 bit)
//   tile_code = code_lo | (code_hi << 8) | (gfx_bank << 11)        // 0..4095
//   color     = color_5bit | (palette_bank << 5)                    // 0..63
//
// Sprite decode (16x8, 2 char affiancati):
//   for offs in steps of 4 in spriteram:
//     sx       = spriteram[offs]
//     sy       = 248 - spriteram[offs+1]
//     attr     = spriteram[offs+2]   (bits 0-1 = code-hi, bits 3-7 = color)
//     code_lo  = spriteram[offs+3]
//     code = code_lo | ((attr & 0x03) << 8) | (gfx_bank << 10)      // 0..2047
//     left_char  = code * 2,   right_char = code * 2 + 1
// ============================================================================
#include "arkanoid.h"

#ifdef ENABLE_ARKANOID
#include "arkanoid_rom_cpu.h"
#include "arkanoid_tilemap.h"
#include "arkanoid_palette.h"
#include "arkanoid_logo.h"

// ---- paddle dall'ISR EC11 (input.cpp) ----
extern volatile int16_t ec11_paddle_x;

// ---- area memory layout (in `memory` da machineBase, RAMSIZE=16384) ----
//   0x0000-0x07FF  RAM lavoro C000-C7FF (2KB)
//   0x0800-0x0FFF  RAM extra E840-EFFF (~2KB, allineata a 2KB per semplicita')
//   0x1000-0x17FF  VRAM E000-E7FF (2KB)
//   0x1800-0x183F  Sprite RAM E800-E83F (64 byte, 16 sprite)
#define MEM_RAM_BASE     0x0000   // mappa C000-C7FF
#define MEM_RAMX_BASE    0x0800   // mappa E840-EFFF
#define MEM_VRAM_BASE    0x1000   // mappa E000-E7FF
#define MEM_SPRRAM_BASE  0x1800   // mappa E800-E83F

// Schermo gioco: 256 wide × 224 tall, 32 col × 28 row di tile 8x8
#define ARK_SCREEN_W   256
#define ARK_SCREEN_H   224
#define ARK_COLS        32
#define ARK_ROWS        28
#define ARK_MAX_SPRITES 16    // bootleg arkangc: spriteram E800-E83F = 64 byte / 4

Arkanoid::Arkanoid()
  : gfx_bank(0), palette_bank(0), ay_addr(0),
    in0_state(0xFF), dsw1_state(0xFF), paddle_latch(128) {
}

void Arkanoid::init(Input *in, unsigned short *fb,
                    sprite_S *sb, unsigned char *mem) {
  machineBase::init(in, fb, sb, mem);
  for (int c = 0; c < 4; c++) {
    sn_period[0][c]    = 0;
    sn_volume[0][c]    = 15;
    sn_min_volume[0][c]= 15;
    sn_hold[0][c]      = 0;
  }
}

void Arkanoid::reset() {
  machineBase::reset();
  gfx_bank     = 0;
  palette_bank = 0;
  ay_addr      = 0;
  in0_state    = 0xFF;
  dsw1_state   = 0xFF;
  paddle_latch = 128;
}

const unsigned short *Arkanoid::logo(void) {
  return arkanoid_logo;
}

// ---------------------------------------------------------------------------
// Z80 memory access
// ---------------------------------------------------------------------------
unsigned char Arkanoid::opZ80(unsigned short Addr) {
  static bool seen[16] = {0};
  static int trace_phase = 0;
  // Phase 0..3: registra il PERCORSO che porta al loop BAD HARDWARE (0x0398)
  if (trace_phase < 200) {
    switch (Addr) {
      case 0x038C: if (!seen[0]) { Serial.println(">> 038C (routine start CALL 21D0)"); seen[0]=true; trace_phase++; } break;
      case 0x038F: if (!seen[1]) { Serial.println(">> 038F (LD A,(D018))"); seen[1]=true; trace_phase++; } break;
      case 0x0392: if (!seen[2]) { Serial.println(">> 0392 (XOR A)"); seen[2]=true; trace_phase++; } break;
      case 0x0393: if (!seen[3]) { Serial.println(">> 0393 (LD ED8A,A)"); seen[3]=true; trace_phase++; } break;
      case 0x0396: if (!seen[4]) { Serial.println(">> 0396 (JR Z +35)"); seen[4]=true; trace_phase++; } break;
      case 0x0398: if (!seen[5]) { Serial.println("XX 0398 BAD HW entry !"); seen[5]=true; trace_phase++; } break;
      case 0x03BB: if (!seen[6]) { Serial.println(">> 03BB (post-string OK!)"); seen[6]=true; trace_phase++; } break;
      // entry alternativi da indagare
      case 0x0363: if (!seen[7]) { Serial.println("XX 0363 (U69)"); seen[7]=true; trace_phase++; } break;
      case 0x0378: if (!seen[8]) { Serial.println("XX 0378 (U79)"); seen[8]=true; trace_phase++; } break;
      case 0x0436: if (!seen[9]) { Serial.println("XX 0436 (BADHW D018)"); seen[9]=true; trace_phase++; } break;
      case 0x036A: if (!seen[10]) { Serial.println(">> 036A (LD HL,E520)"); seen[10]=true; trace_phase++; } break;
      case 0x036D: if (!seen[11]) { Serial.println(">> 036D (JR +51)"); seen[11]=true; trace_phase++; } break;
      case 0x03A2: if (!seen[12]) { Serial.println(">> 03A2 (CALL 03D2 PRINT)"); seen[12]=true; trace_phase++; } break;
    }
  }
  if (Addr < 0xC000) return pgm_read_byte(&arkanoid_rom_cpu[Addr]);
  return rdZ80(Addr);
}

unsigned char Arkanoid::rdZ80(unsigned short Addr) {
  // ROM 0x0000-0xBFFF (48KB)
  if (Addr < 0xC000)
    return pgm_read_byte(&arkanoid_rom_cpu[Addr]);

  // RAM lavoro 0xC000-0xC7FF
  if (Addr >= 0xC000 && Addr <= 0xC7FF)
    return memory[MEM_RAM_BASE + (Addr & 0x07FF)];

  // I/O 0xD000-0xD0xx
  switch (Addr) {
    case 0xD001: {                            // AY-3-8910 data read
      // port_a (reg 14) = "UNUSED", port_b (reg 15) = DSW.
      // 0xFF = continue OFF, screen normal, NO service mode, easy, std bonus, 3 lives, 1C/1C
      if (ay_addr == 0x0E) return 0xFF;     // port_a UNUSED
      if (ay_addr == 0x0F) return 0xFF;     // port_b DSW
      return soundregs[ay_addr & 0x0F];      // echo back register (audio renderer source)
    }
    case 0xD008: {                            // arkanoid_bootleg_d008_r per ARKANGC
      static int n=0;
      if (n<10) { Serial.printf("RD D008 -> 00 [%d]\n", n); n++; }
      return 0x00;
    }
    case 0xD00C:                              // SYSTEM port
      return read_input_port(Addr);
    case 0xD010:                              // BUTTONS port
      return read_input_port(Addr);
    case 0xD018: {                            // input_mux_r => paddle DIAL
      int16_t p = ec11_paddle_x;
      if (p < 0) p = 0; else if (p > 255) p = 255;
      static int n=0;
      if (n<10) { Serial.printf("RD D018 -> %02X [%d]\n", (unsigned char)p, n); n++; }
      return (unsigned char)p;
    }
    default: break;
  }

  // VRAM 0xE000-0xE7FF
  if (Addr >= 0xE000 && Addr <= 0xE7FF)
    return memory[MEM_VRAM_BASE + (Addr & 0x07FF)];

  // Sprite RAM 0xE800-0xE83F (64 byte)
  if (Addr >= 0xE800 && Addr <= 0xE83F)
    return memory[MEM_SPRRAM_BASE + (Addr & 0x3F)];

  // RAM extra 0xE840-0xEFFF
  if (Addr >= 0xE840 && Addr <= 0xEFFF)
    return memory[MEM_RAMX_BASE + ((Addr - 0xE840) & 0x07FF)];

  // F000-FFFF: per arkangc le letture F000 e F002 sono installate come
  // handler dedicati che ritornano 0x00 (m_bootleg_cmd dipendente, ma
  // per ARKANGC m_bootleg_cmd e' sempre 0 -> sempre 0x00).
  if (Addr == 0xF000 || Addr == 0xF002) {
    static int n=0;
    if (n<10) { Serial.printf("RD F00%X -> 00 [%d]\n", Addr & 0xF, n); n++; }
    return 0x00;
  }
  // resto dello spazio F000-FFFF: open bus (nopr fixes instant death final lvl)
  return 0xFF;
}

void Arkanoid::wrZ80(unsigned short Addr, unsigned char Value) {
  // RAM lavoro
  if (Addr >= 0xC000 && Addr <= 0xC7FF) {
    memory[MEM_RAM_BASE + (Addr & 0x07FF)] = Value; return;
  }
  // I/O
  if (Addr >= 0xD000 && Addr <= 0xD018) {
    write_d000_d018(Addr, Value); return;
  }
  // VRAM
  if (Addr >= 0xE000 && Addr <= 0xE7FF) {
    memory[MEM_VRAM_BASE + (Addr & 0x07FF)] = Value; return;
  }
  // Sprite RAM
  if (Addr >= 0xE800 && Addr <= 0xE83F) {
    memory[MEM_SPRRAM_BASE + (Addr & 0x3F)] = Value; return;
  }
  // RAM extra
  if (Addr >= 0xE840 && Addr <= 0xEFFF) {
    memory[MEM_RAMX_BASE + ((Addr - 0xE840) & 0x07FF)] = Value; return;
  }
}

unsigned char Arkanoid::read_input_port(unsigned short addr) {
  unsigned char b = input->buttons_get();
  unsigned char v = 0xFF;
  if (addr == 0xD00C) {
    // SYSTEM port (MAME arkangc bootleg input map):
    //   bit 0 = START1   (active LOW)
    //   bit 1 = START2   (active LOW)
    //   bit 2 = SERVICE  (active LOW)
    //   bit 3 = TILT     (active LOW)
    //   bit 4 = COIN1    (ACTIVE HIGH)
    //   bit 5 = COIN2    (ACTIVE HIGH)
    //   bit 6 = CUSTOM   (fixed = 1 per bootleg arkangc)
    //   bit 7 = CUSTOM   (fixed = 0 per bootleg arkangc)
    v &= ~0x10;                        // COIN1 default 0
    v &= ~0x20;                        // COIN2 default 0
    if (b & BUTTON_COIN)  v |=  0x10;  // 1 = inserito (active HIGH)
    if (b & BUTTON_START) v &= ~0x01;  // 0 = premuto (active LOW)
    v |=  0x40;                        // bit 6 fisso = 1 (bootleg)
    v &= ~0x80;                        // bit 7 fisso = 0 (bootleg)
    return v;
  }
  if (addr == 0xD010) {
    // BUTTONS port (verificato MAME PORT_START "BUTTONS"):
    //   bit 0 = BUTTON1 (FIRE/lancio palla, active LOW)
    //   bit 2 = BUTTON1 cocktail (active LOW)
    if (b & BUTTON_FIRE) v &= ~0x01;
    return v;
  }
  return 0xFF;
}

void Arkanoid::write_d000_d018(unsigned short addr, unsigned char val) {
  switch (addr) {
    case 0xD000: ay_addr = val & 0x0F; break;
    case 0xD001:
      // Reg 14/15 sono porte I/O (port_a/port_b = DSW): NON sovrascrivere,
      // altrimenti il read DSW restituirebbe spazzatura.
      if (ay_addr < 14) soundregs[ay_addr & 0x0F] = val;
      break;
    case 0xD008: {
      // arkanoid_d008_w (verificato MAME taito/arkanoid_v.cpp):
      //   bit 0 = flipX, bit 1 = flipY, bit 2 = paddle_select,
      //   bit 3 = coin lockout, bit 5 = gfx_bank, bit 6 = palette_bank,
      //   bit 7 = MCU reset (no-op per bootleg)
      gfx_bank     = (val >> 5) & 0x01;
      palette_bank = (val >> 6) & 0x01;
      paddle_latch = (val & 0x04) ? 1 : 0;     // riusa come paddle_select
      static int n=0;
      if (n<20) { Serial.printf("WR D008 = %02X [%d]\n", val, n); n++; }
      break;
    }
    case 0xD010: break;                     // watchdog
    case 0xD018: {
      static int n=0;
      if (n<10) { Serial.printf("WR D018 = %02X [%d]\n", val, n); n++; }
      break;
    }
  }
}

unsigned char Arkanoid::inZ80(unsigned short Port)                    { return 0xFF; }
void          Arkanoid::outZ80(unsigned short Port, unsigned char V)  { }

// ---------------------------------------------------------------------------
// Frame loop
// ---------------------------------------------------------------------------
void Arkanoid::prepare_frame(void) {
  paddle_latch = (unsigned char)(ec11_paddle_x & 0xFF);

  // Scansione sprite RAM (E800-E8FF, 64 sprite max)
  unsigned char *sram = memory + MEM_SPRRAM_BASE;
  active_sprites = 0;
  for (int i = 0; i < ARK_MAX_SPRITES; i++) {
    int offs = i * 4;
    unsigned char sx_b   = sram[offs + 0];
    unsigned char sy_b   = sram[offs + 1];
    unsigned char attr   = sram[offs + 2];
    unsigned char codelo = sram[offs + 3];

    // Sprite spento: tipicamente sy=0 o attr+code tutti 0
    if ((attr | codelo | sx_b | sy_b) == 0) continue;

    sprite_S &sp = sprite[active_sprites];
    sp.x      = (short)sx_b;
    sp.y      = (short)(248 - (int)sy_b);
    sp.code   = ((unsigned short)codelo) | (((unsigned short)(attr & 0x03)) << 8)
              | (((unsigned short)gfx_bank) << 10);
    sp.color  = (attr >> 3) & 0x1F;     // 5-bit color
    sp.flags  = attr;                    // riserva bit 2 = future flip
    sp.color_block = 0; sp.is_32x32 = 0;
    sp.flip_x = 0; sp.flip_y = 0;
    active_sprites++;
    if (active_sprites >= 32) break;     // budget render per riga
  }
}

void Arkanoid::run_frame(void) {
  // Pattern Galagino: INST_PER_FRAME loop con 4 StepZ80 per iterazione.
  current_cpu = 0;
  for (int i = 0; i < INST_PER_FRAME; i++) {
    StepZ80(cpu); StepZ80(cpu); StepZ80(cpu); StepZ80(cpu);
  }
  IntZ80(cpu, INT_RST38);                // VBLANK IRQ (mode 1 RST 38h)
  if (!game_started) game_started = 1;
}

// ---------------------------------------------------------------------------
// Renderer (pipeline Galagino: chiama render_row(r) per r=0..ARK_ROWS-1)
// ---------------------------------------------------------------------------
void Arkanoid::render_row(short row) {
  for (char col = 0; col < ARK_COLS; col++) {
    blit_tile(row, col);
  }
  for (unsigned char s = 0; s < active_sprites; s++) {
    blit_sprite(row, s);
  }
}

// 8x8 tile, 3bpp (4 bit per pixel packed in long), opaco
// Skip blanking arkanoid: vbstart=16 -> render parte da tile row 2
#define ARK_VBLANK_TILE_OFFSET 2

void Arkanoid::blit_tile(short row, char col) {
  unsigned int offs = (((row + ARK_VBLANK_TILE_OFFSET) * 32) + col) * 2;
  unsigned char b0 = memory[MEM_VRAM_BASE + offs + 0];
  unsigned char b1 = memory[MEM_VRAM_BASE + offs + 1];
  unsigned int code  = (unsigned int)b1
                     | ((unsigned int)(b0 & 0x07) << 8)
                     | ((unsigned int)gfx_bank << 11);
  unsigned int color = ((b0 & 0xF8) >> 3) | (palette_bank << 5);
  if (code >= ARKANOID_NUM_TILES) code &= (ARKANOID_NUM_TILES - 1);

  // base palette: ogni "color" indicizza 8 colori consecutivi (3bpp)
  const unsigned short *colors = &arkanoid_palette[(color & 0x3F) * 8];

  // Frame buffer: 256 px wide, 1 short per pixel
  unsigned short *ptr = frame_buffer + (col * 8);

  for (char r = 0; r < 8; r++, ptr += (ARK_SCREEN_W - 8)) {
    unsigned long pix_row = pgm_read_dword(&arkanoid_tilemap[code][r]);
    for (char c = 0; c < 8; c++, pix_row >>= 4) {
      unsigned char pen = pix_row & 0x07;
      *ptr++ = pgm_read_word(&colors[pen]);
    }
  }
}

// 8x16 sprite = 2 char 8x8 STACKED VERTICALMENTE (top = 2*code, bottom = 2*code+1)
// Riferimento MAME draw_sprites: (sx, sy-8) top, (sx, sy) bottom.
// In prepare_frame ho gia' calcolato sp.y = 248 - byte; il TOP del sprite
// e' (sp.y - 8). Cioe' sprite occupa righe [sp.y - 8, sp.y + 8).
// Trasparenza: pen 0
void Arkanoid::blit_sprite(short strip_row, unsigned char s) {
  const sprite_S &sp = sprite[s];
  short sx = sp.x;
  // Compensa blanking: sprite Y in MAME parte da y=16 (visible),
  // sul mio rendering devo togliere 16 (= ARK_VBLANK_TILE_OFFSET*8)
  short sy_top = sp.y - 8 - (ARK_VBLANK_TILE_OFFSET * 8);
  if (sx <= -8 || sx >= ARK_SCREEN_W) return;

  short strip_y0 = strip_row * 8;
  if (sy_top + 16 <= strip_y0) return;
  if (sy_top      >= strip_y0 + 8) return;

  unsigned int spcode  = sp.code;
  unsigned int colbase = (sp.color & 0x1F) | (palette_bank << 5);
  const unsigned short *colors = &arkanoid_palette[(colbase & 0x3F) * 8];

  unsigned int char_top = (spcode * 2)     & (ARKANOID_NUM_TILES - 1);
  unsigned int char_bot = (spcode * 2 + 1) & (ARKANOID_NUM_TILES - 1);

  // Clip orizzontale
  short cx_start = (sx < 0) ? -sx : 0;
  short cx_end   = 8;
  if (sx + 8 > ARK_SCREEN_W) cx_end = ARK_SCREEN_W - sx;
  if (cx_start >= cx_end) return;

  // Per ogni riga della strip che interseca lo sprite
  for (short y_in_strip = 0; y_in_strip < 8; y_in_strip++) {
    short y_screen = strip_y0 + y_in_strip;
    if (y_screen < sy_top || y_screen >= sy_top + 16) continue;

    short row_in_sprite = y_screen - sy_top;   // 0..15
    unsigned int char_idx;
    short char_row;
    if (row_in_sprite < 8) {
      char_idx = char_top;
      char_row = row_in_sprite;
    } else {
      char_idx = char_bot;
      char_row = row_in_sprite - 8;
    }

    unsigned long pix = pgm_read_dword(&arkanoid_tilemap[char_idx][char_row]);
    pix >>= (cx_start * 4);
    unsigned short *base = frame_buffer + y_in_strip * ARK_SCREEN_W + sx;
    for (short c = cx_start; c < cx_end; c++, pix >>= 4) {
      unsigned char pen = pix & 0x07;
      if (pen) base[c] = pgm_read_word(&colors[pen]);
    }
  }
}

#endif // ENABLE_ARKANOID
