// ============================================================================
// machines/goindol/goindol.cpp — Goindol (SunA Electronics 1987, set goindol)
//
// Z80 main + (FASE 3) Z80 sound + YM2203. Doppia tilemap (BG opaco + FG scroll),
// sprite 8x16 che riusano i tile char. Port fedele da MAME suna/goindol.cpp.
// Template strutturale: machines/gigas (stesso produttore SunA).
// ============================================================================
#include "goindol.h"

#ifdef ENABLE_GOINDOL
#include "goindol_logo.h"
#include "goindol_rom_fixed.h"
#include "goindol_rom_bank.h"
#include "goindol_palette.h"
#include "goindol_gfx_fg_p0.h"
#include "goindol_gfx_fg_p1.h"
#include "goindol_gfx_fg_p2.h"
#include "goindol_gfx_bg_p0.h"
#include "goindol_gfx_bg_p1.h"
#include "goindol_gfx_bg_p2.h"

// Dial free-running counter (wraps mod 256) — letto come spinner relativo.
extern volatile uint8_t ec11_dial_counter;

#include "goindol_rom_audio.h"   // r10, sound Z80 (32KB)
#include "machines/boblbobl/snd/emu2149.h"   // PSG (SSG dello YM2203). emu2149.c
                                             // compilato in TU separata goindol_snd.cpp.

// ============================================================================
// AUDIO (Fase 3) — YM2203 reale via fm.c. fm.h e' gia' incluso dal .ino PRIMA di
// questo file (guardia ENABLE_GOINDOL), quindi NON va re-incluso qui (eviterebbe
// la ridefinizione di ssg_callbacks). I tipi/funzioni ym2203_* sono visibili.
// ============================================================================
static Goindol *g_goindol = nullptr;

// Timer YM2203: handler NULL -> fm.c auto-temporizza i timer interni (usati solo
// per lo status, che goindol non legge mai). IRQ del YM2203 NON e' collegato alla
// CPU su goindol (l'IRQ del sound CPU e' un periodico fisso 240Hz), quindi noop.
static void goindol_fm_irq(void *p, int irq) { (void)p; (void)irq; }

// SSG (parte PSG dello YM2203) -> emu2149 (Fase 3b). fm.c invoca questi callback.
static void goindol_ssg_setclock(void *p, int clock) {
  (void)p;
  if (g_goindol && g_goindol->psg_chip) PSG_setClock((PSG*)g_goindol->psg_chip, (uint32_t)clock);
}
static void goindol_ssg_write(void *p, int addr, int data) {
  (void)p;
  if (!g_goindol || !g_goindol->psg_chip) return;
  if (addr == 0) g_goindol->ssg_latch = (uint8_t)data;                       // register select
  else PSG_writeReg((PSG*)g_goindol->psg_chip, g_goindol->ssg_latch, (uint32_t)data);
}
static int  goindol_ssg_read(void *p) {
  (void)p;
  return (g_goindol && g_goindol->psg_chip) ? PSG_readReg((PSG*)g_goindol->psg_chip, g_goindol->ssg_latch) : 0;
}
static void goindol_ssg_reset(void *p) {
  (void)p;
  if (g_goindol && g_goindol->psg_chip) PSG_reset((PSG*)g_goindol->psg_chip);
}
static const ssg_callbacks goindol_ssg = {
  goindol_ssg_setclock, goindol_ssg_write, goindol_ssg_read, goindol_ssg_reset
};

void Goindol::sound_init_chips() {
  g_goindol = this;
  // SSG: clock YM2203/2 = 750kHz (il callback setclock lo riallinea al valore
  // esatto passato da fm.c). rate 32kHz.
  if (!psg_chip) psg_chip = PSG_new(750000, 32000);
  if (psg_chip)  PSG_reset((PSG*)psg_chip);
  // YM2203 clock = 12MHz/8 = 1.5MHz; rate 32kHz (= I2S). TimerHandler=NULL.
  if (!ym2203_chip) ym2203_chip = ym2203_init(this, 1500000, 32000,
                                              /*TimerHandler=*/NULL, goindol_fm_irq, &goindol_ssg);
  if (ym2203_chip) ym2203_reset_chip(ym2203_chip);
}

