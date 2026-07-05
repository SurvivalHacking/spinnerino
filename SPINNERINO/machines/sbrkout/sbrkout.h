#ifndef SBRKOUT_H
#define SBRKOUT_H

#include "../machineBase.h"

#ifdef ENABLE_SBRKOUT

// ============================================================================
// Super Breakout (Atari 1978) — set "sbrkout" rev 04
//
// Hardware reale (driver MAME atari/sbrkout.cpp):
//   CPU      : M6502 @ 756 kHz (12.096 MHz XTAL / 16)
//   Video    : 256x224 visible monocromatico (B/W fisso)
//              tilemap 32x32 char 8x8 + 3 ball sprite 3x3
//   Audio    : DAC 1-bit (videoram[0x391] & (scanline>>2))
//   Input    : paddle analogico + Serve button + Start1/2 + Coin1/2
//
// Memory map 6502:
//   0x0000-0x007F  zero page RAM (mirror 0x80/0x100/.../0x380)
//   0x0400-0x07FF  VRAM (tilemap 32x32 byte; bit 7 = visibile + ball state)
//   0x0800-0x08FF  switch input read (multiplex per offset bits)
//   0x0C00-0x0C7F  output latch F9334 write
//   0x0C80         watchdog reset
//   0x0E00         IRQ ack
//   0x2800-0x3FFF  ROM (6 KB)
//   Address bus 14-bit → 0xFFFC/0xFFFD reset vector mirrors da 0x3FFC/0x3FFD.
//
// IRQ: ogni 32 scanline (offset 16). 262 scanline/frame → ~8 IRQ per frame.
// Paddle: NMI scatta quando scanline == 56 + (pot/2). NMI handler legge la
// posizione del paddle dal timer hardware.
// ============================================================================

#define SBRKOUT_SCREEN_W 256
#define SBRKOUT_SCREEN_H 224
#define SBRKOUT_COLS     32
#define SBRKOUT_ROWS     28
// Visible area MAME: x=0..255, y=0..223. Tilemap interno 32x32 = 256x256:
// le 4 righe extra (32 tile) non si vedono a schermo.

class Sbrkout : public machineBase {
public:
  Sbrkout();

  void init(Input *input, unsigned short *framebuffer,
            sprite_S *spritebuffer, unsigned char *memorybuffer) override;
  void reset() override;

  signed char machineType()      override { return MCH_SBRKOUT; }
  bool         isLandscape()     override { return false; }
  bool         hasOpaqueBG()     override { return true;  }
  bool         freeRunEmulation()override { return false; }

  unsigned char rd6502(unsigned short Addr) override;
  void          wr6502(unsigned short Addr, unsigned char Value) override;

  void run_frame()      override;
  void prepare_frame()  override;
  void render_row(short row) override;

  const unsigned short *logo(void) override;

private:
  // render trasposto inline in render_row(): non uso blit_tile/blit_sprite virtuali

  // Output latch F9334 (bit:significato)
  unsigned char outlatch;   // bit 7 = coin counter; bit 5,6 paddle mask; ecc.

  // Paddle/NMI helper
  unsigned char paddle_value;          // 0-255 valore corrente
  unsigned short nmi_scanline_target;  // scanline alla quale scatta NMI
  unsigned char pot_trigger;           // 0 = not ready, 1 = ready (set da NMI)
  unsigned char pot_mask[2];           // 1=masked (no NMI), 0=enabled. MAME-fedele
                                       // (gioco scrive bit 5/6 outlatch=1 per smascherare)
  unsigned short scanline;             // counter 0..261 esposto a sync_r
  unsigned char sync2_value;           // hsync mid-line flag esposto a sync2_r
  bool nmi_fired_this_frame;
  // IRQ MAME-fedele: la linea 16V e' LEVEL-held (asserita ogni 32 scanline,
  // offset 16) finche' il gioco non scrive $0E00 (ack). Va consegnata SOLO se il
  // flag I del 6502 e' clear: con SEI (sezione critica fisica palla + disegno
  // paddle) resta pendente. Il vecchio codice chiamava irq6502() incondizionato
  // -> IRQ forzato anche con I=1 -> paddle non disegnato a centro/destra sotto
  // carico (palla in movimento). Questo flag tiene la linea pendente.
  bool irq_asserted;
  // MAME-fedele: pot_trigger e' LATCHED. Se il gioco smaschera l'NMI (outlatch
  // bit5) DOPO che il trigger (scanline 56+pot/2) e' gia' avvenuto, l'NMI deve
  // partire allo smascheramento. Senza questo, durante il gioco attivo (ball in
  // movimento, main-loop piu' lungo) il paddle non viene riposizionato per certe
  // posizioni (centro/destra) -> invisibile. Lo segnalo qui e lo eseguo in run_frame.
  bool nmi_pending;
  // Finestra di letture dopo l'NMI durante la quale sync_r/sync2_r ritornano il
  // VERO bit mezza-linea del pot (paddle_value&1), come MAME che fa fire il pot
  // a hpos=(pot%2)*128. Fuori dalla finestra si usa il toggle finto (anti busy-loop).
  unsigned char pot_sample_reads;

  // Switch read multiplex
  unsigned char switch_read(unsigned char offs);

  // Selezione modalità di gioco (sostituisce il commutatore fisico):
  //   0 = Progressive (muro che scende)  → SELECT $0830 = 0x3F
  //   1 = Cavity      (2 palline centro) → SELECT $0830 = 0x7F
  //   2 = Double      (doppia paddle)    → SELECT $0830 = 0xBF
  // Ciclato dal tasto START in attract mode.
  unsigned char game_select;
};

#endif // ENABLE_SBRKOUT
#endif // SBRKOUT_H