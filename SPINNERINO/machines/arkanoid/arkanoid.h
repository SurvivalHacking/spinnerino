#ifndef ARKANOID_H
#define ARKANOID_H

#include "../machineBase.h"

#ifdef ENABLE_ARKANOID

class Arkanoid : public machineBase {
public:
  Arkanoid();

  void init(Input *input, unsigned short *framebuffer,
            sprite_S *spritebuffer, unsigned char *memorybuffer) override;
  void reset() override;

  signed char machineType()    override { return MCH_ARKANOID; }
  signed char videoFlipY()     override { return 0; }
  signed char videoFlipX()     override { return 0; }
  signed char useVideoHalfRate() override { return 0; }
  bool isLandscape()           override { return false; } // 256x224 verticale
  bool hasOpaqueBG()           override { return true;  } // BG opaco (riempie tile)
  bool freeRunEmulation()      override { return false; }

  // Z80 memory + I/O
  unsigned char rdZ80(unsigned short Addr) override;
  void          wrZ80(unsigned short Addr, unsigned char Value) override;
  unsigned char opZ80(unsigned short Addr) override;
  unsigned char inZ80(unsigned short Port) override;
  void          outZ80(unsigned short Port, unsigned char Value) override;

  void run_frame()    override;
  void prepare_frame() override;
  void render_row(short row) override;

  const signed char *waveRom(unsigned char value) override { return 0; }
  const unsigned short *logo(void) override;

private:
  void blit_tile(short row, char col)            override;
  void blit_sprite(short row, unsigned char s)   override;

  // ---- registri/latch macchina arkangc ----
  unsigned char gfx_bank;          // banco GFX (D008 bit)
  unsigned char palette_bank;      // banco palette (D008 bit)
  unsigned char ay_addr;            // ultimo registro AY selezionato (i registri stanno in soundregs[])

  // input latches
  unsigned char in0_state;          // coin/start/service
  unsigned char dsw1_state;         // dipswitch board
  unsigned char paddle_latch;       // valore paddle 8-bit (da ec11_paddle_x)

  // helper memory map sub-handlers
  unsigned char read_input_port(unsigned short addr);
  void          write_d000_d018(unsigned short addr, unsigned char val);
};

#endif // ENABLE_ARKANOID
#endif // ARKANOID_H
