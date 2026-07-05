// ============================================================================
// machines/sbrkout/sbrkout.cpp — Super Breakout (Atari 1978)
// Driver MAME: atari/sbrkout.cpp (set "sbrkout" rev 04)
// ============================================================================
#include "sbrkout.h"

#ifdef ENABLE_SBRKOUT
#include "sbrkout_logo.h"
#include "sbrkout_rom_cpu.h"
#include "sbrkout_gfx_char.h"
#include "sbrkout_gfx_ball.h"

// fake6502 entry points (file fake6502.c incluso in SPINNERINO.ino)
extern void reset6502(void);
extern void step6502(void);
extern void exec6502(uint32_t tickcount);   // esecuzione cycle-accurate (N cicli)
extern void irq6502(void);
extern void nmi6502(void);
extern uint32_t clockticks6502;
extern uint32_t clockgoal6502;
extern uint16_t m6502_pc;
extern uint8_t  m6502_a, m6502_x, m6502_y, m6502_sp, m6502_status;

// EC11 paddle counter free-running (shared con Gigas)
extern volatile uint8_t ec11_dial_counter;

// ============================================================================
// Memory layout interno (RAMSIZE=8192)
//   memory[0x000-0x07F]  zero-page RAM (128 byte)
//   memory[0x080-0x47F]  VRAM 1KB (mappata da 0x0400-0x07FF)
// ============================================================================
#define MEM_RAM_BASE   0x000
#define MEM_VRAM_BASE  0x080
#define VRAM(off)      memory[MEM_VRAM_BASE + (off)]

// ============================================================================
// PROBE OCCUPANCY (diagnostica una-tantum, 2026-06-06).
// Quando attivo, ogni tile con bit7 settato in VRAM viene disegnato come blocco
// MAGENTA pieno 8x8, bypassando il char ROM. Serve a distinguere il bug paddle:
//   - se a centro/destra (palla in movimento) compare comunque una BARRA magenta
//     che attraversa = la VRAM CONTIENE i byte del paddle → bug di RENDERING/char.
//   - se a centro la barra magenta ha un BUCO/è assente = la CPU NON scrive i
//     byte → bug di EMULAZIONE (timing pot/NMI sotto carico).
//   - se la barra appare SPOSTATA = i byte ci sono ma a tile_row sbagliata
//     (bug posizione/sync).
// Commentare la riga per tornare al rendering normale.
//#define SBRKOUT_PADDLE_PROBE 1

// ============================================================================
// Color overlay (PCB-accurate, NON il sbrkout.lay default MAME).
// Il MAME .lay copre tutto Y con bande full-width → cornice e paddle finiscono
// colorati (multiply blend). Il PCB Atari originale aveva invece un overlay
// plastico ristretto SOLO alla zona mattoni; cornice/paddle/score restano
// bianchi. Replichiamo il PCB layout (zone Y misurate dalla foto utente):
//   Y    0..15  → White   (cornice/muro top)
//   Y   15..82  → Blue    (mattoni riga 1)
//   Y   82..101 → Orange  (mattoni riga 2)
//   Y  101..144 → Green   (mattoni riga 3)
//   Y  144..200 → Yellow  (mattoni riga 4, non in foto se distrutti)
//   Y  200..255 → White   (area gioco, paddle, score)
// Convertiti a RGB565 byte-swap (ESP32 SPI BE).
// ============================================================================
#define COL_BLUE    0xBF4A   // 0x4ABF byte-swap
#define COL_ORANGE  0x64F4   // 0xF464 byte-swap
#define COL_GREEN   0x293F   // 0x3F29 byte-swap
#define COL_YELLOW  0xEAFF   // 0xFFEA byte-swap
#define COL_WHITE   0xFFFF

