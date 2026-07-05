#ifndef AUDIO_H
#define AUDIO_H

#include <driver/i2s.h>
#include "../machines/machineBase.h"
#include "../config.h"
#ifdef ENABLE_GALAGA
#include "../machines/galaga/galaga.h"
#endif
#ifdef ENABLE_DKONG
#include "../machines/dkong/dkong.h"
#endif
#ifdef ENABLE_MARIOBROS
#include "../machines/mariobros/mariobros.h"
#endif

// ESP32-P4 with ES8311+NS4150B: I2S digital output, no internal DAC
// The APLL workaround is still needed on ESP32-P4 (no native DAC)
#ifdef SND_I2S_DIGITAL
  #define WORKAROUND_I2S_APLL_PROBLEM
#elif ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 4, 4)
  #define WORKAROUND_I2S_APLL_PROBLEM
#endif

// disctrete notes
#define A5_3      880.00
#define C6_4      1046.50
#define F5_5      739.989   
#define G5_6      783.991
#define E6_7      1318.51
#define B6_8      1975.53 //??
#define XX_B      2100.00 //??
#define D6_E      1174.56
#define B5_F      987.767

class Audio {
public:
  void init();
  void start(machineBase *machineBase);
  void transmit();
  void volumeUpDown(bool up, bool down);
  void setMute(bool m) { muted = m; }   // mute totale (out = 0) per menu boot
  short volumeSetting = 3;

  // Test DAC hardware: genera una sinusoide pura per diagnosi.
  // Se senti un tono pulito -> HW OK, problema nell'emulazione AY.
  // Se senti rumore/distorsione -> HW (alimentazione, SD pin, gain, GND).
  void testSine(int hz, int amplitude);

  // Jingle breve riprodotto allo splash. Suona tramite transmit() (audio_task),
  // quindi unico writer dell'I2S: nessuna race. Si auto-ferma a fine melodia.
  void startSplashMelody();
  void stopMelody() { playMelody = false; }


private:
  void namco_render_buffer(void);
  void ay_render_buffer(void);
  void sn76489_render_buffer(void);
  void spaceinvaders_render_buffer(void);
  void galaxian_render_buffer(void);
  void phoenix_render_buffer(void);
  void sbrkout_render_buffer(void);
  void bombbee_render_buffer(void);
  void goindol_render_buffer(void);   // YM2203 (FM+SSG) via ring buffer (audioPop)
  void pang_render_buffer(void);      // OKI6295 + YM2413 (OPLL) via bridge pang_audio_fill
  void valueToBuffer(int index, short value);
  void discrete_render_buffer(void);
  void generateSinusWave(int32_t amplitude, short* buffer, uint16_t length);
  void melody_render_buffer(void);   // riempie snd_buffer col jingle splash



  machineBase *currentMachine;
  signed char machineType;
  bool muted = false;          // mute totale (output zero) durante menu boot

  // Stato jingle splash (vedi startSplashMelody / melody_render_buffer)
  bool     playMelody = false;
  int      mel_note   = 0;     // indice nota corrente
  int      mel_buf    = 0;     // buffer trascorsi nella nota corrente
  uint32_t mel_phase  = 0;     // accumulatore di fase onda quadra

#ifdef SND_I2S_DIGITAL
  int16_t snd_buffer[128];       // stereo buffer: L+R interleaved for ES8311+NS4150B
#elif defined(SND_DIFF)
  unsigned short snd_buffer[128]; // buffer space for two channels
#else
  unsigned short snd_buffer[64];  // buffer space for a single channel
#endif

  // [5] chip: Gyruss usa 5x AY-3-8910 (gli altri giochi DAVIDE ne usano <=3,
  // indici 0..2 invariati). Con [3] Gyruss andava fuori-bounds = AY corrotti + rumore.
  int ay_period[5][4] = {};
  int ay_volume[5][3] = {};
  int ay_enable[5][3] = {};
  int audio_cnt[5][4] = {};
  int audio_toggle[5][4] = {{1,1,1,1},{1,1,1,1},{1,1,1,1},{1,1,1,1},{1,1,1,1}};
  unsigned long ay_noise_rng[5] = {1, 1, 1, 1, 1};

  unsigned long snd_cnt[3] = {0, 0, 0};
  unsigned long snd_freq[3];
  const signed char *snd_wave[3];
  unsigned char snd_volume[3];

  // Bombjack (envelope) — [5] per coerenza con gli array AY sopra (Gyruss usa 5 chip)
  int ay_envelope_period[5] = {};
  uint8_t ay_envelope_shape[5] = {};
  int ay_envelope_counter[5] = {};
  int ay_envelope_step[5] = {};
  int ay_envelope_holding[5] = {};

  // MrDo / Gigas (SN76489): fino a 4 chip × 4 canali (Gigas ha 4 SN76489A)
  int sn_counter[4][4] = {{0,0,0,0},{0,0,0,0},{0,0,0,0},{0,0,0,0}};
  int sn_toggle[4][4]  = {{1,1,1,1},{1,1,1,1},{1,1,1,1},{1,1,1,1}};


