#ifndef GALAXIAN_H
#define GALAXIAN_H

#include "../machineBase.h"

#ifdef ENABLE_GALAXIAN

#include <pgmspace.h>
#include "galaxian_logo.h"
#include "galaxian_rom.h"
#include "galaxian_dipswitches.h"
#include "galaxian_tilemap.h"
#include "galaxian_spritemap.h"
#include "galaxian_cmap.h"
#include "../tileaddr.h"

// ============================================================================
// Galaxian (Namco/Midway 1979) — porting da GALAGINO Files_FULL33 a SPINNERINO P4.
// HW: Z80 @ 3.072MHz, schermo 224x256 portrait (cabinet ROT90), tile 8x8 +
// sprite 16x16 + 8 bullet + starfield LFSR. Audio discreto (VCO/FS/HIT/FIRE)
// gestito da Audio::galaxian_render_buffer() in audio.cpp.
//
// Memory map Z80 (A15 ignorato → 0x8000-0xFFFF mirror di 0x0000-0x7FFF):
//   0x0000-0x3FFF  ROM (16 KB, galmidw)
//   0x4000-0x47FF  RAM            -> memory[0x0000..0x07FF]
//   0x5000-0x53FF  VRAM tile      -> memory[0x0800..0x0BFF]
//   0x5800-0x58FF  ObjRAM         -> memory[0x0C00..0x0CFF]
//   0x6000         IN0 (coin/dir/fire)
//   0x6004-0x6007  LFO DAC  (soundregs[1-4])
//   0x6800-0x6807  sound latch FS/HIT/FIRE/VOL (soundregs[8-15])
//   0x7001         NMI enable
//   0x7004         stars enable
//   0x7800         VCO pitch (soundregs[0])
//
// RENDER SPINNERINO: trasposto come galaga.cpp (framebuffer landscape 256-wide
// + rotazione MV display). fb_x = arcade_y - offset, fb_y = 223 - arcade_x.
// ============================================================================

// Starfield: max number of visible stars (LFSR generates ~252)
#define GAL_MAX_STARS 256

class galaxian : public machineBase
{
public:
	galaxian() { }
	~galaxian() { }

	signed char machineType()    override { return MCH_GALAXIAN; }
	signed char videoFlipY()     override { return 0; }
	signed char videoFlipX()     override { return 0; }
	bool        isLandscape()    override { return false; } // arcade portrait, render trasposto
	bool        hasOpaqueBG()    override { return false; } // BG nero + stelle, memset richiesto
	bool        freeRunEmulation() override { return false; }
	int         ec11PulseHoldMs() override { return 100; }  // EC11 pulse hold per la nave (come Galaga)

	unsigned char rdZ80(unsigned short Addr) override;
	void          wrZ80(unsigned short Addr, unsigned char Value) override;
	void          outZ80(unsigned short Port, unsigned char Value) override;
	unsigned char opZ80(unsigned short Addr) override;
	unsigned char inZ80(unsigned short Port) override;

	void run_frame(void) override;
	void prepare_frame(void) override;
	void render_row(short row) override;
	const unsigned short *logo(void) override;

protected:
	// SPINNERINO usa il render trasposto via blit_*_t (sotto): override vuoti.
	void blit_tile(short row, char col)          override { }
	void blit_sprite(short row, unsigned char s) override { }

private:
	// Render trasposto (portrait per utente SPINNERINO):
	//   fb_x = arcade_y - ARCADE_Y_OFFSET   fb_y = 223 - arcade_x
	//   render_row(strip_r) copre 1 colonna arcade (col = 27 - strip_r).
	void blit_tile_t(short strip_r, char row_arcade);
	void blit_tile_scroll_t(short strip_r, char row_arcade, unsigned char scroll);
	void blit_sprite_t(short strip_r, unsigned char s);

	// Bullet rendering (8 bullets: 7 enemy shells + 1 player missile)
	short bullet_x[8], bullet_y[8];
	unsigned char bullet_active;  // bitmask of active bullets

	// Starfield
	struct star_entry {
		unsigned char x;       // arcade X
		unsigned char y;       // arcade Y
		unsigned short color;  // RGB565 byte-swapped
	};
	star_entry stars[GAL_MAX_STARS];
	int star_count = 0;
	int star_scroll_offset = 0;
	bool stars_enabled = false;
	bool stars_initialized = false;
	void stars_init();
};

#endif // ENABLE_GALAXIAN
#endif // GALAXIAN_H
