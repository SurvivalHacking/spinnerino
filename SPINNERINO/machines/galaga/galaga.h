#ifndef GALAGA_H
#define GALAGA_H

#include "../machineBase.h"

#ifdef ENABLE_GALAGA

#include "galaga_rom1.h"
#include "galaga_rom2.h"
#include "galaga_rom3.h"
#include "galaga_dipswitches.h"
#include "galaga_logo.h"
#include "galaga_spritemap.h"
#include "galaga_tilemap.h"
#include "galaga_cmap_tiles.h"
#include "galaga_cmap_sprites.h"
#include "galaga_wavetable.h"
#include "galaga_sample_boom.h"
#include "galaga_starseed.h"
#include "../tileaddr.h"

class galaga : public machineBase
{
public:
  galaga() { }
  ~galaga() { }

  signed char machineType()    override { return MCH_GALAGA; }
  signed char videoFlipY()     override { return 0; }
  signed char videoFlipX()     override { return 0; }
  signed char useVideoHalfRate() override { return 0; }
  bool isLandscape()           override { return false; } // arcade portrait, render naturale
  bool hasOpaqueBG()           override { return false; } // BG nero, memset richiesto
  bool freeRunEmulation()      override { return false; }
  bool hasNamcoAudio()         override { return true; }
  int  ec11PulseHoldMs()       override { return 100; }  // EC11 pulse hold per nave

  unsigned char rdZ80(unsigned short Addr) override;
  void          wrZ80(unsigned short Addr, unsigned char Value) override;
  unsigned char opZ80(unsigned short Addr) override;

  void run_frame(void) override;
  void prepare_frame(void) override;
  void render_row(short row) override;

  const signed char    *waveRom(unsigned char value) override;
  const unsigned short *logo(void) override;

  // Sample boom: pubblici per audio.cpp (cast (galaga*)currentMachine)
  unsigned short      snd_boom_cnt = 0;
  const signed char  *snd_boom_ptr = NULL;

protected:
  // Override vuoti: SPINNERINO usa il render trasposto via blit_*_t (sotto)
  void blit_tile(short row, char col)            override { }
  void blit_sprite(short row, unsigned char s)   override { }

private:
  // Render trasposto per portrait su framebuffer SPINNERINO landscape:
  //   fb_x = arcade_y           (clip arcade_y < 256, perdo bottom 32 px)
  //   fb_y = 223 - arcade_x     (no clip)
  void blit_tile_t  (short strip_r, char row_arcade);
  void blit_sprite_t(short strip_r, unsigned char s);
  void render_stars_set_t(short strip_r, const struct galaga_star *set);
  void trigger_sound_explosion(void);

  unsigned char stars_scroll_y = 0;
  unsigned char credit         = 0;
  char          credit_mode    = 0;
  int           namco_cnt      = 0;
  int           namco_busy     = 0;
  unsigned char cs_ctrl        = 0;
  int           nmi_cnt        = 0;
  int           coincredMode   = 0;
  unsigned char starcontrol    = 0;
  char          sub_cpu_reset  = 1;
};

#endif // ENABLE_GALAGA
#endif // GALAGA_H
