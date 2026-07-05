// ============================================================================
// SPINNERINO P4 - machines/galaxian/galaxian.cpp
//
// Porting Galaxian (GALAGINO Files_FULL33) -> framebuffer SPINNERINO.
//
// CPU/IO: copia 1:1 dal sorgente FULL33 (A15 mirror, mappa memoria, NMI).
// RENDER: trasposto come galaga.cpp (framebuffer landscape 256-wide + rotazione
//   MV display). Mapping arcade portrait (224 wide x 256 tall) -> framebuffer:
//     fb_x = arcade_y - ARCADE_Y_OFFSET   (verticale arcade, righe 2..33)
//     fb_y = 223 - arcade_x               (orizzontale arcade, reversed)
//   render_row(strip_r) copre 1 colonna arcade (col_arcade = 27 - strip_r) e
//   tutte le righe arcade (row_arcade 2..33 = fb_x 0..255).
//   Scroll per-riga della nave: nel nativo shifta lungo l'asse orizzontale
//   (col), nel trasposto diventa shift lungo fb_y (asse strip) -> ogni strip
//   riceve il tile principale (col_arcade) + il vicino (col_arcade-1).
// AUDIO: discreto in audio.cpp::galaxian_render_buffer() (gia' presente).
// ============================================================================
#include "galaxian.h"
#include "../../emulation/input.h"

#ifdef ENABLE_GALAXIAN

#define FB_W            256   // stride orizzontale framebuffer SPINNERINO
#define ARCADE_Y_OFFSET 16    // arcade_y 16..271 (righe 2..33) -> fb_x 0..255

// ----------------------------------------------------------------------------
// CPU Z80 / IO (1:1 da FULL33)
// ----------------------------------------------------------------------------
unsigned char galaxian::opZ80(unsigned short Addr) {
  Addr &= 0x7FFF;
  if(Addr < 0x4000)
    return galaxian_rom[Addr];
  return 0xff;
}

unsigned char galaxian::rdZ80(unsigned short Addr) {
  Addr &= 0x7FFF;

  if(Addr < 0x4000)
    return galaxian_rom[Addr];

  if((Addr & 0xf800) == 0x4000)
    return memory[Addr - 0x4000];

  if((Addr & 0xfc00) == 0x5000)
    return memory[Addr - 0x5000 + 0x0800];

  if((Addr & 0xf800) == 0x5800)
    return memory[Addr - 0x5800 + 0x0C00];

  if((Addr & 0xf800) == 0x6000) {
    unsigned char keymask = input->buttons_get();
    unsigned char retval = 0x00;
    if(keymask & BUTTON_COIN)   retval |= 0x01;
    if(keymask & BUTTON_LEFT)   retval |= 0x04;
    if(keymask & BUTTON_RIGHT)  retval |= 0x08;
    if(keymask & BUTTON_FIRE)   retval |= 0x10;
    return retval;
  }

  if((Addr & 0xf800) == 0x6800) {
    unsigned char keymask = input->buttons_get();
    unsigned char retval = GALAXIAN_DIP_IN1;
    if(keymask & BUTTON_START)  retval |= 0x01;
    return retval;
  }

  if((Addr & 0xf800) == 0x7000) {
    return GALAXIAN_DIP_IN2;
  }

  return 0x00;
}

void galaxian::wrZ80(unsigned short Addr, unsigned char Value) {
  Addr &= 0x7FFF;

  if((Addr & 0xf800) == 0x4000) {
    memory[Addr - 0x4000] = Value;
    return;
  }

  if((Addr & 0xfc00) == 0x5000) {
    if(!game_started && Addr == 0x5000 + 800 && Value != 0x10)
      game_started = 1;
    memory[Addr - 0x5000 + 0x0800] = Value;
    return;
  }

  if((Addr & 0xf800) == 0x5800) {
    memory[Addr - 0x5800 + 0x0C00] = Value;
    return;
  }

  if((Addr & 0xfff8) == 0x6000) {
    // 0x6004-0x6007: LFO DAC bits (soundregs[1-4])
    if(Addr >= 0x6004) {
      soundregs[1 + (Addr & 0x03)] = Value & 1;
    }
    return;
  }

  if((Addr & 0xfff8) == 0x6800) {
    // FS1,FS2,FS3,HIT,n/c,FIRE,VOL1,VOL2 (soundregs[8-15])
    soundregs[8 + (Addr & 0x07)] = Value & 1;
    return;
  }

  if((Addr & 0xfff8) == 0x7000) {
    unsigned char offset = Addr & 0x07;
    if(offset == 1) irq_enable[0]  = Value & 1;  // NMI enable
    if(offset == 4) stars_enabled  = (Value & 1); // stars enable
    return;
  }

  if((Addr & 0xf800) == 0x7800) {
    soundregs[0] = Value;  // VCO pitch
    return;
  }
}