// ── Layout memoria interno (RAMSIZE 16384, gia' 16384 perche' ARKANOID attivo) ──
//  memory[0x0000-0x07FF] = work RAM      (0xC000-0xC7FF)  <- patchata da prot
//  memory[0x0800-0x0FFF] = D000 area     (0xD000-0xD7FF)  [0x800..0x83F=spr0]
//  memory[0x1000-0x17FF] = BG video RAM  (0xD800-0xDFFF)
//  memory[0x1800-0x1FFF] = E000 area     (0xE000-0xE7FF)  [0x1800..0x183F=spr1]
//  memory[0x2000-0x27FF] = FG video RAM  (0xE800-0xEFFF)
#define MEM_WORK    0x0000
#define MEM_D000    0x0800
#define MEM_BGVRAM  0x1000
#define MEM_E000    0x1800
#define MEM_FGVRAM  0x2000

// DSW factory default (vedi INPUT_PORTS goindol)
#define GOINDOL_DSW1  0xCE   // 3 vite, Normal, demo On, invuln Off, service Off
#define GOINDOL_DSW2  0xC7   // bonus 100k/200k, 1C1C, Upright, flip Off

Goindol::Goindol() {
  bank = char_bank = flip_screen = 0;
  fg_scrollx = fg_scrolly = 0;
  soundlatch = 0;
  prot_toggle = 0;
  coin_prev = coin_impulse = 0;
}

const unsigned short *Goindol::logo(void) { return goindol_logo; }

void Goindol::init(Input *in, unsigned short *fb, sprite_S *sb, unsigned char *mem) {
  machineBase::init(in, fb, sb, mem);
}

void Goindol::reset() {
  machineBase::reset();
  bank = char_bank = flip_screen = 0;
  fg_scrollx = fg_scrolly = 0;
  soundlatch = 0;
  prot_toggle = 0;
  coin_prev = coin_impulse = 0;
  // ResetZ80 setta SP=0xF000 (= DSW1, read-only). Forzo SP in cima alla work RAM
  // per sicurezza prima che il boot imposti il proprio SP.
  cpu[0].SP.W = 0xC7FF;

  // ── Audio (Fase 3): alloc lazy + init chip + reset sound CPU ──
  if (!sndram) sndram = (unsigned char*)malloc(0x800);   // 0xC000-0xC7FF (2KB)
  if (!ring)   { ring  = (int16_t*)heap_caps_malloc(GOINDOL_RING*sizeof(int16_t), MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT); if (!ring) ring = (int16_t*)malloc(GOINDOL_RING*sizeof(int16_t)); }
  if (!fmbuf)  { fmbuf = (int32_t*)heap_caps_malloc(1024*sizeof(int32_t), MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT); if (!fmbuf) fmbuf = (int32_t*)malloc(1024*sizeof(int32_t)); }
  if (sndram) memset(sndram, 0, 0x800);
  if (ring)   memset(ring, 0, GOINDOL_RING*sizeof(int16_t));
  ring_head = ring_tail = 0;
  snd_accum = 0; last_audio_us = micros();
  ssg_latch = 0; sound_irq_pending = 0;
  cpu[0].TrapBadOps = 0;
  cpu[1].TrapBadOps = 0;
  cpu[1].SP.W = 0xC800;     // top sound RAM (0xC000-0xC7FF) per sicurezza
  sound_init_chips();
  current_cpu = 0;
}

