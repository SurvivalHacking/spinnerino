#pragma once
// =====================================================================
//  YM2413 (OPLL) - core FM compatto per ESP32
//  Port fedele dell'algoritmo di emu2413 (Mitsutaka Okazaki, MIT).
//  Solo 9 canali melodici (niente modalita' ritmica: Pang non la usa).
//  clock fisso 4.000 MHz (Pang: 16MHz/4). Output al sample rate richiesto.
// =====================================================================
#include <Arduino.h>
#include <math.h>
#include <string.h>

class Ym2413 {
public:
  void init(int outputRate) {
    buildTables();
    // Accumulatore di rate INTERO (niente double, emulati via software sull'ESP32).
    // rate interno = clk/288 (~13.9kHz, QUARTER) per ridurre il carico. updates per
    // campione d'uscita = clk / (outputRate*288) = 4000000/6912000 = 0.579.
    ymRateInc    = 4000000UL;                       // clock OPLL
    ymRateThresh = (unsigned long)outputRate * 288; // outputRate * divisore(/288)
    reset();
  }

  void reset() {
    memset(reg, 0, sizeof(reg));
    eg_counter = 0; pm_phase = 0; am_phase = 0; lfo_am = 0;
    slot_key_status = 0; ymAcc = 0; mix_out = 0;
    buildRomPatches();
    // patch custom (indice 0) azzerato
    memset(&patches[0][0], 0, sizeof(Patch));
    memset(&patches[0][1], 0, sizeof(Patch));
    for (int i = 0; i < 18; i++) resetSlot(&slot[i], i);
    for (int ch = 0; ch < 9; ch++) { patch_number[ch] = 0; setPatch(ch, 0); }
  }

  // Scrittura registro (addr gia' latchato, data = valore)
  void writeReg(uint8_t addr, uint8_t data) {
    addr &= 0x3f;
    reg[addr] = data;
    if (addr <= 0x07) {                         // patch custom: ricostruisci da reg 0x00-0x07
      dumpToPatch(reg, &patches[0][0]);         // mod
      dumpToPatchCar(reg, &patches[0][1]);      // car
      for (int ch = 0; ch < 9; ch++) if (patch_number[ch] == 0) {
        requestUpdate(MOD(ch), UPDATE_ALL); requestUpdate(CAR(ch), UPDATE_ALL);
      }
    } else if (addr == 0x0e) {
      // modalita' ritmica non implementata (non usata da Pang) -> ignorata
    } else if (addr >= 0x10 && addr <= 0x18) {
      int ch = addr - 0x10;
      setFnumber(ch, data + ((reg[0x20 + ch] & 1) << 8));
    } else if (addr >= 0x20 && addr <= 0x28) {
      int ch = addr - 0x20;
      setFnumber(ch, ((data & 1) << 8) + reg[0x10 + ch]);
      setBlock(ch, (data >> 1) & 7);
      setSusFlag(ch, (data >> 5) & 1);
      updateKeyStatus();
    } else if (addr >= 0x30 && addr <= 0x38) {
      int ch = addr - 0x30;
      setPatch(ch, (data >> 4) & 15);
      setVolume(ch, (data & 15) << 2);
    }
  }

  // Un campione d'uscita (decimazione dal rate interno). IRAM: niente cache-miss flash.
  int16_t calc() {
    ymAcc += ymRateInc;
    while (ymAcc >= ymRateThresh) {
      ymAcc -= ymRateThresh;
      updateOutput();
      int32_t o = 0;
      for (int i = 0; i < 9; i++) o += ch_out[i];
      mix_out = (int16_t)o;
    }
    return mix_out;
  }

private:
  // ---- costanti ----
  static const int PG_BITS = 10, PG_WIDTH = 1024;
  static const int DP_BITS = 19, DP_WIDTH = 1 << 19, DP_BASE_BITS = 19 - 10;
  static const int EG_MUTE = 127, EG_MAX = 123;
  enum { ATTACK, DECAY, SUSTAIN, RELEASE, DAMP };
  enum { UPDATE_WS = 1, UPDATE_TLL = 2, UPDATE_RKS = 4, UPDATE_EG = 8, UPDATE_ALL = 255 };
  static const int DAMPER_RATE = 12;

