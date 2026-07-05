// ============================================================================
// machines/gigas/gigas.cpp — Gigas (Sega/Nihon System 1986, set bootleg gigasb)
//
// Z80 single + tile/sprite renderer. Pattern preso da Arkanoid 1.
// ============================================================================
#include "gigas.h"

#ifdef ENABLE_GIGAS
#include "gigas_logo.h"
#include "gigas_rom_cpu.h"
#include "gigas_palette.h"
#include "gigas_gfx_tile_p0.h"
#include "gigas_gfx_tile_p1.h"
#include "gigas_gfx_tile_p2.h"
#include "gigas_gfx_spr_p0.h"
#include "gigas_gfx_spr_p1.h"
#include "gigas_gfx_spr_p2.h"

// ec11 spinner
extern volatile int16_t ec11_paddle_x;
// Dial free-running counter (wraps mod 256) — Gigas legge questo come dial
// relativo invece del paddle clamped 0..255.
extern volatile uint8_t ec11_dial_counter;

// Memory layout interno (RAMSIZE=16384)
//  memory[0x0000-0x0FFF] = main RAM (4 KB, $C000-$CFFF)
//  memory[0x1000-0x17FF] = VRAM (2 KB, $D000-$D7FF) [0..3FF=code, 400..7FF=attr]
//  memory[0x1800-0x18FF] = sprite RAM (256 B, $D800-$D8FF)
//  memory[0x1900-0x1FFF] = extra RAM ($D900-$DFFF, 1.75 KB)
#define MEM_RAM_BASE     0x0000
#define MEM_VRAM_BASE    0x1000
#define MEM_SPRRAM_BASE  0x1800
#define MEM_RAM_HI_BASE  0x1900

#define GIGAS_NUM_TILES    2048    // 16KB ROM / 8 byte per tile per plane
#define GIGAS_NUM_SPRITES  512     // 16KB ROM / 32 byte per tile per plane

Gigas::Gigas() {
  spinner_sel  = 0;
  nmi_enable   = 0;
  flip_screen  = 0;
  boot_button1_mask_frames = 0;
  for (int i = 0; i < 4; i++) sn_latched[i] = 0;
}

// ============================================================================
// SN76489A byte-stream parser (data sheet PSG):
//   byte con bit 7 = 1 → LATCH:
//     bits 6,5 = canale (0..3, c=3 = noise)
//     bit 4    = tipo (0 = period, 1 = volume)
//     bits 3-0 = nibble basso (period bassi 4 bit, oppure volume 4 bit)
//   byte con bit 7 = 0 → DATA continuation: bits 5-0 = period bassi 6 bit
//                       (per il registro period precedentemente latched)
//   Volume: 0=loud, 15=mute. Period: 10-bit (0=tono molto alto). Noise:
//     bit 2 di period = mode (0 periodic, 1 white), bit 0,1 = shift rate.
// ============================================================================
void Gigas::sn76489_write(int chip, unsigned char data) {
  if (data & 0x80) {
    // LATCH byte: register select + low 4 bits
    unsigned char reg = (data >> 4) & 0x07;       // bits 6,5,4
    sn_latched[chip] = reg;
    unsigned char low = data & 0x0F;
    int channel = (reg >> 1) & 0x03;
    if (reg & 0x01) {
      // Volume register
      sn_volume[chip][channel] = low;
    } else {
      // Period register (low 4 bit)
      int per = sn_period[chip][channel];
      sn_period[chip][channel] = (per & 0x3F0) | low;
    }
  } else {
    // DATA continuation: high 6 bit of last latched period reg
    unsigned char reg = sn_latched[chip];
    int channel = (reg >> 1) & 0x03;
    if (reg & 0x01) {
      // Latched register e' volume → continuation rilancia il volume nibble
      sn_volume[chip][channel] = data & 0x0F;
    } else {
      // Period register: combina low 4 bit precedenti + nuovi bit alti
      int per = sn_period[chip][channel];
      sn_period[chip][channel] = (per & 0x0F) | ((data & 0x3F) << 4);
    }
  }
}

