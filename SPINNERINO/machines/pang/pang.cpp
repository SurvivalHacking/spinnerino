// pang.cpp - Pang / Buster Bros (Mitchell 1989) - IMPLEMENTAZIONE.
// Incluso dal .ino (come goindol.cpp). Gli asset PROGMEM sono inclusi SOLO qui.
#include "pang.h"

#ifdef ENABLE_PANG
#include "esp_heap_caps.h"
#include <math.h>
#include <string.h>
#include "pang_rom_assets.h"     // pang_op_fixed/data_fixed/op_banks/data_banks/chars/sprites/oki
#include "pang_logo.h"           // pang_logo (224x96 RGB565)

// ===================== OKI6295 ADPCM (tabelle, questa TU) =================
static int           s_okiDiff[49 * 16];
static bool          s_okiTabDone = false;
static const signed char  s_okiIndexShift[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };
static const unsigned char s_okiVolTab[16]   = { 32, 22, 16, 11, 8, 6, 4, 3, 2, 0, 0, 0, 0, 0, 0, 0 };

const unsigned short *pang::logo(void) { return pang_logo; }

void pang::reset() {
  cacheROM();
  okiInitTable();
  okiResetChip();
  ymAddr = 0; ymWrites = 0; memset(ymReg, 0, sizeof(ymReg));
  opll.init(32000);                  // YM2413 @ 4MHz -> uscita 32kHz (rate audio SPINNERINO)
  memset(memory, 0, RAMSIZE);
  bankIndex = 0; videoBank = 0; paletteBank = 0; flipscreen = 0; vblankToggle = 0;
  switchBankCache(0);                // riallinea la cache banco interna a bank 0
  irq_source = 0;
  eeprom.reset();
  current_cpu = 0;
  game_started = 0;
  ResetZ80(&cpu[0]);
  if (!pangFrame) {
    // Frame nativo 384x240 (~180KB): il render fa 2 passaggi (prepare_frame scrive,
    // render_row rilegge). In PSRAM = ~150k accessi/frame su bus lento -> fps bassi.
    // Provo RAM INTERNA (banda ~GB/s); fallback PSRAM se non c'e' spazio.
    unsigned long sz = (unsigned long)PANG_GAME_W * PANG_GAME_H * 2;
    pangFrame = (unsigned short *)heap_caps_malloc(sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!pangFrame) pangFrame = (unsigned short *)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
  }
  if (pangFrame) memset(pangFrame, 0, (unsigned long)PANG_GAME_W * PANG_GAME_H * 2);
}