void galaxian::outZ80(unsigned short Port, unsigned char Value) {
}

unsigned char galaxian::inZ80(unsigned short Port) {
  return 0x00;
}

void galaxian::run_frame(void) {
  if(!game_started) game_started = 1;

  current_cpu = 0;
  for(int i = 0; i < INST_PER_FRAME; i++) {
    StepZ80(&cpu[0]); StepZ80(&cpu[0]); StepZ80(&cpu[0]); StepZ80(&cpu[0]);
  }

  if(irq_enable[0]) {
    IntZ80(&cpu[0], INT_NMI);
  }
}

// ----------------------------------------------------------------------------
// prepare_frame: sprite (ObjRAM+0x40) + bullet (ObjRAM+0x60) + starfield init
// ----------------------------------------------------------------------------
void galaxian::prepare_frame(void) {
  if(!stars_initialized) stars_init();

  if(stars_enabled) {
    star_scroll_offset = (star_scroll_offset + 1) % 288;
  }

  active_sprites = 0;

  // Sprite: 8 entries da 4 byte a memory[0x0C40]
  //   base[0]=Y  base[1]=code|flip  base[2]=color  base[3]=X
  for(int idx = 7; idx >= 0 && active_sprites < 92; idx--) {
    struct sprite_S spr;
    unsigned char *base = memory + 0x0C40 + idx * 4;

    spr.code  = base[1] & 0x3f;
    spr.flags = (base[1] >> 6) & 3;
    spr.color = base[2] & 7;
    spr.x = base[0] - 16;   // portrait X (orizzontale arcade)
    spr.y = base[3] + 16;   // portrait Y (verticale arcade)

    if(base[3] && (spr.y > -16) && (spr.y < 288) && (spr.x > -16) && (spr.x < 224)) {
      sprite[active_sprites++] = spr;
    }
  }

  // Bullet: 8 entries da 4 byte a memory[0x0C60]
  bullet_active = 0;
  for(int idx = 0; idx < 8; idx++) {
    unsigned char *bbase = memory + 0x0C60 + idx * 4;
    bullet_x[idx] = bbase[1] - 16;
    bullet_y[idx] = 266 - bbase[3];
    if(bbase[1] && bbase[3] && bullet_x[idx] >= 0 && bullet_x[idx] < 224 &&
       bullet_y[idx] > -4 && bullet_y[idx] < 288)
      bullet_active |= (1 << idx);
  }
}

// ----------------------------------------------------------------------------
// Tile 8x8 trasposto (scroll == 0). col_arcade = 27 - strip_r fissa la colonna;
// row_arcade itera le righe arcade. fb_x = arcade_y, fb_y_strip = 7 - px.
// ----------------------------------------------------------------------------
void galaxian::blit_tile_t(short strip_r, char row_arcade) {
  char col_arcade = 27 - strip_r;
  unsigned short addr = tileaddr[row_arcade][col_arcade];

  const unsigned short *tile   = galaxian_tilemap[memory[0x0800 + addr]];
  int c = memory[0x0C00 + 2 * (addr & 31) + 1] & 7;
  const unsigned short *colors = galaxian_colormap[c];

  int fb_x_base = row_arcade * 8 - ARCADE_Y_OFFSET;
  if(fb_x_base + 8 <= 0 || fb_x_base >= FB_W) return;

  for(int py = 0; py < 8; py++) {
    int fb_x = fb_x_base + py;
    if(fb_x < 0)     continue;
    if(fb_x >= FB_W) break;

    unsigned short pix = tile[py];
    for(int px = 0; px < 8; px++, pix >>= 2) {
      unsigned char pen = pix & 3;
      if(pen) {
        int fb_y_strip = 7 - px;
        frame_buffer[fb_y_strip * FB_W + fb_x] = colors[pen];
      }
    }
  }
}

