#ifndef GOINDOL_H
#define GOINDOL_H

#include "../machineBase.h"

#ifdef ENABLE_GOINDOL

// ============================================================================
// Goindol (SunA Electronics 1987) — set `goindol` (World)
// Driver MAME: src/mame/suna/goindol.cpp
//
// Hardware reale:
//   CPU main : Z80 @ 12 MHz/2 (6 MHz)         -> cpu[0]
//   CPU sound: Z80 @ 12 MHz/2                 -> cpu[1] (FASE 3, per ora stub)
//   Audio    : YM2203 (12 MHz/8)              -> FASE 3
//   Display  : 256x224, ROT90 (verticale, = orientamento nativo SPINNERINO)
//   Controllo: DIAL/spinner (0xC820) + joystick + 2 button + START + COIN
//   Protezione: blocco epoxy presso lo Z80 main (handler R/W patch RAM)
//
// Memory map main Z80 (main_map):
//   0x0000-0x7FFF  ROM fissa (r1w)
//   0x8000-0xBFFF  ROM banked (4 banchi da 16KB, sel da 0xC810 bit0-1)
//   0xC000-0xC7FF  work RAM (m_ram) — patchata dalla protezione
//   0xC800         soundlatch write / watchdog (read nop)
//   0xC810         bankswitch_w (b0-1 bank, b4 char_bank, b5 flip)
//   0xC820         R: DIAL    W: fg_scrolly
//   0xC830         R: P1      W: fg_scrollx
//   0xC834         R: P2
//   0xD000-0xD03F  spriteram[0]   (16 sprite x 4 byte; gfx = bg_chars)
//   0xD040-0xD7FF  RAM
//   0xD800-0xDFFF  BG video RAM (bg_videoram_w)
//   0xE000-0xE03F  spriteram[1]   (16 sprite x 4 byte; gfx = fg_chars)
//   0xE040-0xE7FF  RAM
//   0xE800-0xEFFF  FG video RAM (fg_videoram_w)
//   0xF000         DSW1
//   0xF422         prot_f422_r  (toggle bit 7)
//   0xF800         DSW2
//   0xFC44/FC66/FCB0/FD99  prot_*_w (patch work RAM 0x419..0x425)
//
// Interrupt: main = IRQ vblank (IM1 -> RST38) 1x/frame.
//            sound = IRQ periodico 240 Hz (FASE 3).
//
// Tilemap (video_start):
//   bg_tilemap 8x8 32x32 OPACO (gfx bg_chars)
//   fg_tilemap 8x8 32x32 TRASPARENTE pen0 (gfx fg_chars) + scroll
//   get_*_tile_info: code = vram[2*idx+1] | ((attr&7)<<8) | (char_bank<<11)
//                    color = (attr & 0xF8) >> 3 ; attr = vram[2*idx]
//   screen_update: bg.draw, fg.draw, sprites(gfx1,spr0), sprites(gfx0,spr1)
//
// Sprite (draw_sprites): NO ROM sprite dedicata, riusa i tile char 8x8.
//   sx = spr[0]; sy = 240 - spr[1]; visibile se (spr[1]>>3) && sx<248
//   tile = (spr[3] | ((spr[2]&7)<<8)) * 2 ; palette = spr[2] >> 3
//   disegna tile @ (sx,sy) e tile+1 @ (sx,sy+8) -> sprite 8x16
//
// GFX 8x8 3bpp: plane p0/p1/p2 da file r4/r5/r6 (fg) e r7/r8/r9 (bg).
//   charlayout MAME planeoffset = {RGN_FRAC(0,3),(1,3),(2,3)} -> planeoffset[0]=
//   p0=MSB, p2=LSB. pix=(p0<<2)|(p1<<1)|p2. (NON come gigas che ha layout
//   diverso; con p0=LSB i colori erano permutati 1<->4,3<->6, fix 2026-06-29.)
// ============================================================================

