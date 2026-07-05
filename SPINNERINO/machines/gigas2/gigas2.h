#ifndef GIGAS2_H
#define GIGAS2_H

#include "../machineBase.h"

#ifdef ENABLE_GIGAS2

// ============================================================================
// Gigas Mark II (Sega/Nihon System 1986) — bootleg `gigasm2b` (NO MC-8123)
//
// Hardware identico a Gigas (vedi machines/gigas/gigas.h):
//   CPU      : Z80 @ 3.072 MHz singolo, dual-mapping DATA+OPCODES (96 KB)
//   Audio    : 4× SN76489A PSG
//   Display  : 256x224 portrait (ROT270) → flip 180° SW per ROT90 user view
//   Controllo: dial (spinner) + 2 button + START + COIN
//
// L'unica differenza concreta dal porting Gigas e' la ROM region maincpu:
// gigasm2b la costruisce da TRE ROM 32KB (8.rom + 7.rom + 9.rom) invece di
// g-7 (32KB) + g-8 (64KB). Il converter Python ricostruisce comunque un'unica
// region 96KB nello stesso layout (DATA 0x0000-0xBFFF, OPCODES 0xC000-0x17FFF),
// quindi rdZ80/opZ80 sono identici al Gigas originale.
// Le PROM palette sono identiche (stessi CRC), quindi tabella colore uguale.
// ============================================================================

#define GIGAS2_SCREEN_W 256
#define GIGAS2_SCREEN_H 224
#define GIGAS2_COLS     32
#define GIGAS2_ROWS     28
#define GIGAS2_VRAM_OFFSET_ROWS 2

class Gigas2 : public machineBase {
public:
  Gigas2();

  void init(Input *input, unsigned short *framebuffer,
            sprite_S *spritebuffer, unsigned char *memorybuffer) override;
  void reset() override;

  signed char machineType()      override { return MCH_GIGAS2; }
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

  unsigned char spinner_sel;
  unsigned char nmi_enable;
  unsigned char flip_screen;
  unsigned short boot_button1_mask_frames;

  unsigned char sn_latched[4];
  void sn76489_write(int chip, unsigned char data);
};

#endif // ENABLE_GIGAS2
#endif // GIGAS2_H
