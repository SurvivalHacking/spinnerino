// ============================================================================
// machines/bombbee/bombbee.cpp — Bomb Bee (Namco 1979)
// Driver MAME: namco/warpwarp.cpp (set "bombbee")
//
// CPU Intel 8080 @ 2.048 MHz emulato su core Z80. Vedi bombbee.h per la
// memory map completa e le note hardware.
// ============================================================================
#include "bombbee.h"

#ifdef ENABLE_BOMBBEE

// paddle dall'ISR EC11 (input.cpp): 0..255 clamped, stile Arkanoid
extern volatile int16_t ec11_paddle_x;

// ---- Layout interno nel buffer condiviso memory[] (RAMSIZE>=8192) ----
//   memory[0x0000..0x03FF]  RAM lavoro   (CPU 0x2000-0x23FF)
//   memory[0x0400..0x07FF]  VRAM tile    (CPU 0x4000-0x43FF)
//   memory[0x0800..0x0BFF]  VRAM colore  (CPU 0x4400-0x47FF)
#define BB_RAM        0x0000
#define BB_VRAM       0x0400      // tile codes; colore a BB_VRAM+0x400

// 8080 @ 2.048 MHz, ~60.6 fps -> ~33.8k cicli/frame. A ~6.5 cicli/istruzione
// medi ~5200 istruzioni/frame. Parametro di tuning per la velocita' di gioco:
// alzare se troppo lento, abbassare se troppo veloce.
#define BB_INST_PER_FRAME  5200

// Velocita' di gioco: tick di emulazione (vblank) ogni 60 frame reali.
// 60 = velocita' nativa 60Hz. >60 = piu' veloce (pallina + tempo musica, la
// tonalita' resta invariata). NB: e' un fattore COSTANTE, quindi alza sia la
// velocita' iniziale sia quella di punta (la palla accelera da sola, come
// nell'originale). 62 = ~+3% (boost leggero in avvio, punta non eccessiva).
#define BB_SPEED_TICKS     62

// Sensibilita' paddle EC11 (solo Bomb Bee): guadagno % attorno al centro.
// 100 = nativo, 118 = +18% (meno rotazione per coprire lo schermo).
#define BB_PADDLE_GAIN     118

// convert da 32x32 a 34x28 (TILEMAP_MAPPER warpwarp_state::tilemap_scan).
// col 0 e col 33 = colonne laterali (score); col 1..32 = playfield.
static inline int bb_scan(int col, int row) {
  row += 2;
  int c = col - 1;                       // offs_t: col=0 -> c=-1 (tutti 1)
  if (c & 0x20) return (row + ((c & 1) << 5)) & 0x3FF;
  else          return (c + (row << 5)) & 0x3FF;
}

// ============================================================================
// Calibrazione orientamento. Bomb Bee e' ROT90; Space Invaders (stesso display
// MV) e' ROT270, quindi parto dalla stessa trasposizione di SpaceInvaders e
// inverto entrambi gli assi. Se al primo flash l'immagine appare
// specchiata/capovolta, basta cambiare questi due flag (0/1).
// ============================================================================
#define BB_FLIP_LONG   0     // asse lungo (colonne / orizzontale arcade)
#define BB_FLIP_SHORT  0     // asse corto (righe / verticale arcade)

void BombBee::build_color_lut(void) {
  // warpwarp_palette: R/G da 3 resistori {1600,820,390}, B da 2 {820,390}.
  const double sum_rg = 1.0/1600 + 1.0/820 + 1.0/390;
  const double wr0 = (1.0/1600)/sum_rg, wr1 = (1.0/820)/sum_rg, wr2 = (1.0/390)/sum_rg;
  const double sum_b = 1.0/820 + 1.0/390;
  const double wb0 = (1.0/820)/sum_b, wb1 = (1.0/390)/sum_b;
  for (int i = 0; i < 256; i++) {
    int r = (int)(255.0 * (wr0*((i>>0)&1) + wr1*((i>>1)&1) + wr2*((i>>2)&1)) + 0.5);
    int g = (int)(255.0 * (wr0*((i>>3)&1) + wr1*((i>>4)&1) + wr2*((i>>5)&1)) + 0.5);
    int b = (int)(255.0 * (wb0*((i>>6)&1) + wb1*((i>>7)&1)) + 0.5);
    unsigned short c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    color_lut[i] = (unsigned short)((c >> 8) | (c << 8));  // byte-swap SPI BE
  }
}

