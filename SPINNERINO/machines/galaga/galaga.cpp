// ============================================================================
// SPINNERINO P4 - machines/galaga/galaga.cpp
//
// Adattamento Galaga (Files_FULL28) per framebuffer SPINNERINO.
//
// Modifiche rispetto al sorgente Galagino V3:
//   - Stride orizzontale framebuffer 224 -> 256 (frame_buffer SPINNERINO).
//     Il game resta 224 wide e viene scritto sui primi 224 px del frame_buffer
//     256 wide, lasciando 32 px neri a destra (coperti dal padding TFT).
//   - render_row chiamato per row 0..27 (28 strip = 224 tall di SPINNERINO).
//     Galaga arcade e' 224x288 (36 strip), quindi le ultime 8 righe (zona
//     ships indicator bottom) NON sono renderizzate (perdita 64 px verticali).
//   - Rimosso percorso LED_PIN (Waveshare ESP32-P4 non ha LED utente).
//   - EC11: il PULSE_HOLD di 100 ms in input.cpp tiene LEFT/RIGHT premuto
//     abbastanza a lungo che il firmware Galaga via 51xx legga input continuo.
// ============================================================================
#include "galaga.h"
#include "../../emulation/input.h"

#ifdef ENABLE_GALAGA

#define FB_W 256   // stride orizzontale framebuffer SPINNERINO

// Shift verticale arcade -> framebuffer per centrare la play area:
// Galaga arcade e' 288 tall; SPINNERINO ne mostra 256 (fb_x ∈ [0..255]).
// Con offset 32 mostriamo arcade_y ∈ [32..287]: scartiamo lo score header in
// alto e teniamo visibili la play area + l'astronave (~y=240-260) + ships
// indicator (y > 272).
#define ARCADE_Y_OFFSET 32

unsigned char galaga::opZ80(unsigned short Addr) {
  if (current_cpu == 0)      return galaga_rom_cpu1[Addr];
  else if (current_cpu == 1) return galaga_rom_cpu2[Addr];
  else                       return galaga_rom_cpu3[Addr];
}

unsigned char galaga::rdZ80(unsigned short Addr) {
  if (Addr < 16384) {
    if (current_cpu == 0)      return galaga_rom_cpu1[Addr];
    else if (current_cpu == 1) return galaga_rom_cpu2[Addr];
    else                       return galaga_rom_cpu3[Addr];
  }

  // VRAM/Sprite RAM
  if ((Addr & 0xe000) == 0x8000)
    return memory[Addr - 0x8000];

  // DIP latch
  if ((Addr & 0xfff8) == 0x6800) {
    unsigned char dip_a = (GALAGA_DIPA & (0x80 >> (Addr & 7))) ? 0 : 1;
    unsigned char dip_b = ((GALAGA_DIPB | (input->demoSoundsOff() ? 0 : GALAGA_DIPB_DEMO_SND))
                          & (0x80 >> (Addr & 7))) ? 0 : 2;
    return dip_a + dip_b;
  }

  // Namco 06xx ctrl_r
  if ((Addr & 0xfe00) == 0x7000) {
    if (Addr & 0x100) {
      return namco_busy ? 0x00 : 0x10;  // cmd ack
    } else {
      unsigned char retval = 0x00;
      if (cs_ctrl & 1) {  // 51xx selected
        if (!credit_mode) {
          unsigned char map71[] = { 0b11111111, 0xff, 0xff };
          if (namco_cnt > 2) return 0xff;
          retval = map71[namco_cnt];
        } else {
          static unsigned char prev_mask = 0;
          static unsigned char fire_timer = 0;
          unsigned char mapb1[] = {
            (unsigned char)(16 * (credit / 10) + credit % 10),
            0b11111111, 0b11111111
          };
          unsigned char keymask = input->buttons_get();

          if (keymask & BUTTON_LEFT)  mapb1[1] &= ~0x08;
          if (keymask & BUTTON_UP)    mapb1[1] &= ~0x04;
          if (keymask & BUTTON_RIGHT) mapb1[1] &= ~0x02;
          if (keymask & BUTTON_DOWN)  mapb1[1] &= ~0x01;

          if ((keymask & BUTTON_FIRE) && !(prev_mask & BUTTON_FIRE)) {
            mapb1[1] &= ~0x10;
            fire_timer = 1;
          } else if (fire_timer) {
            mapb1[1] &= ~0x10;
            fire_timer--;
          }

          if ((keymask & BUTTON_START) && !(prev_mask & BUTTON_START) && credit)
            credit -= 1;
          if ((keymask & BUTTON_COIN) && !(prev_mask & BUTTON_COIN) && (credit < 99))
            credit += 1;

          if (namco_cnt > 2) return 0xff;
          retval = mapb1[namco_cnt];
          prev_mask = keymask;
        }
        namco_cnt++;
      }
      return retval;
    }
  }
  return 0xff;
}