// MAME sbrkout.lay (bande Y 0..256 user post-ROT270):
//   Y    0..40   → Blue   (cornice top + primi mattoni)
//   Y   40..72   → Orange
//   Y   72..104  → Green
//   Y  104..231  → Yellow
//   Y  231..240  → Blue   (banda stretta, area paddle)
//   Y  240..256  → White  (bottom score area)
// HW rotation SPINNERINO P4p (verificato empiricamente): user_y_display
// = 287 - arcade_y (cioe' arcade_y ALTI corrispondono a TOP user).
// Mapping: MAME .lay Y 0..40 (BLU top) → arcade_y 215..255 (top user).
// Muri laterali del playfield (mame_y 0..7 e 216..223) sempre bianchi.
static inline unsigned short overlay_color(int arcade_y, int mame_y) {
  // Muri laterali (sx/dx user view): sempre bianchi, no overlay
  if (mame_y < 8 || mame_y >= 216) return COL_WHITE;
  if (arcade_y < 16)  return COL_WHITE;     // bottom user: score area
  if (arcade_y < 25)  return COL_BLUE;      // banda BLU sopra paddle
  if (arcade_y < 117) return COL_YELLOW;    // mattoni gialli (bottom)
  if (arcade_y < 172) return COL_GREEN;
  if (arcade_y < 192) return COL_ORANGE;
  if (arcade_y < 237) return COL_BLUE;      // mattoni BLU top
  return COL_WHITE;                          // cornice top muro orizzontale
}

Sbrkout::Sbrkout() {
  outlatch = 0;
  paddle_value = 128;
  game_select = 0;
  nmi_scanline_target = 0;
  pot_trigger = 0;
  pot_mask[0] = 1;             // MAME default: outlatch reset → mask=!0 = 1 (masked)
  pot_mask[1] = 1;             // gioco scrive bit 5/6 = 1 per smascherare
  scanline = 0;
  sync2_value = 0;
  nmi_fired_this_frame = false;
  nmi_pending = false;
  pot_sample_reads = 0;
  irq_asserted = false;
}

const unsigned short *Sbrkout::logo(void) {
  return sbrkout_logo;
}

void Sbrkout::init(Input *in, unsigned short *fb, sprite_S *sb, unsigned char *mem) {
  machineBase::init(in, fb, sb, mem);
}

void Sbrkout::reset() {
  machineBase::reset();
  outlatch = 0;
  paddle_value = 128;
  game_select = 0;
  pot_mask[0] = 1;             // F9334 reset → tutti bit a 0 → pot_mask invertito = 1
  pot_mask[1] = 1;
  pot_trigger = 0;
  nmi_pending = false;
  pot_sample_reads = 0;
  irq_asserted = false;
  reset6502();
}

