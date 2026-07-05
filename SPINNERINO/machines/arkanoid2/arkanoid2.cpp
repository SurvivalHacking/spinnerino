// ============================================================================
// ARKANOID 2 - GALAGONE_4.3_P4_NEW_multi
// FASE 1B-3: SD ROM load + Z80 dual + X1-001 sprite rendering + ROT90 CW.
// Audio YM2203 e paddle EC11 nelle fasi successive (1B-4, 1B-5).
//
// Hardware: Taito TNZS, 2x Z80 + YM2203 + Seta X1-001
// ============================================================================
#include "arkanoid2.h"

#ifdef ENABLE_ARKANOID2
// SPINNERINO P4: ROM in flash via .h (NON SD come P4 multi)
#include "arkanoid2_rom_main.h"
#include "arkanoid2_rom_sub.h"
#include "arkanoid2_palette.h"
#include "arkanoid2_gfx_decoded.h"
#include "arkanoid2_logo.h"
#include "../../emulation/input.h"

// Sensibilita' paddle SPINNERINO (ridotta dal P4 multi):
// ARK2_EC11_STEP=2 (era 3): compensa l'aumento di sensibilita' dovuto al
// passaggio da ec11_paddle_x (step EC11_PADDLE_STEP) a ec11_dial_counter
// (step 6). Vedi fix bug "paddle non va a sinistra alla 2a vita" 2026-05-23.
#define ARK2_EC11_STEP   2
#define ARK2_JOY_STEP    8

#define ARK2_FB_W       224     // display portrait width
#define ARK2_SCREEN_W   256     // MAME native width
#define ARK2_SCREEN_H   224     // MAME visible area height
#define ARK2_DISP_TOP_BLANK 2
#define ARK2_DISP_VIS_ROWS  32

// CPU timing MAME real: main 6 MHz, sub 6 MHz.
// Sub a ITER 4200 = ~100k cycles/frame = MAME-accurate.
// Aumentare oltre saturerebbe CPU P4 → frame rate scende → BGM rallenta
// invece di accelerare.
#define ARK2_MAIN_ITER_PER_FRAME 4200
#define ARK2_SUB_ITER_PER_FRAME  4200

// Shift = 0: scratch vblank-stripped, formula sy_a = 240-X mappa MAME
// visible y=16..239 a scratch_y=0..223.
#define ARK2_VIEW_SHIFT_LEFT  0

// =========================================================================
// SNAPSHOT_STABLE_v4 (ULTIMO BUONO):
//   ARK2_SCREEN_H        = 224
//   ARK2_VIEW_SHIFT_LEFT = 0
//   FG sprite formula    = (240 - 35) - ((sy_raw - 0x12) & 0xFF)
//                         (shift uniforme 35 px LEFT su tutti gli FG)
//   FG_LEFT_SHIFT        = 35
//   BG_XOFFS             = 0
//   BG_YOFFS             = 1   (MAME default)
//   BG sy_a              = (sy & 0xFF) - 16
//   Paddle: NESSUN clamp (uPD4701 = delta encoder)
// Stato: FG shifted 35 px LEFT, BG MAME-accurate, paddle libero in
// entrambe le direzioni. Paddle può "uscire" dal muro destro visualmente
// ma è recuperabile ruotando l'encoder a sinistra.
// =========================================================================
//
// SNAPSHOT_STABLE_v3 (precedente, con clamp paddle):
//   PADDLE_MAX = 0x800 in prepare_frame.
//   Problema: a saturazione, paddle non rispondeva al ritorno verso sinistra.
// =========================================================================
//
// SNAPSHOT_STABLE_v2 (precedente, alternativa per rollback):
//   ARK2_SCREEN_H        = 224
//   ARK2_VIEW_SHIFT_LEFT = 0
//   FG sprite formula    = 240 - ((sy_raw - 0x12) & 0xFF)
//                         poi -35 (FG_LEFT_SHIFT) se sx_a < FG_HUD_TOP_X
//   FG_HUD_TOP_X         = 240
//   FG_LEFT_SHIFT        = 35   (calibrazione utente: 16+16+4-2+1 = 35)
//   BG_XOFFS             = 0
//   BG_YOFFS             = 1   (MAME default)
//   BG sy_a              = (sy & 0xFF) - 16
// Stato v2: HUD top (1UP/HIGH SCORE) MAME-accurate; punteggio sotto +
//   mattoni + paddle/palla shiftati 35 px LEFT. Le lettere multi-sprite
//   (es. "ARKANOID" titolo, "D" finale) si spezzavano sulla threshold 240.
// =========================================================================