void galaga::wrZ80(unsigned short Addr, unsigned char Value) {
  if (Addr < 16384) return;

  if ((Addr & 0xe000) == 0x8000) {
    memory[Addr - 0x8000] = Value;
  }

  // Namco 06xx
  if ((Addr & 0xf800) == 0x7000) {
    if (Addr & 0x100) {  // 7100
      namco_cnt = 0;
      cs_ctrl = Value;
      namco_busy = 5000;
      if (Value == 0xa8) trigger_sound_explosion();
    } else {              // 7000
      if (cs_ctrl & 1) {
        if (coincredMode) { coincredMode--; return; }
        switch (Value) {
          case 1: coincredMode = 4; break;
          case 2: credit_mode = 1;  break;
          case 3: case 4: break;
          case 5: credit_mode = 0;  break;
        }
        namco_cnt++;
      }
    }
  }

  // Sound regs (Namco WSG)
  if ((Addr & 0xffe0) == 0x6800) {
    int offset = Addr - 0x6800;
    Value &= 0x0f;
    if (soundregs[offset] != Value)
      soundregs[offset] = Value;
    return;
  }

  // IRQ enable / sub-CPU reset
  if ((Addr & 0xfffc) == 0x6820) {
    if ((Addr & 3) < 3)
      irq_enable[Addr & 3] = Value;
    else {
      sub_cpu_reset = !Value;
      credit_mode = 0;
      if (sub_cpu_reset) {
        current_cpu = 1; ResetZ80(&cpu[1]);
        current_cpu = 2; ResetZ80(&cpu[2]);
      }
    }
    return;
  }

  if (Value == 0 && Addr == 0x8210) {
    game_started = 1;
  }

  // Star control (a000-a007)
  if ((Addr & 0xfff0) == 0xa000) {
    if (Value & 1)  starcontrol |=  (1 << (Addr & 7));
    else            starcontrol &= ~(1 << (Addr & 7));
    return;
  }
}

void galaga::run_frame(void) {
  for (int i = 0; i < INST_PER_FRAME; i++) {
    current_cpu = 0;
    StepZ80(&cpu[0]); StepZ80(&cpu[0]); StepZ80(&cpu[0]); StepZ80(&cpu[0]);
    if (!sub_cpu_reset) {
      current_cpu = 1;
      StepZ80(&cpu[1]); StepZ80(&cpu[1]); StepZ80(&cpu[1]); StepZ80(&cpu[1]);
      current_cpu = 2;
      StepZ80(&cpu[2]); StepZ80(&cpu[2]); StepZ80(&cpu[2]); StepZ80(&cpu[2]);
    }

    if (namco_busy) namco_busy--;

    if ((cs_ctrl & 0xe0) != 0) {
      if (nmi_cnt < (cs_ctrl >> 5) * 64) {
        nmi_cnt++;
      } else {
        current_cpu = 0;
        IntZ80(&cpu[0], INT_NMI);
        nmi_cnt = 0;
      }
    }

    if (!sub_cpu_reset && !irq_enable[2] &&
        ((i == INST_PER_FRAME / 4) || (i == 3 * INST_PER_FRAME / 4))) {
      current_cpu = 2;
      IntZ80(&cpu[2], INT_NMI);
    }
  }

  if (irq_enable[0]) {
    current_cpu = 0;
    IntZ80(&cpu[0], INT_RST38);
  }
  if (!sub_cpu_reset && irq_enable[1]) {
    current_cpu = 1;
    IntZ80(&cpu[1], INT_RST38);
  }
}