// ============================================================================
// 6502 memory accessors. Address bus 14-bit effettivi → mirror modulo 0x4000.
// ============================================================================
unsigned char Sbrkout::rd6502(unsigned short Addr) {
  unsigned short la = Addr & 0x3FFF;        // global_mask 0x3FFF (14-bit bus)

  // ROM 0x2800-0x3FFF
  if (la >= 0x2800) {
    return pgm_read_byte(&sbrkout_rom_cpu[la]);
  }
  // Zero page RAM 0x0000-0x007F mirror 0x0380 (tutto 0x0000-0x03FF)
  if (la < 0x0400) {
    return memory[MEM_RAM_BASE + (la & 0x7F)];
  }
  // VRAM 0x0400-0x07FF. ATTENZIONE: in MAME `bank1` punta a videoram[0x380],
  // quindi vram[$380..$3FF] e' un ALIAS di zero page memory[$00..$7F]:
  //   vram[$390] = ZP $10 (ball 0 X)
  //   vram[$391] = ZP $11 (noise gen bits, 4 toni D0-D3) ⭐ AUDIO
  //   vram[$398] = ZP $18 (ball 0 Y)
  //   vram[$399] = ZP $19 (ball pic)
  // Senza questo alias, il gioco scrive in ZP ma renderer/audio leggono vram
  // separato → ball coords/audio sempre 0 (era il bug "palla invisibile"!).
  if (la < 0x0800) {
    unsigned short voff = la - 0x0400;
    if (voff >= 0x380) {
      return memory[MEM_RAM_BASE + (voff & 0x7F)];
    }
    return VRAM(voff);
  }
  // 0x0800-0x083F: switches_r (DIPS + SELECT + pot_trigger + SERVE)
  if (la < 0x0840) {
    return switch_read((unsigned char)(la & 0x3F));
  }
  // 0x0840-0x087F: COIN port (MAME: 0x40=COIN1 active_high, 0x80=COIN2 active_high,
  //                            bit 0..5 = IPT_UNUSED active_low → 1 a riposo).
  // Idle = 0x3F (bit 0..5 alti). Press COIN1 = 0x3F | 0x40 = 0x7F.
  if (la < 0x0880) {
    unsigned char btn = input ? input->buttons_get() : 0;
    return 0x3F | ((btn & BUTTON_COIN) ? 0x40 : 0x00);
  }
  // 0x0880-0x08BF: START port (MAME: 0x40=START1 active_low, 0x80=START2 active_low,
  //                            bit 0..5 = IPT_UNUSED active_low → 1 a riposo).
  // Idle = 0xFF. Press START1 = clear bit 6 → 0xBF.
  if (la < 0x08C0) {
    unsigned char btn = input ? input->buttons_get() : 0;
    return (btn & BUTTON_START) ? 0xBF : 0xFF;
  }
  // 0x08C0-0x08FF: SERVICE/TILT (active LOW, idle = 0xFF — bit 0..5 alti come da MAME)
  if (la < 0x0900) {
    return 0xFF;
  }
  // 0x0C00-0x0FFF: sync_r (= scanline counter, current vpos).
  // MAME aggiorna anche m_sync2_value qui basato su hpos orizzontale.
  // Non avendo hpos vero, simulo con counter che toggle ad ogni read: il gioco
  // in busy-loop su sync2_r vede comunque alternare bit 7 → non blocca.
  if (la >= 0x0C00 && la <= 0x0FFF) {
    if (pot_sample_reads > 0) {
      // Finestra post-NMI: restituisci la scanline al momento dell'NMI
      // (nmi_scanline_target), NON scanline corrente che potrebbe essere
      // avanzata se l'NMI era pending/ritardato. Questo è il valore che
      // l'handler NMI deve leggere per calcolare correttamente il delta paddle.
      sync2_value = (unsigned char)(paddle_value & 1);
      pot_sample_reads--;
      return (unsigned char)(nmi_scanline_target & 0xFF);
    } else {
      static unsigned char hpos_sim = 0;
      hpos_sim++;
      sync2_value = (hpos_sim & 0x20) ? 1 : 0;
    }
    return (unsigned char)(scanline & 0xFF);
  }
  // 0x1000-0x13FF: sync2_r (= mid-line flag bit 7)
  if (la >= 0x1000 && la <= 0x13FF) {
    if (pot_sample_reads > 0) {
      // Finestra post-NMI: restituisci il vero bit LSB del pot (pot&1).
      sync2_value = (unsigned char)(paddle_value & 1);
      pot_sample_reads--;
    } else {
      // Toggle anche qui in caso il gioco non passi da sync_r prima
      static unsigned char hpos_sim2 = 0;
      hpos_sim2++;
      if ((hpos_sim2 & 0x07) == 0) sync2_value ^= 1;
    }
    return (sync2_value ? 0x80 : 0x00) | 0x7F;
  }
  return 0xFF;
}

