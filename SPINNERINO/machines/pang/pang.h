// pang.h - Pang / Buster Bros (Mitchell, 1989) - DICHIARAZIONE classe.
// =====================================================================
// Struttura split .h/.cpp (come goindol): qui solo dichiarazioni + membri,
// implementazione + asset PROGMEM in pang.cpp (incluso dal .ino). Cosi' gli
// asset finiscono in UNA sola TU e audio.cpp resta pulito (usa il bridge).
//
// CPU Z80 singola, KABUKI decifrato offline (op/data separati). Video: il
// frame nativo 384x240 viene reso in pangFrame (PSRAM) da prepare_frame() e
// ricampionato RUOTATO 90° in render_row() (PANG e' landscape, display portrait).
//
// RAM in memory[] (RAMSIZE pang = 0x5800):
//   0x0000 palette(2 banchi) 0x1000 colorRAM 0x1800 videoRAM 0x2800 objRAM 0x3800 work
// Riferimento: src/mame/capcom/mitchell.cpp.
// =====================================================================
#ifndef PANG_H
#define PANG_H

#include "Arduino.h"
#include "../machineBase.h"

#ifdef ENABLE_PANG
#include "ym2413.h"             // tipo Ym2413 (membro opll), header-only leggero

#define PANG_NUM_BANKS   8
// --- Geometria: PANG landscape 384x240; display SPINNERINO portrait (pannello
// montato ruotato). prepare_frame rende il nativo 384x240, render_row ruota 90°
// CCW + STRETCH a tutto schermo (scelta utente 2026-07-01: riempi tutto, no bande,
// deforma leggermente l'aspect). ---
#define PANG_GAME_W      384          // larghezza visibile nativa (gx 0..383)
#define PANG_GAME_H      240          // altezza visibile nativa (gy 0..239)
#define PANG_OUT_W       256          // stride frame_buffer del framework
#define PANG_OUT_ROWS    224          // 28 bande * 8 = 224 righe (py 0..223)

// offset nel buffer memory[]
#define PANG_PAL    0x0000
#define PANG_COL    0x1000
#define PANG_VRAM   0x1800
#define PANG_OBJ    0x2800
#define PANG_WORK   0x3800

// --- Serial EEPROM 93C46 (64 x 16 bit) bit-banged via I/O ports (mitchell.cpp:
// cs@0x08, clk@0x10, di@0x18, do = SYS0 bit7). Blank (0xffff) -> default. ---
struct PangEEPROM {
  unsigned char mem[128];          // 64 words, big-endian
  bool cs, clk;
  unsigned char di;
  bool dout;
  bool start, wen;
  int bits; unsigned long sr;
  int rdbits; unsigned short rdsr;

  void reset() {
    memset(mem, 0xff, sizeof(mem));
    cs = clk = false; di = 0; dout = true;
    start = false; wen = false; bits = 0; sr = 0; rdbits = 0; rdsr = 0;
  }
  void writeCS(unsigned char v) {
    bool nv = (v & 1);
    if (!nv) { start = false; bits = 0; sr = 0; rdbits = 0; dout = true; }
    cs = nv;
  }
  void writeDI(unsigned char v) { di = (v & 1); }
  unsigned char readDO() { return dout ? 1 : 0; }
  void writeCLK(unsigned char v) {
    bool nv = (v & 1);
    if (cs && !clk && nv) rising();
    clk = nv;
  }
  void rising() {
    if (rdbits > 0) {
      // MAME eepromser.cpp: sulla READ un bit DUMMY 0 precede i 16 bit dati
      // (esce sul PRIMO clock di read, dopo gli 8 bit di comando). rdbits parte
      // da 17: il primo (>16) e' il dummy, i successivi 16 sono i dati MSB-first.
      if (rdbits > 16) dout = false;                 // dummy 0
      else { dout = (rdsr & 0x8000) != 0; rdsr <<= 1; }
      rdbits--;
      return;
    }
    if (!start) { if (di) { start = true; bits = 0; sr = 0; } return; }
    sr = (sr << 1) | (di & 1);
    bits++;
    if (bits == 8) {
      int op = (sr >> 6) & 3;
      int addr = sr & 0x3f;
      if (op == 2) {
        rdsr = (unsigned short)((mem[addr * 2] << 8) | mem[addr * 2 + 1]);
        rdbits = 17;                 // 1 dummy 0 + 16 bit dati (no dout qui: e' ancora comando)
        start = false; bits = 0; sr = 0;
      } else if (op == 0) {
        int mode = (addr >> 4) & 3;
        if (mode == 3) wen = true; else if (mode == 0) wen = false;
        start = false; bits = 0; sr = 0;
      }
    } else if (bits == 24) {
      int op = (sr >> 22) & 3;
      int addr = (sr >> 16) & 0x3f;
      unsigned short data = (unsigned short)(sr & 0xffff);
      if (op == 1 && wen) { mem[addr * 2] = data >> 8; mem[addr * 2 + 1] = data & 0xff; }
      else if (op == 3 && wen) { mem[addr * 2] = 0xff; mem[addr * 2 + 1] = 0xff; }
      start = false; bits = 0; sr = 0;
    }
  }
};