void galaga::prepare_frame(void) {
  active_sprites = 0;
  for (int idx = 0; idx < 64 && active_sprites < 92; idx++) {
    unsigned char *sprite_base_ptr = memory + 2 * (63 - idx);
    if ((sprite_base_ptr[0x1b80 + 1] & 2) == 0) {
      struct sprite_S spr;
      spr.code  = sprite_base_ptr[0x0b80];
      spr.color = sprite_base_ptr[0x0b80 + 1];
      spr.flags = sprite_base_ptr[0x1b80];
      spr.x = sprite_base_ptr[0x1380] - 16;
      spr.y = sprite_base_ptr[0x1380 + 1] +
              0x100 * (sprite_base_ptr[0x1b80 + 1] & 1) - 40;

      if ((spr.code < 128) &&
          (spr.y > -16) && (spr.y < 288) &&
          (spr.x > -16) && (spr.x < 224)) {
        sprite[active_sprites] = spr;
        if (spr.flags & 0x08) sprite[active_sprites].code += 2;
        active_sprites++;
      }

      if ((spr.flags & 0x08) &&
          (spr.y > -16) && (spr.y < 288) &&
          ((spr.x + 16) >= -16) && ((spr.x + 16) < 224)) {
        sprite[active_sprites] = spr;
        sprite[active_sprites].x += 16;
        active_sprites++;
      }

      if ((spr.flags & 0x04) &&
          ((spr.y + 16) > -16) && ((spr.y + 16) < 288) &&
          (spr.x > -16) && (spr.x < 224)) {
        sprite[active_sprites] = spr;
        sprite[active_sprites].code += 3;
        sprite[active_sprites].y += 16;
        active_sprites++;
      }

      if (((spr.flags & 0x0c) == 0x0c) &&
          ((spr.y + 16) > -16) && ((spr.y + 16) < 288) &&
          ((spr.x + 16) > -16) && ((spr.x + 16) < 224)) {
        sprite[active_sprites] = spr;
        sprite[active_sprites].code += 1;
        sprite[active_sprites].x += 16;
        sprite[active_sprites].y += 16;
        active_sprites++;
      }
    }
  }

  static const signed char speeds[8] = { -1, -2, -3, 0, 3, 2, 1, 0 };
  stars_scroll_y += 2 * speeds[starcontrol & 7];
}

void galaga::trigger_sound_explosion(void) {
  snd_boom_cnt = 2 * sizeof(galaga_sample_boom);
  snd_boom_ptr = (const signed char*)galaga_sample_boom;
}

// ============================================================================
// RENDER TRASPOSTO (portrait per utente SPINNERINO)
//
// Mapping arcade Galaga (224 wide x 288 tall portrait) -> frame_buffer
// SPINNERINO (256 wide x 224 tall landscape, ruotato MV in portrait al user):
//
//   fb_x = arcade_y - ARCADE_Y_OFFSET    (verticale arcade, ARCADE_Y_OFFSET=32
//                                         cosi' visualizziamo arcade_y ∈ [32..287])
//   fb_y = 223 - arcade_x                (orizzontale arcade, reversed)
//
// render_row(strip_r) per strip_r ∈ [0..27]:
//   - copre fb_y ∈ [strip_r*8 .. strip_r*8+7]
//   - corrisponde ad arcade_x ∈ [(27-strip_r)*8 .. (27-strip_r)*8+7]
//     cioè una SINGOLA colonna arcade col_arcade = 27 - strip_r
//   - fb_x copre tutto [0..255] = arcade_y ∈ [32..287]
//
// Perdita: arcade_y ∈ [0..31] = 32 px top (zona score header), invece del
// bottom che invece contiene player + ships indicator (importanti).
// ============================================================================

// 8x8 tile - render trasposto. Strip_r fissa la colonna arcade (col=27-strip_r),
// row_arcade ∈ [0..31] itera sulle righe arcade del game (perde row 32..35).
void galaga::blit_tile_t(short strip_r, char row_arcade) {
  char col_arcade = 27 - strip_r;
  unsigned short addr = tileaddr[row_arcade][col_arcade];
  if (memory[addr] == 0x24) return;  // skip blank tile

  const unsigned short *tile   = galaga_tilemap[memory[addr]];
  const unsigned short *colors = galaga_colormap_tiles[memory[0x400 + addr] & 63];

  int fb_x_base = row_arcade * 8 - ARCADE_Y_OFFSET;  // arcade_y -> fb_x
  if (fb_x_base + 8 <= 0 || fb_x_base >= FB_W) return;

  // py = y_in_tile (0..7) -> arcade_y offset -> fb_x offset
  // px = x_in_tile (0..7) -> arcade_x offset -> fb_y_strip = 7 - px (reversed)
  for (int py = 0; py < 8; py++) {
    int fb_x = fb_x_base + py;
    if (fb_x < 0)      continue;
    if (fb_x >= FB_W)  break;

    unsigned short pix = tile[py];
    // pix LSB-first: c=0 -> px=0 (leftmost arcade tile, bottom strip)
    for (int px = 0; px < 8; px++, pix >>= 2) {
      unsigned char pen = pix & 3;
      if (pen) {
        int fb_y_strip = 7 - px;
        frame_buffer[fb_y_strip * FB_W + fb_x] = colors[pen];
      }
    }
  }
}