void Sbrkout::wr6502(unsigned short Addr, unsigned char Value) {
  unsigned short la = Addr & 0x3FFF;
  if (la >= 0x2800) return;                 // ROM: ignora
  if (la < 0x0400) {
    memory[MEM_RAM_BASE + (la & 0x7F)] = Value;
    // ZP $11 = noise generation bits (D0-D3 = 4 toni). MAME usa vram[$391]
    // come mirror della stessa locazione: aggiorno soundregs[0] sia che il
    // gioco scriva a ZP $11 sia a $791.
    if ((la & 0x7F) == 0x11) soundregs[0] = Value;
    return;
  }
  if (la < 0x0800) {
    unsigned short voff = la - 0x0400;
    if (voff >= 0x380) {
      // Mirror zero page: scrittura a vram[$380..$3FF] = scrittura a ZP $00..$7F
      memory[MEM_RAM_BASE + (voff & 0x7F)] = Value;
      if ((voff & 0x7F) == 0x11) soundregs[0] = Value;
    } else {
      VRAM(voff) = Value;
    }
    return;
  }
  // 0x0C00-0x0C7F: F9334 output latch (Adr(6:4)=bit selector, Value bit 0 = valore)
  // MAME F9334 bit assignments (atari/sbrkout.cpp):
  //   bit 1 = SERVE LED (active low, invertito)
  //   bit 3 = LAMP1
  //   bit 4 = LAMP2
  //   bit 5 = POT_MASK1  (logica invertita: state=1 → mask=0=enabled)
  //   bit 6 = POT_MASK2  (logica invertita)
  //   bit 7 = COIN COUNTER
  if (la >= 0x0C00 && la < 0x0C80) {
    // F9334 demux: bit selector e VALORE entrambi codificati nell'indirizzo.
    // MAME: write_bit(offset >> 4, offset & 1) — il dato scritto (Value) e' ignorato.
    unsigned char bit_n = (la >> 4) & 0x07;
    unsigned char state = (la & 1);
    (void)Value;
    if (state) outlatch |=  (1 << bit_n);
    else       outlatch &= ~(1 << bit_n);
    if (bit_n == 5) {
      pot_mask[0] = !state;
      if (pot_mask[0]) {
        pot_trigger = 0;                   // mask=1 → clear trigger (MAME pot_mask1_w)
        nmi_pending = false;               // annulla NMI pendente se ri-mascherato
      } else if (pot_trigger && !nmi_fired_this_frame) {
        // SMASCHERAMENTO con trigger gia' latchato → MAME asserisce l'NMI ORA.
        // Eseguito in run_frame (fuori da step6502, per sicurezza re-entrancy).
        nmi_pending = true;
      }
    } else if (bit_n == 6) {
      pot_mask[1] = !state;
      if (pot_mask[1]) {
        // pot_trigger[1] non esiste (singolo paddle), no-op
      }
    }
    return;
  }
  // 0x0C80 mirror 0x7F: watchdog kick — no-op (MiSTer bypassa pure)
  if (la >= 0x0C80 && la < 0x0D00) return;
  // 0x0E00 mirror 0x7F: IRQ ack write. MAME: irq_ack_w → set_input_line(0, CLEAR).
  // Abbassa la linea IRQ level-held (vedi run_frame). Senza ack la linea resta
  // asserita e l'IRQ verrebbe riconsegnato appena il flag I si abbassa (come HW).
  if (la >= 0x0E00 && la < 0x0E80) { irq_asserted = false; return; }
}