const unsigned short *Gigas::logo(void) {
  return gigas_logo;
}

void Gigas::init(Input *in, unsigned short *fb, sprite_S *sb, unsigned char *mem) {
  machineBase::init(in, fb, sb, mem);
}

void Gigas::reset() {
  machineBase::reset();
  spinner_sel  = 0;
  nmi_enable   = 0;
  flip_screen  = 0;
  // Maschera BUTTON1 (FIRE in game) per i primi 90 frame (~1.5 sec a 60 fps).
  // Durante questa finestra il game completa il boot init e legge IN0 per
  // decidere se entrare in service test mode (= se BUTTON1 letto pressed,
  // entra). Dopo la finestra, FIRE ridiventa pulsante FIRE in game.
  boot_button1_mask_frames = 90;
  for (int i = 0; i < 4; i++) sn_latched[i] = 0;
  // CRITICO: ResetZ80 setta SP=0xF000 di default. In Gigas 0xF000 è IO
  // address (DSW1), NON RAM → PUSH del programma vanno in IO ignorato →
  // POP successivo legge spazzatura → CPU crash.
  // Setto SP a 0xCFFF = top di main RAM (0xC000-0xCFFF).
  cpu[0].SP.W = 0xCFFF;
}

// ============================================================================
// Z80 memory map
// ============================================================================
// Opcode fetch (M1 cycle) — separato da data fetch.
// MAME init_gigasb (freekick.cpp:1892):
//   bank0d  = "maincpu" + 0xc000  → opcode 0x0000-0x7FFF da ROM[0xC000-0x13FFF]
//   opbank  = "maincpu" + 0x14000 → opcode 0x8000-0xBFFF da ROM[0x14000-0x17FFF]
// In totale: opcode addr Z → ROM[Z + 0xC000] per Z in [0..0xBFFF]
// Reset vector PC=0 fetcha ROM[0xC000] = 0xAF (XOR A) — corretto reset code.
unsigned char Gigas::opZ80(unsigned short Addr) {
  if (Addr < 0xC000) return pgm_read_byte(&gigas_rom_cpu[Addr + 0xC000]);
  // sopra 0xC000 = RAM/IO → leggi da rdZ80
  return rdZ80(Addr);
}