// ============================================================================
// Z80 memory map (main_map MAME)
// ============================================================================
unsigned char Goindol::rdZ80(unsigned short Addr) {
  // ── SOUND CPU (cpu[1]) — sound_map MAME ──
  if (current_cpu == 1) {
    if (Addr < 0x8000) return pgm_read_byte(&goindol_rom_audio[Addr]);   // r10
    if (Addr >= 0xC000 && Addr < 0xC800) return sndram[Addr - 0xC000];   // sound RAM
    if (Addr == 0xD800) return soundlatch;                               // soundlatch read
    return 0xFF;
  }
  if (Addr < 0x8000) return pgm_read_byte(&goindol_rom_fixed[Addr]);
  if (Addr < 0xC000) return pgm_read_byte(&goindol_rom_bank[(bank & 3) * 0x4000 + (Addr - 0x8000)]);
  if (Addr < 0xC800) return memory[MEM_WORK + (Addr - 0xC000)];
  if (Addr < 0xD000) {
    // IO 0xC800-0xCFFF
    if (Addr == 0xC820) return ec11_dial_counter;          // DIAL (spinner)
    if (Addr == 0xC830) {                                  // P1 (active low)
      unsigned char b = input ? input->buttons_get() : 0;
      unsigned char v = 0xFF;
      if (b & BUTTON_UP)    v &= ~0x01;
      if (b & BUTTON_DOWN)  v &= ~0x02;
      if (b & BUTTON_LEFT)  v &= ~0x04;
      if (b & BUTTON_RIGHT) v &= ~0x08;
      if (b & BUTTON_FIRE)  v &= ~0x10;   // BUTTON1
      if (b & BUTTON_EXTRA) v &= ~0x20;   // BUTTON2
      if (b & BUTTON_START) v &= ~0x40;   // START1
      if (coin_impulse)     v &= ~0x80;   // COIN1 (impulso 1 frame, MAME PORT_IMPULSE(1))
      return v;
    }
    if (Addr == 0xC834) return 0xFF;                       // P2 (cocktail, non usato)
    return 0xFF;
  }
  if (Addr < 0xD800) return memory[MEM_D000   + (Addr - 0xD000)];
  if (Addr < 0xE000) return memory[MEM_BGVRAM + (Addr - 0xD800)];
  if (Addr < 0xE800) return memory[MEM_E000   + (Addr - 0xE000)];
  if (Addr < 0xF000) return memory[MEM_FGVRAM + (Addr - 0xE800)];
  if (Addr == 0xF000) return GOINDOL_DSW1;
  if (Addr == 0xF422) { prot_toggle ^= 0x80; return prot_toggle; }  // prot_f422_r
  if (Addr == 0xF800) return GOINDOL_DSW2;
  return 0xFF;
}

unsigned char Goindol::opZ80(unsigned short Addr) {
  // Nessuna cifratura: opcode = rom (per dati IO usa rdZ80, ma l'M1 non vi accede)
  if (current_cpu == 1) {
    if (Addr < 0x8000) return pgm_read_byte(&goindol_rom_audio[Addr]);
    return rdZ80(Addr);
  }
  if (Addr < 0x8000) return pgm_read_byte(&goindol_rom_fixed[Addr]);
  if (Addr < 0xC000) return pgm_read_byte(&goindol_rom_bank[(bank & 3) * 0x4000 + (Addr - 0x8000)]);
  return rdZ80(Addr);
}

void Goindol::wrZ80(unsigned short Addr, unsigned char Value) {
  // ── SOUND CPU (cpu[1]) — sound_map MAME ──
  if (current_cpu == 1) {
    if (Addr == 0xA000 || Addr == 0xA001) {                        // YM2203 reg/data
      if (ym2203_chip) ym2203_write(ym2203_chip, Addr & 1, Value);
      return;
    }
    if (Addr >= 0xC000 && Addr < 0xC800) { sndram[Addr - 0xC000] = Value; return; }
    return;                                                        // resto: nop
  }
  if (Addr < 0xC000) return;                                       // ROM
  if (Addr < 0xC800) { memory[MEM_WORK + (Addr - 0xC000)] = Value; return; }
  if (Addr < 0xD000) {
    // IO write 0xC800-0xCFFF
    if (Addr == 0xC800) { soundlatch = Value; return; }            // soundlatch
    if (Addr == 0xC810) {                                          // bankswitch_w
      bank       = Value & 0x03;
      char_bank  = (Value >> 4) & 1;
      flip_screen= (Value >> 5) & 1;
      return;
    }
    if (Addr == 0xC820) { fg_scrolly = Value; return; }            // fg scroll Y
    if (Addr == 0xC830) { fg_scrollx = Value; return; }            // fg scroll X
    return;
  }
  if (Addr < 0xD800) { memory[MEM_D000   + (Addr - 0xD000)] = Value; return; }
  if (Addr < 0xE000) { memory[MEM_BGVRAM + (Addr - 0xD800)] = Value; return; }
  if (Addr < 0xE800) { memory[MEM_E000   + (Addr - 0xE000)] = Value; return; }
  if (Addr < 0xF000) { memory[MEM_FGVRAM + (Addr - 0xE800)] = Value; return; }
  // ── protezione (patch work RAM, da MAME) ──
  if (Addr == 0xFC44) { memory[MEM_WORK+0x419]=0x5b; memory[MEM_WORK+0x41a]=0x3f; memory[MEM_WORK+0x41b]=0x6d; return; }
  if (Addr == 0xFD99) { memory[MEM_WORK+0x421]=0x3f; return; }
  if (Addr == 0xFC66) { memory[MEM_WORK+0x423]=0x06; return; }
  if (Addr == 0xFCB0) { memory[MEM_WORK+0x425]=0x06; return; }
}