// ============================================================================
// switches_r (porting fedele da MAME atari/sbrkout.cpp::switches_r).
// Range 0x0800-0x083F (passato come 6-bit offset).
//
// DIPS port (default factory):
//   bit 0,1 = Language (00 = English)
//   bit 2,3 = Coinage  (10 = 1C/1C → DIPS bits 2,3 = 1,0)
//   bit 4-6 = Extended Play (000 = None)
//   bit 7   = Lives (1 = 3 lives)
//   default DIPS = 0x88 =0 ENG 3 lives
//           DIPS = 0x08 =0 ENG 5 lives
// SELECT port (game type): bit 0,1 = mode (00 = Progressive)
//   default SELECT = 0x00
// SERVE: bit 7 = BUTTON1 (active HIGH)
// ============================================================================
unsigned char Sbrkout::switch_read(unsigned char offs) {
  unsigned char btn = input ? input->buttons_get() : 0;
  unsigned int result = 0xFF;

  const unsigned char DIPS_v   = 0x08;
  // SELECT $0830: bit7,6 determinano la modalità di gioco.
  // game_select 0→0x3F (Progressive), 1→0x7F (Cavity), 2→0xBF (Double)
  static const unsigned char select_table[3] = { 0x3F, 0x7F, 0xBF };
  const unsigned char SELECT_BYTE = select_table[game_select < 3 ? game_select : 0];
  const unsigned char SELECT_v = 0x00;  // usato solo per offset $00/$07, non $30
  unsigned char SERVE_v = (btn & BUTTON_FIRE) ? 0x80 : 0x00;   // active HIGH

  // SELECT $0830: offs=$30, offs&0x3F=$30. Restituisce il byte di selezione gioco.
  // bit7,6 di SELECT_BYTE codificano la modalità (0x3F/0x7F/0xBF).
  if (offs == 0x30 || offs == 0x31) return SELECT_BYTE;

  // DIP rows — ogni riga espone in bit7 il bit corrispondente di DIPS_v.
  // MAME: switches_r restituisce (DIPS_v >> bit_n) & 1 in posizione 7.
  // shift corretto: per portare il bit N di DIPS_v in posizione 7 serve << (7-N).
  //   offs=0x00 → bit 0 → << 7
  //   offs=0x01 → bit 1 → << 6
  //   offs=0x02 → bit 2 → << 5
  //   offs=0x03 → bit 3 → << 4
  // Con DIPS_v=0x88=10001000: bit0=0, bit1=0 → Language=00=English ✓
  if ((offs & 0x0B) == 0x00) result &= ((unsigned)DIPS_v << 7) | 0x7F;
  if ((offs & 0x0B) == 0x01) result &= ((unsigned)DIPS_v << 6) | 0x7F;
  if ((offs & 0x0B) == 0x02) result &= ((unsigned)DIPS_v << 5) | 0x7F;
  if ((offs & 0x0B) == 0x03) result &= ((unsigned)DIPS_v << 4) | 0x7F;

  // Other switches (MAME-fedele, atari/sbrkout.cpp::switches_r)
  if ((offs & 0x17) == 0x00) result &= ((unsigned)SELECT_v << 7) | 0x7F;
  // Pot trigger e' valido solo se non mascherato (MAME: (trigger & ~mask) << 7)
  if ((offs & 0x17) == 0x04) result &= ((unsigned)(pot_trigger & ~pot_mask[0]) << 7) | 0x7F;
  if ((offs & 0x17) == 0x05) result &= ((unsigned)(0           & ~pot_mask[1]) << 7) | 0x7F;
  if ((offs & 0x17) == 0x06) result &= SERVE_v | 0x7F;         // bit 7 = BUTTON1
  if ((offs & 0x17) == 0x07) result &= ((unsigned)SELECT_v << 6) | 0x7F;

  return (unsigned char)result;
}

