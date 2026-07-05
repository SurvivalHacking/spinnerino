#ifndef BOMBBEE_H
#define BOMBBEE_H

#include "../machineBase.h"

#ifdef ENABLE_BOMBBEE

#include <pgmspace.h>
#include "bombbee_rom.h"
#include "bombbee_gfx.h"
#include "bombbee_logo.h"

// ============================================================================
// Bomb Bee (Namco 1979) — driver MAME namco/warpwarp.cpp, set "bombbee"
//
// CPU   : Intel 8080 @ 2.048 MHz (MASTER_CLOCK 18.432/9) emulato su core Z80
//         (8080 e' subset di Z80, come Space Invaders).
// Video : raster 272x224 ROT90; tilemap 8x8 34x28 (scan 32x32->34x28) +
//         "ball" hardware 4x4. Colore generato da rete resistori
//         (warpwarp_palette) — nessuna PROM.
// Audio : WARPWARP_SOUND custom (NE556), 2 canali. [Fase 4]
// Input : paddle analogico (VOLIN1 0x14..0xAC REVERSE) + serve (IN0 bit4).
//
// IRQ: la linea vblank e' GATED da ball_on (latch bit6). Prima che il gioco
// abiliti la palla la CPU gira a polling; durante il gioco scatta RST 38h
// (INT_RST38 = 0xFF, default 8080) una volta per frame.
//
// Memory map 8080 (bombbee_map):
//   0x0000-0x1FFF  ROM programma (8 KB)
//   0x2000-0x23FF  RAM lavoro (1 KB)
//   0x4000-0x47FF  Video RAM (2 KB): 0x4000-0x43FF tile, 0x4400-0x47FF colore
//   0x4800-0x4FFF  ROM grafica char (2 KB)
//   0x6000-0x600F  R: switch IN0 (bit-serial)  W: ball_h/ball_v/sound0/watchdog
//   0x6010-0x601F  R: paddle (VOLIN1)           W: music ch1
//   0x6020-0x602F  R: DSW1 (bit-serial)         W: music ch2
//   0x6030-0x6037  W: latch LS259 (b6=ball_on, b7=flip, lamp/coin/counter)
// ============================================================================
class BombBee : public machineBase {
public:
  BombBee() { }
  ~BombBee() { }

  signed char machineType()      override { return MCH_BOMBBEE; }
  signed char videoFlipY()       override { return 0; }
  signed char videoFlipX()       override { return 0; }
  signed char useVideoHalfRate() override { return 0; }
  bool        isLandscape()      override { return false; } // arcade portrait, render trasposto
  bool        hasOpaqueBG()      override { return false; } // tile su nero -> memset richiesto
  bool        freeRunEmulation() override { return false; }
  int         ec11PulseHoldMs()  override { return 0; }     // paddle continuo, niente hold

  unsigned char rdZ80(unsigned short Addr) override;
  void          wrZ80(unsigned short Addr, unsigned char Value) override;
  unsigned char opZ80(unsigned short Addr) override;
  // inZ80/outZ80 non usati: l'I/O di Bomb Bee e' memory-mapped (bombbee_map).

  void run_frame(void) override;
  void prepare_frame(void) override;
  void render_row(short row) override;
  const unsigned short *logo(void) override;
  void reset() override;

protected:
  void blit_tile(short row, char col) override { }
  void blit_sprite(short row, unsigned char s) override { }

private:
  uint8_t  ball_h, ball_v;     // posizione palla hardware (write 0x6000/0x6001)
  uint8_t  ball_on;            // latch bit6: abilita palla + IRQ vblank
  uint8_t  flip;               // latch bit7: flip screen (cocktail)
  uint8_t  dsw1;               // dip switch (coinage/lives/replay)
  uint8_t  last_coin;          // edge detection coin

  // Tabella colore precalcolata (warpwarp_palette), 256 attr -> RGB565 swap.
  unsigned short color_lut[256];
  void build_color_lut(void);

  uint8_t  read_paddle(void);  // EC11 -> range Bomb Bee 0x14..0xAC
};

#endif // ENABLE_BOMBBEE
#endif // BOMBBEE_H