// ============================================================================
// Frame loop — main Z80 (cpu[0]) + sound Z80 (cpu[1]) + YM2203, interleavati.
// Main IRQ vblank (RST38, 1x/frame). Sound IRQ periodico 240Hz (4x/frame).
// ============================================================================
void Goindol::run_frame() {
  g_goindol = this;   // i callback C dei chip usano questo puntatore

  // Coin impulse (MAME PORT_IMPULSE(1)): genera UN solo frame attivo sul fronte
  // di salita del COIN fisico. Senza questo il gioco vede COIN premuto per molti
  // frame e accredita un gettone per frame -> "va subito a 9".
  {
    unsigned char btn = input ? input->buttons_get() : 0;
    unsigned char coin_now = (btn & BUTTON_COIN) ? 1 : 0;
    if (coin_now && !coin_prev) coin_impulse = 1;   // fronte di salita -> 1 frame attivo
    coin_prev = coin_now;
  }

  // Campioni audio per il TEMPO REALE trascorso (32000/s): tempo musicale
  // corretto e ring bilanciato a prescindere dal rate effettivo di run_frame.
  uint32_t now = micros();
  uint32_t elapsed = now - last_audio_us;
  last_audio_us = now;
  if (elapsed > 32000) elapsed = 32000;        // clamp (primo frame / hiccup)
  snd_accum += elapsed * 32;                    // 32 campioni per ms
  int frame_samples = (int)(snd_accum / 1000);
  snd_accum %= 1000;

  // Main Z80 @ 6MHz ~= 20000 StepZ80/frame (=5000 iter x4, come prima).
  // Sound Z80 @ 6MHz: budget ridotto (~12500 StepZ80/frame). IRQ 240Hz fisso
  // (4/frame) = irq0_line_hold, consegnato quando il sound CPU abilita gli IRQ.
  const int INTERLEAVE = 16;
  const int main_per = 5000 / INTERLEAVE;   // ~312 iter/slice
  const int snd_per  = 3125 / INTERLEAVE;   // ~195 iter/slice

  for (int slice = 0; slice < INTERLEAVE; slice++) {
    current_cpu = 0;
    for (int i = 0; i < main_per; i++) { StepZ80(&cpu[0]); StepZ80(&cpu[0]); StepZ80(&cpu[0]); StepZ80(&cpu[0]); }

    // Avanza il YM2203 per la quota di campioni di questa slice (PRIMA del sound CPU).
    int s0 = (int)((long)frame_samples * slice       / INTERLEAVE);
    int s1 = (int)((long)frame_samples * (slice + 1)  / INTERLEAVE);
    if (s1 > s0) gen_audio(s1 - s0);

    // Sound CPU + IRQ 240Hz (4 slot/frame: slice 3,7,11,15).
    current_cpu = 1;
    if ((slice & 3) == 3) sound_irq_pending = 1;
    for (int i = 0; i < snd_per; i++) {
      StepZ80(&cpu[1]); StepZ80(&cpu[1]); StepZ80(&cpu[1]); StepZ80(&cpu[1]);
      // StepZ80 non onora IRequest latchato: ricontrollo la linea dopo ogni
      // gruppo cosi' l'IRQ entra appena il sound CPU riabilita gli interrupt (EI).
      if (sound_irq_pending && (cpu[1].IFF & IFF_1)) {
        IntZ80(&cpu[1], INT_RST38);
        sound_irq_pending = 0;
      }
    }
  }

  current_cpu = 0;
  IntZ80(&cpu[0], INT_RST38);   // vblank IRQ main (1x/frame, come prima)

  if (coin_impulse) coin_impulse--;   // consuma l'impulso a fine frame
  if (!game_started) game_started = 1;
}