  struct Patch { uint8_t TL, FB, EG, ML, AR, DR, SL, RR, KR, KL, AM, PM, WS; };

  struct Slot {
    uint8_t type;            // 0=modulatore 1=portante
    const Patch* patch;
    int32_t output[2];
    const uint16_t* wave;
    uint32_t pg_phase, pg_out;
    uint16_t blk_fnum, fnum; uint8_t blk;
    uint8_t eg_state; int32_t volume; uint8_t key_flag, sus_flag;
    uint16_t tll; uint8_t rks, eg_rate_h, eg_rate_l; uint32_t eg_shift, eg_out;
    uint8_t update_requests;
  };

  // ---- stato ----
  uint8_t reg[0x40];
  Patch patches[16][2];        // [0]=custom, [1..15]=ROM ; [ ][0]=mod [ ][1]=car
  uint8_t patch_number[9];
  Slot slot[18];
  uint32_t eg_counter, pm_phase, am_phase; uint8_t lfo_am;
  uint32_t slot_key_status;
  unsigned long ymRateInc, ymRateThresh, ymAcc; int16_t mix_out;
  int32_t ch_out[9];

  Slot* MOD(int ch) { return &slot[ch << 1]; }
  Slot* CAR(int ch) { return &slot[(ch << 1) | 1]; }

  // ---- tabelle (statiche, costruite una volta) ----
  static uint16_t expTbl[256];
  static uint16_t fullsin[PG_WIDTH];
  static uint16_t halfsin[PG_WIDTH];
  static uint8_t  amTbl[210];
  static int8_t   pmTbl[8][8];
  static uint8_t  egStep[4][8];
  static uint32_t mlTbl[16];
  static double   klTbl[16];
  static bool     tablesReady;

  const uint16_t* waveMap(int ws) { return ws ? halfsin : fullsin; }

  static void buildTables() {
    if (tablesReady) return;
    // exp_table: round((2^(x/256)-1)*1024)
    for (int x = 0; x < 256; x++) expTbl[x] = (uint16_t)lround((pow(2.0, x / 256.0) - 1.0) * 1024.0);
    // fullsin: -log2(sin((x+0.5)*pi/(PG_WIDTH/4)/2))*256, primo quarto + simmetrie
    for (int x = 0; x < PG_WIDTH / 4; x++) {
      double s = sin((x + 0.5) * M_PI / (PG_WIDTH / 4) / 2.0);
      fullsin[x] = (uint16_t)lround(-log(s) / log(2.0) * 256.0);
    }
    for (int x = 0; x < PG_WIDTH / 4; x++) fullsin[PG_WIDTH / 4 + x] = fullsin[PG_WIDTH / 4 - x - 1];
    for (int x = 0; x < PG_WIDTH / 2; x++) fullsin[PG_WIDTH / 2 + x] = 0x8000 | fullsin[x];
    for (int x = 0; x < PG_WIDTH / 2; x++) halfsin[x] = fullsin[x];
    for (int x = PG_WIDTH / 2; x < PG_WIDTH; x++) halfsin[x] = 0xfff;
    // am_table (curva triangolare verificata)
    static const uint8_t amraw[210] = {
      0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1, 2,2,2,2,2,2,2,2,3,3,3,3,3,3,3,3,
      4,4,4,4,4,4,4,4,5,5,5,5,5,5,5,5, 6,6,6,6,6,6,6,6,7,7,7,7,7,7,7,7,
      8,8,8,8,8,8,8,8,9,9,9,9,9,9,9,9, 10,10,10,10,10,10,10,10,11,11,11,11,11,11,11,11,
      12,12,12,12,12,12,12,12, 13,13,13, 12,12,12,12,12,12,12,12,
      11,11,11,11,11,11,11,11,10,10,10,10,10,10,10,10, 9,9,9,9,9,9,9,9,8,8,8,8,8,8,8,8,
      7,7,7,7,7,7,7,7,6,6,6,6,6,6,6,6, 5,5,5,5,5,5,5,5,4,4,4,4,4,4,4,4,
      3,3,3,3,3,3,3,3,2,2,2,2,2,2,2,2, 1,1,1,1,1,1,1,1,0,0,0,0,0,0,0 };
    memcpy(amTbl, amraw, sizeof(amTbl));
    static const int8_t pmraw[8][8] = {
      {0,0,0,0,0,0,0,0},{0,0,1,0,0,0,-1,0},{0,1,2,1,0,-1,-2,-1},{0,1,3,1,0,-1,-3,-1},
      {0,2,4,2,0,-2,-4,-2},{0,2,5,2,0,-2,-5,-2},{0,3,6,3,0,-3,-6,-3},{0,3,7,3,0,-3,-7,-3} };
    memcpy(pmTbl, pmraw, sizeof(pmTbl));
    static const uint8_t egraw[4][8] = {
      {0,1,0,1,0,1,0,1},{0,1,0,1,1,1,0,1},{0,1,1,1,0,1,1,1},{0,1,1,1,1,1,1,1} };
    memcpy(egStep, egraw, sizeof(egStep));
    static const uint32_t mlraw[16] = {1,2,4,6,8,10,12,14,16,18,20,20,24,24,30,30};
    memcpy(mlTbl, mlraw, sizeof(mlTbl));
    static const double klraw[16] = {0,18,24,27.75,30,32.25,33.75,35.25,36,37.5,38.25,39,39.75,40.5,41.25,42};
    memcpy(klTbl, klraw, sizeof(klTbl));
    tablesReady = true;
  }