// ----------------------------------------------------------------------------
// MCU 8042 stub (port da MAME taito/tnzs.cpp arknoid2_state)
// ----------------------------------------------------------------------------
void Arkanoid2::mcu_reset_state() {
  mcu_initializing  = 3;
  mcu_command       = 0;
  mcu_credits       = 0;
  mcu_readcredits   = 0;
  mcu_reportcoin    = 0;
  mcu_coinage_init  = 0;
  mcu_coinage[0] = 1;
  mcu_coinage[1] = 1;
  mcu_coinage[2] = 1;
  mcu_coinage[3] = 1;
  mcu_coins_a    = 0;
  mcu_coins_b    = 0;
  mcu_insertcoin = 0;
}

void Arkanoid2::mcu_handle_coins(int coin) {
  if (coin & 0x08) {
    mcu_reportcoin = coin;
  } else if (coin && coin != mcu_insertcoin) {
    if (coin & 0x01) {
      mcu_coins_a++;
      if (mcu_coins_a >= mcu_coinage[0]) {
        mcu_coins_a -= mcu_coinage[0];
        mcu_credits += mcu_coinage[1];
        if (mcu_credits >= 9) mcu_credits = 9;
      }
    }
    if (coin & 0x02) {
      mcu_coins_b++;
      if (mcu_coins_b >= mcu_coinage[2]) {
        mcu_coins_b -= mcu_coinage[2];
        mcu_credits += mcu_coinage[3];
        if (mcu_credits >= 9) mcu_credits = 9;
      }
    }
    if (coin & 0x04) mcu_credits++;
    mcu_reportcoin = coin;
  } else {
    mcu_reportcoin = 0;
  }
  mcu_insertcoin = coin;
}

// ----------------------------------------------------------------------------
// Init / Reset / FreeRoms
// ----------------------------------------------------------------------------
void Arkanoid2::init(Input *in, unsigned short *fb,
                     sprite_S *sb, unsigned char *mem) {
  machineBase::init(in, fb, sb, mem);
}