// ============================================================================
// Audio: generazione (core emulazione) -> ring buffer. YM2203 = FM (fm.c) + SSG
// (emu2149, generato campione per campione con PSG_calc). Mix/scaling: il peso
// della SSG (<<1) e lo shift in goindol_render_buffer sono punti di calibrazione.
// ============================================================================
void Goindol::gen_audio(int n) {
  if (n <= 0 || !ring || !fmbuf) return;
  if (n > 1024) n = 1024;
  FMSAMPLE *fmb[2] = { (FMSAMPLE*)fmbuf, (FMSAMPLE*)fmbuf };   // YM2203 mono
  if (ym2203_chip) ym2203_update_one(ym2203_chip, fmb, n);
  else             memset(fmbuf, 0, n * sizeof(int32_t));

  PSG *psg = (PSG*)psg_chip;
  uint32_t tail = ring_tail, head = ring_head;
  for (int i = 0; i < n; i++) {
    int ssg = psg ? PSG_calc(psg) : 0;
    int32_t s = fmbuf[i] + (ssg << 1);          // FM (full) + SSG (peso da tarare)
    if (s >  32767) s =  32767;
    if (s < -32768) s = -32768;
    uint32_t nt = (tail + 1) & (GOINDOL_RING - 1);
    if (nt == head) break;                       // ring pieno
    ring[tail] = (int16_t)s;
    tail = nt;
  }
  ring_tail = tail;
}

// Estrazione dal ring (task audio, core 0). SPSC lock-free.
int Goindol::audioPop(int16_t *out, int n) {
  uint32_t head = ring_head, tail = ring_tail;
  int got = 0;
  while (got < n && head != tail) {
    out[got++] = ring[head];
    head = (head + 1) & (GOINDOL_RING - 1);
  }
  ring_head = head;
  return got;
}

void Goindol::prepare_frame() {
  // Sprite disegnati direttamente in render_row dalla sprite RAM live: niente snapshot.
}

// ── stub (rendering fatto in render_layers/draw_sprite_bank) ──
void Goindol::blit_tile(short, char) {}
void Goindol::blit_sprite(short, unsigned char) {}

// ============================================================================
// Render di una banda 8px: BG opaco -> FG (scroll, trasparente pen0) -> sprite.
// frame_buffer = strip 8 righe x 256 colonne. Nessun flip (ROT90 = nativo).
// ============================================================================
void Goindol::render_row(short row) {
  render_layers(row);
  // MAME: draw_sprites(gfx1=bg_chars, spr0@D000) poi (gfx0=fg_chars, spr1@E000)
  draw_sprite_bank(row, memory + MEM_D000, /*use_fg_gfx=*/0);
  draw_sprite_bank(row, memory + MEM_E000, /*use_fg_gfx=*/1);
}

void Goindol::render_layers(short row) {
  const unsigned char *bgv = memory + MEM_BGVRAM;
  const unsigned char *fgv = memory + MEM_FGVRAM;
  const unsigned int   cb  = (unsigned)char_bank << 11;

  for (int r = 0; r < 8; r++) {
    int screen_y = GOINDOL_VRAM_OFFSET_ROWS * 8 + row * 8 + r;   // 16 + row*8 + r
    unsigned short *out = frame_buffer + r * GOINDOL_SCREEN_W;

    // ---- BG (opaco, no scroll) ----
    int by = screen_y & 0xFF;
    int brow = by >> 3;
    int bpy  = by & 7;
    for (int tcol = 0; tcol < GOINDOL_COLS; tcol++) {
      int tidx = brow * 32 + tcol;
      unsigned char attr = bgv[2 * tidx];
      unsigned int  code = (unsigned)bgv[2 * tidx + 1] | (((unsigned)(attr & 7)) << 8) | cb;
      code &= (GOINDOL_NUM_TILES - 1);
      const unsigned short *pal = &goindol_palette[((attr & 0xF8) >> 3) * 8];
      unsigned int bo = code * 8 + bpy;
      unsigned char p0 = pgm_read_byte(&goindol_gfx_bg_p0[bo]);
      unsigned char p1 = pgm_read_byte(&goindol_gfx_bg_p1[bo]);
      unsigned char p2 = pgm_read_byte(&goindol_gfx_bg_p2[bo]);
      unsigned short *o = out + tcol * 8;
      for (int x = 0; x < 8; x++) {
        int bit = 7 - x;
        // Plane order MAME charlayout goindol: planeoffset[0]=RGN_FRAC(0,3)=p0=MSB,
        // planeoffset[2]=RGN_FRAC(2,3)=p2=LSB. pix = (p0<<2)|(p1<<1)|p2.
        unsigned char pix = (((p0 >> bit) & 1) << 2) | (((p1 >> bit) & 1) << 1) | ((p2 >> bit) & 1);
        o[x] = pal[pix];
      }
    }

    // ---- FG (scroll, trasparente pen 0) ----
    int fy = (screen_y + fg_scrolly) & 0xFF;
    int frow = fy >> 3;
    int fpy  = fy & 7;
    // bytes del tile fg correntemente caricato
    int cur_tcol = -1;
    unsigned char fp0 = 0, fp1 = 0, fp2 = 0;
    const unsigned short *fpal = goindol_palette;
    for (int gx = 0; gx < GOINDOL_SCREEN_W; gx++) {
      int fx = (gx + fg_scrollx) & 0xFF;
      int tcol = fx >> 3;
      if (tcol != cur_tcol) {
        cur_tcol = tcol;
        int tidx = frow * 32 + tcol;
        unsigned char attr = fgv[2 * tidx];
        unsigned int  code = (unsigned)fgv[2 * tidx + 1] | (((unsigned)(attr & 7)) << 8) | cb;
        code &= (GOINDOL_NUM_TILES - 1);
        fpal = &goindol_palette[((attr & 0xF8) >> 3) * 8];
        unsigned int fo = code * 8 + fpy;
        fp0 = pgm_read_byte(&goindol_gfx_fg_p0[fo]);
        fp1 = pgm_read_byte(&goindol_gfx_fg_p1[fo]);
        fp2 = pgm_read_byte(&goindol_gfx_fg_p2[fo]);
      }
      int bit = 7 - (fx & 7);
      // Plane order p0=MSB, p2=LSB (vedi nota in render_layers BG).
      unsigned char pix = (((fp0 >> bit) & 1) << 2) | (((fp1 >> bit) & 1) << 1) | ((fp2 >> bit) & 1);
      if (pix) out[gx] = fpal[pix];
    }
  }
}