unsigned char Gigas::rdZ80(unsigned short Addr) {
  // ROM 0x0000-0xBFFF: data fetch dal "bank0d/opbank data" = ROM region offset 0..0xBFFF
  // (decrypted_opcodes_map è solo per opcode fetch; data va in program_map)
  if (Addr < 0xC000) return pgm_read_byte(&gigas_rom_cpu[Addr]);
  // RAM general 0xC000-0xCFFF
  if (Addr < 0xD000) return memory[MEM_RAM_BASE + (Addr - 0xC000)];
  // VRAM 0xD000-0xD7FF
  if (Addr < 0xD800) return memory[MEM_VRAM_BASE + (Addr - 0xD000)];
  // sprite RAM 0xD800-0xD8FF
  if (Addr < 0xD900) return memory[MEM_SPRRAM_BASE + (Addr - 0xD800)];
  // RAM extra 0xD900-0xDFFF
  if (Addr < 0xE000) return memory[MEM_RAM_HI_BASE + (Addr - 0xD900)];
  // IN0 read 0xE000 — pbillrd/gigas IN0 mapping (ACTIVE LOW):
  //   bit 0 = BUTTON1, bit 6 = START1, bit 7 = COIN1
  // Mapping (richiesta utente 2026-05-09):
  //   physical FIRE (EC11 SW) → BUTTON1 (FIRE in game) + START1 (start game)
  //   physical START → START1 (alternativa per avviare partita)
  //   physical COIN → COIN1 (inserisci credito)
  // BOOT MASK: BUTTON1 e' soppresso per i primi N frame dopo reset per evitare
  // che il game legga BUTTON1=pressed durante init e finisca in service test.
  // Dopo la finestra, FIRE funziona regolare come fire button in game.
  if (Addr == 0xE000) {
    unsigned char b = input ? input->buttons_get() : 0;
    unsigned char v = 0xFF;
    if (b & BUTTON_FIRE) {
      if (boot_button1_mask_frames == 0) v &= ~0x01;  // BUTTON1 (FIRE in game)
      v &= ~0x40;                                     // START1
    }
    if (b & BUTTON_START) v &= ~0x40;     // START1 alternativo
    if (b & BUTTON_COIN)  v &= ~0x80;     // COIN1
    return v;
  }
  // IN1 read 0xE800 (cocktail unused)
  if (Addr == 0xE800) return 0xFF;
  // DSW1 read 0xF000 — factory default MAME pbillrd/gigas = 0xFF.
  // bit 0=1 (3 vite), bit 1,2=1,1 (bonus 20k+60k+ogni 60k), bit 3,4=1,1 (Easy),
  // bit 5=1 (Continue Yes), bit 6=1 (Upright!), bit 7=1 (Flip OFF!).
  // ATTENZIONE: il valore precedente 0x01 dava cocktail=ON + flip=ON
  // simultaneamente (bit 6=0 + bit 7=0) = schermo cocktail ruotato che
  // appariva come "schermata test" stuck.
  if (Addr == 0xF000) return 0xFF;
  // DSW2 read 0xF800 — factory default = 0xFF (Coin A 1c1c, Coin B 1c1c)
  if (Addr == 0xF800) return 0xFF;
  return 0xFF;
}

void Gigas::wrZ80(unsigned short Addr, unsigned char Value) {
  // ROM 0x0000-0xBFFF: ignora
  if (Addr < 0xC000) return;
  if (Addr < 0xD000) { memory[MEM_RAM_BASE + (Addr - 0xC000)] = Value;     return; }
  if (Addr < 0xD800) { memory[MEM_VRAM_BASE + (Addr - 0xD000)] = Value;    return; }
  if (Addr < 0xD900) { memory[MEM_SPRRAM_BASE + (Addr - 0xD800)] = Value;  return; }
  if (Addr < 0xE000) { memory[MEM_RAM_HI_BASE + (Addr - 0xD900)] = Value;  return; }
  // 0xE000-0xE007: outlatch LS259 (bit 0 di Value → bit (Addr&7) del latch)
  if (Addr >= 0xE000 && Addr < 0xE008) {
    unsigned char bit = Addr & 0x07;
    unsigned char d0 = Value & 1;
    if      (bit == 0) flip_screen = !d0;     // bit 0 inverted
    else if (bit == 4) nmi_enable  = d0;
    return;
  }
  // 0xF000 = bankswitch? (MAME nopw, forse non implementato)
  if (Addr == 0xF000) return;
  // 0xFC00-0xFC03 = 4 × SN76489A write (mono mixed in real HW).
  // Ogni address pilota un chip diverso (sn1..sn4 in MAME).
  if (Addr >= 0xFC00 && Addr <= 0xFC03) {
    int chip = Addr & 0x03;
    soundregs[chip] = Value;       // legacy stub (compat)
    sn76489_write(chip, Value);    // parser proper → aggiorna sn_period/sn_volume
    return;
  }
}