#define GOINDOL_SCREEN_W 256
#define GOINDOL_SCREEN_H 224
#define GOINDOL_COLS     32         // 256/8
#define GOINDOL_ROWS     28         // 224/8 righe visibili
#define GOINDOL_VRAM_OFFSET_ROWS 2  // visarea Y inizia a screen y=16 -> +2 tile row
#define GOINDOL_NUM_TILES 4096      // 96KB/3plane/8 = 4096 tile per regione

class Goindol : public machineBase {
public:
  Goindol();

  void init(Input *input, unsigned short *framebuffer,
            sprite_S *spritebuffer, unsigned char *memorybuffer) override;
  void reset() override;

  signed char machineType()      override { return MCH_GOINDOL; }
  bool         isLandscape()     override { return false; }   // verticale (ROT90)
  bool         hasOpaqueBG()     override { return true;  }    // bg riempie tutto
  bool         freeRunEmulation()override { return false; }

  unsigned char rdZ80(unsigned short Addr) override;
  void          wrZ80(unsigned short Addr, unsigned char Value) override;
  unsigned char opZ80(unsigned short Addr) override;

  void run_frame()      override;
  void prepare_frame()  override;
  void render_row(short row) override;

  const unsigned short *logo(void) override;

  // ===== AUDIO (Fase 3) — YM2203 reale via fm.c, generato sul core emulazione =====
  // Sound Z80 = cpu[1]. IRQ periodico fisso 240Hz (NON dal timer YM2203, che su
  // goindol non e' collegato alla CPU). FM via fm.c; SSG (Fase 3b) via emu2149.
  void  *ym2203_chip = nullptr;
  void  *psg_chip    = nullptr;           // SSG (3 voci PSG dello YM2203) via emu2149
  uint8_t ssg_latch  = 0;                 // registro SSG selezionato (callback fm.c)
  // Ring buffer audio SPSC (produttore=core emul in gen_audio, consumatore=task audio).
  static const int GOINDOL_RING = 4096;   // potenza di 2
  int16_t *ring = nullptr;
  volatile uint32_t ring_head = 0;
  volatile uint32_t ring_tail = 0;
  int audioPop(int16_t *out, int n);      // estrae n campioni mono (task audio)

private:
  void blit_tile(short row, char col) override;          // non usato (render per-pixel)
  void blit_sprite(short row, unsigned char s) override; // non usato

  // Render di una banda 8px (per-pixel, con scroll fg)
  void render_layers(short row);
  void draw_sprite_bank(short row, const unsigned char *spr, char use_fg_gfx);

  // ── stato I/O ──
  unsigned char bank;          // 0xC810 bit0-1
  unsigned char char_bank;     // 0xC810 bit4
  unsigned char flip_screen;   // 0xC810 bit5
  unsigned char fg_scrollx;    // 0xC830 write
  unsigned char fg_scrolly;    // 0xC820 write
  unsigned char soundlatch;    // 0xC800 write
  unsigned char prot_toggle;   // 0xF422 read toggle
  // ── coin impulse (MAME PORT_IMPULSE(1)): la linea COIN resta attiva UN SOLO
  //    frame per pressione, altrimenti il gioco conta un credito per ogni frame
  //    in cui il tasto e' tenuto premuto (bug "va subito a 9"). ──
  unsigned char coin_prev;     // stato fisico COIN del frame precedente (edge detect)
  unsigned char coin_impulse;  // frame residui in cui la linea COIN resta attiva

  // ── Audio (Fase 3) — sound CPU + YM2203 ──
  unsigned char *sndram = nullptr;     // sound RAM 0xC000-0xC7FF (2KB)
  unsigned char  soundlatch_snd = 0;   // (alias di soundlatch, lato sound)
  unsigned char  sound_irq_pending = 0;// IRQ periodico 240Hz da consegnare al sound CPU
  int32_t       *fmbuf = nullptr;      // buffer temporaneo FM (1 chunk)
  uint32_t       snd_accum = 0;        // accumulatore frazionario us->campioni
  uint32_t       last_audio_us = 0;    // timestamp ultima generazione (tempo reale)
  void sound_init_chips();
  void gen_audio(int nsamples);
};

#endif // ENABLE_GOINDOL
#endif // GOINDOL_H