  // ROM dei 15 strumenti YM2413 (default_inst[0] di emu2413, verificato)
  void buildRomPatches() {
    static const uint8_t rom[16][8] = {
      {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
      {0x71,0x61,0x1e,0x17,0xd0,0x78,0x00,0x17},
      {0x13,0x41,0x1a,0x0d,0xd8,0xf7,0x23,0x13},
      {0x13,0x01,0x99,0x00,0xf2,0xc4,0x21,0x23},
      {0x11,0x61,0x0e,0x07,0x8d,0x64,0x70,0x27},
      {0x32,0x21,0x1e,0x06,0xe1,0x76,0x01,0x28},
      {0x31,0x22,0x16,0x05,0xe0,0x71,0x00,0x18},
      {0x21,0x61,0x1d,0x07,0x82,0x81,0x11,0x07},
      {0x33,0x21,0x2d,0x13,0xb0,0x70,0x00,0x07},
      {0x61,0x61,0x1b,0x06,0x64,0x65,0x10,0x17},
      {0x41,0x61,0x0b,0x18,0x85,0xf0,0x81,0x07},
      {0x33,0x01,0x83,0x11,0xea,0xef,0x10,0x04},
      {0x17,0xc1,0x24,0x07,0xf8,0xf8,0x22,0x12},
      {0x61,0x50,0x0c,0x05,0xd2,0xf5,0x40,0x42},
      {0x01,0x01,0x55,0x03,0xe9,0x90,0x03,0x02},
      {0x41,0x41,0x89,0x03,0xf1,0xe4,0xc0,0x13} };
    for (int n = 1; n < 16; n++) { dumpToPatch(rom[n], &patches[n][0]); dumpToPatchCar(rom[n], &patches[n][1]); }
  }

  // dump 8 byte -> patch modulatore (slot 0)
  static void dumpToPatch(const uint8_t* d, Patch* p) {
    p->AM = (d[0] >> 7) & 1; p->PM = (d[0] >> 6) & 1; p->EG = (d[0] >> 5) & 1; p->KR = (d[0] >> 4) & 1;
    p->ML = d[0] & 15; p->KL = (d[2] >> 6) & 3; p->TL = d[2] & 63; p->FB = d[3] & 7;
    p->WS = (d[3] >> 3) & 1; p->AR = (d[4] >> 4) & 15; p->DR = d[4] & 15;
    p->SL = (d[6] >> 4) & 15; p->RR = d[6] & 15;
  }
  // dump 8 byte -> patch portante (slot 1)
  static void dumpToPatchCar(const uint8_t* d, Patch* p) {
    p->AM = (d[1] >> 7) & 1; p->PM = (d[1] >> 6) & 1; p->EG = (d[1] >> 5) & 1; p->KR = (d[1] >> 4) & 1;
    p->ML = d[1] & 15; p->KL = (d[3] >> 6) & 3; p->TL = 0; p->FB = 0;
    p->WS = (d[3] >> 4) & 1; p->AR = (d[5] >> 4) & 15; p->DR = d[5] & 15;
    p->SL = (d[7] >> 4) & 15; p->RR = d[7] & 15;
  }

  void resetSlot(Slot* s, int number) {
    s->type = number & 1; s->wave = fullsin;
    s->pg_phase = 0; s->output[0] = s->output[1] = 0;
    s->eg_state = RELEASE; s->eg_shift = 0; s->rks = 0; s->tll = 0;
    s->key_flag = 0; s->sus_flag = 0; s->blk_fnum = 0; s->blk = 0; s->fnum = 0;
    s->volume = 0; s->pg_out = 0; s->eg_out = EG_MUTE;
    s->eg_rate_h = 0; s->eg_rate_l = 0; s->update_requests = 0;
    s->patch = &patches[0][number & 1];
  }

  void requestUpdate(Slot* s, int flag) { s->update_requests |= flag; }

  int getParameterRate(Slot* s) {
    if ((s->type & 1) == 0 && s->key_flag == 0) return 0;
    switch (s->eg_state) {
      case ATTACK:  return s->patch->AR;
      case DECAY:   return s->patch->DR;
      case SUSTAIN: return s->patch->EG ? 0 : s->patch->RR;
      case RELEASE: return s->sus_flag ? 5 : (s->patch->EG ? s->patch->RR : 7);
      case DAMP:    return DAMPER_RATE;
      default:      return 0;
    }
  }

  uint16_t computeTll(uint16_t blk_fnum, int level, int KL) {
    int idx = blk_fnum >> 5, block = idx >> 4, fnum4 = idx & 15, tl2 = level << 1;
    if (KL == 0) return tl2;
    int tmp = (int)(klTbl[fnum4] - 6.0 * (7 - block));   // dB2(3)=6
    if (tmp <= 0) return tl2;
    return (uint16_t)((uint32_t)((tmp >> (3 - KL)) / 0.375) + tl2);
  }

  void commitSlot(Slot* s) {
    if (s->update_requests & UPDATE_WS) s->wave = waveMap(s->patch->WS);
    if (s->update_requests & UPDATE_TLL) {
      int level = ((s->type & 1) == 0) ? s->patch->TL : s->volume;
      s->tll = computeTll(s->blk_fnum, level, s->patch->KL);
    }
    if (s->update_requests & UPDATE_RKS) {
      // rks_table[blk_fnum>>8][KR]:  blk_fnum>>8 == (block<<1)|fnum8
      int idx = s->blk_fnum >> 8;
      s->rks = s->patch->KR ? idx : (idx >> 2);   // KR=1: idx ; KR=0: block>>1
    }
    if (s->update_requests & (UPDATE_RKS | UPDATE_EG)) {
      int p_rate = getParameterRate(s);
      if (p_rate == 0) { s->eg_shift = 0; s->eg_rate_h = 0; s->eg_rate_l = 0; s->update_requests = 0; return; }
      s->eg_rate_h = min(15, p_rate + (s->rks >> 2));
      s->eg_rate_l = s->rks & 3;
      if (s->eg_state == ATTACK)
        s->eg_shift = (0 < s->eg_rate_h && s->eg_rate_h < 12) ? (13 - s->eg_rate_h) : 0;
      else
        s->eg_shift = (s->eg_rate_h < 13) ? (13 - s->eg_rate_h) : 0;
    }
    s->update_requests = 0;
  }

  void setPatch(int ch, int num) {
    patch_number[ch] = num;
    MOD(ch)->patch = &patches[num][0];
    CAR(ch)->patch = &patches[num][1];
    requestUpdate(MOD(ch), UPDATE_ALL); requestUpdate(CAR(ch), UPDATE_ALL);
  }
  void setSusFlag(int ch, int flag) {
    CAR(ch)->sus_flag = flag; requestUpdate(CAR(ch), UPDATE_EG);
  }
  void setVolume(int ch, int volume) {
    CAR(ch)->volume = volume; requestUpdate(CAR(ch), UPDATE_TLL);
  }
  void setFnumber(int ch, int fnum) {
    Slot *c = CAR(ch), *m = MOD(ch);
    c->fnum = fnum; c->blk_fnum = (c->blk_fnum & 0xe00) | (fnum & 0x1ff);
    m->fnum = fnum; m->blk_fnum = (m->blk_fnum & 0xe00) | (fnum & 0x1ff);
    requestUpdate(c, UPDATE_EG | UPDATE_RKS | UPDATE_TLL);
    requestUpdate(m, UPDATE_EG | UPDATE_RKS | UPDATE_TLL);
  }
  void setBlock(int ch, int blk) {
    Slot *c = CAR(ch), *m = MOD(ch);
    c->blk = blk; c->blk_fnum = ((blk & 7) << 9) | (c->blk_fnum & 0x1ff);
    m->blk = blk; m->blk_fnum = ((blk & 7) << 9) | (m->blk_fnum & 0x1ff);
    requestUpdate(c, UPDATE_EG | UPDATE_RKS | UPDATE_TLL);
    requestUpdate(m, UPDATE_EG | UPDATE_RKS | UPDATE_TLL);
  }

  void slotOn(int i)  { slot[i].key_flag = 1; slot[i].eg_state = DAMP; requestUpdate(&slot[i], UPDATE_EG); }
  void slotOff(int i) { slot[i].key_flag = 0; if (slot[i].type & 1) { slot[i].eg_state = RELEASE; requestUpdate(&slot[i], UPDATE_EG); } }

  void updateKeyStatus() {
    uint32_t now = 0;
    for (int ch = 0; ch < 9; ch++) if (reg[0x20 + ch] & 0x10) now |= 3u << (ch * 2);
    uint32_t changed = slot_key_status ^ now;
    if (changed) for (int i = 0; i < 18; i++) if ((changed >> i) & 1) { if ((now >> i) & 1) slotOn(i); else slotOff(i); }
    slot_key_status = now;
  }

  void startEnvelope(Slot* s) {
    if (min(15, s->patch->AR + (s->rks >> 2)) == 15) { s->eg_state = DECAY; s->eg_out = 0; }
    else s->eg_state = ATTACK;
    requestUpdate(s, UPDATE_EG);
  }

  uint8_t lookupAttackStep(Slot* s, uint32_t c) {
    int index;
    switch (s->eg_rate_h) {
      case 12: index = (c & 0xc) >> 1; return 4 - egStep[s->eg_rate_l][index];
      case 13: index = (c & 0xc) >> 1; return 3 - egStep[s->eg_rate_l][index];
      case 14: index = (c & 0xc) >> 1; return 2 - egStep[s->eg_rate_l][index];
      case 0: case 15: return 0;
      default: index = c >> s->eg_shift; return egStep[s->eg_rate_l][index & 7] ? 4 : 0;
    }
  }
  uint8_t lookupDecayStep(Slot* s, uint32_t c) {
    int index;
    switch (s->eg_rate_h) {
      case 0: return 0;
      case 13: index = ((c & 0xc) >> 1) | (c & 1); return egStep[s->eg_rate_l][index];
      case 14: index = (c & 0xc) >> 1; return egStep[s->eg_rate_l][index] + 1;
      case 15: return 2;
      default: index = c >> s->eg_shift; return egStep[s->eg_rate_l][index & 7];
    }
  }

  void calcEnvelope(Slot* s, Slot* buddy, uint16_t c) {
    uint32_t mask = (1u << s->eg_shift) - 1; uint8_t st;
    if (s->eg_state == ATTACK) {
      if (0 < s->eg_out && 0 < s->eg_rate_h && (c & mask & ~3u) == 0) {
        st = lookupAttackStep(s, c);
        if (0 < st) s->eg_out = max(0, ((int)s->eg_out - (int)(s->eg_out >> st) - 1));
      }
    } else {
      if (s->eg_rate_h > 0 && (c & mask) == 0)
        s->eg_out = min((int)EG_MUTE, (int)(s->eg_out + lookupDecayStep(s, c)));
    }
    switch (s->eg_state) {
      case DAMP:
        if (s->eg_out >= EG_MAX && (c & mask) == 0) {
          startEnvelope(s);
          if (s->type & 1) { s->pg_phase = 0; if (buddy) buddy->pg_phase = 0; }
        }
        break;
      case ATTACK: if (s->eg_out == 0) { s->eg_state = DECAY; requestUpdate(s, UPDATE_EG); } break;
      case DECAY:  if ((s->eg_out >> 3) == s->patch->SL) { s->eg_state = SUSTAIN; requestUpdate(s, UPDATE_EG); } break;
      default: break;
    }
  }

  void calcPhase(Slot* s, uint32_t pm) {
    int8_t pmv = s->patch->PM ? pmTbl[(s->fnum >> 6) & 7][(pm >> 10) & 7] : 0;
    // ×4 (rate a un quarto): stessa frequenza udibile con un quarto degli update.
    s->pg_phase += (((s->fnum & 0x1ff) * 2 + pmv) * mlTbl[s->patch->ML]) << s->blk;
    s->pg_phase &= (DP_WIDTH - 1);
    s->pg_out = s->pg_phase >> DP_BASE_BITS;
  }

  void updateAmPm() {
    pm_phase += 4; am_phase += 4;   // x4: vibrato/tremolo alla stessa frequenza col rate a 1/4
    lfo_am = amTbl[(am_phase >> 6) % 210];
  }

  void updateOutput() {
    updateAmPm();
    eg_counter += 4;   // x4: inviluppi alla stessa velocita' reale col rate a 1/4
    for (int ch = 0; ch < 9; ch++) {
      Slot* mod = MOD(ch); Slot* car = CAR(ch);
      // Un canale e' udibile se la portante e' premuta o non ancora del tutto rilasciata.
      // I canali silenziosi non vengono calcolati (lossless: uscirebbero comunque 0).
      bool active = car->key_flag || car->eg_state != RELEASE || car->eg_out < EG_MUTE;
      if (!active) { ch_out[ch] = 0; continue; }
      if (mod->update_requests) commitSlot(mod);
      calcEnvelope(mod, car, eg_counter);
      calcPhase(mod, pm_phase);
      if (car->update_requests) commitSlot(car);
      calcEnvelope(car, mod, eg_counter);
      calcPhase(car, pm_phase);
      ch_out[ch] = -(calcSlotCar(ch, calcSlotMod(ch))) >> 1;   // _MO
    }
  }

  int16_t lookupExp(uint16_t i) {
    int16_t t = expTbl[(i & 0xff) ^ 0xff] + 1024;
    int16_t res = t >> ((i & 0x7f00) >> 8);
    return ((i & 0x8000) ? ~res : res) << 1;
  }
  int16_t toLinear(uint16_t h, Slot* s, int16_t am) {
    if (s->eg_out > EG_MAX) return 0;
    uint16_t att = min((int)EG_MUTE, (int)(s->eg_out + s->tll + am)) << 4;
    return lookupExp(h + att);
  }
  int16_t calcSlotCar(int ch, int16_t fm) {
    Slot* s = CAR(ch);
    uint8_t am = s->patch->AM ? lfo_am : 0;
    s->output[1] = s->output[0];
    s->output[0] = toLinear(s->wave[(s->pg_out + 2 * (fm >> 1)) & (PG_WIDTH - 1)], s, am);
    return s->output[0];
  }
  int16_t calcSlotMod(int ch) {
    Slot* s = MOD(ch);
    int16_t fm = s->patch->FB > 0 ? (s->output[1] + s->output[0]) >> (9 - s->patch->FB) : 0;
    uint8_t am = s->patch->AM ? lfo_am : 0;
    s->output[1] = s->output[0];
    s->output[0] = toLinear(s->wave[(s->pg_out + fm) & (PG_WIDTH - 1)], s, am);
    return s->output[0];
  }
};

// definizioni tabelle statiche
uint16_t Ym2413::expTbl[256];
uint16_t Ym2413::fullsin[Ym2413::PG_WIDTH];
uint16_t Ym2413::halfsin[Ym2413::PG_WIDTH];
uint8_t  Ym2413::amTbl[210];
int8_t   Ym2413::pmTbl[8][8];
uint8_t  Ym2413::egStep[4][8];
uint32_t Ym2413::mlTbl[16];
double   Ym2413::klTbl[16];
bool     Ym2413::tablesReady = false;