// Copia la ROM KABUKI (op/data) in RAM interna/PSRAM una volta (toglie cache-miss flash).
void pang::cacheROM() {
  if (romOpFx) return;
  romOpFx = (unsigned char *)heap_caps_malloc(0x8000, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  romDtFx = (unsigned char *)heap_caps_malloc(0x8000, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!romOpFx) romOpFx = (unsigned char *)heap_caps_malloc(0x8000, MALLOC_CAP_SPIRAM);
  if (!romDtFx) romDtFx = (unsigned char *)heap_caps_malloc(0x8000, MALLOC_CAP_SPIRAM);
  if (romOpFx) for (unsigned long i = 0; i < 0x8000; i++) romOpFx[i] = pgm_read_byte_near(&pang_op_fixed[i]);
  if (romDtFx) for (unsigned long i = 0; i < 0x8000; i++) romDtFx[i] = pgm_read_byte_near(&pang_data_fixed[i]);
  romOpBk = (unsigned char *)heap_caps_malloc(0x20000, MALLOC_CAP_SPIRAM);
  romDtBk = (unsigned char *)heap_caps_malloc(0x20000, MALLOC_CAP_SPIRAM);
  if (romOpBk) for (unsigned long i = 0; i < 0x20000; i++) romOpBk[i] = pgm_read_byte_near(&pang_op_banks[i]);
  if (romDtBk) for (unsigned long i = 0; i < 0x20000; i++) romDtBk[i] = pgm_read_byte_near(&pang_data_banks[i]);

  // Cache del banco attivo in RAM INTERNA veloce: ~94% dell'esecuzione e' in
  // 0x8000-0xBFFF (bancato). Senza questa cache ogni fetch e' una lettura PSRAM
  // lenta -> emulazione al rallentatore. Copiamo il banco selezionato (16K) in
  // RAM interna ad ogni cambio banco (raro); i fetch diventano tutti veloci.
  romOpBankC = (unsigned char *)heap_caps_malloc(0x4000, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  romDtBankC = (unsigned char *)heap_caps_malloc(0x4000, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  cachedBank = 0xFF;
  switchBankCache(0);
}

// Copia il banco 'idx' (op+data, 16K ciascuno) da PSRAM/flash in RAM interna.
void pang::switchBankCache(unsigned char idx) {
  idx &= (PANG_NUM_BANKS - 1);
  if (idx == cachedBank) return;
  cachedBank = idx;
  if (!romOpBankC || !romDtBankC) return;   // alloc fallita -> fetch usa il fallback
  unsigned long base = (unsigned long)idx * 0x4000;
  if (romOpBk) memcpy(romOpBankC, romOpBk + base, 0x4000);
  else for (unsigned long i = 0; i < 0x4000; i++) romOpBankC[i] = pgm_read_byte_near(&pang_op_banks[base + i]);
  if (romDtBk) memcpy(romDtBankC, romDtBk + base, 0x4000);
  else for (unsigned long i = 0; i < 0x4000; i++) romDtBankC[i] = pgm_read_byte_near(&pang_data_banks[base + i]);
}

// ---- KABUKI opcode fetch ----
unsigned char pang::opZ80(unsigned short Addr) {
  if (Addr < 0x8000) return romOpFx ? romOpFx[Addr] : pgm_read_byte_near(&pang_op_fixed[Addr]);
  if (Addr < 0xC000) {
    if (romOpBankC && bankIndex == cachedBank) return romOpBankC[Addr - 0x8000];  // RAM interna
    unsigned long o = (unsigned long)bankIndex * 0x4000 + (Addr - 0x8000);
    return romOpBk ? romOpBk[o] : pgm_read_byte_near(&pang_op_banks[o]);
  }
  return rdZ80(Addr);
}

// ---- KABUKI data read + RAM/IO ----
unsigned char pang::rdZ80(unsigned short Addr) {
  if (Addr < 0x8000) return romDtFx ? romDtFx[Addr] : pgm_read_byte_near(&pang_data_fixed[Addr]);
  if (Addr < 0xC000) {
    if (romDtBankC && bankIndex == cachedBank) return romDtBankC[Addr - 0x8000];  // RAM interna
    unsigned long o = (unsigned long)bankIndex * 0x4000 + (Addr - 0x8000);
    return romDtBk ? romDtBk[o] : pgm_read_byte_near(&pang_data_banks[o]);
  }
  if (Addr < 0xC800) return memory[PANG_PAL + (Addr - 0xC000) + (paletteBank ? 0x800 : 0)];
  if (Addr < 0xD000) return memory[PANG_COL + (Addr - 0xC800)];
  if (Addr < 0xE000) {
    if (videoBank) return memory[PANG_OBJ + (Addr - 0xD000)];
    return memory[PANG_VRAM + (Addr - 0xD000)];
  }
  return memory[PANG_WORK + (Addr - 0xE000)];
}

void pang::wrZ80(unsigned short Addr, unsigned char Value) {
  if (Addr < 0xC000) return;                       // ROM: ignora (niente conteggio)
  unsigned char *p;
  if (Addr < 0xC800)      p = &memory[PANG_PAL + (Addr - 0xC000) + (paletteBank ? 0x800 : 0)];
  else if (Addr < 0xD000) p = &memory[PANG_COL + (Addr - 0xC800)];
  else if (Addr < 0xE000) p = videoBank ? &memory[PANG_OBJ + (Addr - 0xD000)]
                                        : &memory[PANG_VRAM + (Addr - 0xD000)];
  else                    p = &memory[PANG_WORK + (Addr - 0xE000)];
  *p = Value;
}

unsigned char pang::inZ80(unsigned short Port) {
  Port &= 0xFF;
  switch (Port) {
    case 0x00: return readInput(0);
    case 0x01: return readInput(1);
    case 0x02: return readInput(2);
    case 0x05: {
      // MAME port5_r: (sys0 & 0xfe) | (irq_source & 1). bit0=irq_source,
      // bit3=VBLANK REALE (segnale schermo, indipendente da irq_source),
      // bit7=EEPROM DO (active high). Base sys0 idle = 0x76 (bit1,2,4,5,6).
      unsigned char v = 0x76;
      if (irq_source) v |= 0x01;          // bit0 = irq_source
      if (vblank)     v |= 0x08;          // bit3 = vblank (separato!)
      if (eeprom.readDO()) v |= 0x80;     // bit7 = EEPROM DO
      return v;
    }
  }
  return 0xFF;
}

void pang::outZ80(unsigned short Port, unsigned char Value) {
  Port &= 0xFF;
  switch (Port) {
    case 0x00: gfxctrl(Value); break;
    case 0x02: { unsigned char nb = Value & (PANG_NUM_BANKS - 1); bankIndex = nb; switchBankCache(nb); } break;
    case 0x03: ymWrite(Value); break;
    case 0x04: ymAddr = Value; break;
    case 0x05: okiWrite(Value); break;
    case 0x06: break;                              // 86S105 DMA (TODO)
    case 0x07: videoBank = Value & 1; break;
    case 0x08: eeprom.writeCS(Value); break;
    case 0x10: eeprom.writeCLK(Value); break;
    case 0x18: eeprom.writeDI(Value); break;
  }
}

void pang::run_frame(void) {
  // Riscrittura fedele allo schema dei giochi FUNZIONANTI di questo framework
  // (Gigas / Goindol) + timing reale MAME capcom/mitchell.cpp:
  //   - Z80 @ 8 MHz, refresh 57.42 Hz -> 139324 cicli/frame ~= 28000 StepZ80/frame
  //     (stessa taratura ~5 cicli/istr dei giochi che vanno alla giusta velocita':
  //     Gigas 6MHz = 20000/frame).
  //   - 2 IRQ/frame: mitchell_irq a scanline 0 (irq_source=0) e 240 (irq_source=1),
  //     linea in HOLD_LINE. StepZ80 non onora la linea latchata, quindi consegno
  //     l'IRQ (RST38) appena la CPU riabilita gli interrupt (IFF1): il boot fa
  //     DI...EI nell'attesa vblank e altrimenti l'IRQ andrebbe perso.
  //   - UN game-frame per run_frame, come TUTTI i giochi che funzionano. Niente
  //     spin-detection / CAP / multistep (machinery che gli altri non hanno e che
  //     alterava il numero di frame-gioco eseguiti).
  vblankToggle++;
  const int HALF = 3500;                      // 3500*4 = 14000 StepZ80/half -> 28000/frame
  for (int half = 0; half < 2; half++) {
    irq_source = (half == 1) ? 1 : 0;         // MAME: scanline 240 (2a meta') -> irq_source=1
    vblank     = (half == 1) ? 1 : 0;         // vblank reale attivo nella 2a meta' (port 0x05 bit3)
    bool irqPending = true;                   // nuova IRQ vblank (HOLD_LINE)
    for (int i = 0; i < HALF; i++) {
      StepZ80(&cpu[0]); StepZ80(&cpu[0]); StepZ80(&cpu[0]); StepZ80(&cpu[0]);
      if (irqPending && (cpu[0].IFF & IFF_1)) {   // consegna appena EI abilita gli interrupt
        IntZ80(&cpu[0], INT_RST38);
        irqPending = false;
      }
    }
  }
  if (!game_started) game_started = 1;
}

// ---- prepare_frame: render NATIVO 384x240 in pangFrame (PSRAM) ----
// Pre-converte tutta la palette RAM (128 set x 16 pen) in RGB565 una volta per
// frame, invece di riconvertire 16 colori ad ogni cambio-tile in colorSetChar.
void pang::buildPalCache() {
  for (int i = 0; i < 128 * 16; i++) {
    unsigned short c = memory[PANG_PAL + i * 2] | (memory[PANG_PAL + i * 2 + 1] << 8);
    palCache[i] = rgb444ToRGB565(c);
  }
}

void pang::prepare_frame(void) {
  if (!pangFrame) return;
  buildPalCache();
  for (int gy = 0; gy < PANG_GAME_H; gy++) {
    int sy   = gy + 8;                              // finestra visibile Y
    int trow = sy >> 3;
    int trc  = sy & 7;
    unsigned short *dst = pangFrame + gy * PANG_GAME_W;
    int lastCol = -1;
    unsigned char lastColor = 0xFF;
    unsigned char flipx = 0, lo0 = 0, lo1 = 0, hi0 = 0, hi1 = 0;
    const unsigned short *pal = csbuf;
    for (int gx = 0; gx < PANG_GAME_W; gx++) {
      int sx  = gx + 64;                            // finestra visibile X
      int col = sx >> 3;
      if (col != lastCol) {
        lastCol = col;
        int ti = trow * 64 + col;                   // tilemap 64x32
        unsigned short code = memory[PANG_VRAM + 2*ti] | (memory[PANG_VRAM + 2*ti + 1] << 8);
        unsigned char attr = memory[PANG_COL + ti];
        flipx = (attr & 0x80) ? 1 : 0;
        unsigned char cc = attr & 0x7f;
        if (cc != lastColor) { pal = colorSetChar(cc); lastColor = cc; }
        unsigned long cb = (unsigned long)code * 16 + trc * 2;
        lo0 = pgm_read_byte_near(&pang_chars[cb]);
        lo1 = pgm_read_byte_near(&pang_chars[cb + 1]);
        hi0 = pgm_read_byte_near(&pang_chars[PANG_CHARS_HALF + cb]);
        hi1 = pgm_read_byte_near(&pang_chars[PANG_CHARS_HALF + cb + 1]);
      }
      int tx = sx & 7; if (flipx) tx = 7 - tx;
      unsigned char pen;
      if (tx < 4) {
        pen =  ((lo0 >> (7 - tx)) & 1)
            | (((lo0 >> (3 - tx)) & 1) << 1)
            | (((hi0 >> (7 - tx)) & 1) << 2)
            | (((hi0 >> (3 - tx)) & 1) << 3);
      } else {
        int xx = tx - 4;
        pen =  ((lo1 >> (7 - xx)) & 1)
            | (((lo1 >> (3 - xx)) & 1) << 1)
            | (((hi1 >> (7 - xx)) & 1) << 2)
            | (((hi1 >> (3 - xx)) & 1) << 3);
      }
      dst[gx] = pal[pen];
    }
  }
  drawSpritesFull();
}

// ---- render_row: ricampiona pangFrame -> frame_buffer, RUOTATO 90° ORARIO + STRETCH ----
// PANG 384x240 landscape; pannello portrait montato ruotato. Scelta utente
// 2026-07-01: riempi tutto (stretch, no bande) + rotazione 90° ORARIA (CW) cosi'
// le scritte si leggono dritte. Trasposizione assi: py indicizza la LARGHEZZA
// gioco (gx), px indicizza l'ALTEZZA gioco (gy).
//   gx = 383 - py*384/224   ;   gy = px*240/256      (transpose = rotazione 90°)
// Se dovesse risultare ruotato al contrario (CCW), SCAMBIARE le inversioni:
//   gx = py*384/224   ;   gy = 239 - px*240/256
void pang::render_row(short row) {
  if (row < 0 || row >= 28) return;
  for (int r = 0; r < 8; r++) {
    int py = row * 8 + r;                                // 0..223
    unsigned short *fb = frame_buffer + r * PANG_OUT_W;
    if (!pangFrame) {
      for (int px = 0; px < PANG_OUT_W; px++) fb[px] = 0;
      continue;
    }
    // py (asse orizz. fisico) -> larghezza gioco gx (invertito). Costante nella
    // riga: campioniamo una COLONNA di pangFrame (stride PANG_GAME_W).
    int gx = PANG_GAME_W - 1 - (py * PANG_GAME_W) / PANG_OUT_ROWS;
    if (gx < 0) gx = 0; else if (gx >= PANG_GAME_W) gx = PANG_GAME_W - 1;
    for (int px = 0; px < PANG_OUT_W; px++) {
      // px (asse vert. fisico) -> altezza gioco gy (diretto). px 0..255 -> 0..239.
      int gy = (px * PANG_GAME_H) / PANG_OUT_W;
      fb[px] = pangFrame[gy * PANG_GAME_W + gx];
    }
  }
}

// --- decodifica sprite 16x16 4bpp ---
unsigned char pang::spritePixel(unsigned short code, int x, int y) {
  static const int xoff[16] = {0,1,2,3,8,9,10,11,256,257,258,259,264,265,266,267};
  unsigned long base = (unsigned long)code * 64;
  int bb = y * 16 + xoff[x];
  unsigned long bi = base + (bb >> 3);
  int sh = 7 - (bb & 7);
  unsigned long bi4 = base + ((bb + 4) >> 3);
  int sh4 = 7 - ((bb + 4) & 7);
  unsigned char lo  = pgm_read_byte_near(&pang_sprites[bi]);
  unsigned char lo4 = pgm_read_byte_near(&pang_sprites[bi4]);
  unsigned char hi  = pgm_read_byte_near(&pang_sprites[PANG_SPRITES_HALF + bi]);
  unsigned char hi4 = pgm_read_byte_near(&pang_sprites[PANG_SPRITES_HALF + bi4]);
  unsigned char p0 = (lo >> sh) & 1, p1 = (lo4 >> sh4) & 1;
  unsigned char p2 = (hi >> sh) & 1, p3 = (hi4 >> sh4) & 1;
  return p0 | (p1 << 1) | (p2 << 2) | (p3 << 3);
}

// Sprite nel frame nativo pangFrame (coord gioco gx 0..383, gy 0..239).
void pang::drawSpritesFull(void) {
  for (int offs = 0x1000 - 0x40; offs >= 0; offs -= 0x20) {
    unsigned char c0 = memory[PANG_OBJ + offs];
    unsigned char at = memory[PANG_OBJ + offs + 1];
    int color = at & 0x0f;
    int sx = memory[PANG_OBJ + offs + 3] + ((at & 0x10) << 4);
    int sy = ((memory[PANG_OBJ + offs + 2] + 8) & 0xff) - 8;
    unsigned short code = c0 + ((at & 0xe0) << 3);
    int fy = sy - 8;
    const unsigned short *pal = colorSetSprite(color);
    for (int yy = 0; yy < 16; yy++) {
      int gy = fy + yy;
      if (gy < 0 || gy >= PANG_GAME_H) continue;
      unsigned short *dst = pangFrame + gy * PANG_GAME_W;
      for (int xx = 0; xx < 16; xx++) {
        int gx = sx + xx - 64;
        if (gx < 0 || gx >= PANG_GAME_W) continue;
        unsigned char pen = spritePixel(code, xx, yy);
        if (pen == 15) continue;                     // transpen
        dst[gx] = pal[pen];
      }
    }
  }
}

// --- palette: xRGB444 in palette RAM (2 byte/colore, little-endian) ---
unsigned short pang::rgb444ToRGB565(unsigned short c) {
  int r = (c >> 8) & 0xF, g = (c >> 4) & 0xF, b = c & 0xF;
  unsigned short v = ((r * 2) << 11) | ((g * 4) << 5) | (b * 2);
  return (v >> 8) | (v << 8);   // byte-swap per il pannello
}
// Ritornano un puntatore dentro palCache (gia' convertita in buildPalCache):
// niente conversione rgb444->565 per-tile/per-sprite.
const unsigned short *pang::colorSetChar(unsigned char color)   { return &palCache[(color & 0x7f) * 16]; }
const unsigned short *pang::colorSetSprite(unsigned char color) { return &palCache[(color & 0x0f) * 16]; }

unsigned char pang::readInput(int which) {
  unsigned char raw = input ? input->buttons_get() : 0;
  if (which == 0) {                       // IN0: coin / start
    unsigned char v = 0xFF;
    if (raw & BUTTON_COIN)  v &= ~0x80;
    if (raw & BUTTON_START) v &= ~0x08;
    return v;
  }
  if (which == 1) {                       // IN1: P1 joystick + shot
    unsigned char v = 0xFF;
    if (raw & BUTTON_UP)    v &= ~0x80;
    if (raw & BUTTON_DOWN)  v &= ~0x40;
    if (raw & BUTTON_LEFT)  v &= ~0x20;
    if (raw & BUTTON_RIGHT) v &= ~0x10;
    if (raw & BUTTON_FIRE)  v &= ~0x08;   // BUTTON1 = shot
    return v;
  }
  return 0xFF;
}

void pang::gfxctrl(unsigned char v) {
  // mitchell.cpp gfxctrl_w: bit2=flip, bit4=OKI bank, bit5=palette bank.
  paletteBank = (v >> 5) & 1;
  flipscreen  = (v >> 2) & 1;
  okiBank     = (v >> 4) & 1;
}

unsigned char pang::okiRdRaw(unsigned long a) {
  return pgm_read_byte_near(&pang_oki[a & 0x1FFFF]);
}

void pang::okiInitTable() {
  if (s_okiTabDone) return;
  s_okiTabDone = true;
  static const signed char nbl2bit[16][4] = {
    { 1,0,0,0},{ 1,0,0,1},{ 1,0,1,0},{ 1,0,1,1},{ 1,1,0,0},{ 1,1,0,1},{ 1,1,1,0},{ 1,1,1,1},
    {-1,0,0,0},{-1,0,0,1},{-1,0,1,0},{-1,0,1,1},{-1,1,0,0},{-1,1,0,1},{-1,1,1,0},{-1,1,1,1}
  };
  for (int st = 0; st <= 48; st++) {
    int sv = (int)floor(16.0 * pow(11.0 / 10.0, (double)st));
    for (int nb = 0; nb < 16; nb++)
      s_okiDiff[st * 16 + nb] = nbl2bit[nb][0] * (sv * nbl2bit[nb][1] + sv / 2 * nbl2bit[nb][2] + sv / 4 * nbl2bit[nb][3] + sv / 8);
  }
}

void pang::okiResetChip() {
  for (int v = 0; v < 4; v++) okiV[v] = OkiVoice();
  okiCmd = -1; okiPhase = 0; okiLpf = 0;
}

void pang::okiWrite(unsigned char cmd) {
  if (okiCmd != -1) {                          // 2o byte: voci (bit4-7) + volume (bit0-3)
    int mask = cmd >> 4;
    unsigned long bankbase = okiBank ? 0x10000UL : 0;
    for (int v = 0; v < 4; v++) if ((mask >> v) & 1) {
      OkiVoice &V = okiV[v];
      if (!V.playing) {
        unsigned long hb = bankbase + (unsigned long)okiCmd * 8;
        unsigned long start = ((unsigned long)okiRdRaw(hb) << 16) | ((unsigned long)okiRdRaw(hb + 1) << 8) | okiRdRaw(hb + 2);
        unsigned long stop  = ((unsigned long)okiRdRaw(hb + 3) << 16) | ((unsigned long)okiRdRaw(hb + 4) << 8) | okiRdRaw(hb + 5);
        start &= 0x3ffff; stop &= 0x3ffff;
        if (start < stop) {
          V.playing = true;
          V.base = bankbase + start;
          V.sample = 0;
          V.count = 2 * (stop - start + 1);
          V.signal = 0; V.step = 0; V.sigPrev = 0;
          V.vol = s_okiVolTab[cmd & 0x0f];
        }
      }
    }
    okiCmd = -1;
  } else if (cmd & 0x80) {                     // 1o byte: latch numero campione
    okiCmd = cmd & 0x7f;
  } else {                                     // stop: voci in bit3-6
    int mask = cmd >> 3;
    for (int v = 0; v < 4; v++) if ((mask >> v) & 1) okiV[v].playing = false;
  }
}

void pang::ymWrite(unsigned char value) {
  ymReg[ymAddr & 0x3f] = value;
  ymWrites++;
  opll.writeReg(ymAddr, value);
}

void pang::renderAudio(int16_t *out, int frames) { render_audio(out, frames); }

// Genera 'frames' campioni mono 16-bit a 32kHz (OKI ADPCM + YM2413 OPLL).
void pang::render_audio(int16_t *out, int frames) {
  unsigned long _ta = micros();
  static const unsigned long OKI_STEP = (7576UL * 4096) / 32000;
  for (int i = 0; i < frames; i++) {
    okiPhase += OKI_STEP;
    while (okiPhase >= 4096) {
      okiPhase -= 4096;
      for (int v = 0; v < 4; v++) {
        OkiVoice &V = okiV[v];
        if (!V.playing) continue;
        V.sigPrev = V.signal;
        unsigned char b = okiRdRaw(V.base + (V.sample >> 1));
        int nib = (V.sample & 1) ? (b & 0x0f) : (b >> 4);
        V.signal += s_okiDiff[V.step * 16 + (nib & 15)];
        if (V.signal > 2047) V.signal = 2047; else if (V.signal < -2048) V.signal = -2048;
        V.step += s_okiIndexShift[nib & 7];
        if (V.step > 48) V.step = 48; else if (V.step < 0) V.step = 0;
        if (++V.sample >= V.count) V.playing = false;
      }
    }
    int frac = (int)okiPhase;
    int acc = 0;
    for (int v = 0; v < 4; v++) {
      OkiVoice &V = okiV[v];
      if (!V.playing) continue;
      int interp = V.sigPrev + (((V.signal - V.sigPrev) * frac) >> 12);
      acc += interp * V.vol;
    }
    int raw = acc >> 2;
    okiLpf += ((raw - okiLpf) * 3) >> 2;
    int okiSample = okiLpf >> 6;
    int ymSample = opll.calc() >> 4;
    // Volume PANG piu' alto: GUADAGNO reale sul segnale (x2), non solo tetto del
    // clamp (il segnale non arrivava a ±500, quindi alzare il clamp da solo non
    // cambiava nulla). Clamp ±1200: valueToBuffer scala *128/volumeSetting su
    // ±32767, a volume default (5) la soglia di clip e' ~1280 -> resto sotto.
    int mixed = (okiSample + ymSample) * 2;
    if (mixed > 1200) { mixed = 1200; okiClip++; } else if (mixed < -1200) { mixed = -1200; okiClip++; }
    out[i] = (int16_t)mixed;
  }
  audioUsAcc += micros() - _ta;
}

#endif // ENABLE_PANG