// ============================================================================
// Frame loop: ~262 scanline @ 60 Hz, ~14 istruzioni / scanline a 756 kHz.
//   IRQ ogni 32 scanline (offset 16): 8 IRQ/frame.
//   NMI a scanline (56 + paddle/2): timing del paddle pot.
// ============================================================================
void Sbrkout::run_frame() {
  current_cpu = 0;

  // Aggiorna paddle value dal dial EC11. Track delta cumulativo CLAMPED 0..255
  // per evitare wrap dial_counter (paddle salta dal muro destro al sinistro).
  static int  paddle_pos = 128;
  static uint8_t dial_last = 0;
  static bool dial_init = false;
  uint8_t cur = ec11_dial_counter;
  if (!dial_init) { dial_last = cur; dial_init = true; }
  int8_t delta = (int8_t)(cur - dial_last);     // sign-extend 8-bit
  dial_last = cur;
  paddle_pos += delta;
  if (paddle_pos < 0)   paddle_pos = 0;
  if (paddle_pos > 255) paddle_pos = 255;
  paddle_value = (unsigned char)paddle_pos;
  nmi_scanline_target = 56 + (paddle_value / 2);

  // Reset stato per-frame
  pot_trigger = 0;
  nmi_fired_this_frame = false;
  nmi_pending = false;

  const int SCANLINES_PER_FRAME = 262;
  // Cycle-accurate: 756 kHz / 60 Hz / 262 scanline ≈ 48 cicli CPU per scanline.
  // Prima si eseguiva un numero FISSO di istruzioni (14) per scanline: sotto
  // carico (fisica palla) le istruzioni durano piu' cicli, quindi 14 istruzioni
  // != 48 cicli reali → il PC della CPU si desincronizzava dal punto in cui
  // NMI/IRQ devono interromperla → routine di disegno paddle saltata a
  // centro/destra. exec6502(N) gira esattamente N cicli (clockticks cumulativo).
  const uint32_t CYCLES_PER_SCANLINE = 48;

  // Azzero i contatori-ciclo a inizio frame (numeri piccoli → niente overflow
  // uint32 a lungo termine; exec6502 usa clockgoal come bound del while-loop).
  clockticks6502 = 0;
  clockgoal6502  = 0;

  for (unsigned short sc = 0; sc < SCANLINES_PER_FRAME; sc++) {
    scanline = sc;
    // sync2_value: gestito dinamicamente dentro rd6502 (toggle ad ogni read),
    // NON da sovrascrivere qui altrimenti il busy-loop su sync2_r non vede cambi.

    // IRQ 16V level-held (MAME): asserito ogni 32 scanline (offset 16), tenuto
    // fino all'ack ($0E00). Consegnato SOLO se il flag I e' clear: con SEI resta
    // pendente e NON corrompe la sezione critica (era il bug del paddle).
    if ((sc & 31) == 16) irq_asserted = true;
    if (irq_asserted && !(m6502_status & 0x04)) {   // 0x04 = FLAG_INTERRUPT (I)
      irq6502();   // push PC/status + set I; resta pendente finche' ack $0E00
    }

    // Esecuzione cycle-accurate della scanline (≈48 cicli reali).
    exec6502(CYCLES_PER_SCANLINE);

    // NMI paddle pot: scatta a scanline (56 + pot/2). MAME-fedele:
    // - pot_trigger viene LATCHATO al match di scanline (sempre, anche se masked)
    // - se gia' smascherato (pot_mask[0]==0) → NMI subito
    // - se masked → resta latched; quando il gioco smaschera (wr6502 bit5) parte
    //   nmi_pending, gestito sotto. QUESTO e' il fix del paddle invisibile in gioco:
    //   con ball in movimento il gioco smaschera DOPO il trigger per centro/destra.
    if (!nmi_fired_this_frame && sc == nmi_scanline_target) {
      pot_trigger = 1;
      if (pot_mask[0] == 0) {
        // MAME fa fire il pot a hpos=(pot%2)*128 → il flag mezza-linea letto
        // dall'handler NMI e' pot&1. Latch il bit reale e apri una finestra di
        // letture (sync_r->sync2_r dell'handler) in cui restituirlo invece del
        // toggle finto.
        sync2_value      = (unsigned char)(paddle_value & 1);
        pot_sample_reads = 8;
        nmi6502();
        nmi_fired_this_frame = true;
      }
    }

    // NMI latchato + smascherato in ritardo (oltre la scanline di trigger):
    // il gioco ha scritto outlatch bit5 durante questa scanline con pot_trigger
    // gia' a 1. Eseguiamo l'NMI ora (fuori da step6502).
    if (nmi_pending && !nmi_fired_this_frame) {
      nmi_pending      = false;
      sync2_value      = (unsigned char)(paddle_value & 1);
      pot_sample_reads = 8;
      nmi6502();
      nmi_fired_this_frame = true;
    }
  }

  // Cicla modalità gioco con SERVE (BUTTON_FIRE = SW encoder) in attract.
  // In attract FIRE non ha altro uso. Ogni pressione avanza: 0→1→2→0.
  // L'attract mode cambia subito a riflettere la nuova modalità.
  // Inserendo il credito dopo, il gioco parte in quella modalità.
  {
    static bool fire_prev = false;
    unsigned char *zp = memory + MEM_RAM_BASE;
    bool in_attract = ((zp[0x00] & 0x08) == 0);
    unsigned char btn = input ? input->buttons_get() : 0;
    bool fire_now = (btn & BUTTON_FIRE) != 0;
    if (in_attract && fire_now && !fire_prev) {
      game_select = (game_select + 1) % 3;
      static const char *names[] = {"Progressive","Cavity","Double"};
      Serial.printf("[SBRKOUT] Game mode: %s\n", names[game_select]);
    }
    fire_prev = fire_now;
  }

  if (!game_started) game_started = 1;
}