// OKI6295 (MSM6295) ADPCM: 4 voci, campioni in pang_oki[] (128KB, 2 banchi).
struct OkiVoice {
  bool playing = false;
  unsigned long base = 0;
  unsigned long sample = 0;
  unsigned long count = 0;
  int vol = 0;
  int signal = 0;
  int sigPrev = 0;
  int step = 0;
};

class pang : public machineBase {
public:
  signed char machineType() override { return MCH_PANG; }
  bool isLandscape() override { return true; }
  // Render pesante (384x240 nativo + ricampionamento 2-passaggi): non regge 57fps.
  // Free-run = emulazione a 57.42Hz reali (timer fisso) disaccoppiata dal render,
  // altrimenti il gioco sarebbe costretto alla velocita' del render (rallentatore).
  bool freeRunEmulation() override { return true; }
  int  ec11PulseHoldMs() override { return 100; }   // paddle hold (IN1 letto +volte/frame)
  const unsigned short *logo(void) override;

  void reset() override;
  unsigned char opZ80(unsigned short Addr) override;
  unsigned char rdZ80(unsigned short Addr) override;
  void          wrZ80(unsigned short Addr, unsigned char Value) override;
  unsigned char inZ80(unsigned short Port) override;
  void          outZ80(unsigned short Port, unsigned char Value) override;
  void run_frame(void) override;
  void prepare_frame(void) override;
  void render_row(short row) override;

  // Bridge audio (chiamato da pang_audio_fill nel .ino -> Audio::pang_render_buffer).
  void renderAudio(int16_t *out, int frames);

private:
  void cacheROM();
  void switchBankCache(unsigned char idx);   // copia il banco attivo in RAM interna
  unsigned char spritePixel(unsigned short code, int x, int y);
  void drawSpritesFull(void);
  unsigned short rgb444ToRGB565(unsigned short c);
  const unsigned short *colorSetChar(unsigned char color);
  const unsigned short *colorSetSprite(unsigned char color);
  unsigned char readInput(int which);
  void gfxctrl(unsigned char v);
  static unsigned char okiRdRaw(unsigned long a);
  void okiInitTable();
  void okiResetChip();
  void okiWrite(unsigned char cmd);
  void ymWrite(unsigned char value);
  void render_audio(int16_t *out, int frames);

  unsigned short *pangFrame = nullptr;   // frame nativo 384x240 (PSRAM)
  unsigned char bankIndex = 0, videoBank = 0, paletteBank = 0, flipscreen = 0, vblankToggle = 0;
  unsigned char vblank = 0;           // bit3 port 0x05: vblank reale (MAME sys0)
  unsigned char irq_source = 0;
  unsigned char *romOpFx = nullptr;   // op_fixed in RAM (32K)
  unsigned char *romDtFx = nullptr;   // data_fixed in RAM (32K)
  unsigned char *romOpBk = nullptr;   // op_banks in PSRAM (128K)
  unsigned char *romDtBk = nullptr;   // data_banks in PSRAM (128K)
  unsigned char *romOpBankC = nullptr; // banco attivo op in RAM INTERNA (16K, veloce)
  unsigned char *romDtBankC = nullptr; // banco attivo data in RAM INTERNA (16K, veloce)
  unsigned char  cachedBank = 0xFF;    // banco attualmente copiato nella cache interna
  int okiCmd = -1;                    // OKI6295: latch numero campione
  unsigned char okiBank = 0;          // OKI6295: banco ROM (gfxctrl bit4)
  OkiVoice okiV[4];                   // OKI6295: 4 voci ADPCM
  unsigned long okiPhase = 0;         // OKI6295: accumulatore ricampionamento
  int okiLpf = 0;                     // OKI6295: stato filtro passa-basso
  unsigned char ymAddr = 0;           // YM2413: latch registro
  unsigned char ymReg[0x40] = {};     // YM2413: shadow registri
  unsigned long ymWrites = 0;         // YM2413: contatore scritture
  Ym2413 opll;                        // YM2413: core FM (musica)
  unsigned long audioUsAcc = 0;       // diagnostica
  unsigned long okiClip = 0;          // diagnostica
  PangEEPROM eeprom;
  unsigned short csbuf[16];
  unsigned short csbuf2[16];
  unsigned short palCache[128 * 16];   // palette pre-convertita RGB565 (1 volta/frame)
  void buildPalCache();
};

#endif // ENABLE_PANG
#endif // PANG_H
