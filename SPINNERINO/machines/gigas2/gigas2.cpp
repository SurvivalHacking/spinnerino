// ============================================================================
// machines/gigas2/gigas2.cpp — Gigas Mark II (Sega/Nihon System 1986, gigasm2b)
//
// Clone della classe Gigas (machines/gigas/gigas.cpp). Hardware identico: la
// sola differenza e' la composizione del maincpu ROM region (3 file 32KB
// invece di 2). Layout DATA/OPCODES nello stesso schema dual-mapping.
// ============================================================================
#include "gigas2.h"

#ifdef ENABLE_GIGAS2
#include "gigas2_logo.h"
#include "gigas2_rom_cpu.h"
#include "gigas2_palette.h"
#include "gigas2_gfx_tile_p0.h"
#include "gigas2_gfx_tile_p1.h"
#include "gigas2_gfx_tile_p2.h"
#include "gigas2_gfx_spr_p0.h"
#include "gigas2_gfx_spr_p1.h"
#include "gigas2_gfx_spr_p2.h"

extern volatile int16_t ec11_paddle_x;
extern volatile uint8_t ec11_dial_counter;

#define MEM_RAM_BASE     0x0000
#define MEM_VRAM_BASE    0x1000
#define MEM_SPRRAM_BASE  0x1800
#define MEM_RAM_HI_BASE  0x1900

#define GIGAS2_NUM_TILES    2048
#define GIGAS2_NUM_SPRITES  512

Gigas2::Gigas2() {
  spinner_sel  = 0;
  nmi_enable   = 0;
  flip_screen  = 0;
  boot_button1_mask_frames = 0;
  for (int i = 0; i < 4; i++) sn_latched[i] = 0;
}

void Gigas2::sn76489_write(int chip, unsigned char data) {
  if (data & 0x80) {
    unsigned char reg = (data >> 4) & 0x07;
    sn_latched[chip] = reg;
    unsigned char low = data & 0x0F;
    int channel = (reg >> 1) & 0x03;
    if (reg & 0x01) {
      sn_volume[chip][channel] = low;
    } else {
      int per = sn_period[chip][channel];
      sn_period[chip][channel] = (per & 0x3F0) | low;
    }
  } else {
    unsigned char reg = sn_latched[chip];
    int channel = (reg >> 1) & 0x03;
    if (reg & 0x01) {
      sn_volume[chip][channel] = data & 0x0F;
    } else {
      int per = sn_period[chip][channel];
      sn_period[chip][channel] = (per & 0x0F) | ((data & 0x3F) << 4);
    }
  }
}

const unsigned short *Gigas2::logo(void) {
  return gigas2_logo;
}

void Gigas2::init(Input *in, unsigned short *fb, sprite_S *sb, unsigned char *mem) {
  machineBase::init(in, fb, sb, mem);
}

void Gigas2::reset() {
  machineBase::reset();
  spinner_sel  = 0;
  nmi_enable   = 0;
  flip_screen  = 0;
  boot_button1_mask_frames = 90;
  for (int i = 0; i < 4; i++) sn_latched[i] = 0;
  cpu[0].SP.W = 0xCFFF;
  Serial.println(F("[GIGAS2] reset done (SP=0xCFFF, BUTTON1 mask=90f)"));
}

unsigned char Gigas2::opZ80(unsigned short Addr) {
  if (Addr < 0xC000) return pgm_read_byte(&gigas2_rom_cpu[Addr + 0xC000]);
  return rdZ80(Addr);
}