void Sbrkout::prepare_frame() {
  // Estraggo le 3 ball sprite dalla ZERO PAGE (MAME-fedele):
  //   ZP $10/$12/$14 = ball X (ball 0/1/2)  → sx = 31*8 - x
  //   ZP $18/$1A/$1C = ball Y (ball 0/1/2)  → sy = 30*8 - y
  //   ZP $19/$1B/$1D = ball picture (D7 = no ball flag)
  // In MAME vram[$390..$39F] e' un alias di ZP $10..$1F (bank1 punta a videoram[$380]).
  // Qui leggo direttamente dalla zero page (memory[$10..$1F]).
  active_sprites = 0;
  unsigned char *zp = memory + MEM_RAM_BASE;
  for (int ball = 0; ball < 3; ball++) {
    unsigned char vx = zp[0x10 + ball * 2];
    unsigned char vy = zp[0x18 + ball * 2];
    unsigned char vc = zp[0x18 + ball * 2 + 1];
    short sx = (short)(31 * 8) - (short)vx;
    short sy = (short)(30 * 8) - (short)vy;
    sprite_S &sp = sprite[active_sprites];
    sp.x        = sx;
    sp.y        = sy;
    // ROM ball (033282.k6, conferma MiSTer K6_PROM.vhd):
    //   entry 0 (byte 0..2) = 0xE0 0xE0 0xE0 → 3x3 pieno
    //   entry 1 (byte 3..5) = 0x00 0x00 0x00 → vuoto
    // Force code=0 per palla sempre visibile (entry 1 vuota).
    sp.code     = 0;
    if (vc & 0x80) continue;  // bit7 = no-ball flag: palla non attiva
    sp.color    = 0;
    sp.flags    = 0;
    sp.color_block = 0;
    sp.is_32x32 = 0;
    sp.flip_x   = 0;
    sp.flip_y   = 0;
    active_sprites++;
  }
}