// ----------------------------------------------------------------------------
// Tile 8x8 trasposto con scroll. Lo strip (col_arcade) riceve due tile:
//   principale (native col = col_arcade)   -> fb_y_strip = 7  - sub - px
//   vicino     (native col = col_arcade-1) -> fb_y_strip = 15 - sub - px
// coarse scroll: addr += (scroll & ~7) << 2 (= colonne intere in VRAM).
// ----------------------------------------------------------------------------
void galaxian::blit_tile_scroll_t(short strip_r, char row_arcade, unsigned char scroll) {
  char col_arcade = 27 - strip_r;
  int  sub    = scroll & 0x07;
  int  coarse = (scroll & ~7) << 2;
  int  fb_x_base = row_arcade * 8 - ARCADE_Y_OFFSET;
  if(fb_x_base + 8 <= 0 || fb_x_base >= FB_W) return;

  // --- tile principale (native col = col_arcade) ---
  {
    unsigned short addr = (tileaddr[row_arcade][col_arcade] + coarse) & 1023;
    const unsigned short *tile   = galaxian_tilemap[memory[0x0800 + addr]];
    int c = memory[0x0C00 + 2 * (addr & 31) + 1] & 7;
    const unsigned short *colors = galaxian_colormap[c];

    for(int py = 0; py < 8; py++) {
      int fb_x = fb_x_base + py;
      if(fb_x < 0)     continue;
      if(fb_x >= FB_W) break;
      unsigned short pix = tile[py];
      for(int px = 0; px < 8; px++, pix >>= 2) {
        unsigned char pen = pix & 3;
        if(pen) {
          int fb_y_strip = 7 - sub - px;
          if(fb_y_strip >= 0 && fb_y_strip < 8)
            frame_buffer[fb_y_strip * FB_W + fb_x] = colors[pen];
        }
      }
    }
  }

  // --- tile vicino (native col = col_arcade - 1) ---
  {
    unsigned short addr;
    if(col_arcade - 1 >= 0)
      addr = (tileaddr[row_arcade][col_arcade - 1] + coarse) & 1023;
    else
      addr = (tileaddr[row_arcade][0] + 32 + coarse) & 1023;  // col == -1 (galagino)

    const unsigned short *tile   = galaxian_tilemap[memory[0x0800 + addr]];
    int c = memory[0x0C00 + 2 * (addr & 31) + 1] & 7;
    const unsigned short *colors = galaxian_colormap[c];

    for(int py = 0; py < 8; py++) {
      int fb_x = fb_x_base + py;
      if(fb_x < 0)     continue;
      if(fb_x >= FB_W) break;
      unsigned short pix = tile[py];
      for(int px = 0; px < 8; px++, pix >>= 2) {
        unsigned char pen = pix & 3;
        if(pen) {
          int fb_y_strip = 15 - sub - px;
          if(fb_y_strip >= 0 && fb_y_strip < 8)
            frame_buffer[fb_y_strip * FB_W + fb_x] = colors[pen];
        }
      }
    }
  }
}

// ----------------------------------------------------------------------------
// Sprite 16x16 trasposto (come galaga). sp->x = arcade X, sp->y = arcade Y.
//   fb_x = sp->y + sy - OFFSET    fb_y_strip = (223 - sp->x - sx) - strip_r*8
// ----------------------------------------------------------------------------
void galaxian::blit_sprite_t(short strip_r, unsigned char s) {
  const sprite_S *sp = &sprite[s];

  int col_arcade = 27 - strip_r;
  int lo = col_arcade * 8;
  int hi = lo + 7;

  int sx_min = lo - sp->x;  if(sx_min < 0)  sx_min = 0;
  int sx_max = hi - sp->x;  if(sx_max > 15) sx_max = 15;
  if(sx_min > sx_max) return;

  const unsigned long  *spr    = galaxian_spritemap[sp->flags & 3][sp->code];
  const unsigned short *colors = galaxian_colormap[sp->color];

  for(int sy = 0; sy < 16; sy++) {
    int fb_x = sp->y + sy - ARCADE_Y_OFFSET;
    if(fb_x < 0 || fb_x >= FB_W) continue;

    unsigned long pix = spr[sy];
    for(int sx = sx_min; sx <= sx_max; sx++) {
      unsigned char pen = (unsigned char)((pix >> (sx * 2)) & 3);
      if(pen) {
        int fb_y_strip = (223 - sp->x - sx) - strip_r * 8;
        frame_buffer[fb_y_strip * FB_W + fb_x] = colors[pen];
      }
    }
  }
}

// ----------------------------------------------------------------------------
// Starfield LFSR (MAME galaxian). Pre-calcola stelle visibili in coord arcade.
// ----------------------------------------------------------------------------
static inline unsigned short rgb_to_swapped565(unsigned char r, unsigned char g, unsigned char b) {
  unsigned short c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
  return (c >> 8) | (c << 8);
}