void BombBee::reset() {
  machineBase::reset();
  Z80_i8080 = 1;   // 8080: flag P/V = PARITA' (la fisica usa salti JPE/JPO)
  ball_h = ball_v = 0;
  ball_on = 0;
  flip = 0;
  // DSW1: bit0-1 coinage 1C/1C (0x03); bit2-3 lives=3 (0x00); bit4 unused (0x10);
  //       bit5-7 replay 50000 (0x00).
  dsw1 = 0x13;
  last_coin = 0;
  build_color_lut();
}

unsigned char BombBee::opZ80(unsigned short Addr) {
  return rdZ80(Addr);
}

unsigned char BombBee::rdZ80(unsigned short Addr) {
  if (Addr < 0x2000)
    return pgm_read_byte(&bombbee_rom[Addr]);
  if (Addr >= 0x2000 && Addr < 0x2400)
    return memory[BB_RAM + (Addr - 0x2000)];
  if (Addr >= 0x4000 && Addr < 0x4800)
    return memory[BB_VRAM + (Addr - 0x4000)];
  if (Addr >= 0x4800 && Addr < 0x5000)
    return pgm_read_byte(&bombbee_gfx[Addr - 0x4800]);
  if (Addr >= 0x6000 && Addr < 0x6010) {
    // warpwarp_sw_r: ritorna il bit (Addr&7) di IN0 (active LOW).
    uint8_t in0 = 0xFF;                 // idle; bit6 cabinet=1 (upright)
    unsigned char k = input->buttons_get();
    if (k & BUTTON_COIN)  in0 &= ~0x01;
    if (k & BUTTON_START) in0 &= ~0x04;
    if (k & BUTTON_FIRE)  in0 &= ~0x10; // serve / lancio palla
    return (in0 >> (Addr & 7)) & 1;
  }
  if (Addr >= 0x6010 && Addr < 0x6020)
    return read_paddle();
  if (Addr >= 0x6020 && Addr < 0x6030)
    return (dsw1 >> (Addr & 7)) & 1;
  return 0xFF;
}

void BombBee::wrZ80(unsigned short Addr, unsigned char Value) {
  if (Addr >= 0x2000 && Addr < 0x2400) {
    memory[BB_RAM + (Addr - 0x2000)] = Value;
    return;
  }
  if (Addr >= 0x4000 && Addr < 0x4800) {
    memory[BB_VRAM + (Addr - 0x4000)] = Value;
    if (!game_started && Value) game_started = 1;
    return;
  }
  if (Addr >= 0x6000 && Addr < 0x6010) {     // warpwarp_out0_w
    switch (Addr & 3) {
      case 0: ball_h = Value; break;
      case 1: ball_v = Value; break;
      case 2: soundregs[0] = Value; soundregs[3] = 1; break;  // sound_w + trigger envelope
      case 3: break;                         // watchdog
    }
    return;
  }
  if (Addr >= 0x6010 && Addr < 0x6020) { soundregs[1] = Value; return; }              // music1 (freq)
  if (Addr >= 0x6020 && Addr < 0x6030) { soundregs[2] = Value; soundregs[4] = 1; return; }  // music2 + trigger
  if (Addr >= 0x6030 && Addr < 0x6038) {     // latch LS259 (bit0 = dato)
    int n = Addr & 7;
    uint8_t bit = Value & 1;
    if      (n == 6) ball_on = bit;          // abilita palla + IRQ vblank
    else if (n == 7) flip    = bit;          // flip screen (cocktail)
    // n 0-2 lamp, 4 coin lockout, 5 counter: ignorati
    return;
  }
  // 0x0000-0x1FFF ROM, 0x4800-0x4FFF gfx ROM, resto: ignora
}

uint8_t BombBee::read_paddle(void) {
  // EC11 0..255 -> range Bomb Bee 0x14..0xAC, con PORT_REVERSE.
  const int lo = 0x14, hi = 0xAC;
  // Sensibilita': amplifica lo spostamento attorno al centro (128).
  int p = (int)ec11_paddle_x - 128;      // -128..127
  p = (p * BB_PADDLE_GAIN) / 100;        // guadagno
  p += 128;
  if (p < 0)   p = 0;
  if (p > 255) p = 255;
  int v = lo + (p * (hi - lo)) / 255;    // 0x14..0xAC
  v = hi - (v - lo);                     // REVERSE
  if (v < lo) v = lo;
  if (v > hi) v = hi;
  return (uint8_t)v;
}