unsigned char Gigas::inZ80(unsigned short Port) {
  Port &= 0xFF;
  if (Port == 0x00) {
    // Spinner read 1P: 8-bit dial counter free-running (wraps mod 256).
    // Usiamo ec11_dial_counter (NON ec11_paddle_x): paddle_x e' clamped
    // 0..255 per Arkanoid e quando arriva ai limiti il delta letto dal game
    // diventa 0 → racchetta non fa la corsa completa. Il dial counter wrappa
    // liberamente, quindi il game leggendo due volte vede sempre delta corretti.
    // INVERTITO (MAME PORT_REVERSE) per allineare verso rotazione → racchetta.
    if (spinner_sel == 0) return (unsigned char)(-(int8_t)ec11_dial_counter);
    return 0xFF;   // cocktail spinner 2P: open bus high (non collegato)
  }
  if (Port == 0x01) return 0xFF;  // DSW3 unused: tutti DIP off = idle
  return 0xFF;
}

void Gigas::outZ80(unsigned short Port, unsigned char Value) {
  Port &= 0xFF;
  if (Port == 0x00) { spinner_sel = Value & 1; return; }
}

// ============================================================================
// Frame loop
// ============================================================================
void Gigas::run_frame() {
  current_cpu = 0;
  // Decrementa boot mask BUTTON1 (1.5s dopo il reset → FIRE puo' fungere da fire)
  if (boot_button1_mask_frames > 0) boot_button1_mask_frames--;

  // NB: nessuna strumentazione di debug / Serial qui. Su questa board (P4 USB-CDC)
  // il Serial non e' letto: una raffica di Serial.printf per-frame riempiva il
  // buffer TX e la write si bloccava -> starve dei task -> FREEZE durante il
  // gameplay in attract. Identico schema (snello) di Gigas2, che non si pianta.
  const int HALF = 2500;
  for (int i = 0; i < HALF; i++) {
    StepZ80(cpu); StepZ80(cpu); StepZ80(cpu); StepZ80(cpu);
  }
  IntZ80(cpu, INT_RST38);
  for (int i = 0; i < HALF; i++) {
    StepZ80(cpu); StepZ80(cpu); StepZ80(cpu); StepZ80(cpu);
  }
  IntZ80(cpu, INT_RST38);
  if (nmi_enable) IntZ80(cpu, INT_NMI);
  if (!game_started) game_started = 1;
}

void Gigas::prepare_frame() {
  // Snapshot sprite RAM (256 byte = 64 sprite × 4 byte) per MAME draw_sprites.
  // Decodifica byte (verificato MAME freekick.cpp gigas_state::draw_sprites):
  //   byte 0 = code low (8 bit)
  //   byte 1 bit 5 = code hi (1 bit) → code totale 9-bit (0..511)
  //   byte 1 bits 4-0 = color (5 bit, 32 entry palette)
  //   byte 2 = ypos raw → MAME plotta a Y = 240 - byte2 (sempre)
  //   byte 3 = xpos raw
  // Niente flipx/flipy in HW, niente sprite 32x32 multi-tile (sono 16x16
  // piazzati adiacenti dal game).
  unsigned char *sram = memory + MEM_SPRRAM_BASE;
  active_sprites = 0;
  for (int i = 0; i < 64; i++) {           // ITERA TUTTI I 64 SLOT (era 32)
    int offs = i * 4;
    unsigned char b0 = sram[offs + 0];
    unsigned char b1 = sram[offs + 1];
    unsigned char b2 = sram[offs + 2];
    unsigned char b3 = sram[offs + 3];

    // Skip solo se TUTTI 4 byte sono zero (sprite vuoto): conservativo
    if ((b0 | b1 | b2 | b3) == 0) continue;

    sprite_S &sp = sprite[active_sprites];
    // Per ROT270 (Gigas portrait) + 180° SW flip in render_row, gli assi del
    // buffer mappano cosi' sull'output user view portrait:
    //   buffer X (sp.x = b3) → display Y user view (verticale, su/giu')
    //   buffer Y (sp.y = 240-b2) → display X user view (orizzontale, sx/dx)
    // FIX (2026-05-09): -16 su sp.y compensa il clamp interno del game ROM
    // Gigas che limita il paddle a un sub-range del playfield, rendendo
    // visivamente la racchetta corta sul muro sx. Verificato funzionante.
    sp.x        = (short)b3;
    sp.y        = (short)(240 - b2 - 16);
    sp.code     = ((unsigned short)b0) | (((unsigned short)(b1 & 0x20)) << 3);
    sp.color    = b1 & 0x1F;
    sp.flags    = 0;
    sp.color_block = 0;
    sp.is_32x32 = 0;
    sp.flip_x   = 0;
    sp.flip_y   = 0;
    active_sprites++;
    // 64 = MAME max (era erroneamente 32 → drago/paddle in 2 pezzi)
    if (active_sprites >= 64) break;
  }
}