void Arkanoid2::reset() {
  // SPINNERINO P4: ROM in flash via .h (cache CPU mappa direttamente)
  if (!rom_main)    rom_main    = arkanoid2_rom_main;
  if (!rom_sub)     rom_sub     = arkanoid2_rom_sub;
  if (!palette) {
    palette = (const unsigned short*)arkanoid2_palette;
    for (int i = 0; i < 512; i++) palette_rgb565[i] = palette[i];
  }
  if (!gfx_decoded) gfx_decoded = arkanoid2_gfx_decoded;

  // Allocazioni RAM (DRAM-first, fallback PSRAM)
  if (!main_ram) {
    main_ram = (unsigned char*)heap_caps_malloc(0x4000,
                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!main_ram) main_ram = (unsigned char*)ps_malloc(0x4000);
  }
  if (main_ram) memset(main_ram, 0, 0x4000);

  if (!sub_ram) {
    sub_ram = (unsigned char*)heap_caps_malloc(0x1000,
                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!sub_ram) sub_ram = (unsigned char*)ps_malloc(0x1000);
  }
  if (sub_ram) memset(sub_ram, 0, 0x1000);

  if (!shared_ram) {
    shared_ram = (unsigned char*)heap_caps_malloc(0x1000,
                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!shared_ram) shared_ram = (unsigned char*)ps_malloc(0x1000);
  }
  if (shared_ram) memset(shared_ram, 0, 0x1000);

  // X1-001 sprite RAM (16 KB tot: 8K visibili + 8K shadow per buffer flip)
  if (!spritecode) {
    spritecode = (unsigned short*)heap_caps_malloc(
        0x2000 * sizeof(unsigned short),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!spritecode)
      spritecode = (unsigned short*)ps_malloc(0x2000 * sizeof(unsigned short));
  }
  // MAME init: spritecode 0xFFFF
  if (spritecode) {
    for (int i = 0; i < 0x2000; i++) spritecode[i] = 0xFFFF;
  }

  if (!spriteylow) {
    spriteylow = (unsigned char*)heap_caps_malloc(0x300,
                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!spriteylow) spriteylow = (unsigned char*)ps_malloc(0x300);
  }
  if (spriteylow) memset(spriteylow, 0xFF, 0x300);

  // Frame scratch 256×224 RGB565 (112 KB).
  // PRIMA SRAM interna (latenza ~3x inferiore alla PSRAM), PSRAM solo fallback.
  // L'azzeramento + blit BG/FG + 28x rilettura per render_row e' il path piu'
  // hot dell'intera macchina: in PSRAM costa ~10ms/frame extra → gioco lento.
  if (!frame_scratch) {
    frame_scratch = (unsigned short*)heap_caps_malloc(
        ARK2_SCREEN_W * ARK2_SCREEN_H * sizeof(unsigned short),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (frame_scratch) {
      Serial.println(F("[ARK2] frame_scratch in SRAM interna (fast)"));
    } else {
      frame_scratch = (unsigned short*)ps_malloc(
          ARK2_SCREEN_W * ARK2_SCREEN_H * sizeof(unsigned short));
      if (frame_scratch)
        Serial.println(F("[ARK2] frame_scratch fallback PSRAM (slow!)"));
    }
  }

  memset(sprite_ctrl, 0, sizeof(sprite_ctrl));
  sprite_bg_flag = 0;
  frame_counter = 0;
  ym2203_addr = 0;
  mcu_reset_state();

  machineBase::reset();   // resetta tutti i Z80 e soundregs

  // Azzera registri YM2203 e key-on FM: necessario per evitare loop audio
  // residuo quando si torna al menu boot (il task audio dedicato continua a
  // renderizzare basandosi su questi registri se non azzerati).
  memset(ym_regs,  0, sizeof(ym_regs));
  memset(fm_keyed, 0, sizeof(fm_keyed));

  // MAME default main_bank=2 (ROM 0x8000+); sub in reset finche' main scrive bit 4
  main_bank = 2;
  sub_bank  = 0;
  sub_running = false;    // attivato dal main quando scrive 0xF600 bit 4
}

void Arkanoid2::freeRoms() {
  rom_main = nullptr;
  rom_sub = nullptr;
  palette = nullptr;
  gfx_decoded = nullptr;
  // RAM e scratch restano allocate (riusati al prossimo reset)
}

const unsigned short *Arkanoid2::logo(void) {
  return arkanoid2_logo;
}

// ----------------------------------------------------------------------------
// Z80 memory map (current_cpu: 0=main, 1=sub)
// ----------------------------------------------------------------------------
unsigned char Arkanoid2::opZ80(unsigned short Addr) {
  return rdZ80(Addr);
}

unsigned char Arkanoid2::rdZ80(unsigned short Addr) {
  if (current_cpu == 1) {
    // ── SUB Z80 ──
    if (Addr < 0x8000) return rom_sub ? rom_sub[Addr] : 0xFF;
    if (Addr >= 0x8000 && Addr <= 0x9FFF) {
      uint32_t off = 0x8000 + (uint32_t)(sub_bank & 0x03) * 0x2000 + (Addr - 0x8000);
      if (rom_sub && off < 65536) return rom_sub[off];
      return 0xFF;
    }
    if (Addr == 0xA000) return 0xFF;
    // YM2203 status (0xB000): bit 0 = timer A overflow, bit 1 = timer B overflow
    if (Addr == 0xB000) {
      unsigned char status = 0;
      if (timer_a_overflow) status |= 0x01;
      if (timer_b_overflow) status |= 0x02;
      return status;
    }
    if (Addr == 0xB001) return 0xFF;
    // MCU 8042 stub (port da MAME arknoid2_state::mcu_r)
    if (Addr == 0xC000) {
      // 0xC000 = MCU data port
      if (mcu_initializing) {
        static const unsigned char mcu_startup[] = { 0x55, 0xAA, 0x5A };
        mcu_initializing--;
        return mcu_startup[2 - mcu_initializing];
      }
      switch (mcu_command) {
        case 0x41: return mcu_credits;
        case 0xC1:
          if (mcu_readcredits == 0) {
            mcu_readcredits = 1;
            if (mcu_reportcoin & 0x08) {
              mcu_initializing = 3;
              return 0xEE;   // tilt
            }
            return mcu_credits;
          }
          // Buttons IN0: bit 4 = FIRE (LOW), bit 7 = START (LOW)
          {
            unsigned char b = input ? input->buttons_get() : 0;
            unsigned char v = 0xFF;
            if (b & BUTTON_FIRE)  v &= ~(1 << 4);
            if (b & BUTTON_START) v &= ~(1 << 7);
            return v;
          }
        default:
          return 0xFF;
      }
    }
    if (Addr == 0xC001) {
      // 0xC001 = MCU status
      if (mcu_reportcoin & 0x08) return 0xE1;
      if (mcu_reportcoin & 0x01) return 0x11;
      if (mcu_reportcoin & 0x02) return 0x21;
      if (mcu_reportcoin & 0x04) return 0x31;
      return 0x01;
    }
    if (Addr >= 0xD000 && Addr <= 0xDFFF)
      return sub_ram ? sub_ram[Addr - 0xD000] : 0xFF;
    if (Addr >= 0xE000 && Addr <= 0xEFFF)
      return shared_ram ? shared_ram[Addr - 0xE000] : 0xFF;
    // uPD4701 paddle X/Y read (12-bit signed delta accumulato)
    if (Addr >= 0xF000 && Addr <= 0xF003) {
      int paddle = paddle_pos & 0x0FFF;             // 12-bit
      switch (Addr) {
        case 0xF000: return paddle & 0xFF;
        case 0xF001: return (paddle >> 8) & 0x0F;
        case 0xF002: return 0;                       // Y axis non usato
        case 0xF003: return 0;
      }
    }
    return 0xFF;
  }

  // ── MAIN Z80 ──
  if (Addr < 0x8000)
    return rom_main ? rom_main[Addr] : 0xFF;

  if (Addr >= 0x8000 && Addr <= 0xBFFF) {
    if (main_bank <= 1) {
      return main_ram ? main_ram[Addr - 0x8000] : 0xFF;
    } else {
      uint32_t off = (uint32_t)main_bank * 0x4000 + (Addr - 0x8000);
      if (rom_main && off < 65536) return rom_main[off];
      return 0xFF;
    }
  }

  // X1-001 sprite RAM
  if (Addr >= 0xC000 && Addr <= 0xCFFF) {
    return spritecode ? (spritecode[Addr - 0xC000] & 0xFF) : 0xFF;
  }
  if (Addr >= 0xD000 && Addr <= 0xDFFF) {
    return spritecode ? ((spritecode[Addr - 0xD000] >> 8) & 0xFF) : 0xFF;
  }
  if (Addr >= 0xE000 && Addr <= 0xEFFF)
    return shared_ram ? shared_ram[Addr - 0xE000] : 0xFF;
  if (Addr >= 0xF000 && Addr <= 0xF2FF)
    return spriteylow ? spriteylow[Addr - 0xF000] : 0xFF;
  if (Addr >= 0xF300 && Addr <= 0xF3FF)
    return sprite_ctrl[Addr & 0x03];
  if (Addr == 0xF400) return sprite_bg_flag;
  // 0xF800-0xFBFF palette PROM read: irrilevante (palette gestita interna)
  return 0xFF;
}

void Arkanoid2::wrZ80(unsigned short Addr, unsigned char Value) {
  if (current_cpu == 1) {
    // ── SUB Z80 ──
    if (Addr < 0x8000) return;
    if (Addr >= 0x8000 && Addr <= 0x9FFF) return;
    if (Addr == 0xA000) {
      sub_bank = Value & 0x03;
      if (Value & 0x04) mcu_reset_state();    // MCU reset trigger
      return;
    }
    if (Addr == 0xB000) {
      ym2203_addr = Value;
      return;
    }
    if (Addr == 0xB001) {
      unsigned char reg = ym2203_addr;
      ym_regs[0][reg] = Value;
      // PSG (regs 0..13) duplicato in soundregs[] per il path AY render
      if (reg < 14) soundregs[reg] = Value;
      // FM key-on (reg 0x28): bit 4..7 = ops keyed, bit 0..1 = channel
      if (reg == 0x28) {
        unsigned char ch = Value & 0x03;
        if (ch < 3) fm_keyed[0][ch] = (Value >> 4) & 0x0F;
      }
      // Timer A high (reg 0x24): bit 9..2 di period
      if (reg == 0x24) {
        timer_a_period = (timer_a_period & 0x03) | ((unsigned)Value << 2);
      }
      // Timer A low (reg 0x25): bit 1..0 di period
      if (reg == 0x25) {
        timer_a_period = (timer_a_period & 0x3FC) | (Value & 0x03);
      }
      // Timer B period (reg 0x26)
      if (reg == 0x26) timer_b_period = Value;
      // Timer A/B control (reg 0x27)
      if (reg == 0x27) {
        if (Value & 0x10) timer_a_overflow = false;   // reset flag A
        if (Value & 0x20) timer_b_overflow = false;   // reset flag B
        bool prev_a = timer_a_started;
        bool prev_b = timer_b_started;
        timer_a_started = (Value & 0x01) != 0;
        timer_a_enabled = (Value & 0x04) != 0;
        timer_b_started = (Value & 0x02) != 0;
        timer_b_enabled = (Value & 0x08) != 0;
        if (timer_a_started && !prev_a) timer_a_count = 0;
        if (timer_b_started && !prev_b) timer_b_count = 0;
      }
      return;
    }
    // MCU 8042 stub (port da MAME arknoid2_state::mcu_w)
    if (Addr == 0xC000) {
      // 0xC000 = MCU data port
      if (mcu_command == 0x41)
        mcu_credits = (mcu_credits + Value) & 0xFF;
      return;
    }
    if (Addr == 0xC001) {
      // 0xC001 = MCU command
      if (mcu_initializing) {
        if (mcu_coinage_init < 4) mcu_coinage[mcu_coinage_init++] = Value;
      }
      if (Value == 0xC1) mcu_readcredits = 0;
      if (Value == 0x15) {
        if (mcu_credits) mcu_credits--;
      }
      mcu_command = Value;
      return;
    }
    if (Addr >= 0xD000 && Addr <= 0xDFFF) {
      if (sub_ram) sub_ram[Addr - 0xD000] = Value; return;
    }
    if (Addr >= 0xE000 && Addr <= 0xEFFF) {
      if (shared_ram) shared_ram[Addr - 0xE000] = Value; return;
    }
    return;
  }

  // ── MAIN Z80 ──
  if (Addr < 0x8000) return;

  if (Addr >= 0x8000 && Addr <= 0xBFFF) {
    if (main_bank <= 1 && main_ram) {
      main_ram[Addr - 0x8000] = Value;
    }
    return;
  }

  // X1-001 sprite code low (0xC000-0xCFFF): byte basso di spritecode[off]
  if (Addr >= 0xC000 && Addr <= 0xCFFF) {
    if (spritecode) {
      unsigned off = Addr - 0xC000;
      spritecode[off] = (spritecode[off] & 0xFF00) | Value;
    }
    return;
  }
  // X1-001 sprite code high (0xD000-0xDFFF): byte alto
  if (Addr >= 0xD000 && Addr <= 0xDFFF) {
    if (spritecode) {
      unsigned off = Addr - 0xD000;
      spritecode[off] = (spritecode[off] & 0x00FF) | ((unsigned short)Value << 8);
    }
    return;
  }

  if (Addr >= 0xE000 && Addr <= 0xEFFF) {
    if (shared_ram) shared_ram[Addr - 0xE000] = Value;
    return;
  }
  if (Addr >= 0xF000 && Addr <= 0xF2FF) {
    if (spriteylow) spriteylow[Addr - 0xF000] = Value;
    return;
  }
  if (Addr >= 0xF300 && Addr <= 0xF3FF) {
    sprite_ctrl[Addr & 0x03] = Value;
    return;
  }
  if (Addr == 0xF400) {
    sprite_bg_flag = Value;
    return;
  }
  if (Addr == 0xF600) {
    main_bank = Value & 0x07;
    bool new_sub_running = (Value & 0x10) != 0;
    if (new_sub_running && !sub_running) {
      ResetZ80(&cpu[1]);   // rising edge: sub parte da PC=0
    }
    sub_running = new_sub_running;
    if (!new_sub_running) {
      ResetZ80(&cpu[1]);
    }
    return;
  }
}

unsigned char Arkanoid2::inZ80(unsigned short Port)                    { return 0xFF; }
void          Arkanoid2::outZ80(unsigned short Port, unsigned char V)  { }

// ============================================================================
// X1-001 sprite engine (port da MAME devices/video/x1_001.cpp)
// ============================================================================

// Tile lookup: gfx_decoded e' 1 byte per pixel, 16x16 = 256 byte per tile.
static inline const uint8_t *ark2_tile_pixels_ptr(const uint8_t *gfx, uint16_t tile) {
  return &gfx[tile * 256];
}

static void ark2_blit_tile_scratch(unsigned short *scratch,
                                   const uint8_t *gfx,
                                   const unsigned short *palette,
                                   uint16_t tile, int x, int y,
                                   uint8_t color_base, bool flipx, bool flipy,
                                   uint8_t transpen) {
  if (y >= ARK2_SCREEN_H || y + 16 <= 0) return;
  if (x >= ARK2_SCREEN_W || x + 16 <= 0) return;
  const uint8_t *tile_pix = ark2_tile_pixels_ptr(gfx, tile);
  unsigned pal_off = ((unsigned)color_base << 4);
  int dx_lo = (x < 0) ? -x : 0;
  int dx_hi = (x + 16 > ARK2_SCREEN_W) ? (ARK2_SCREEN_W - x) : 16;
  for (int dy = 0; dy < 16; dy++) {
    int sy = y + dy;
    if (sy < 0 || sy >= ARK2_SCREEN_H) continue;
    int src_y = flipy ? (15 - dy) : dy;
    const uint8_t *row_pix = &tile_pix[src_y * 16];
    unsigned short *row = &scratch[sy * ARK2_SCREEN_W + x];
    if (!flipx) {
      for (int dx = dx_lo; dx < dx_hi; dx++) {
        uint8_t pix = row_pix[dx];
        if (pix != transpen) row[dx] = palette[pal_off | pix];
      }
    } else {
      for (int dx = dx_lo; dx < dx_hi; dx++) {
        uint8_t pix = row_pix[15 - dx];
        if (pix != transpen) row[dx] = palette[pal_off | pix];
      }
    }
  }
}

// MAME tnzs_eof: buffer flip a fine vblank (X1-001)
static void ark2_tnzs_eof(unsigned short *spritecode, uint8_t ctrl2) {
  if (~ctrl2 & 0x20) {
    if (ctrl2 & 0x40) {
      for (int i = 0; i < 0x400; i++) spritecode[i] = spritecode[0x0800 + i];
    } else {
      for (int i = 0; i < 0x400; i++) spritecode[0x0800 + i] = spritecode[i];
    }
    for (int i = 0; i < 0x0400; i++) spritecode[0x0400 + i] = spritecode[0x0C00 + i];
  }
}

static void ark2_draw_foreground(unsigned short *spritecode,
                                 unsigned char *spriteylow,
                                 unsigned char *spritectrl,
                                 const unsigned char *gfx,
                                 const unsigned short *palette,
                                 unsigned short *scratch) {
  // Port MAME-fedele da Paolo (GALAGONE_4.3_P4_NEW_multi/arknoid2.cpp).
  // MAME draw_foreground: screen_y_top = (max_y - (sy_raw + yoffs)) & 0xFF
  //   max_y = 256, yoffs = 0x0E (set_fg_yoffsets noflip)
  //   => screen_y_top = (256 - sy_raw - 14) & 0xFF = (242 - sy_raw) & 0xFF
  // Scratch SPINNERINO è MAME visible vblank-stripped (mame_y 16..239 -> scratch 0..223),
  // quindi scratch_y = mame_y_top - 16. ark2_blit_tile_scratch clippa fuori-scratch
  // automaticamente, no 4× wrap blit (fonte di ghost-pixel nel render precedente).
  uint8_t ctrl2 = spritectrl[1];
  unsigned short *char_ptr = &spritecode[0];
  unsigned short *x_ptr    = &spritecode[0x200];
  if ((ctrl2 ^ (~ctrl2 << 1)) & 0x40) {
    char_ptr += 0x800;
    x_ptr    += 0x800;
  }
  for (int i = 0x1FF; i >= 0; i--) {
    uint16_t raw = char_ptr[i];
    if (raw == 0xFFFF) continue;
    uint16_t code = raw & 0x3FFF;
    if (code >= 4096) continue;
    uint16_t xraw  = x_ptr[i];
    uint8_t  color = (xraw >> 11) & 0x1F;
    int sx_raw = (xraw & 0x00FF) - (xraw & 0x0100);   // signed 9-bit (-256..255)
    int sy_raw = spriteylow[i] & 0xFF;
    bool flipx = (raw & 0x8000) != 0;
    bool flipy = (raw & 0x4000) != 0;
    int mame_y_top = (242 - sy_raw) & 0xFF;
    int scratch_sy = mame_y_top - 16;
    ark2_blit_tile_scratch(scratch, gfx, palette, code, sx_raw, scratch_sy,
                           color, flipx, flipy, 0);
  }
}

static void ark2_draw_background(unsigned short *spritecode,
                                 unsigned char *spriteylow,
                                 unsigned char *spritectrl,
                                 unsigned char bgflag,
                                 const unsigned char *gfx,
                                 const unsigned short *palette,
                                 unsigned short *scratch) {
  // Port MAME-fedele da Paolo (GALAGONE_4.3_P4_NEW_multi/arknoid2.cpp).
  // tnzs_base set_bg_yoffsets(+1, -1) -> yoffs = -1 noflip, +1 flip.
  // X1-001 BG: numcol×0x20 entries, scrollx/scrolly per col, bank flip A↔B
  // gestito da bit 6 di sprite_ctrl[1]. Niente 4× wrap blit (fonte di
  // ghost-tile nel render SPINNERINO precedente). Niente "upper" register
  // (= sprite_ctrl[2]/[3]): in arknoid2 è sempre 0, MAME-pure non lo usa.
  // Flip ctrl bit 6 ribalta sy + flipx/flipy come in MAME draw_background.
  uint8_t ctrl  = spritectrl[0];
  uint8_t ctrl2 = spritectrl[1];
  int numcol = ctrl2 & 0x0F;
  if (numcol == 1) numcol = 16;
  if (numcol == 0) return;

  int startcol = 0;
  if (ctrl & 0x01) startcol += 0x4;
  if (ctrl & 0x02) startcol += 0x8;

  bool flip = (ctrl & 0x40) != 0;
  int yoffs = flip ? +1 : -1;

  uint16_t bank = ((ctrl2 ^ (~ctrl2 << 1)) & 0x40) ? 0x800 : 0;
  unsigned char *scrollram = &spriteylow[0x200];
  uint8_t transpen = (bgflag & 0x80) ? 0xFF : 0;

  for (int col = 0; col < numcol; col++) {
    int scrollx = scrollram[col * 0x10 + 4];
    int scrolly = scrollram[col * 0x10];
    for (int offs = 0; offs < 0x20; offs++) {
      int sx = (scrollx + (offs & 1) * 16) & 0x1FF;            // 9-bit positive
      int sy = (-(scrolly + yoffs) + (offs / 2) * 16) & 0xFF;

      int i = ((col + startcol) & 0x0F) * 32 + offs;
      uint16_t entry_code  = spritecode[(i + 0x400 + bank) & 0xFFF];
      uint16_t entry_color = spritecode[(i + 0x600 + bank) & 0xFFF];
      if (entry_code == 0xFFFF) continue;
      uint16_t code = entry_code & 0x3FFF;
      if (code >= 4096) continue;
      bool flipx = (entry_code & 0x8000) != 0;
      bool flipy = (entry_code & 0x4000) != 0;
      if (flip) { sy = 0xF0 - sy; flipx = !flipx; flipy = !flipy; }
      uint8_t color = (entry_color >> 11) & 0x1F;

      // MAME visible 16..239 -> scratch 0..223. ark2_blit_tile_scratch clippa
      // sx fuori 0..255 e scratch_sy fuori 0..223.
      int scratch_sy = sy - 16;
      ark2_blit_tile_scratch(scratch, gfx, palette, code, sx, scratch_sy,
                             color, flipx, flipy, transpen);
    }
  }
}

// ----------------------------------------------------------------------------
// Frame loop: alterna step main + sub con interleave 8 slice; IRQ a slice 6
// ----------------------------------------------------------------------------
void Arkanoid2::prepare_frame(void) {
  // CRITICO: buttons_get() ATTIVA il long-press START 4s detection in
  // input.cpp. Senza, non si esce mai dal gioco perché il main Z80
  // resta bloccato in attesa MCU e non polla gli input port.
  unsigned char b = input ? input->buttons_get() : 0;

  // Paddle uPD4701 SPINNERINO: usiamo ec11_dial_counter (free-running mod 256)
  // come sorgente delta. NON usare ec11_paddle_x che e' clampato 0..255
  // dall'ISR: una volta arrivati al clamp (es. utente gira a sinistra a lungo
  // in vita 1), i delta successivi nello stesso verso vengono persi e alla
  // 2a vita il paddle non puo' piu' andare a sinistra. Il dial counter wrappa
  // naturalmente, e la differenza in int8_t fa il sign-extend corretto.
  extern volatile uint8_t ec11_dial_counter;
  static uint8_t dial_last = 0;
  static bool    dial_init = false;
  uint8_t cur = ec11_dial_counter;
  if (!dial_init) { dial_last = cur; dial_init = true; }
  int8_t delta = (int8_t)(cur - dial_last);   // sign-extend 8-bit → ±127
  dial_last = cur;
  paddle_pos += delta * ARK2_EC11_STEP;
  if (b & BUTTON_LEFT)  paddle_pos -= ARK2_JOY_STEP;
  if (b & BUTTON_RIGHT) paddle_pos += ARK2_JOY_STEP;

  // Niente clamp su paddle_pos: il game tratta uPD4701 come encoder delta,
  // quindi un clamp forzava perdita di movimento al boundary (paddle non
  // ritornava da destra).

  if (!frame_scratch || !spritecode || !spriteylow || !gfx_decoded) return;

  // Fill scratch col background MAME (palette index 0x1F0)
  unsigned short bg_clear = palette_rgb565[0x1F0];
  unsigned short *p = frame_scratch;
  for (int i = 0; i < ARK2_SCREEN_W * ARK2_SCREEN_H; i++) p[i] = bg_clear;

  ark2_draw_background(spritecode, spriteylow, sprite_ctrl, sprite_bg_flag,
                       gfx_decoded, palette_rgb565, frame_scratch);
  ark2_draw_foreground(spritecode, spriteylow, sprite_ctrl,
                       gfx_decoded, palette_rgb565, frame_scratch);
}

void Arkanoid2::run_frame(void) {
  frame_counter++;
  const int INTERLEAVE = 8;
  const int MAIN_PER_SLICE = ARK2_MAIN_ITER_PER_FRAME / INTERLEAVE;
  const int SUB_PER_SLICE  = ARK2_SUB_ITER_PER_FRAME / INTERLEAVE;

  for (int slice = 0; slice < INTERLEAVE; slice++) {
    current_cpu = 0;
    for (int i = 0; i < MAIN_PER_SLICE; i++) {
      StepZ80(&cpu[0]); StepZ80(&cpu[0]); StepZ80(&cpu[0]); StepZ80(&cpu[0]);
    }
    if (sub_running) {
      current_cpu = 1;
      for (int i = 0; i < SUB_PER_SLICE; i++) {
        StepZ80(&cpu[1]); StepZ80(&cpu[1]); StepZ80(&cpu[1]); StepZ80(&cpu[1]);
      }
    }

    // Tick timer YM2203 (8 slice/frame, ~2 ms ognuno).
    // Timer A clock = 3 MHz / 72 = 41666 Hz → ~83 tick/slice.
    // Timer B clock = 3 MHz / 1152 = 2604 Hz → ~5 tick/slice.
    const int TIMER_A_TICKS_PER_SLICE = 83;
    const int TIMER_B_TICKS_PER_SLICE = 5;
    bool fire_irq = false;
    if (timer_a_started) {
      timer_a_count += TIMER_A_TICKS_PER_SLICE;
      int period_a = 1024 - timer_a_period;
      while (timer_a_count >= period_a) {
        timer_a_count -= period_a;
        if (timer_a_enabled && !timer_a_overflow) {
          timer_a_overflow = true;
          fire_irq = true;
        }
      }
    }
    if (timer_b_started) {
      timer_b_count += TIMER_B_TICKS_PER_SLICE;
      int period_b = 256 - timer_b_period;
      while (timer_b_count >= period_b) {
        timer_b_count -= period_b;
        if (timer_b_enabled && !timer_b_overflow) {
          timer_b_overflow = true;
          fire_irq = true;
        }
      }
    }
    if (fire_irq && sub_running) {
      current_cpu = 1;
      IntZ80(&cpu[1], INT_IRQ);
    }
    if (slice == 6) {
      // VBLANK rising edge: buffer flip + coin handling + IRQ entrambe le CPU
      if (spritecode) ark2_tnzs_eof(spritecode, sprite_ctrl[1]);
      // mcu_interrupt MAME tnzs.cpp arknoid2_state:
      //   coin = ((coin1 & 1) << 0) | ((coin2 & 1) << 1) | ((in2 & 3) << 2);
      //   coin ^= 0x0c;
      // coin1/coin2 ACTIVE_HIGH; in2 bit 0=SERVICE active_low, bit 1=TILT active_low
      unsigned char b = input ? input->buttons_get() : 0;
      int coin1_bit = (b & BUTTON_COIN) ? 1 : 0;
      int coin2_bit = 0;
      int in2_bits  = 0x03;
      int coin = (coin1_bit << 0) | (coin2_bit << 1) | (in2_bits << 2);
      coin ^= 0x0c;
      mcu_handle_coins(coin);

      current_cpu = 0;
      IntZ80(&cpu[0], INT_IRQ);
      if (sub_running) {
        current_cpu = 1;
        IntZ80(&cpu[1], INT_IRQ);
      }
    }
  }
  if (!game_started) game_started = 1;
}

// ----------------------------------------------------------------------------
// SPINNERINO: framework strip 256x8. Arkanoid 2 ha frame_scratch in orientamento
// MAME ROT270 originale (TNZS), che sul nostro display appare capovolto rispetto
// ad Arkanoid 1. Applichiamo rotazione software 180 (flip Y + flip X).
// ----------------------------------------------------------------------------
void Arkanoid2::render_row(short row) {
  if (row < 0 || row >= 28 || !frame_scratch) {
    memset(frame_buffer, 0, 256 * 8 * sizeof(unsigned short));
    return;
  }
  int my_base_target = row * 8;
  for (int dy = 0; dy < 8; dy++) {
    int my_target = my_base_target + dy;
    int my_src    = (ARK2_SCREEN_H - 1) - my_target;     // flip Y
    unsigned short *fb_row = &frame_buffer[dy * 256];
    if (my_src < 0 || my_src >= ARK2_SCREEN_H) {
      memset(fb_row, 0, 256 * sizeof(unsigned short));
      continue;
    }
    for (int dx = 0; dx < 256; dx++) {
      int mx_src = (ARK2_SCREEN_W - 1) - dx;             // flip X
      fb_row[dx] = frame_scratch[my_src * ARK2_SCREEN_W + mx_src];
    }
  }
}

#endif // ENABLE_ARKANOID2