unsigned char Gigas2::rdZ80(unsigned short Addr) {
  if (Addr < 0xC000) return pgm_read_byte(&gigas2_rom_cpu[Addr]);
  if (Addr < 0xD000) return memory[MEM_RAM_BASE + (Addr - 0xC000)];
  if (Addr < 0xD800) return memory[MEM_VRAM_BASE + (Addr - 0xD000)];
  if (Addr < 0xD900) return memory[MEM_SPRRAM_BASE + (Addr - 0xD800)];
  if (Addr < 0xE000) return memory[MEM_RAM_HI_BASE + (Addr - 0xD900)];
  if (Addr == 0xE000) {
    unsigned char b = input ? input->buttons_get() : 0;
    unsigned char v = 0xFF;
    if (b & BUTTON_FIRE) {
      if (boot_button1_mask_frames == 0) v &= ~0x01;
      v &= ~0x40;
    }
    if (b & BUTTON_START) v &= ~0x40;
    if (b & BUTTON_COIN)  v &= ~0x80;
    return v;
  }
  if (Addr == 0xE800) return 0xFF;
  if (Addr == 0xF000) return 0xFF;
  if (Addr == 0xF800) return 0xFF;
  return 0xFF;
}

void Gigas2::wrZ80(unsigned short Addr, unsigned char Value) {
  if (Addr < 0xC000) return;
  if (Addr < 0xD000) { memory[MEM_RAM_BASE + (Addr - 0xC000)] = Value;     return; }
  if (Addr < 0xD800) { memory[MEM_VRAM_BASE + (Addr - 0xD000)] = Value;    return; }
  if (Addr < 0xD900) { memory[MEM_SPRRAM_BASE + (Addr - 0xD800)] = Value;  return; }
  if (Addr < 0xE000) { memory[MEM_RAM_HI_BASE + (Addr - 0xD900)] = Value;  return; }
  if (Addr >= 0xE000 && Addr < 0xE008) {
    unsigned char bit = Addr & 0x07;
    unsigned char d0 = Value & 1;
    if      (bit == 0) flip_screen = !d0;
    else if (bit == 4) nmi_enable  = d0;
    return;
  }
  if (Addr == 0xF000) return;
  if (Addr >= 0xFC00 && Addr <= 0xFC03) {
    int chip = Addr & 0x03;
    soundregs[chip] = Value;
    sn76489_write(chip, Value);
    return;
  }
}

unsigned char Gigas2::inZ80(unsigned short Port) {
  Port &= 0xFF;
  if (Port == 0x00) {
    if (spinner_sel == 0) return (unsigned char)(-(int8_t)ec11_dial_counter);
    return 0xFF;
  }
  if (Port == 0x01) return 0xFF;
  return 0xFF;
}

void Gigas2::outZ80(unsigned short Port, unsigned char Value) {
  Port &= 0xFF;
  if (Port == 0x00) { spinner_sel = Value & 1; return; }
}