void galaxian::stars_init() {
  static const unsigned char starmap[4] = { 0, 150, 200, 255 };

  unsigned short star_color_lut[64];
  for(int i = 0; i < 64; i++) {
    unsigned char r = starmap[((i >> 4) & 2) | ((i >> 5) & 1)];
    unsigned char g = starmap[((i >> 2) & 2) | ((i >> 3) & 1)];
    unsigned char b = starmap[((i >> 0) & 2) | ((i >> 1) & 1)];
    star_color_lut[i] = rgb_to_swapped565(r, g, b);
  }

  star_count = 0;
  uint32_t shiftreg = 0;
  for(int i = 0; i < 131071 && star_count < GAL_MAX_STARS; i++) {
    if((shiftreg & 0x1fe01) == 0x1fe00) {
      int color_idx = (~shiftreg >> 3) & 0x3f;
      int x = (i % 512) / 2;
      int y = i / 512;

      if((y ^ (x >> 3)) & 1) {
        int gx = 255 - y;
        int gy = x + 16;

        if(gx >= 0 && gx < 224 && gy >= 16 && gy < 288) {
          stars[star_count].x = gx;
          stars[star_count].y = gy;
          stars[star_count].color = star_color_lut[color_idx];
          star_count++;
        }
      }
    }
    shiftreg = (shiftreg >> 1) | ((((shiftreg >> 12) ^ ~shiftreg) & 1) << 16);
  }
  stars_initialized = true;
}

// ----------------------------------------------------------------------------
// render_row(strip_r): stelle -> tile (con scroll) -> sprite -> bullet.
// ----------------------------------------------------------------------------
void galaxian::render_row(short row) {
  if(row < 0 || row >= 28) return;
  int strip_r   = row;
  int strip_top = strip_r * 8;

  // --- stelle (background, i tile le sovrascrivono) ---
  if(stars_enabled && stars_initialized) {
    for(int i = 0; i < star_count; i++) {
      int sy = ((int)stars[i].y + star_scroll_offset) % 288;
      if(sy < 16) sy += 272;
      int sx = stars[i].x;
      if(sx < 0 || sx >= 224) continue;
      int fb_y_strip = (223 - sx) - strip_top;
      if(fb_y_strip < 0 || fb_y_strip >= 8) continue;
      int fb_x = sy - ARCADE_Y_OFFSET;
      if(fb_x < 0 || fb_x >= FB_W) continue;
      int idx = fb_y_strip * FB_W + fb_x;
      if(frame_buffer[idx] == 0x0000)
        frame_buffer[idx] = stars[i].color;
    }
  }

  // --- tile (per-riga scroll) ---
  for(char row_arcade = 2; row_arcade < 34; row_arcade++) {
    unsigned char scroll = memory[0x0C00 + 2 * (row_arcade - 2)];
    if(scroll == 0)
      blit_tile_t(strip_r, row_arcade);
    else
      blit_tile_scroll_t(strip_r, row_arcade, scroll);
  }

  // --- sprite visibili nella colonna arcade della strip ---
  int col_arcade = 27 - strip_r;
  int strip_lo = col_arcade * 8;
  int strip_hi = strip_lo + 7;
  for(unsigned char s = 0; s < active_sprites; s++) {
    if(((sprite[s].x + 15) >= strip_lo) && (sprite[s].x <= strip_hi))
      blit_sprite_t(strip_r, s);
  }

  // --- bullet: linea 4px lungo fb_x, 1px in fb_y ---
  //   shells (0-6) = bianco, player missile (7) = giallo
  if(bullet_active) {
    for(int b = 0; b < 8; b++) {
      if(!(bullet_active & (1 << b))) continue;
      int bx = bullet_x[b];
      int by = bullet_y[b];
      if(bx < 0 || bx >= 224) continue;
      int fb_y_strip = (223 - bx) - strip_top;
      if(fb_y_strip < 0 || fb_y_strip >= 8) continue;
      unsigned short color = (b == 7) ? 0xE0FF : 0xFFFF;  // byte-swap giallo/bianco
      for(int py = 0; py < 4; py++) {
        int fb_x = by + py - ARCADE_Y_OFFSET;
        if(fb_x < 0 || fb_x >= FB_W) continue;
        frame_buffer[fb_y_strip * FB_W + fb_x] = color;
      }
    }
  }
}

const unsigned short *galaxian::logo(void) {
  return galaxian_logo;
}

#endif // ENABLE_GALAXIAN