  // Galaxian discrete audio
  unsigned long gal_tone_cnt = 0;     // VCO tone phase accumulator
  int gal_tone_toggle = 1;            // VCO square wave state
  unsigned long gal_fs_cnt[3] = {0,0,0};  // FS1/FS2/FS3 phase accumulators
  int gal_fs_toggle[3] = {1,1,1};     // FS1/FS2/FS3 square wave states
  unsigned long gal_lfo_acc = 0;      // LFO sawtooth phase accumulator
  unsigned long gal_noise_rng = 1;    // noise LFSR (HIT)
  int gal_noise_cnt = 0;              // noise clock counter
  unsigned long gal_fire_rng = 0x1234; // noise LFSR (FIRE)
  int gal_fire_cnt = 0;               // fire noise clock counter
  int gal_last_pitch = 0xFF;          // VCO pitch sweep tracker (per credit sound)
  int gal_pitch_active = 0;           // residual VCO sustain in samples


  // Space Invaders discrete audio (MAME mw8080bw_a.cpp reference)
  // Noise LFSR: 17-bit, taps at bits 4+16, clocked at 7515 Hz
  unsigned long si_noise_rng = 0x1FFFF; // 17-bit LFSR
  int si_noise_clock = 0;              // noise clock accumulator
  int si_noise_out = 0;                // current noise output (bit 12)
  // UFO: SN76477 - SLF triangle ~5.3Hz modulates VCO 1220-3700Hz
  unsigned long si_ufo_sweep = 0;      // SLF position (0..4527)
  unsigned long si_ufo_cnt = 0;        // VCO phase accumulator
  int si_ufo_toggle = 1;              // VCO square wave state
  // Shot: sample playback
  unsigned long si_shot_cnt = 0;       // unused (kept for compat)
  int si_shot_toggle = 1;             // unused (kept for compat)
  int si_shot_freq = 0;               // unused (kept for compat)
  int si_shot_pos = 0;                // current sample position
  int si_shot_playing = 0;            // 1 if playing
  // Explosion: noise burst with slow decay (~2s)
  unsigned long si_explo_cnt = 0;      // decay tick counter
  int si_explo_env = 0;               // envelope level (decays from 120)
  // Invader die: sample playback
  unsigned long si_invdie_cnt = 0;     // unused (kept for compat)
  unsigned long si_invdie_vco = 0;     // unused (kept for compat)
  int si_invdie_toggle = 1;           // unused (kept for compat)
  int si_invdie_freq = 0;             // unused (kept for compat)
  int si_invhit_pos = 0;              // current sample position
  int si_invhit_playing = 0;          // 1 if playing
  // Fleet: 4 low tones (55/40/37/33 Hz, doubled for small speaker)
  unsigned long si_fleet_cnt = 0;      // tone phase accumulator
  int si_fleet_toggle = 1;            // tone square wave state
  // UFO hit: descending warble tone ~2000Hz
  unsigned long si_ufohit_cnt = 0;     // tone phase accumulator
  int si_ufohit_toggle = 1;           // tone square wave state
  int si_ufohit_freq = 0;             // current frequency (descends)
  unsigned long si_ufohit_warble = 0;  // warble phase counter
  // Coin insert: metallic clink sound
  unsigned long si_coin_cnt = 0;       // tone phase accumulator
  unsigned long si_coin_cnt2 = 0;      // overtone phase accumulator
  int si_coin_toggle = 1;             // primary tone square wave
  int si_coin_toggle2 = 1;            // overtone square wave
  int si_coin_timer = 0;              // samples remaining for coin sound
  int si_coin_env = 0;                // envelope level (decays)

  // Mario Bros DAC low-pass filter
  short mario_lpf = 0;

  // Phoenix discrete audio (TMS36xx melody + effect 1 noise + effect 2 tone)
  // Effect 2 (Sound A 0x6000): shoot tone / wing flap — square wave + envelope
  uint16_t ph_e2_phase = 0;
  uint16_t ph_e2_freq  = 0;       // Hz, da bit 4-5 di sound A
  uint16_t ph_e2_env   = 0;       // envelope decay 0..1023
  // Effect 1 (Sound B 0x6800): noise/explosion/shield — LFSR + filtro + envelope
  uint32_t ph_lfsr     = 0x1ACE;
  uint16_t ph_n_period = 64;      // sample fra step LFSR
  uint16_t ph_n_acc    = 0;
  uint16_t ph_n_env    = 0;       // envelope decay 0..1023
  bool     ph_n_filt   = false;   // bit 5 sound B
  int16_t  ph_n_lpf    = 0;       // single-pole LPF state
  // Melody (4 tune triggered da bit 6-7 sound B)
  uint8_t  ph_mel_idx  = 0;       // indice nota corrente
  uint8_t  ph_mel_tune = 0;       // 0..3 tune corrente
  uint16_t ph_mel_phase= 0;
  uint16_t ph_mel_freq = 0;
  uint16_t ph_mel_timer= 0;       // sample fino a prossima nota
  bool     ph_mel_active = false;
  // Boom sintetico esplosione navetta player (Effect 2 sound A) — pattern
  // stretto data==15 + freq_sel==0 con cooldown anti-spurio. Synth: noise
  // LFSR + DOPPIO LPF cascata + envelope decay quadratico ~500 ms.
  int16_t  ph_expl_env      = 0;
  int32_t  ph_expl_lpf1     = 0;
  int32_t  ph_expl_lpf2     = 0;
  int16_t  ph_expl_cooldown = 0;

  // Bagman
  unsigned short positionLast;
  short sinusWaveBuffer[256];
};

#endif