void BombBee::run_frame(void) {
  current_cpu = 0;
  // IRQ vblank (RST 38h) gated da ball_on, come l'hardware (vblank_irq).
  // IMPORTANTE: il vblank e' una richiesta MANTENUTA per tutto il frame, da
  // consegnare appena il gioco riabilita gli interrupt (EI), UNA volta per
  // frame. Consegnarla solo a fine frame (come Space Invaders) NON basta:
  // Bomb Bee passa quasi tutto il frame con interrupt disabilitati (delay/DI),
  // quindi perderebbe ~tutti i vblank -> il tick (0x04AA) non gira mai -> la
  // display-list non viene costruita -> schermo statico e coin ignorato.
  // (Diagnosi via emulazione 8080 offline: con questo modello 522/600 vblank
  // contro 6/600 del modello a fine frame.)
  // Velocita' di gioco: di norma 1 tick (vblank) per frame reale = 60Hz nativo.
  // Con BB_SPEED_TICKS>60 si aggiunge periodicamente un tick extra (fast-forward
  // di un frame) per rendere pallina/gioco un filo piu' veloci.
  static int speed_acc = 0;
  speed_acc += BB_SPEED_TICKS;
  int ticks = 0;
  while (speed_acc >= 60) { speed_acc -= 60; ticks++; }
  if (ticks < 1) ticks = 1;

  for (int t = 0; t < ticks; t++) {
    bool pending = (ball_on != 0);
    for (int i = 0; i < BB_INST_PER_FRAME; i++) {
      if (pending && (cpu[0].IFF & IFF_1)) {   // interrupt abilitati -> consegna
        IntZ80(&cpu[0], INT_RST38);
        pending = false;
      }
      StepZ80(&cpu[0]);
    }
  }
}

void BombBee::prepare_frame(void) {
  active_sprites = 0;     // niente sprite HW: solo tilemap + ball
}

// Render trasposto su framebuffer landscape 256x224 (28 strip da 8 px).
// Mappo solo le 32 colonne centrali (playfield) sui 256 px; le 2 colonne
// laterali (score) sono fuori dai 256 px del framebuffer [da rivedere Fase 3].
// Ottimizzazione: il lookup tile (bb_scan + VRAM + gfx + colore) si fa UNA
// volta per tile (ogni 8 px) invece che per ogni pixel (8x meno lavoro).
void BombBee::render_row(short row) {
  if (row < 0 || row >= 28) return;

  unsigned char *vtile = memory + BB_VRAM;
  unsigned char *vcol  = memory + BB_VRAM + 0x400;

  // Palla in coordinate raw (272x224): x=264-ball_h, y=240-ball_v, blob 4x4.
  int bx = 264 - ball_h;
  int by = 240 - ball_v;

  for (int sr = 0; sr < 8; sr++) {
    int fy = row * 8 + sr;                       // 0..223
    int raw_y = BB_FLIP_SHORT ? (223 - fy) : fy; // asse corto (28 righe)
    int trow  = raw_y >> 3;
    int py    = raw_y & 7;
    unsigned short *dst = frame_buffer + sr * 256;

    bool ball_row = ball_on && raw_y >= by - 4 && raw_y <= by - 1;

    int last_tcol = -1;
    unsigned short color = 0;
    uint8_t bits = 0;

    for (int fx = 0; fx < 256; fx++) {
      int lng   = BB_FLIP_LONG ? (255 - fx) : fx; // 0..255 asse lungo
      int raw_x = lng + 8;                        // 8..263 -> colonne 1..32
      int tcol  = raw_x >> 3;
      int px    = raw_x & 7;

      if (tcol != last_tcol) {                    // nuovo tile: ricarica
        int off = bb_scan(tcol, trow);
        uint8_t code = vtile[off];
        bits  = pgm_read_byte(&bombbee_gfx[((code << 3) + py) & 0x7FF]);
        color = color_lut[vcol[off]];
        last_tcol = tcol;
      }

      if (bits & (0x80 >> px))
        dst[fx] = color;

      // overlay palla (pen bianco)
      if (ball_row && raw_x >= bx - 4 && raw_x <= bx - 1)
        dst[fx] = 0xFFFF;
    }
  }
}

const unsigned short *BombBee::logo(void) {
  return bombbee_logo;    // 224x96, generato da convert_bombbee_logo.py
}

#endif // ENABLE_BOMBBEE