// ============================================================================
// Render: tilemap monocromatico + 3 ball sprite. Output mono come RGB565:
//   0 (background) = 0x0000 (nero)
//   1 (pixel set)  = 0xFFFF (bianco)
// Flip 180° SW come Gigas: render con strip "speculare", poi reverse alla fine.
// ============================================================================
// ============================================================================
// Render trasposto stile SpaceInvaders (arcade portrait → FB landscape).
// User portrait view (arcade nativo) ha dimensioni 224w x 256h.
//   user_x (orizz portrait) ∈ [0..223] → y_fb landscape  (= 223 - y_fb)
//   user_y (vert  portrait) ∈ [0..255] → x_fb landscape  (= 255 - x_fb)
// Cioe' MAME (x_orig, y_orig) con ROT270 → user (x_user=y_orig, y_user=255-x_orig).
// Equivalente: FB (x_fb, y_fb) ↔ MAME (x_orig=255-x_fb, y_orig=223-y_fb).
// Tile index: (y_orig/8) * 32 + (x_orig/8).
//
// Niente flip 180° SW separato: la trasposizione gestisce gia' la rotazione.
// ============================================================================
void Sbrkout::render_row(short strip_row) {
  if (strip_row < 0 || strip_row >= 28) return;

  // BG nero: FB landscape opaco. Riempi tutta la strip 256x8.
  for (int i = 0; i < SBRKOUT_SCREEN_W * 8; i++) frame_buffer[i] = 0x0000;

  unsigned char *vram = memory + MEM_VRAM_BASE;

  // Mapping FB landscape <-> MAME pre-rot (ROT270 MAME → user portrait).
  // Pattern Space Invaders: ENTRAMBI gli assi MAME invertiti rispetto al FB.
  //   y_fb (0..223) → y_orig = 223 - y_fb (= user_x portrait orizz)
  //   x_fb (0..255) → x_orig = 255 - x_fb (= 255 - user_y portrait vert)
  // Cioe' arcade_y (user portrait Y) = 255 - x_fb (top user view = x_fb 255).
  // Era "y_orig = y_fb" (no inv): produceva mirror Y → scritte COIN/PLAYER
  // specchiate, mattoni in banda overlay sbagliata, cornice non bianca.
  for (int sr = 0; sr < 8; sr++) {
    int y_fb = strip_row * 8 + sr;
    int y_orig = 223 - y_fb;                                 // INVERTI Y (ROT270)
    if (y_orig < 0 || y_orig >= 224) continue;
    int tile_row    = y_orig >> 3;
    int tile_pix_y  = y_orig & 7;

    unsigned short *line = frame_buffer + sr * SBRKOUT_SCREEN_W;

    for (int tile_col = 0; tile_col < 32; tile_col++) {
      unsigned char v = vram[tile_row * 32 + tile_col];

#ifdef SBRKOUT_PADDLE_PROBE
      // Probe: qualunque tile "acceso" (bit7) → blocco magenta pieno.
      if (v & 0x80) {
        int x_fb_base_p = 255 - tile_col * 8;
        for (int px = 0; px < 8; px++) {
          int x_fb = x_fb_base_p - px;
          if (x_fb >= 0 && x_fb < SBRKOUT_SCREEN_W) line[x_fb] = 0x1FF8; // magenta
        }
        continue;
      }
#endif

      unsigned int code = (v & 0x80) ? (v & 0x3F) : 0;       // 64 char totali

      unsigned char L = pgm_read_byte(&sbrkout_gfx_char[          code * 8 + tile_pix_y]);
      unsigned char R = pgm_read_byte(&sbrkout_gfx_char[0x200 +   code * 8 + tile_pix_y]);

      int x_fb_base = 255 - tile_col * 8;
      for (int px = 0; px < 8; px++) {
        unsigned char b = (px < 4) ? L : R;
        int bit_pos = 3 - (px & 3);
        int bit = (b >> bit_pos) & 1;
        if (!bit) continue;
        int x_fb = x_fb_base - px;
        if (x_fb >= 0 && x_fb < SBRKOUT_SCREEN_W) {
          int arcade_y = 255 - x_fb;
          line[x_fb] = overlay_color(arcade_y, y_orig);
        }
      }
    }
  }

  // Ball sprite: la 3x3 ROM e' troppo piccola da vedere sul display.
  // Render come blob 6x6 GIALLO fisso (request utente).
  for (unsigned char s = 0; s < active_sprites; s++) {
    sprite_S &sp = sprite[s];
    int sx_orig = sp.x;
    int sy_orig = sp.y;
    for (int dy = 0; dy < 6; dy++) {
      int y_orig_pix = sy_orig + dy;
      if (y_orig_pix < 0 || y_orig_pix >= 224) continue;
      int y_fb = 223 - y_orig_pix;
      int sr   = y_fb - strip_row * 8;
      if (sr < 0 || sr >= 8) continue;
      unsigned short *line = frame_buffer + sr * SBRKOUT_SCREEN_W;
      for (int dx = 0; dx < 6; dx++) {
        int x_orig_pix = sx_orig + dx;
        if (x_orig_pix < 0 || x_orig_pix >= 256) continue;
        int x_fb = 255 - x_orig_pix;
        if (x_fb < 0 || x_fb >= SBRKOUT_SCREEN_W) continue;
        line[x_fb] = COL_YELLOW;
      }
    }
  }

}

#endif // ENABLE_SBRKOUT