void Gigas::render_row(short row) {
  // Flip 180° SOFTWARE: rendero la strip "speculare" e poi inverto il buffer.
  // strip mostrata in alto = strip originale dal basso.
  short eff_row = (GIGAS_ROWS - 1) - row;

  for (char col = 0; col < GIGAS_COLS; col++) {
    blit_tile(eff_row, col);
  }
  for (unsigned char s = 0; s < active_sprites; s++) {
    blit_sprite(eff_row, s);
  }

  // Reverse 8 righe × 256 col del frame_buffer:
  //   - swap righe (riga 0 ↔ 7, 1 ↔ 6, 2 ↔ 5, 3 ↔ 4)
  //   - inverto colonne in ogni riga (col 0 ↔ 255)
  for (int r0 = 0; r0 < 4; r0++) {
    int r1 = 7 - r0;
    unsigned short *p0 = frame_buffer + r0 * GIGAS_SCREEN_W;
    unsigned short *p1 = frame_buffer + r1 * GIGAS_SCREEN_W;
    for (int c = 0; c < GIGAS_SCREEN_W / 2; c++) {
      unsigned short a = p0[c];
      unsigned short b = p0[GIGAS_SCREEN_W - 1 - c];
      unsigned short cc = p1[c];
      unsigned short d  = p1[GIGAS_SCREEN_W - 1 - c];
      p0[c] = d;
      p0[GIGAS_SCREEN_W - 1 - c] = cc;
      p1[c] = b;
      p1[GIGAS_SCREEN_W - 1 - c] = a;
    }
  }
}

// ============================================================================
// Tile blit: 8x8 px, 3 bit-plane, 8 colori per tile
// VRAM mapping:
//   tile_index = (vram_row * 32) + col   con vram_row = render_row + offset(2)
//   code = vram[tile_index] | ((vram[tile_index + 0x400] & 0xE0) << 3)
//   color = vram[tile_index + 0x400] & 0x1F
// Ogni "color" indica un blocco di 8 colori palette consecutivi (3-bit pixel).
// Tile palette base = 0x000 (gfx_freekick GFXDECODE_ENTRY tile color base).
// ============================================================================
void Gigas::blit_tile(short row, char col) {
  unsigned int vram_row = (unsigned)(row + GIGAS_VRAM_OFFSET_ROWS);
  unsigned int tile_index = vram_row * 32 + (unsigned)col;
  unsigned char code_lo = memory[MEM_VRAM_BASE + tile_index];
  unsigned char attr    = memory[MEM_VRAM_BASE + 0x400 + tile_index];
  unsigned int code = (unsigned)code_lo | (((unsigned)(attr & 0xE0)) << 3);
  unsigned int color = attr & 0x1F;
  if (code >= GIGAS_NUM_TILES) code &= (GIGAS_NUM_TILES - 1);

  // Tile palette base 0x000, 8 colori per "color" entry (3-bit pixel)
  const unsigned short *pal = &gigas_palette[(color & 0x1F) * 8];

  unsigned short *ptr = frame_buffer + (col * 8);
  // 8 righe del tile
  for (int r = 0; r < 8; r++, ptr += GIGAS_SCREEN_W - 8) {
    unsigned int byte_off = (unsigned)code * 8 + r;
    unsigned char p0 = pgm_read_byte(&gigas_gfx_tile_p0[byte_off]);
    unsigned char p1 = pgm_read_byte(&gigas_gfx_tile_p1[byte_off]);
    unsigned char p2 = pgm_read_byte(&gigas_gfx_tile_p2[byte_off]);
    for (int x = 0; x < 8; x++, ptr++) {
      int bit_pos = 7 - x;
      // Plane order per MAME freekick.cpp gfx_8x8x3_planar STANDARD:
      // {RGN_FRAC(0,3), RGN_FRAC(1,3), RGN_FRAC(2,3)} → plane[0]=LSB, plane[2]=MSB.
      // Romconv outputs file-order: p0 = primo file = plane[0] = LSB, p2 = MSB.
      // FIX (2026-05-09): in precedenza p0 era trattato come MSB (sbagliato) →
      // colori scambiati (bit 0 e bit 2 invertiti) → BG garbled.
      unsigned char pix = ((p2 >> bit_pos) & 1) << 2   // MSB ← p2 (terzo file)
                        | ((p1 >> bit_pos) & 1) << 1   // MID ← p1
                        | ((p0 >> bit_pos) & 1);      // LSB ← p0 (primo file)
      *ptr = pal[pix];
    }
  }
}