// ============================================================================
// Sprite bank: 16 sprite x 4 byte. Ogni sprite = 2 tile 8x8 verticali (8x16).
// gfx = bg_chars (use_fg_gfx=0) o fg_chars (use_fg_gfx=1).
// ============================================================================
void Goindol::draw_sprite_bank(short row, const unsigned char *spr, char use_fg_gfx) {
  const unsigned char *gp0 = use_fg_gfx ? goindol_gfx_fg_p0 : goindol_gfx_bg_p0;
  const unsigned char *gp1 = use_fg_gfx ? goindol_gfx_fg_p1 : goindol_gfx_bg_p1;
  const unsigned char *gp2 = use_fg_gfx ? goindol_gfx_fg_p2 : goindol_gfx_bg_p2;

  int strip_y0 = GOINDOL_VRAM_OFFSET_ROWS * 8 + row * 8;   // screen y top della banda

  for (int offs = 0; offs < 0x40; offs += 4) {
    unsigned char b0 = spr[offs], b1 = spr[offs + 1], b2 = spr[offs + 2], b3 = spr[offs + 3];
    int sx = b0;
    int sy = 240 - b1;
    if (!((b1 >> 3) && sx < 248)) continue;
    unsigned int tile = ((unsigned)b3 | (((unsigned)(b2 & 7)) << 8)) * 2;
    const unsigned short *pal = &goindol_palette[(b2 >> 3) * 8];

    for (int part = 0; part < 2; part++) {
      unsigned int t = (tile + part) & (GOINDOL_NUM_TILES - 1);
      int ty = sy + part * 8;
      for (int dy = 0; dy < 8; dy++) {
        int screen_y = ty + dy;
        if (screen_y < strip_y0 || screen_y > strip_y0 + 7) continue;
        int ry = screen_y - strip_y0;
        unsigned int bo = t * 8 + dy;
        unsigned char p0 = pgm_read_byte(&gp0[bo]);
        unsigned char p1 = pgm_read_byte(&gp1[bo]);
        unsigned char p2 = pgm_read_byte(&gp2[bo]);
        unsigned short *o = frame_buffer + ry * GOINDOL_SCREEN_W;
        for (int dx = 0; dx < 8; dx++) {
          int screen_x = sx + dx;
          if (screen_x < 0 || screen_x >= GOINDOL_SCREEN_W) continue;
          int bit = 7 - dx;
          // Plane order p0=MSB, p2=LSB (sprite riusa la stessa charlayout dei tile).
          unsigned char pix = (((p0 >> bit) & 1) << 2) | (((p1 >> bit) & 1) << 1) | ((p2 >> bit) & 1);
          if (pix == 0) continue;
          o[screen_x] = pal[pix];
        }
      }
    }
  }
}

#endif // ENABLE_GOINDOL