// ============================================================================
// Frame loop: stesso schema di Gigas (2 RST38 per frame + NMI vblank gated).
// ============================================================================
void Gigas2::run_frame() {
  current_cpu = 0;
  if (boot_button1_mask_frames > 0) boot_button1_mask_frames--;

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

void Gigas2::prepare_frame() {
  unsigned char *sram = memory + MEM_SPRRAM_BASE;
  active_sprites = 0;
  for (int i = 0; i < 64; i++) {
    int offs = i * 4;
    unsigned char b0 = sram[offs + 0];
    unsigned char b1 = sram[offs + 1];
    unsigned char b2 = sram[offs + 2];
    unsigned char b3 = sram[offs + 3];
    if ((b0 | b1 | b2 | b3) == 0) continue;

    sprite_S &sp = sprite[active_sprites];
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
    if (active_sprites >= 64) break;
  }
}

void Gigas2::render_row(short row) {
  short eff_row = (GIGAS2_ROWS - 1) - row;

  for (char col = 0; col < GIGAS2_COLS; col++) {
    blit_tile(eff_row, col);
  }
  for (unsigned char s = 0; s < active_sprites; s++) {
    blit_sprite(eff_row, s);
  }

  // Flip 180° SW: reverse 8 righe x 256 col del frame_buffer
  for (int r0 = 0; r0 < 4; r0++) {
    int r1 = 7 - r0;
    unsigned short *p0 = frame_buffer + r0 * GIGAS2_SCREEN_W;
    unsigned short *p1 = frame_buffer + r1 * GIGAS2_SCREEN_W;
    for (int c = 0; c < GIGAS2_SCREEN_W / 2; c++) {
      unsigned short a = p0[c];
      unsigned short b = p0[GIGAS2_SCREEN_W - 1 - c];
      unsigned short cc = p1[c];
      unsigned short d  = p1[GIGAS2_SCREEN_W - 1 - c];
      p0[c] = d;
      p0[GIGAS2_SCREEN_W - 1 - c] = cc;
      p1[c] = b;
      p1[GIGAS2_SCREEN_W - 1 - c] = a;
    }
  }
}

void Gigas2::blit_tile(short row, char col) {
  unsigned int vram_row = (unsigned)(row + GIGAS2_VRAM_OFFSET_ROWS);
  unsigned int tile_index = vram_row * 32 + (unsigned)col;
  unsigned char code_lo = memory[MEM_VRAM_BASE + tile_index];
  unsigned char attr    = memory[MEM_VRAM_BASE + 0x400 + tile_index];
  unsigned int code = (unsigned)code_lo | (((unsigned)(attr & 0xE0)) << 3);
  unsigned int color = attr & 0x1F;
  if (code >= GIGAS2_NUM_TILES) code &= (GIGAS2_NUM_TILES - 1);

  const unsigned short *pal = &gigas2_palette[(color & 0x1F) * 8];

  unsigned short *ptr = frame_buffer + (col * 8);
  for (int r = 0; r < 8; r++, ptr += GIGAS2_SCREEN_W - 8) {
    unsigned int byte_off = (unsigned)code * 8 + r;
    unsigned char p0 = pgm_read_byte(&gigas2_gfx_tile_p0[byte_off]);
    unsigned char p1 = pgm_read_byte(&gigas2_gfx_tile_p1[byte_off]);
    unsigned char p2 = pgm_read_byte(&gigas2_gfx_tile_p2[byte_off]);
    for (int x = 0; x < 8; x++, ptr++) {
      int bit_pos = 7 - x;
      unsigned char pix = ((p2 >> bit_pos) & 1) << 2
                        | ((p1 >> bit_pos) & 1) << 1
                        | ((p0 >> bit_pos) & 1);
      *ptr = pal[pix];
    }
  }
}

void Gigas2::blit_sprite(short strip_row, unsigned char s) {
  sprite_S &sp = sprite[s];
  short top = strip_row * 8;
  short sx = sp.x;
  short sy = sp.y;
  if (sy + 16 <= top || sy >= top + 8) return;
  if (sx >= GIGAS2_SCREEN_W || sx + 16 <= 0) return;

  unsigned int code = sp.code & (GIGAS2_NUM_SPRITES - 1);
  unsigned int base = code * 32;

  const unsigned short *pal = &gigas2_palette[0x100 + (sp.color & 0x1F) * 8];

  for (int dy = 0; dy < 16; dy++) {
    int y_screen = sy + dy;
    int strip_y  = y_screen - top;
    if (strip_y < 0 || strip_y >= 8) continue;
    unsigned short *ptr = frame_buffer + strip_y * GIGAS2_SCREEN_W;
    for (int dx = 0; dx < 16; dx++) {
      int x_screen = sx + dx;
      if (x_screen < 0 || x_screen >= GIGAS2_SCREEN_W) continue;
      int byte_off = base + dy + (dx >= 8 ? 16 : 0);
      int bit_pos  = 7 - (dx & 7);
      unsigned char p0 = pgm_read_byte(&gigas2_gfx_spr_p0[byte_off]);
      unsigned char p1 = pgm_read_byte(&gigas2_gfx_spr_p1[byte_off]);
      unsigned char p2 = pgm_read_byte(&gigas2_gfx_spr_p2[byte_off]);
      unsigned char pix = ((p0 >> bit_pos) & 1) << 2
                        | ((p1 >> bit_pos) & 1) << 1
                        | ((p2 >> bit_pos) & 1);
      if (pix == 0) continue;
      ptr[x_screen] = pal[pix];
    }
  }
}

#endif // ENABLE_GIGAS2