// ============================================================================
// Sprite blit: 16x16 px, 3 bit-plane custom layout, palette base 0x100
// Plane order (RGN_FRAC 0/3, 2/3, 1/3):
//   plane[0] = MSB = ROM g-1 (offset 0)
//   plane[1] =       ROM g-2 (offset 0x8000 in MAME, ma noi abbiamo file separati)
//   plane[2] = LSB = ROM g-3
// byte_offset(x, y) = y + (x >= 8 ? 16 : 0)
// bit_pos = 7 - (x & 7)
// ============================================================================
void Gigas::blit_sprite(short strip_row, unsigned char s) {
  sprite_S &sp = sprite[s];
  short top = strip_row * 8;          // top y dello strip corrente
  short sx = sp.x;
  short sy = sp.y;
  if (sy + 16 <= top || sy >= top + 8) return;   // sprite non in questo strip
  if (sx >= GIGAS_SCREEN_W || sx + 16 <= 0) return;

  unsigned int code = sp.code & (GIGAS_NUM_SPRITES - 1);
  unsigned int base = code * 32;     // 32 byte per tile per plane

  // Palette sprite: base 0x100, 8 colori per "color" (3-bit pixel)
  const unsigned short *pal = &gigas_palette[0x100 + (sp.color & 0x1F) * 8];

  for (int dy = 0; dy < 16; dy++) {
    int y_screen = sy + dy;
    int strip_y  = y_screen - top;
    if (strip_y < 0 || strip_y >= 8) continue;
    unsigned short *ptr = frame_buffer + strip_y * GIGAS_SCREEN_W;
    for (int dx = 0; dx < 16; dx++) {
      int x_screen = sx + dx;
      if (x_screen < 0 || x_screen >= GIGAS_SCREEN_W) continue;
      int byte_off = base + dy + (dx >= 8 ? 16 : 0);
      int bit_pos  = 7 - (dx & 7);
      unsigned char p0 = pgm_read_byte(&gigas_gfx_spr_p0[byte_off]);   // MSB
      unsigned char p1 = pgm_read_byte(&gigas_gfx_spr_p1[byte_off]);
      unsigned char p2 = pgm_read_byte(&gigas_gfx_spr_p2[byte_off]);   // LSB
      unsigned char pix = ((p0 >> bit_pos) & 1) << 2
                        | ((p1 >> bit_pos) & 1) << 1
                        | ((p2 >> bit_pos) & 1);
      if (pix == 0) continue;        // pixel 0 = trasparente per sprite
      ptr[x_screen] = pal[pix];
    }
  }
}

#endif // ENABLE_GIGAS