// 16x16 sprite - render trasposto.
// Lo sprite tocca strip_r se il suo arcade_x range [sx..sx+15] interseca
// la colonna arcade della strip [col_arcade*8 .. col_arcade*8+7].
void galaga::blit_sprite_t(short strip_r, unsigned char s) {
  const sprite_S *sp = &sprite[s];

  int col_arcade = 27 - strip_r;
  int strip_arcade_x_lo = col_arcade * 8;
  int strip_arcade_x_hi = strip_arcade_x_lo + 7;

  // Range di sx (colonna nel sprite) che cade nella strip
  int sx_min = strip_arcade_x_lo - sp->x;
  int sx_max = strip_arcade_x_hi - sp->x;
  if (sx_min < 0)  sx_min = 0;
  if (sx_max > 15) sx_max = 15;
  if (sx_min > sx_max) return;

  const unsigned long  *spr    = galaga_sprites[sp->flags & 3][sp->code];
  const unsigned short *colors = galaga_colormap_sprites[sp->color & 63];
  if (colors[0] != 0) return;  // colormap entry non valida

  // Per ogni riga sprite (sy = arcade_y offset)
  for (int sy = 0; sy < 16; sy++) {
    int fb_x = sp->y + sy - ARCADE_Y_OFFSET;
    if (fb_x < 0 || fb_x >= FB_W) continue;

    unsigned long pix = spr[sy];

    for (int sx = sx_min; sx <= sx_max; sx++) {
      unsigned char pen = (unsigned char)((pix >> (sx * 2)) & 3);
      if (pen) {
        unsigned short col = colors[pen];
        if (col) {
          // fb_y_strip = (223 - (sp->x + sx)) - strip_r*8
          int fb_y_strip = (223 - sp->x - sx) - strip_r * 8;
          frame_buffer[fb_y_strip * FB_W + fb_x] = col;
        }
      }
    }
  }
}

// Star pattern - render trasposto.
void galaga::render_stars_set_t(short strip_r, const struct galaga_star *set) {
  int col_arcade = 27 - strip_r;
  int strip_arcade_x_lo = col_arcade * 8;
  int strip_arcade_x_hi = strip_arcade_x_lo + 7;

  for (char star_cntr = 0; star_cntr < 63; star_cntr++) {
    const struct galaga_star *s = set + star_cntr;
    int arcade_x = (244 - s->x) & 0xff;
    if (arcade_x < strip_arcade_x_lo || arcade_x > strip_arcade_x_hi) continue;

    // arcade_y nel sorgente ha offset +16 (zona top non-game)
    int arcade_y = ((s->y + stars_scroll_y) & 0xff) + 16;
    int fb_x = arcade_y - ARCADE_Y_OFFSET;
    if (fb_x < 0 || fb_x >= FB_W) continue;
    int fb_y_strip = (223 - arcade_x) - strip_r * 8;
    frame_buffer[fb_y_strip * FB_W + fb_x] = s->col;
  }
}

void galaga::render_row(short row) {
  // SPINNERINO: 28 strip × 8 = 224 tall (fb_y).
  // Render trasposto: ogni strip copre 1 colonna arcade (col = 27-row) e 32
  // righe arcade (row_arcade 0..31, le ultime 4 sono fuori frame_buffer).
  if (row < 0 || row >= 28) return;

  // Stars
  if (starcontrol & 0x20) {
    render_stars_set_t(row, galaga_star_set[(starcontrol & 0x08) ? 1 : 0]);
    render_stars_set_t(row, galaga_star_set[(starcontrol & 0x10) ? 3 : 2]);
  }

  // Sprite visibili nella strip (in coords arcade_x)
  int col_arcade = 27 - row;
  int strip_arcade_x_lo = col_arcade * 8;
  int strip_arcade_x_hi = strip_arcade_x_lo + 7;
  for (unsigned char s = 0; s < active_sprites; s++) {
    if (((sprite[s].x + 15) >= strip_arcade_x_lo) &&
        (sprite[s].x         <= strip_arcade_x_hi)) {
      blit_sprite_t(row, s);
    }
  }

  // Tiles: 36 row arcade per la singola colonna arcade della strip;
  // blit_tile_t fa early-return per i tile fuori da fb_x ∈ [0..255]
  for (char row_arcade = 0; row_arcade < 36; row_arcade++) {
    blit_tile_t(row, row_arcade);
  }
}

const signed char *galaga::waveRom(unsigned char value) {
  return galaga_wavetable[value];
}

const unsigned short *galaga::logo(void) {
  return galaga_logo;
}

#endif // ENABLE_GALAGA
