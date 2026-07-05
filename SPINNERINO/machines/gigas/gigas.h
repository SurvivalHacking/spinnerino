#ifndef GIGAS_H
#define GIGAS_H

#include "../machineBase.h"

#ifdef ENABLE_GIGAS

// ============================================================================
// Gigas (Sega/Nihon System 1986) — bootleg `gigasb` (NO MC-8123)
//
// Hardware reale (driver MAME misc/freekick.cpp):
//   CPU      : Z80 @ 3.072 MHz (single)
//   Audio    : 4× SN76489A PSG (per ora stub)
//   Display  : 256x224 portrait (ROT270)
//   Controllo: dial (spinner) + 2 button + START + COIN
//
// Memory map Z80 (gigas_program_map):
//   0x0000-0xBFFF  ROM (48 KB usabili dei 96 totali ricostruiti)
//   0xC000-0xCFFF  RAM general (4 KB)
//   0xD000-0xD3FF  VRAM tile codes 8-bit (1024 tile slot)
//   0xD400-0xD7FF  VRAM attr (palette 5-bit + tile code high 3-bit)
//   0xD800-0xD8FF  spriteRAM (256 B = 64 sprite × 4 byte)
//   0xD900-0xDFFF  RAM extra (1.75 KB)
//   0xE000         IN0 read / outlatch LS259 write
//   0xE800         IN1 read
//   0xF000         DSW1 read
//   0xF800         DSW2 read
//   0xFC00-0xFC03  4× SN76489A write
//
// I/O ports:
//   0x00  spinner_r / spinner_select_w
//   0x01  unused (DSW3)
//
// Outlatch LS259 (write a 0xE000+N, bit 0 di data → bit N del latch):
//   bit 0 = flip_screen (invertito)
//   bit 2 = coin counter A
//   bit 3 = coin counter B
//   bit 4 = NMI ENABLE  ← critico per vblank
//
// Vblank: NMI (NON IRQ standard). Asserito ad ogni vblank SE nmi_en=1.
//
// GFX layout:
//   Tile  : 8x8 px, 3 plane (g-4, g-5, g-6) → 2048 tile, palette base 0x000
//   Sprite: 16x16 px, 3 plane (g-1=MSB, g-2, g-3=LSB) → 512 sprite, base 0x100
// ============================================================================

#define GIGAS_SCREEN_W 256
#define GIGAS_SCREEN_H 224
#define GIGAS_COLS     32         // 256/8 = 32 tile per riga
#define GIGAS_ROWS     28         // 224/8 = 28 tile righe visibili
#define GIGAS_VRAM_OFFSET_ROWS 2  // visarea Y inizia a y=16 → tile_row += 2

class Gigas : public machineBase {
public:
  Gigas();

  void init(Input *input, unsigned short *framebuffer,
            sprite_S *spritebuffer, unsigned char *memorybuffer) override;
  void reset() override;

  signed char machineType()      override { return MCH_GIGAS; }
  // Gigas ROT270 vs SPINNERINO ROT90 → diff 180°.
  // Implemento flip 180° in SOFTWARE in render_row (driver hardware non
  // supporta double-flip simultaneo: flipVertical e flipHorizontal scrivono
  // lo stesso bit MADCTL).
  bool         isLandscape()     override { return false; }
  bool         hasOpaqueBG()     override { return true;  }
  bool         freeRunEmulation()override { return false; }

  unsigned char rdZ80(unsigned short Addr) override;
  void          wrZ80(unsigned short Addr, unsigned char Value) override;
  unsigned char opZ80(unsigned short Addr) override;
  unsigned char inZ80(unsigned short Port) override;
  void          outZ80(unsigned short Port, unsigned char Value) override;

  void run_frame()      override;
  void prepare_frame()  override;
  void render_row(short row) override;

  const unsigned short *logo(void) override;

private:
  void blit_tile(short row, char col)          override;
  void blit_sprite(short row, unsigned char s) override;

  // ── stato I/O ──
  unsigned char spinner_sel;       // bit 0 di IN port 0x00 write
  unsigned char nmi_enable;        // bit 4 di outlatch (0xE004 write d0)
  unsigned char flip_screen;       // bit 0 di outlatch

  // Boot mask per BUTTON1 (FIRE in game): nei primi N frame dopo il reset
  // sopprimi BUTTON1 anche se physical FIRE e' premuto, per evitare che il
  // game legga BUTTON1=pressed durante init e finisca in service test screen.
  // Dopo la finestra di mask, FIRE ridiventa il pulsante FIRE in game.
  unsigned short boot_button1_mask_frames;

  // SN76489A: ultimo registro latched per ciascuno dei 4 chip.
  // SN76489 stream: byte con bit 7=1 e' "latch" (selezione registro + nibble
  // basso); byte con bit 7=0 e' "data continuation" (nibble alto period).
  // 8 registri per chip: 4 period + 4 volume (canali 0..3, c=3 noise).
  unsigned char sn_latched[4];     // [chip] = reg index 0..7
  void sn76489_write(int chip, unsigned char data);
};

#endif // ENABLE_GIGAS
#endif // GIGAS_H
