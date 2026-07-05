#include "emulation/audio.h"
#ifdef ENABLE_SPACE
  #include "machines/spaceinvaders/spaceinvaders_samples.h"
#endif
#ifdef ENABLE_MOONPATROL
  #include "machines/moonpatrol/moonpatrol.h"
  extern moonpatrol *g_moonpatrol_instance;
#endif
#ifdef ENABLE_ARKANOID2
  #include "machines/arkanoid2/arkanoid2.h"
  #include "machines/arkanoid2/ym2203/fm.h"
#endif
#ifdef ENABLE_GYRUSS
  #include "machines/gyruss/gyruss.h"   // per i8039_sample()/step_i8039() (drums)
#endif
#ifdef ENABLE_ROADFIGHTER
  #include "machines/roadfighter/roadfighter.h"   // per roadf_dac_sample() (DAC engine/crash)
#endif
#ifdef ENABLE_GOINDOL
  #include "machines/goindol/goindol.h"   // audio FEDELE YM2203: ring buffer (audioPop)
#endif

// SPINNERINO P4: ES8311 codec + NS4150B amp via I2C + I2S nuova API IDF v5
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"

// Jingle splash: PCM 16-bit mono @ 32 kHz (generato da convert_splash_audio.py)
#include "splash_audio.h"

// ES8311 register set (porting da driver GALAGONE 4.3 P4)
#define ES8311_REG_RESET        0x00
#define ES8311_REG_CLK_MANAGER1 0x01
#define ES8311_REG_CLK_MANAGER6 0x06
#define ES8311_REG_CLK_MANAGER8 0x08
#define ES8311_REG_SDP_IN       0x09
#define ES8311_REG_SDP_OUT      0x0A
#define ES8311_REG_SYSTEM_0B    0x0B
#define ES8311_REG_SYSTEM_0C    0x0C
#define ES8311_REG_SYSTEM       0x0D
#define ES8311_REG_SYSTEM_0E    0x0E
#define ES8311_REG_CHIP_PWR     0x10
#define ES8311_REG_ANALOG_PWR   0x11
#define ES8311_REG_SYSTEM_12    0x12
#define ES8311_REG_SYSTEM_13    0x13
#define ES8311_REG_ADC_CTRL     0x14
#define ES8311_REG_DAC_VOL      0x32

static i2c_master_dev_handle_t es8311_dev = NULL;
static i2s_chan_handle_t       i2s_tx_handle = NULL;

static esp_err_t es8311_write_reg(uint8_t reg, uint8_t val) {
  uint8_t buf[2] = {reg, val};
  return i2c_master_transmit(es8311_dev, buf, 2, 100);
}

static void es8311_codec_init() {
  // 1. Reset
  es8311_write_reg(ES8311_REG_RESET, 0x1F); delay(20);
  es8311_write_reg(ES8311_REG_RESET, 0x00); delay(20);
  // 2. Clock manager (slave, MCLK auto-detect 256xfs)
  es8311_write_reg(ES8311_REG_CLK_MANAGER1, 0x30);
  es8311_write_reg(0x02, 0x00); es8311_write_reg(0x03, 0x10);
  es8311_write_reg(0x04, 0x10); es8311_write_reg(0x05, 0x00);
  es8311_write_reg(ES8311_REG_CLK_MANAGER6, 0x03);
  es8311_write_reg(0x07, 0x00); es8311_write_reg(ES8311_REG_CLK_MANAGER8, 0xFF);
  // 3. SDP I2S 16-bit
  es8311_write_reg(ES8311_REG_SDP_IN,  0x00);
  es8311_write_reg(ES8311_REG_SDP_OUT, 0x00);
  // 4. Power management reset
  es8311_write_reg(ES8311_REG_SYSTEM_0B, 0x00);
  es8311_write_reg(ES8311_REG_SYSTEM_0C, 0x00);
  // 5. Power up chip + analog
  es8311_write_reg(ES8311_REG_CHIP_PWR,   0x1F);
  es8311_write_reg(ES8311_REG_ANALOG_PWR, 0x7F); delay(10);
  // 6. Start CSM
  es8311_write_reg(ES8311_REG_RESET, 0x80); delay(10);
  // 7. Enable DAC output
  es8311_write_reg(ES8311_REG_SYSTEM,    0x01);
  es8311_write_reg(ES8311_REG_SYSTEM_0E, 0x02);
  es8311_write_reg(ES8311_REG_SYSTEM_12, 0x00);
  es8311_write_reg(ES8311_REG_SYSTEM_13, 0x10);
  es8311_write_reg(ES8311_REG_ADC_CTRL,  0x1A);
  // 8. Start clocking
  es8311_write_reg(ES8311_REG_CLK_MANAGER1, 0x3F);
  // 9. DAC volume 0dB
  es8311_write_reg(ES8311_REG_DAC_VOL, 0xBF);
  Serial.println("[AUDIO] ES8311 codec initialized");
}

void Audio::init() {
  // SPINNERINO P4: ES8311 + NS4150B (I2S nuova API obbligatoria su P4)
  // 32 kHz 16-bit stereo come MAX98357A del sorgente S3 originale (compatibile rendering AY)

  // PA amplifier ON
  pinMode(ES8311_PA_PIN, OUTPUT);
  digitalWrite(ES8311_PA_PIN, HIGH);
  Serial.printf("[AUDIO] PA amp ON (GPIO %d)\n", ES8311_PA_PIN);

  // I2C master bus
  i2c_master_bus_config_t bus_cfg = {};
  bus_cfg.i2c_port = I2C_NUM_0;
  bus_cfg.sda_io_num = (gpio_num_t)I2C_SDA;
  bus_cfg.scl_io_num = (gpio_num_t)I2C_SCL;
  bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
  bus_cfg.glitch_ignore_cnt = 7;
  i2c_master_bus_handle_t i2c_bus;
  i2c_new_master_bus(&bus_cfg, &i2c_bus);

  i2c_device_config_t dev_cfg = {};
  dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  dev_cfg.device_address = ES8311_I2C_ADDR;
  dev_cfg.scl_speed_hz = 100000;
  i2c_master_bus_add_device(i2c_bus, &dev_cfg, &es8311_dev);

  // I2S TX nuova API (legacy non funziona su P4)
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan_cfg.dma_desc_num = 6;
  chan_cfg.dma_frame_num = 256;
  i2s_new_channel(&chan_cfg, &i2s_tx_handle, NULL);

  i2s_std_config_t std_cfg = {
    .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(32000),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
    .gpio_cfg = {
      .mclk = (gpio_num_t)I2S_MCLK,
      .bclk = (gpio_num_t)I2S_BCLK,
      .ws   = (gpio_num_t)I2S_LRC,
      .dout = (gpio_num_t)I2S_DOUT,
      .din  = I2S_GPIO_UNUSED,
      .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
    },
  };
  std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  i2s_channel_init_std_mode(i2s_tx_handle, &std_cfg);
  i2s_channel_enable(i2s_tx_handle);
  delay(10);

  // Init codec DOPO che I2S sta generando MCLK
  es8311_codec_init();
  Serial.println("[AUDIO] init complete (32kHz 16bit stereo)");

  generateSinusWave(256, sinusWaveBuffer, sizeof(sinusWaveBuffer)  / 2 );
}

void Audio::start(machineBase *machineBase) {
  currentMachine = machineBase;
  machineType = currentMachine->machineType();
  mario_lpf = 0;

  // Reset stato audio per-machine residuale (Galaxian VCO sweep, ecc.)
  // cosi' quando si torna al menu boot non resta hum/click.
  gal_last_pitch    = 0xFF;
  gal_pitch_active  = 0;
  gal_tone_cnt      = 0;
  gal_tone_toggle   = 1;
  gal_lfo_acc       = 0;
  for (int i = 0; i < 3; i++) {
    gal_fs_cnt[i]    = 0;
    gal_fs_toggle[i] = 1;
  }
    
#if defined(SND_I2S_DIGITAL)
  // MAX98357A: sample rate changes not needed (always 24kHz)
#elif !defined(WORKAROUND_I2S_APLL_PROBLEM)
  signed char machineType = currentMachine->machineType();
  i2s_set_sample_rates(I2S_NUM_0, (machineType == MCH_DKONG || machineType == MCH_MARIOBROS) ? 11765 : 24000);
#endif
}

void Audio::volumeUpDown(bool up, bool down) {
  // Edge detection è già gestita in input.cpp, qui basta applicare
  if (up && volumeSetting > 1)
    volumeSetting--;
  if (down && volumeSetting < 30)
    volumeSetting++;
}

void Audio::transmit() {
#if !defined(SND_I2S_DIGITAL) && !defined(SND_DIFF) && !defined(SND_LEFT_CHANNEL)
  // No audio hardware - skip entirely
  return;
#else
  size_t bytesOut = 0;
  do {
    i2s_channel_write(i2s_tx_handle, snd_buffer, sizeof(snd_buffer), &bytesOut, 0);

    // render the next audio chunk if data has actually been sent
    if(bytesOut) {
      // Jingle splash: ha priorita' su tutto (anche sul mute), unico writer.
      if (playMelody) {
        melody_render_buffer();
        continue;
      }
      if (muted) {
        // Mute totale: riempi il buffer con zero senza chiamare i render
        // della machine (evita residual hum/click quando torni al menu boot).
        memset(snd_buffer, 0, sizeof(snd_buffer));
        continue;
      }
      if (currentMachine->hasNamcoAudio())
        namco_render_buffer();
      else if(
#ifdef ENABLE_MRDO
        machineType == MCH_MRDO ||
#endif
#ifdef ENABLE_LADYBUG
        machineType == MCH_LADYBUG ||
#endif
#ifdef ENABLE_GREENBERET
        machineType == MCH_GREENBERET ||
#endif
#ifdef ENABLE_GIGAS
        machineType == MCH_GIGAS ||
#endif
#ifdef ENABLE_GIGAS2
        machineType == MCH_GIGAS2 ||
#endif
#ifdef ENABLE_ROADFIGHTER
        machineType == MCH_ROADFIGHTER ||
#endif
        false)
        sn76489_render_buffer();
#ifdef ENABLE_BAGMAN
      else if(machineType == MCH_BAGMAN)
        discrete_render_buffer();
#endif
#ifdef ENABLE_GALAXIAN
      else if(machineType == MCH_GALAXIAN)
        galaxian_render_buffer();
#endif
#ifdef ENABLE_MOONCRESTA
      else if(machineType == MCH_MOONCRESTA)
        galaxian_render_buffer();
#endif
#ifdef ENABLE_SPACE
      else if(machineType == MCH_SPACE)
        spaceinvaders_render_buffer();
#endif
#ifdef ENABLE_PHOENIX
      else if(machineType == MCH_PHOENIX)
        phoenix_render_buffer();
#endif
#ifdef ENABLE_SBRKOUT
      else if(machineType == MCH_SBRKOUT)
        sbrkout_render_buffer();
#endif
#ifdef ENABLE_BOMBBEE
      else if(machineType == MCH_BOMBBEE)
        bombbee_render_buffer();
#endif
#ifdef ENABLE_GOINDOL
      else if(machineType == MCH_GOINDOL)
        goindol_render_buffer();
#endif
#ifdef ENABLE_PANG
      else if(machineType == MCH_PANG)
        pang_render_buffer();
#endif
      else
        ay_render_buffer();
    }
  } while(bytesOut);
#endif // audio hardware available
}

#ifdef ENABLE_GOINDOL
// Goindol: il core emulazione genera l'audio YM2203 (FM + SSG) in un ring buffer;
// qui (task audio) lo svuotiamo. Lo shift e' il punto di calibrazione del volume
// (>>5 + clamp +/-2000, vedi nota in goindol gen_audio).
void Audio::goindol_render_buffer(void) {
  Goindol *g = (Goindol*)currentMachine;
  int16_t buf[64];
  int got = g->audioPop(buf, 64);
  for (int i = 0; i < 64; i++) {
    int v = (i < got) ? (buf[i] >> 5) : 0;
    if (v >  2000) v =  2000;
    if (v < -2000) v = -2000;
    valueToBuffer(i, (short)v);
  }
}
#endif // ENABLE_GOINDOL

#ifdef ENABLE_PANG
// Pang: l'OKI6295 (ADPCM) + YM2413 (OPLL musica) sono generati dentro la
// machine (renderAudio), gia' mixati e clampati a ~+/-500 (livello valueToBuffer).
// Il bridge pang_audio_fill e' definito in SPINNERINO.ino (dove pang.h e' incluso).
extern void pang_audio_fill(int16_t *out, int frames);
void Audio::pang_render_buffer(void) {
  int16_t buf[64];
  pang_audio_fill(buf, 64);
  for (int i = 0; i < 64; i++)
    valueToBuffer(i, (short)buf[i]);
}
#endif // ENABLE_PANG

void Audio::ay_render_buffer(void) {
  char AY = 2; 
  char AY_INC = 8;
  char AY_VOL = 5;

#ifdef ENABLE_FROGGER
  if (machineType == MCH_FROGGER) { AY = 1; AY_INC = 9; AY_VOL = 11; }
#endif
#ifdef ENABLE_ANTEATER
  if (machineType == MCH_ANTEATER) { AY = 2; AY_INC = 9; AY_VOL = 5; }
#endif
#ifdef ENABLE_TIMEPLT
  if (machineType == MCH_TIMEPLT) { AY = 2; AY_INC = 9; AY_VOL = 5; }
#endif
#ifdef ENABLE_BOMBJACK
  if (machineType == MCH_BOMBJACK) { AY = 3; AY_INC = 8; AY_VOL = 10; }
#endif
#ifdef ENABLE_GYRUSS
  // Gyruss: tutti e 5 gli AY (5*3=15 canali); AY_VOL=4 per non clippare.
  // Per il TEMPO/velocita' vedi l'emulazione CPU (gyruss.cpp run_frame).
  if (machineType == MCH_GYRUSS) { AY = 5; AY_INC = 9; AY_VOL = 4; }
#endif
#ifdef ENABLE_TUTANKHM
  if (machineType == MCH_TUTANKHM) { AY = 2; AY_INC = 7; AY_VOL = 5; }
#endif
#ifdef ENABLE_MOTORACE
  if (machineType == MCH_MOTORACE) { AY = 2; AY_INC = 5; AY_VOL = 5; }
#endif
#ifdef ENABLE_MOONPATROL
  if (machineType == MCH_MOONPATROL) { AY = 2; AY_INC = 5; AY_VOL = 7; }
#endif
#ifdef ENABLE_SCRAMBLE
  if (machineType == MCH_SCRAMBLE) { AY = 2; AY_INC = 8; AY_VOL = 5; }
#endif
#ifdef ENABLE_BRUBBER
  if (machineType == MCH_BRUBBER) { AY = 2; AY_INC = 5; AY_VOL = 5; }
#endif
#ifdef ENABLE_ARKANOID
  // Arkanoid bootleg arkangc: 1x AY-3-8910 con pin clock = 12MHz XTAL / 4 = 3 MHz
  // (MAME taito/arkanoid.cpp:1382 + bootleg() linea 1409 sostituisce con AY8910).
  // AY-3-8910 STANDARD ha divider interno /8 (MAME ay8910.cpp:1342).
  // AY_INC = pin_clock / (8 * SAMPLE_RATE) = 3000000/(8*32000) = 11.7 -> 12
  // (sample rate 32 kHz, vedi commento in init() su MAX98357A).
  // AY_VOL=1 perche' usiamo ay_dac_log[] gia' scalata (vedi render path sotto).
  if (machineType == MCH_ARKANOID) { AY = 1; AY_INC = 12; AY_VOL = 1; }
#endif
#ifdef ENABLE_ARKANOID2
  // Arkanoid 2 bootleg arknoid2b: 1x YM2203 PSG @ 3 MHz (MAME tnzs.cpp).
  // YM2203 ha step counter /4 (vs /8 di AY-3-8910 standard, MAME ay8910.cpp).
  // A parita' di clock pin avanza 2x piu' veloce → AY_INC raddoppiato:
  //   3000000 / (4 * 32000) = 23.4 → 23
  // Riusa ay_dac_log[] + envelope path di Arkanoid 1 (registri 0..13 identici).
  // SPINNERINO P4: FM (YM2203) disabilitato (ym2203_chip=nullptr).
  // AY_VOL=1 era per bilanciare il mix FM; senza FM serve volume PSG pieno.
  if (machineType == MCH_ARKANOID2) { AY = 1; AY_INC = 23; AY_VOL = 5; }
#endif

  for(char ay = 0; ay < AY; ay++) {
    int ay_off = 16 * ay;
    for(char c = 0; c < 3; c++) {
      ay_period[ay][c] = currentMachine->soundregs[ay_off + 2 * c] + 256 * (currentMachine->soundregs[ay_off + 2 * c + 1] & 15);
      ay_enable[ay][c] = (((currentMachine->soundregs[ay_off + 7] >> c) & 1) | ((currentMachine->soundregs[ay_off + 7] >> (c + 2)) & 2)) ^ 3;
#ifdef ENABLE_MOTORACE
      // MotoRace: muta il noise channel AY (engine drone) che sovrasta la
      // musica. Il LFSR continua a girare, ma non viene mixato sui canali tone.
      if (machineType == MCH_MOTORACE) ay_enable[ay][c] &= 1;
#endif
      { uint8_t vreg = currentMachine->soundregs[ay_off + 8 + c];
        // Per giochi che usano envelope (BombJack, Arkanoid) preserva il bit 4
        // (envelope-mode flag) cosi' il render path puo' distinguerlo.
        bool keep_env_bit =
#ifdef ENABLE_BOMBJACK
          (machineType == MCH_BOMBJACK) ||
#endif
#ifdef ENABLE_ARKANOID
          (machineType == MCH_ARKANOID) ||
#endif
#ifdef ENABLE_ARKANOID2
          (machineType == MCH_ARKANOID2) ||
#endif
          false;
        ay_volume[ay][c] = keep_env_bit ? (vreg & 0x1F)
                                        : ((vreg & 0x10) ? 15 : (vreg & 0x0f));
      }
    }
    ay_period[ay][3] = currentMachine->soundregs[ay_off + 6] & 0x1f;

#if defined(ENABLE_BOMBJACK) || defined(ENABLE_ARKANOID) || defined(ENABLE_ARKANOID2)
    if (
#ifdef ENABLE_BOMBJACK
        machineType == MCH_BOMBJACK ||
#endif
#ifdef ENABLE_ARKANOID
        machineType == MCH_ARKANOID ||
#endif
#ifdef ENABLE_ARKANOID2
        machineType == MCH_ARKANOID2 ||
#endif
        false) {
      ay_envelope_period[ay] = currentMachine->soundregs[ay_off + 11] + 256 * currentMachine->soundregs[ay_off + 12];
      uint8_t new_shape = currentMachine->soundregs[ay_off + 13];
      if (new_shape != ay_envelope_shape[ay]) {
        ay_envelope_shape[ay] = new_shape & 0x0F;
        ay_envelope_counter[ay] = 0;
        ay_envelope_step[ay] = (ay_envelope_shape[ay] < 4 || (ay_envelope_shape[ay] >= 8 && ay_envelope_shape[ay] < 12)) ? 0 : 15;
        ay_envelope_holding[ay] = 0;
      }
    }
#endif
  }

  // ── Arkanoid 2: FM YM2203 square-wave approximation (port da P4 multi).
  // Decode FNUM/BLOCK -> phase increment, mixato come square wave nel loop.
#ifdef ENABLE_ARKANOID2
  static uint32_t gng_fm_phase_sq[1][3] = { {0,0,0} };
  uint32_t gng_fm_inc[1][3] = { {0,0,0} };
  int      gng_fm_vol[1][3] = { {0,0,0} };
  if (machineType == MCH_ARKANOID2) {
    Arkanoid2 *a = (Arkanoid2*)currentMachine;
    for (int ch = 0; ch < 3; ch++) {
      if (!a->fm_keyed[0][ch]) continue;
      uint8_t lo = a->ym_regs[0][0xA0 + ch];
      uint8_t hi = a->ym_regs[0][0xA4 + ch];
      int block = (hi >> 3) & 7;
      int fnum  = ((hi & 7) << 8) | lo;
      if (!fnum) continue;
      // f = fnum * 20833 / 2^(20-block); phase_inc = f * 2^32 / 32000
      uint64_t inc64 = (uint64_t)fnum * 20833ULL;
      inc64 = (inc64 << (12 + block)) / 32000ULL;
      gng_fm_inc[0][ch] = (uint32_t)inc64;
      uint8_t tl = a->ym_regs[0][0x4C + ch] & 0x7F;
      gng_fm_vol[0][ch] = (127 - tl) >> 3;
    }
  }
#endif

  for(int i = 0; i < 64; i++) {
    short value = 0;

#ifdef ENABLE_GYRUSS
    // Drums Gyruss: steppa l'i8039 N istruzioni per OGNI campione di output
    // (32 kHz) -> il DAC viene campionato regolarmente = niente crackle.
    if (machineType == MCH_GYRUSS) {
      gyruss *gyr = (gyruss*)currentMachine;
      gyr->service_i8039_irq();                       // assert IRQ 8039 se latchato
      for (int k = 0; k < 18; k++) gyr->step_i8039();
    }
#endif

#ifdef ENABLE_DKONG
    if(machineType == MCH_DKONG) {
      dkong *dkongMachine = (dkong*)currentMachine;
      if(dkongMachine->dkong_audio_rptr != dkongMachine->dkong_audio_wptr) {
#ifdef WORKAROUND_I2S_APLL_PROBLEM
        value = dkongMachine->dkong_audio_transfer_buffer[dkongMachine->dkong_audio_rptr][(dkongMachine->dkong_obuf_toggle ? 32 : 0) + (i / 2)];
#else
        value = dkongMachine->dkong_audio_transfer_buffer[dkongMachine->dkong_audio_rptr][i];
#endif
      }
      for(char j = 0; j < 3; j++) {
        if(dkongMachine->dkong_sample_cnt[j]) {
#ifdef WORKAROUND_I2S_APLL_PROBLEM
          value += *dkongMachine->dkong_sample_ptr[j] >> (2 - j); 
          if(i & 1) { dkongMachine->dkong_sample_ptr[j]++; dkongMachine->dkong_sample_cnt[j]--; }
#else
          value += *dkongMachine->dkong_sample_ptr[j]++ >> (2 - j); 
          dkongMachine->dkong_sample_cnt[j]--;
#endif
        }
      }
#ifdef WORKAROUND_I2S_APLL_PROBLEM
      if (i == 63) {
        if(dkongMachine->dkong_obuf_toggle) dkongMachine->dkong_audio_rptr = (dkongMachine->dkong_audio_rptr + 1) & DKONG_AUDIO_QUEUE_MASK;
        dkongMachine->dkong_obuf_toggle = !dkongMachine->dkong_obuf_toggle;
      }
#endif
    } else
#endif
#ifdef ENABLE_MARIOBROS
    if(machineType == MCH_MARIOBROS) {
      mariobros *marioMachine = (mariobros*)currentMachine;
      unsigned char raw;
      if(marioMachine->mario_audio_rptr != marioMachine->mario_audio_wptr) {
#ifdef WORKAROUND_I2S_APLL_PROBLEM
        raw = marioMachine->mario_audio_transfer_buffer[marioMachine->mario_audio_rptr][(marioMachine->mario_obuf_toggle ? 32 : 0) + (i / 2)];
#else
        raw = marioMachine->mario_audio_transfer_buffer[marioMachine->mario_audio_rptr][i];
#endif
      } else {
        raw = marioMachine->mario_dac_value;
      }
      {
        short centered = ((short)raw - 128) * 2;
        mario_lpf = (mario_lpf * 3 + centered) / 4;
        value = mario_lpf;
        if (value > 400) value = 400;
        if (value < -400) value = -400;
      }
#ifdef WORKAROUND_I2S_APLL_PROBLEM
      if (i == 63) {
        if(marioMachine->mario_obuf_toggle)
          marioMachine->mario_audio_rptr = (marioMachine->mario_audio_rptr + 1) & MARIOBROS_AUDIO_QUEUE_MASK;
        marioMachine->mario_obuf_toggle = !marioMachine->mario_obuf_toggle;
      }
#endif
    } else
#endif
    if(
#ifdef ENABLE_FROGGER
       machineType == MCH_FROGGER ||
#endif
#ifdef ENABLE_1942
       machineType == MCH_1942 || 
#endif
#ifdef ENABLE_ANTEATER
       machineType == MCH_ANTEATER || 
#endif
#ifdef ENABLE_TIMEPLT
       machineType == MCH_TIMEPLT || 
#endif
#ifdef ENABLE_GYRUSS
       machineType == MCH_GYRUSS ||
#endif
#ifdef ENABLE_TUTANKHM
       machineType == MCH_TUTANKHM ||
#endif
#ifdef ENABLE_MOTORACE
       machineType == MCH_MOTORACE ||
#endif
#ifdef ENABLE_MOONPATROL
       machineType == MCH_MOONPATROL ||
#endif
#ifdef ENABLE_SCRAMBLE
       machineType == MCH_SCRAMBLE ||
#endif
#ifdef ENABLE_BRUBBER
       machineType == MCH_BRUBBER ||
#endif
       false
    ) {
      for(char ay = 0; ay < AY; ay++) {
        if(ay_period[ay][3]) {
          audio_cnt[ay][3] += AY_INC;
          if(audio_cnt[ay][3] > ay_period[ay][3]) {
            audio_cnt[ay][3] -= ay_period[ay][3];
            ay_noise_rng[ay] ^= (((ay_noise_rng[ay] & 1) ^ ((ay_noise_rng[ay] >> 3) & 1)) << 17);
            ay_noise_rng[ay] >>= 1;
          }
        }
        for(char c = 0; c < 3; c++) {
          if((ay_period[ay][c] || (ay_enable[ay][c] & 2)) && ay_volume[ay][c] && ay_enable[ay][c]) {
            short bit = 1;
            if(ay_enable[ay][c] & 1) bit &= (audio_toggle[ay][c] > 0) ? 1 : 0;
            if(ay_enable[ay][c] & 2) bit &= (ay_noise_rng[ay] & 1) ? 1 : 0;
            if(bit == 0) bit = -1;
            value += AY_VOL * bit * ay_volume[ay][c];
            audio_cnt[ay][c] += AY_INC;
            if(audio_cnt[ay][c] > ay_period[ay][c]) {
              audio_cnt[ay][c] -= ay_period[ay][c];
              audio_toggle[ay][c] = -audio_toggle[ay][c];
            }
          }
        }
      }
#ifdef ENABLE_GYRUSS
      // Drums Gyruss: somma il campione DAC dell'i8039 al mix dei 5 AY.
      // DC blocker (high-pass ~40Hz) per togliere l'offset di riposo dell'8039
      // -> niente pop/distorsione costante. Scala (3) = volume drums (regolabile).
      if (machineType == MCH_GYRUSS) {
        static int dc = 0;
        int s = ((gyruss*)currentMachine)->i8039_sample();  // -128..+127
        dc += (s - dc) >> 7;                                // media lenta = livello DC
        value += (s - dc) * 3;                              // componente AC * volume
      }
#endif
    }
#ifdef ENABLE_BOMBJACK
    else if (machineType == MCH_BOMBJACK) {
      for(char ay = 0; ay < AY; ay++) {
        if (!ay_envelope_holding[ay] && ay_envelope_period[ay] > 0) {
          ay_envelope_counter[ay] += AY_INC;
          if (ay_envelope_counter[ay] >= ay_envelope_period[ay]) {
            ay_envelope_counter[ay] -= ay_envelope_period[ay];
            if (ay_envelope_shape[ay] < 8) {
              ay_envelope_step[ay]++;
              if (ay_envelope_step[ay] > 15) {
                ay_envelope_step[ay] = (ay_envelope_shape[ay] & 1) ? 0 : 15; 
                if (ay_envelope_shape[ay] & 2) ay_envelope_holding[ay] = 1; 
              }
            } else {
              ay_envelope_step[ay]--;
              if (ay_envelope_step[ay] < 0) {
                ay_envelope_step[ay] = (ay_envelope_shape[ay] & 1) ? 15 : 0; 
                if (ay_envelope_shape[ay] & 2) ay_envelope_holding[ay] = 1; 
              }
            }
          }
        }
        if(ay_period[ay][3]) {
          audio_cnt[ay][3] += AY_INC;
          if(audio_cnt[ay][3] > ay_period[ay][3]) {
            audio_cnt[ay][3] -= ay_period[ay][3];
            uint32_t b = (((ay_noise_rng[ay] >> 0) ^ (ay_noise_rng[ay] >> 3)) & 1);
            ay_noise_rng[ay] = (ay_noise_rng[ay] >> 1) | (b << 16);
          }
        }
        for(char c = 0; c < 3; c++) {
          if(ay_period[ay][c] && ay_enable[ay][c]) {
            int cur_v = (ay_volume[ay][c] & 0x10) ? ay_envelope_step[ay] : (ay_volume[ay][c] & 0x0F);
            if (cur_v > 0) {
              short bit = 1;
              if(ay_enable[ay][c] & 1) bit &= (audio_toggle[ay][c] > 0) ? 1:0;
              if(ay_enable[ay][c] & 2) bit &= (ay_noise_rng[ay] & 1) ? 1:0;
              if(bit == 0) bit = -1;
              value += AY_VOL * bit * cur_v;
            }
            audio_cnt[ay][c] += AY_INC;
            if(audio_cnt[ay][c] > ay_period[ay][c]) {
              audio_cnt[ay][c] -= ay_period[ay][c];
              audio_toggle[ay][c] = -audio_toggle[ay][c];
            }
          }
        }
      }
      value = value / 3;
    }
#endif
#if defined(ENABLE_ARKANOID) || defined(ENABLE_ARKANOID2)
    else if (
        false
#ifdef ENABLE_ARKANOID
        || machineType == MCH_ARKANOID
#endif
#ifdef ENABLE_ARKANOID2
        // YM2203 PSG (registri 0..13 binario-compatibili con AY-3-8910).
        // FM channels (reg 0x20+) ignorati: arknoid2 usa principalmente PSG.
        || machineType == MCH_ARKANOID2
#endif
    ) {
      // Path AY dedicato Arkanoid: tabella DAC logaritmica + while loop counter.
      // DAC AY-3-8910 (MAME ay8910.cpp:720, AYUMI ayumi.c:7-23): 16 step
      // logaritmici ~3 dB/step, scalati a max 120 (3 canali sommati = 360 max).
      // Headroom volutamente alto per evitare clipping al gain stage MAX98357A
      // (default GAIN=+9dB amplifica gia' ~2.8x in analogico).
      static const short ay_dac_log[16] = {
          0,   1,   2,   3,   4,   5,   8,  13,
         15,  25,  35,  45,  59,  76,  97, 120
      };
      for(char ay = 0; ay < AY; ay++) {
        // Envelope generator (shape attack/decay/hold, MAME ay8910.cpp)
        if (!ay_envelope_holding[ay] && ay_envelope_period[ay] > 0) {
          ay_envelope_counter[ay] += AY_INC;
          while (ay_envelope_counter[ay] >= ay_envelope_period[ay]) {
            ay_envelope_counter[ay] -= ay_envelope_period[ay];
            if (ay_envelope_shape[ay] < 8) {
              ay_envelope_step[ay]++;
              if (ay_envelope_step[ay] > 15) {
                ay_envelope_step[ay] = (ay_envelope_shape[ay] & 1) ? 0 : 15;
                if (ay_envelope_shape[ay] & 2) ay_envelope_holding[ay] = 1;
              }
            } else {
              ay_envelope_step[ay]--;
              if (ay_envelope_step[ay] < 0) {
                ay_envelope_step[ay] = (ay_envelope_shape[ay] & 1) ? 15 : 0;
                if (ay_envelope_shape[ay] & 2) ay_envelope_holding[ay] = 1;
              }
            }
          }
        }
        // Noise LFSR (while: clocca finche' il counter copre AY_INC step)
        if (ay_period[ay][3]) {
          audio_cnt[ay][3] += AY_INC;
          while (audio_cnt[ay][3] >= ay_period[ay][3]) {
            audio_cnt[ay][3] -= ay_period[ay][3];
            uint32_t b = (((ay_noise_rng[ay] >> 0) ^ (ay_noise_rng[ay] >> 3)) & 1);
            ay_noise_rng[ay] = (ay_noise_rng[ay] >> 1) | (b << 16);
          }
        }
        // 3 canali tone
        for(char c = 0; c < 3; c++) {
          if (ay_period[ay][c] && ay_enable[ay][c]) {
            int cur_v = (ay_volume[ay][c] & 0x10) ? ay_envelope_step[ay]
                                                  : (ay_volume[ay][c] & 0x0F);
            if (cur_v > 0) {
              short bit = 1;
              if (ay_enable[ay][c] & 1) bit &= (audio_toggle[ay][c] > 0) ? 1 : 0;
              if (ay_enable[ay][c] & 2) bit &= (ay_noise_rng[ay] & 1) ? 1 : 0;
              if (bit == 0) bit = -1;
              value += bit * ay_dac_log[cur_v & 0x0F];
            }
            audio_cnt[ay][c] += AY_INC;
            // WHILE (non if): a period basso devo togglare piu' volte per
            // sample, altrimenti perdo i toggle e genero subarmoniche.
            while (audio_cnt[ay][c] >= ay_period[ay][c]) {
              audio_cnt[ay][c] -= ay_period[ay][c];
              audio_toggle[ay][c] = -audio_toggle[ay][c];
            }
          }
        }
      }
    }
#endif // ENABLE_ARKANOID || ENABLE_ARKANOID2
#ifdef ENABLE_ARKANOID2
    // Arkanoid 2: FM YM2203 square-wave (port P4 multi audio.cpp:730-746).
    if (machineType == MCH_ARKANOID2) {
      int gain = 4;
      int fm_sum = 0;
      for (int ch = 0; ch < 3; ch++) {
        int v = gng_fm_vol[0][ch];
        if (!v) continue;
        gng_fm_phase_sq[0][ch] += gng_fm_inc[0][ch];
        short bit = (gng_fm_phase_sq[0][ch] & 0x80000000UL) ? 1 : -1;
        fm_sum += bit * v * gain;
      }
      if (fm_sum >  400) fm_sum =  400;
      if (fm_sum < -400) fm_sum = -400;
      value += fm_sum;
    }
#endif
#ifdef ENABLE_MOONPATROL
    // Mix MSM5205 ADPCM drum/percussion signal (attenuated to balance with AY music)
    if (machineType == MCH_MOONPATROL && g_moonpatrol_instance) {
      value += g_moonpatrol_instance->msm_signal / 2;
    }
#endif
    valueToBuffer(i, value);
  }
}

void Audio::sn76489_render_buffer(void) {
    int sn_inc = 11;  // SN_CLOCK / SAMPLE_RATE (gigas/greenberet)
#ifdef ENABLE_ROADFIGHTER
    // roadf: SN76489A @ 1.79 MHz (XTAL 14.318/8) -> note molto piu' basse di gigas.
    if (machineType == MCH_ROADFIGHTER) sn_inc = 5;
#endif

    // 4 chip max (Gigas), MrDo/Ladybug/GreenBeret usano solo i primi 1-2.
    static uint32_t noise_lfsr[4] = {0x4000, 0x4000, 0x4000, 0x4000};

    // Per Gigas con 4 chip mixati mono, riduci gain per chip per evitare
    // saturazione (gain*4 vs gain*2 dei 2-chip games).
    bool fourChip = false;
#ifdef ENABLE_GIGAS
    if (machineType == MCH_GIGAS) fourChip = true;
#endif
#ifdef ENABLE_GIGAS2
    if (machineType == MCH_GIGAS2) fourChip = true;
#endif
    int chips = fourChip ? 4 : 2;
    int gainPerCh = fourChip ? 3 : 6;   // 6 per 2-chip, 3 per 4-chip (anti-clip)

    // Volumi con hold
    int vol[4][4];
    for (int chip = 0; chip < chips; chip++) {
        for (int c = 0; c < 4; c++) {
            if (currentMachine->sn_hold[chip][c] > 0) {
                vol[chip][c] = currentMachine->sn_min_volume[chip][c];
                currentMachine->sn_hold[chip][c]--;
                if (currentMachine->sn_hold[chip][c] == 0)
                    currentMachine->sn_min_volume[chip][c] = currentMachine->sn_volume[chip][c];
            } else {
                vol[chip][c] = currentMachine->sn_volume[chip][c];
                currentMachine->sn_min_volume[chip][c] = currentMachine->sn_volume[chip][c];
            }
        }
    }

    for (int i = 0; i < 64; i++) {
        short sample = 0;

        for (int chip = 0; chip < chips; chip++) {
            for (int c = 0; c < 4; c++) {
                int period = currentMachine->sn_period[chip][c];

                if (vol[chip][c] < 15 && period > 0) {
                    sn_counter[chip][c] -= sn_inc;

                    while (sn_counter[chip][c] <= 0) {
                        if (c == 3) {  // Noise channel
                            uint32_t feedback = (noise_lfsr[chip] & 0x01) ^ ((noise_lfsr[chip] & 0x02) >> 1);
                            noise_lfsr[chip] = (noise_lfsr[chip] >> 1) | (feedback << 14);
                            sn_toggle[chip][c] = (noise_lfsr[chip] & 0x01) ? 1 : -1;
                            sn_counter[chip][c] += (period << 3);  // Rallenta noise
                        } else {  // Tone
                            sn_counter[chip][c] += period;
                            sn_toggle[chip][c] = -sn_toggle[chip][c];
                        }
                    }
                    sample += sn_toggle[chip][c] * (15 - vol[chip][c]) * gainPerCh;
                }
            }
        }
#ifdef ENABLE_ROADFIGHTER
        // DAC 8-bit R2R (motore/crash). dac_sample = signed -128..127, valore
        // latchato (qualita' base per-buffer); da rifinire con coda stile DKong
        // se il motore suona troppo "grezzo".
        if (machineType == MCH_ROADFIGHTER)
            sample += ((Roadfighter*)currentMachine)->roadf_dac_sample() * 2;
#endif
        valueToBuffer(i, sample);
    }
}


void Audio::namco_render_buffer(void) {
  // parse all three wsg channels
  for(char ch = 0; ch < 3; ch++) {  
    snd_wave[ch] = currentMachine->waveRom(currentMachine->soundregs[ch * 5 + 0x05] & 0x07);
    snd_freq[ch] = (ch == 0) ? currentMachine->soundregs[0x10] : 0; //5050-5054, 5056-5059, 505b-505e
    snd_freq[ch] += currentMachine->soundregs[ch * 5 + 0x11] << 4;
    snd_freq[ch] += currentMachine->soundregs[ch * 5 + 0x12] << 8;
    snd_freq[ch] += currentMachine->soundregs[ch * 5 + 0x13] << 12;
    snd_freq[ch] += currentMachine->soundregs[ch * 5 + 0x14] << 16;        
    snd_volume[ch] = currentMachine->soundregs[ch * 5 + 0x15]; //5055, 505a, 505f
  }

  // render first buffer contents
  for(int i = 0; i < 64; i++) {
    short value = 0;

    // add up to three wave signals
    if(snd_volume[0]) value += snd_volume[0] * snd_wave[0][(snd_cnt[0] >> 13) & 0x1f];
    if(snd_volume[1]) value += snd_volume[1] * snd_wave[1][(snd_cnt[1] >> 13) & 0x1f];
    if(snd_volume[2]) value += snd_volume[2] * snd_wave[2][(snd_cnt[2] >> 13) & 0x1f];

    snd_cnt[0] += snd_freq[0];
    snd_cnt[1] += snd_freq[1];
    snd_cnt[2] += snd_freq[2];
    
#ifdef ENABLE_GALAGA
    if(machineType == MCH_GALAGA) {
      galaga *galagaMachine = (galaga*)currentMachine;

      if(galagaMachine->snd_boom_cnt) {
        value += *galagaMachine->snd_boom_ptr * 3;

        if(galagaMachine->snd_boom_cnt & 1)
          galagaMachine->snd_boom_ptr++;

        galagaMachine->snd_boom_cnt--;
      }
    }
#endif
    valueToBuffer(i, value);
  }
}

#ifdef ENABLE_SPACE
void Audio::spaceinvaders_render_buffer(void) {
  // Space Invaders discrete audio (based on MAME mw8080bw_a.cpp)
  //
  // soundregs[0] = port 3: UFO(0) Shot(1) Explosion(2) InvaderDie(3) ExtPlay(4)
  // soundregs[1] = port 5: Fleet1(0) Fleet2(1) Fleet3(2) Fleet4(3) UFOhit(4)

  uint8_t p3 = currentMachine->soundregs[0];
  uint8_t p5 = currentMachine->soundregs[1];

  // Fleet: pick highest active bit → tone frequency (Hz)
  // Original hardware: 555 timer ~33-55Hz, doubled for small speaker audibility
  const int fleet_freq[4] = { 110, 80, 74, 66 };
  int fleet_f = 0;
  for(int b = 3; b >= 0; b--) {
    if(p5 & (1 << b)) { fleet_f = fleet_freq[b]; break; }
  }

  for(int i = 0; i < 64; i++) {
    short value = 0;

    // ── Advance noise LFSR: 17-bit, taps 4+16, clock 7515 Hz ──
    si_noise_clock += 7515;
    while(si_noise_clock >= 24000) {
      si_noise_clock -= 24000;
      int bit = ((si_noise_rng >> 4) ^ (si_noise_rng >> 16)) & 1;
      si_noise_rng = ((si_noise_rng << 1) | bit) & 0x1FFFF;
      si_noise_out = (si_noise_rng >> 12) & 1;
    }

    // ── UFO: SN76477 – SLF triangle ~5.3Hz modulates VCO 1220-3700Hz ──
    if(p3 & 0x01) {
      // SLF triangle: full cycle = 24000/5.3 ≈ 4528 samples
      si_ufo_sweep = (si_ufo_sweep + 1) % 4528;
      int slf_pos = (si_ufo_sweep < 2264) ? si_ufo_sweep : (4528 - si_ufo_sweep);
      int vco_freq = 1220 + (int)((long)slf_pos * 2480 / 2264);
      // VCO square wave: counter += freq, toggle at 12000 (= 24kHz/2)
      si_ufo_cnt += vco_freq;
      if(si_ufo_cnt >= 12000) {
        si_ufo_cnt -= 12000;
        si_ufo_toggle = -si_ufo_toggle;
      }
      value += si_ufo_toggle * 60;
    } else {
      si_ufo_sweep = 0;
    }

    // ── SHOT: original sample playback (12kHz samples, play each twice for 24kHz) ──
    if(p3 & 0x02) {
      if(!si_shot_playing) { si_shot_playing = 1; si_shot_pos = 0; si_shot_toggle = 0; }
      if((si_shot_pos >> 1) < si_sample_shot_LEN) {
        value += (signed char)pgm_read_byte(&si_sample_shot[si_shot_pos >> 1]) * 3;
        si_shot_pos++;
      }
    } else {
      si_shot_playing = 0;
    }

    // ── COIN INSERT: metallic clink (triggered via soundregs[2]) ──
    if(currentMachine->soundregs[2] && si_coin_timer == 0) {
      si_coin_timer = 360;  // ~15ms
      si_coin_env = 120;
      currentMachine->soundregs[2] = 0;
    }
    if(si_coin_timer > 0) {
      // Primary metallic tone: 4500Hz
      si_coin_cnt += 4500;
      if(si_coin_cnt >= 12000) {
        si_coin_cnt -= 12000;
        si_coin_toggle = -si_coin_toggle;
      }
      // Overtone for metallic ring: 9500Hz
      si_coin_cnt2 += 9500;
      if(si_coin_cnt2 >= 12000) {
        si_coin_cnt2 -= 12000;
        si_coin_toggle2 = -si_coin_toggle2;
      }
      value += (si_coin_toggle * si_coin_env + si_coin_toggle2 * (si_coin_env / 2)) / 2;
      si_coin_timer--;
      if((si_coin_timer % 12) == 0 && si_coin_env > 5) si_coin_env--;
    }

    // ── EXPLOSION: noise burst with slow decay (RC ~2.7s) ──
    if(p3 & 0x04) {
      if(si_explo_env == 0) si_explo_env = 120;  // init on trigger
      int noise = si_noise_out ? 1 : -1;
      value += noise * si_explo_env;
      // Slow decay: decrease envelope every ~50 samples (~2ms)
      si_explo_cnt++;
      if(si_explo_cnt >= 50) {
        si_explo_cnt = 0;
        if(si_explo_env > 15) si_explo_env--;
      }
    } else {
      si_explo_env = 0;
      si_explo_cnt = 0;
    }

    // ── INVADER DIE: original sample playback (12kHz samples, play each twice for 24kHz) ──
    if(p3 & 0x08) {
      if(!si_invhit_playing) { si_invhit_playing = 1; si_invhit_pos = 0; }
      if((si_invhit_pos >> 1) < si_sample_invhit_LEN) {
        value += (signed char)pgm_read_byte(&si_sample_invhit[si_invhit_pos >> 1]) * 3;
        si_invhit_pos++;
      }
    } else {
      si_invhit_playing = 0;
    }

    // ── FLEET MOVEMENT: low bass tone while any fleet bit set ──
    if(fleet_f > 0) {
      si_fleet_cnt += fleet_f;
      if(si_fleet_cnt >= 12000) {
        si_fleet_cnt -= 12000;
        si_fleet_toggle = -si_fleet_toggle;
      }
      value += si_fleet_toggle * 90;
    }

    // ── UFO HIT: descending warble tone ~2000Hz with ~15Hz modulation ──
    if(p5 & 0x10) {
      if(si_ufohit_freq == 0) si_ufohit_freq = 2000;  // init on trigger
      // Warble modulation at ~15Hz: amplitude ±200Hz
      si_ufohit_warble = (si_ufohit_warble + 1) % 1600;  // 24000/15 = 1600
      int warble_pos = (si_ufohit_warble < 800) ?
        (int)si_ufohit_warble : (int)(1600 - si_ufohit_warble);
      int mod_freq = si_ufohit_freq + (warble_pos * 400 / 800 - 200);
      if(mod_freq < 100) mod_freq = 100;
      si_ufohit_cnt += mod_freq;
      if(si_ufohit_cnt >= 12000) {
        si_ufohit_cnt -= 12000;
        si_ufohit_toggle = -si_ufohit_toggle;
      }
      value += si_ufohit_toggle * 80;
      // Descend (~2000→300 over ~1.5s = 36000 samples)
      if(si_ufohit_freq > 300) si_ufohit_freq--;
    } else {
      si_ufohit_freq = 0;
      si_ufohit_warble = 0;
    }

    // Clamp
    if(value > 500) value = 500;
    if(value < -500) value = -500;

    valueToBuffer(i, value);
  }
}
#endif // ENABLE_SPACE


#ifdef ENABLE_BOMBBEE
// ============================================================================
// Bomb Bee (Namco custom sound) — port di namco/warpwarp_a.cpp.
// Due segnali sommati: music (contatore mascherato) + sound (toni/rumore).
// Sintesi interna a 192 kHz (= clock/3/2/16) con downsample 8:1 a 24 kHz.
// Envelope esponenziale (decay) su entrambi i canali.
//
// soundregs: [0]=sound_w(0x6002) [1]=music1(0x6010) [2]=music2(0x6020)
//            [3]=trigger sound_w  [4]=trigger music2_w (settati in wrZ80)
// ============================================================================
void Audio::bombbee_render_buffer(void) {
  static bool     init = false;
  static int16_t  decay_lut[256];           // ampiezza per (volume>>7)
  static uint8_t  s_latch = 0, m1 = 0, m2 = 0;
  static int32_t  s_vol = 0, m_vol = 0;     // 0..0x7fff
  static int32_t  s_vfrac = 0, m_vfrac = 0; // accumulatore decay (milli)
  static int      s_dec = 178, m_dec = 57;  // decremento milli per sample 192k
  static uint16_t noise = 0;
  static int32_t  mcarry = 0, vcarry = 0;
  static uint32_t mcount = 0, vcount = 0;
  static int16_t  s_sig = 0, m_sig = 0;     // +amp / -amp / 0

  const int CLK16H = 192000;   // clock/3/2/16
  const int CLK1V  = 8000;     // clock/3/2/384

  if (!init) {
    // ampiezza bipolare ~±150 max, decadimento esponenziale come MAME m_decay[]
    for (int i = 0; i < 256; i++)
      decay_lut[i] = (int16_t)(150.0 / exp((255 - i) / 32.0) + 0.5);
    init = true;
  }

  // trigger envelope su scrittura (consuma i flag settati in wrZ80)
  if (currentMachine->soundregs[3]) {
    s_latch = currentMachine->soundregs[0] & 0x0f;
    s_vol = 0x7fff; noise = 0;
    s_dec = (s_latch & 8) ? 178 : 89;        // decay rapido/lento (sound)
    currentMachine->soundregs[3] = 0;
  }
  if (currentMachine->soundregs[4]) {
    m_vol = 0x7fff;
    m_dec = (currentMachine->soundregs[2] & 0x10) ? 178 : 57; // rapido/lento (music)
    currentMachine->soundregs[4] = 0;
  }
  m1 = currentMachine->soundregs[1] & 0x3f;  // frequenza music
  m2 = currentMachine->soundregs[2] & 0x3f;  // forma d'onda + gate

  for (int i = 0; i < 64; i++) {
    for (int k = 0; k < 8; k++) {            // 8 step 192k per campione 24k
      int16_t damp_m = decay_lut[m_vol >> 7];
      int16_t damp_s = decay_lut[s_vol >> 7];

      // ---- music: contatore a frequenza CLK16H/(4*(64-m1)) ----
      mcarry -= CLK16H / (4 * (64 - m1));
      while (mcarry < 0) {
        mcarry += CLK16H;
        mcount++;
        bool on = (mcount & ~m2 & 15) != 0;
        if ((m2 & 32) && (noise & 0x8000)) on = true;  // gate da rumore
        m_sig = on ? damp_m : -damp_m;
      }

      // ---- sound + noise: clock 1V = 8 kHz ----
      vcarry -= CLK1V;
      while (vcarry < 0) {
        vcarry += CLK16H;
        vcount++;
        if ((vcount & 3) == 2) {             // LFSR rumore: bit0 = bit0 ^ !bit10
          if ((noise & 1) == ((noise >> 10) & 1)) noise = (noise << 1) | 1;
          else                                     noise = (noise << 1);
        }
        bool on;
        switch (s_latch & 7) {
          case 0: on = vcount & 0x04; break;            // 4V
          case 1: on = vcount & 0x08; break;            // 8V
          case 2: on = vcount & 0x10; break;            // 16V
          case 3: on = vcount & 0x20; break;            // 32V
          case 4: on = !(vcount & 0x01) && !(vcount & 0x10); break;  // TONE1
          case 5: on = !(vcount & 0x02) && !(vcount & 0x20); break;  // TONE2
          case 6: on = !(vcount & 0x04) && !(vcount & 0x40); break;  // TONE3
          default: on = noise & 0x8000; break;          // NOISE
        }
        s_sig = on ? damp_s : -damp_s;
      }

      // ---- decay envelope (decremento volume) ----
      if (s_vol > 0) { s_vfrac += s_dec; while (s_vfrac >= 1000) { s_vfrac -= 1000; if (s_vol > 0) s_vol--; } }
      if (m_vol > 0) { m_vfrac += m_dec; while (m_vfrac >= 1000) { m_vfrac -= 1000; if (m_vol > 0) m_vol--; } }
    }
    valueToBuffer(i, (short)(s_sig + m_sig));
  }
}
#endif // ENABLE_BOMBBEE


void Audio::galaxian_render_buffer(void) {
  // Galaxian discrete sound hardware emulation (MAME galaxian_a.cpp)
  // SOUND_CLOCK = 18.432MHz/6/2 = 1.536MHz
  //
  // soundregs[0]    = VCO pitch (8-bit, written at 0x7800)
  // soundregs[1-4]  = LFO DAC bits (4-bit, 0x6004-0x6007)
  // soundregs[8-10] = FS1/FS2/FS3 background tone enables (0x6800-0x6802)
  // soundregs[11]   = HIT noise enable (0x6803)
  // soundregs[12]   = (unused, offset 4 not wired)
  // soundregs[13]   = FIRE shoot enable (0x6805, offset 5)
  // soundregs[14]   = VOL1 (0x6806, offset 6)
  // soundregs[15]   = VOL2 (0x6807, offset 7)
  // NOTE: No BGEN register — VCO is always active when pitch is audible

  int vco_pitch = currentMachine->soundregs[0];

  // VOL1/VOL2 control VCO output volume via resistor network
  int vol1 = currentMachine->soundregs[14];  // offset 6
  int vol2 = currentMachine->soundregs[15];  // offset 7
  int vco_vol = (vol1 || vol2) ? (20 + vol1 * 20 + vol2 * 20) : 25;

  // VCO frequency: freq = 1.536MHz / (16*(256-pitch)) = 96000 / (256-pitch) Hz
  // At 24kHz sample rate: half_period = (256-pitch) / 8 samples
  // Use 8-bit fixed-point (×32) for sub-sample precision to avoid detuned notes
  unsigned long half_period_fp = (unsigned long)(256 - vco_pitch) * 32;  // fixed-point ×256 / 8

  // Detect pitch sweeps (credit sound): VCO plays through R34 base path
  // when pitch is actively changing even without VOL1/VOL2.
  // gal_last_pitch / gal_pitch_active sono ora membri di Audio (vedi audio.h)
  // azzerati in Audio::start() per evitare residual hum quando torni al menu.
  if(vco_pitch != gal_last_pitch) {
    gal_pitch_active = 500;  // sustain ~20ms (just over 1 frame)
    gal_last_pitch = vco_pitch;
  }
  if(gal_pitch_active > 0) gal_pitch_active--;

  // VCO plays when: VOL is on (normal sounds) OR pitch is sweeping (credit sound)
  bool vco_on = (half_period_fp > 32) && (vol1 || vol2 || gal_pitch_active > 0);

  // FS1/FS2/FS3: 555 timer tones (frequencies from RC values)
  // FS1 ~130Hz, FS2 ~170Hz, FS3 ~230Hz
  // Half-periods at 24kHz sample rate
  static const int fs_period[3] = {92, 71, 52};

  // LFO: 4-bit DAC controls background march speed
  int lfo_val = currentMachine->soundregs[1] |
                (currentMachine->soundregs[2] << 1) |
                (currentMachine->soundregs[3] << 2) |
                (currentMachine->soundregs[4] << 3);

  unsigned long lfo_period = lfo_val ? (24000 / (lfo_val + 1)) : 0;
  unsigned long lfo_on_time = lfo_period * 6 / 10;  // 60% duty cycle

  // FS background march sound disabled — needs tuning
  bool fs_any = false; // currentMachine->soundregs[8] || currentMachine->soundregs[9] || currentMachine->soundregs[10];

  for(int i = 0; i < 64; i++) {
    short value = 0;

    // === VCO tone ===
    if(vco_on) {
      gal_tone_cnt += 256;  // increment by 1.0 in 8-bit fixed-point
      if(gal_tone_cnt >= half_period_fp) {
        gal_tone_cnt -= half_period_fp;
        gal_tone_toggle = -gal_tone_toggle;
      }
      value += gal_tone_toggle * vco_vol;
    }

    // === FS1, FS2, FS3: background march tones pulsed by LFO ===
    if(fs_any) {
      if(lfo_val > 0 && lfo_period > 0) {
        gal_lfo_acc++;
        if(gal_lfo_acc >= lfo_period) gal_lfo_acc = 0;

        bool lfo_gate = (gal_lfo_acc < lfo_on_time);

        if(lfo_gate) {
          int sweep = (int)(gal_lfo_acc * 50 / lfo_on_time);

          for(int fs = 0; fs < 3; fs++) {
            if(currentMachine->soundregs[8 + fs]) {
              int mod_period = fs_period[fs] * (100 - sweep) / 100;
              if(mod_period < 4) mod_period = 4;

              gal_fs_cnt[fs]++;
              if(gal_fs_cnt[fs] >= (unsigned long)mod_period) {
                gal_fs_cnt[fs] = 0;
                gal_fs_toggle[fs] = -gal_fs_toggle[fs];
              }

              int env = (gal_lfo_acc < lfo_on_time / 4)
                ? (int)(gal_lfo_acc * 30 / (lfo_on_time / 4))
                : 30;
              value += gal_fs_toggle[fs] * env;
            }
          }
        }
      } else {
        // LFO=0: continuous FS tones
        for(int fs = 0; fs < 3; fs++) {
          if(currentMachine->soundregs[8 + fs]) {
            gal_fs_cnt[fs]++;
            if(gal_fs_cnt[fs] >= (unsigned long)fs_period[fs]) {
              gal_fs_cnt[fs] = 0;
              gal_fs_toggle[fs] = -gal_fs_toggle[fs];
            }
            value += gal_fs_toggle[fs] * 25;
          }
        }
      }
    }

    // === HIT: explosion noise (LFSR, bandpass ~470Hz) ===
    if(currentMachine->soundregs[11]) {
      uint32_t b = ((gal_noise_rng >> 0) ^ (gal_noise_rng >> 3)) & 1;
      gal_noise_rng = (gal_noise_rng >> 1) | (b << 16);
      value += ((gal_noise_rng & 1) ? 90 : -90);
    }

    // === FIRE: shooting sound (offset 5, 555 astable ~2.7kHz + noise) ===
    if(currentMachine->soundregs[13]) {
      gal_fire_cnt++;
      if(gal_fire_cnt >= 9) {
        gal_fire_cnt = 0;
        uint32_t b = ((gal_fire_rng >> 0) ^ (gal_fire_rng >> 3)) & 1;
        gal_fire_rng = (gal_fire_rng >> 1) | (b << 16);
      }
      value += ((gal_fire_rng & 1) ? 70 : -70);
    }

    valueToBuffer(i, value);
  }
}


#ifdef ENABLE_PHOENIX
// ============================================================================
// PHOENIX discrete audio @ 24 kHz mono
//
// Mappa registri (settati da phoenix.cpp wrZ80):
//   soundregs[0] = sound A latch (effect 2: shoot/wing)
//                  bit 0-3 = level/sweep, bit 4-5 = freq base
//   soundregs[1] = sound B latch (effect 1: noise + melody)
//                  bit 0-3 = noise level, bit 4 = noise freq sel,
//                  bit 5 = noise filter, bit 6-7 = melody tune (0..3)
//   soundregs[2] = trigger A (consumato qui, set su edge da phoenix.cpp)
//   soundregs[3] = trigger B (idem)
//   soundregs[4] = melody trigger (idem, su cambio bit 6-7)
//
// Synth:
//   Effect 2 = square wave 200/400/800/1600 Hz × envelope decay 0.4s
//   Effect 1 = LFSR pseudo-noise periodo 588..6913 Hz × env decay 0.3s
//              + LPF 1-pole se filt attivo (timbro shield)
//   Melody  = 4 tune brevi (4-8 note) come square wave + ottava
// ============================================================================
void Audio::phoenix_render_buffer(void) {
  // Phoenix audio synth Galaxian-style approssimato. Tutti i fix risolti
  // durante porting P4_NEW_multi + FULL28 sono integrati qui:
  //  - FIRE/WING/SWOOP con FREQ_TABLE 4 valori in base a freq_sel (bit 4-5)
  //  - BGM Romance d'Amor 96 step CON SUSTAIN (0=mantieni, 0xFFFF=stop) ONE-SHOT
  //  - Boom esplosione navetta (pattern data==15 + freq_sel==0, cooldown 5s)
  //  - HIT esplosioni nemici (env esteso 8000 sample, LPF heavy >> 2)

  // tune3 MAME tms36xx.cpp voce 1 +1 ottava — 0=sustain, 0xFFFF=stop one-shot
  static const uint16_t TUNE_ROMANCE[] = {
    440, 0, 440, 0, 440, 0,        // A4 A4 A4
    440, 0, 392, 0, 349, 0,        // A4 G4 F4
    349, 0, 330, 0, 294, 0,        // F4 E4 D4
    294, 0, 349, 0, 440, 0,        // D4 F4 A4
    587, 0,   0, 0,   0, 0,        // D5 [sustain lungo]
    587, 0, 523, 0, 466, 0,        // D5 C5 Bb4
    466, 0, 440, 0, 392, 0,        // Bb4 A4 G4
    392, 0, 440, 0, 466, 0,        // G4 A4 Bb4
    440, 0, 466, 0, 440, 0,        // A4 Bb4 A4
    554, 0, 466, 0, 440, 0,        // C#5 Bb4 A4
    440, 0, 392, 0, 349, 0,        // A4 G4 F4
    349, 0, 330, 0, 294, 0,        // F4 E4 D4
    330, 0, 330, 0, 330, 0,        // E4 E4 E4
    330, 0, 349, 0, 330, 0,        // E4 F4 E4
    294, 0, 349, 0, 440, 0,        // D4 F4 A4
    587, 0,   0, 0,   0, 0,        // D5 [sustain finale]
    0xFFFF
  };
  static const uint16_t TUNE_FUR_ELISE[] = {
    659, 622, 659, 622, 659, 494, 587, 523, 440, 0xFFFF
  };
  static const uint16_t TUNE_WARNING[] = {
    523, 784, 523, 784, 523, 784, 523, 784, 0xFFFF
  };
  static const uint16_t* TUNES[4] = {
    TUNE_ROMANCE, TUNE_WARNING, TUNE_FUR_ELISE, TUNE_ROMANCE
  };

  static uint8_t  last_a = 0, last_b = 0;
  static int16_t  fire_env = 0, hit_env = 0;
  static int16_t  fire_cnt = 0;
  static uint16_t fire_freq = 0;
  static int8_t   fire_tog = 1;
  static bool     ph_initialized = false;

  uint8_t a = currentMachine->soundregs[0];
  uint8_t b = currentMachine->soundregs[1];

  if (!ph_initialized) {
    last_a = a; last_b = b;
    fire_env = hit_env = 0;
    ph_mel_active = false;
    ph_initialized = true;
    for (int i = 0; i < 64; i++) valueToBuffer(i, 0);
    return;
  }

  // Cooldown anti-re-trigger boom esplosione
  if (ph_expl_cooldown > 0) ph_expl_cooldown--;

  // ── Effect 2 (sound A): FIRE/WING/SWOOP + esplosione navetta player ──
  if (a != last_a) {
    uint8_t new_lvl  = a & 0x0F;
    uint8_t freq_sel = (a >> 4) & 0x03;
    last_a = a;
    // Esplosione navetta player: pattern stretto data==15 + freq_sel==0
    if (new_lvl == 15 && freq_sel == 0 && ph_expl_cooldown == 0) {
      ph_expl_env      = 12000;
      ph_expl_cooldown = 1875;       // ~5 sec cooldown
      ph_expl_lpf1 = 0;
      ph_expl_lpf2 = 0;
    } else if (new_lvl) {
      // FIRE (freq alta) / WING / SWOOP (freq bassa) in base a freq_sel
      static const uint16_t FREQ_TABLE[4] = { 800, 1200, 1700, 2400 };
      fire_freq = FREQ_TABLE[freq_sel];
      fire_env  = 3000;
    }
  }

  // ── Effect 1 (sound B): HIT esplosioni nemici + tune select ──
  if (b != last_b) {
    uint8_t old = last_b;
    last_b = b;
    uint8_t new_data = b & 0x0F;
    if (new_data && hit_env < 1500) {
      hit_env = 8000;
    }
    uint8_t old_tune = (old >> 6) & 0x03;
    uint8_t new_tune = (b   >> 6) & 0x03;
    if (new_tune != old_tune && new_tune != 0) {
      ph_mel_tune   = new_tune;
      ph_mel_idx    = 0;
      ph_mel_timer  = 1;
      ph_mel_active = true;
    }
    if (new_tune == 0 && old_tune != 0) {
      ph_mel_active = false;
      ph_mel_freq   = 0;
    }
  }

  for (int i = 0; i < 64; i++) {
    int32_t value = 0;

    // BOOM esplosione navetta player (noise + doppio LPF + decay quadratico)
    if (ph_expl_env > 0) {
      uint32_t bit = ((gal_noise_rng >> 0) ^ (gal_noise_rng >> 3)) & 1;
      gal_noise_rng = (gal_noise_rng >> 1) | (bit << 16);
      int32_t noise = (gal_noise_rng & 1) ? 246 : -246;
      ph_expl_lpf1 += (noise - ph_expl_lpf1) >> 2;
      ph_expl_lpf2 += (ph_expl_lpf1 - ph_expl_lpf2) >> 2;
      int32_t env_q = ((int32_t)ph_expl_env * ph_expl_env) / 12000;
      value += (ph_expl_lpf2 * env_q) / 3000;
      ph_expl_env--;
    }

    // FIRE/WING: square + noise + sweep VCO graduale (-1 ogni 4 sample)
    if (fire_env > 0) {
      static uint8_t sweep_div = 0;
      if (++sweep_div >= 4) {
        sweep_div = 0;
        if (fire_freq > 200) fire_freq -= 1;
      }
      fire_cnt++;
      uint16_t half_period = 24000 / (fire_freq * 2);
      if (half_period < 2) half_period = 2;
      if (fire_cnt >= half_period) {
        fire_cnt = 0;
        fire_tog = -fire_tog;
      }
      uint32_t bit = ((ph_lfsr >> 0) ^ (ph_lfsr >> 3)) & 1;
      ph_lfsr = (ph_lfsr >> 1) | (bit << 16);
      int16_t ns = (ph_lfsr & 1) ? 50 : -50;
      int16_t sq = fire_tog * 100;
      int16_t mix = sq + (ns >> 1);
      value += (mix * (int32_t)fire_env) / 3000;
      fire_env--;
    }

    // HIT esplosioni nemici (LPF heavy + envelope decay 8000 sample)
    if (hit_env > 0) {
      uint32_t bit = ((gal_noise_rng >> 0) ^ (gal_noise_rng >> 3)) & 1;
      gal_noise_rng = (gal_noise_rng >> 1) | (bit << 16);
      int16_t noise = (gal_noise_rng & 1) ? 180 : -180;
      ph_n_lpf += (noise - ph_n_lpf) >> 2;
      value += (ph_n_lpf * (int32_t)hit_env) / 8000;
      hit_env--;
    }

    // BGM Romance d'Amor (one-shot, 0=sustain, 0xFFFF=stop)
    if (ph_mel_active) {
      if (--ph_mel_timer == 0) {
        const uint16_t* t = TUNES[ph_mel_tune & 0x03];
        uint16_t f = t[ph_mel_idx];
        if (f == 0xFFFF) {
          ph_mel_active = false;
          ph_mel_freq   = 0;
        } else {
          if (f != 0) ph_mel_freq = f;
          ph_mel_idx++;
          if (ph_mel_idx >= 100) ph_mel_idx = 99;
          ph_mel_timer = 24000 / 5;   // ~200 ms per step
        }
      }
      if (ph_mel_freq) {
        ph_mel_phase += (uint32_t)ph_mel_freq * 65536U / 24000U;
        int16_t m  = (ph_mel_phase & 0x8000) ? 28 : -28;
        int16_t m2 = ((ph_mel_phase << 1) & 0x8000) ? 14 : -14;
        value += m + m2;
      }
    }

    if (value > 32767)  value = 32767;
    if (value < -32768) value = -32768;
    valueToBuffer(i, (short)value);
  }
}
#endif // ENABLE_PHOENIX


void Audio::generateSinusWave(int32_t amplitude, short* buffer, uint16_t length) {
  for (int i=0; i<length; ++i) {
    buffer[i] = int32_t(float(amplitude) * sin(2.0 * PI * (1.0 / length) * i));
  }
}

void Audio::discrete_render_buffer() {
  unsigned short duration = currentMachine->soundregs[0] + (currentMachine->soundregs[1] << 8);

  if (duration > 0)
    duration--;

  currentMachine->soundregs[0] = duration & 0x00ff;
  currentMachine->soundregs[1] = (duration & 0xff00) > 8;

  float frequency;
  switch (currentMachine->soundregs[2]) {
    case 0x3: frequency = A5_3; break;
    case 0x4: frequency = C6_4; break;
    case 0x5: frequency = F5_5; break;
    case 0x6: frequency = G5_6; break;
    case 0x7: frequency = E6_7; break;
    case 0x8: frequency = B6_8; break;
    case 0xE: frequency = D6_E; break;
    case 0xF: frequency = B5_F; break;
    case 0xB: frequency = XX_B; break;
  }

  unsigned short pause = currentMachine->soundregs[3];
  if (pause > 0)
    currentMachine->soundregs[3]--;

  float delta = 0;
  if (duration != 0 && pause == 0)
    delta = (frequency * (sizeof(sinusWaveBuffer) / 2)) / float(24000);
  
  for(int i = 0; i < 64; i++) {
    uint16_t pos = uint32_t(((i + 1) * delta) + positionLast) % (sizeof(sinusWaveBuffer) / 2);
    short value = sinusWaveBuffer[pos];

    if (i == 63)
      positionLast = pos;

    valueToBuffer(i, value);
  }
}

// ============================================================================
// Jingle splash — riproduce il PCM embedded (splash_audio.h): "Game Start"
// 16-bit mono @ 32 kHz. Suona via transmit() (audio_task), 64 sample/buffer.
// mel_phase = posizione corrente nel PCM (in sample mono).
// ============================================================================
void Audio::startSplashMelody() {
  mel_note  = 0;
  mel_buf   = 0;
  mel_phase = 0;          // posizione di lettura nel PCM
  playMelody = true;
}

void Audio::melody_render_buffer(void) {
  uint32_t pos = mel_phase;
  if (pos >= (uint32_t)SPLASH_AUDIO_LEN) {
    for (int i = 0; i < 64; i++) { snd_buffer[2*i] = 0; snd_buffer[2*i+1] = 0; }
    playMelody = false;
    return;
  }
  // Volume utente: 0..30 (mute a >=30). Scala lineare senza clipping.
  int vol = (volumeSetting >= 30) ? 0 : (31 - volumeSetting);
  // Attenuazione dedicata al solo jingle splash (NON tocca i giochi).
  // 100 = pieno, 50 = meta'. Abbassa/alza questo valore per regolarlo.
  const int SPLASH_GAIN_PCT = 50;
  for (int i = 0; i < 64; i++) {
    int16_t s = 0;
    if (pos < (uint32_t)SPLASH_AUDIO_LEN) {
      s = (int16_t)(((int32_t)splash_audio[pos] * vol * SPLASH_GAIN_PCT) / (30 * 100));
      pos++;
    }
    snd_buffer[2*i]     = s;
    snd_buffer[2*i + 1] = s;
  }
  mel_phase = pos;
}

void Audio::testSine(int hz, int amplitude) {
  // Riempie il buffer I2S con sinusoide pura a `hz` Hz, ampiezza `amplitude`
  // (0..32767). Bypassa qualsiasi emulazione: utile per testare il DAC HW.
  static float phase = 0.0f;
  const float TWO_PI_F = 6.28318530718f;
  const float phase_inc = TWO_PI_F * (float)hz / 32000.0f;   // sample rate fisso

#ifdef SND_I2S_DIGITAL
  size_t bytesOut = 0;
  do {
    i2s_channel_write(i2s_tx_handle, snd_buffer, sizeof(snd_buffer), &bytesOut, 0);
    if (bytesOut) {
      for (int i = 0; i < 64; i++) {
        int16_t s = (int16_t)((float)amplitude * sinf(phase));
        phase += phase_inc;
        if (phase >= TWO_PI_F) phase -= TWO_PI_F;
        snd_buffer[2 * i]     = s;
        snd_buffer[2 * i + 1] = s;
      }
    }
  } while (bytesOut);
#endif
}

void Audio::valueToBuffer(int index, short value) {
    // Mute completo al volume minimo (volumeSetting >= 30)
    int32_t expanded;
    if (volumeSetting >= 30) {
        expanded = 0;
    } else {
        // value is now in the range of +/- 512, expand with higher gain for MAX98357A
        expanded = (int32_t)value * 128 / volumeSetting;
    }

    // Clamp to 16-bit signed range to prevent clipping distortion
    if (expanded > 32767) expanded = 32767;
    if (expanded < -32767) expanded = -32767;

#ifdef SND_I2S_DIGITAL
    // MAX98357A: signed 16-bit stereo (same sample on L+R)
    int16_t sample = (int16_t)expanded;
    snd_buffer[2 * index]     = sample;  // Left
    snd_buffer[2 * index + 1] = sample;  // Right
#elif defined(SND_DIFF)
    // generate differential output
    snd_buffer[2 * index]   = 0x8000 + (int16_t)expanded;
    snd_buffer[2 * index + 1] = 0x8000 - (int16_t)expanded;
#else
    // work-around weird byte order bug, see
    // https://github.com/espressif/arduino-esp32/issues/8467#issuecomment-1656616015
    snd_buffer[index ^ 1]   = 0x8000 + (int16_t)expanded;
#endif
}

#ifdef ENABLE_SBRKOUT
// ============================================================================
// Super Breakout - DAC 1-bit (porting da MAME scanline_callback):
//   dac_bit = (videoram[0x391] & (scanline >> 2)) != 0
// scanline counter va 0..261 a rate 60*262 = 15720 Hz.
// videoram[0x391] e' aggiornato da Sbrkout::wr6502 -> soundregs[0].
// Audio sample rate = 24000 Hz. Upsample fix-point: scanline avanza ~0.655/sample.
// ============================================================================
static uint32_t sbr_scanline_acc = 0;
static int      sbr_scanline_v   = 0;

void Audio::sbrkout_render_buffer(void) {
  uint8_t pwm_byte = currentMachine->soundregs[0];

  // Silenzio quando nessun tono attivo (byte == 0): evita sgracchio costante.
  if (pwm_byte == 0) {
    for (int i = 0; i < 64; i++) valueToBuffer(i, 0);
    return;
  }

  for (int i = 0; i < 64; i++) {
    sbr_scanline_acc += 15720;
    while (sbr_scanline_acc >= 24000) {
      sbr_scanline_acc -= 24000;
      sbr_scanline_v = (sbr_scanline_v + 1);
      if (sbr_scanline_v >= 262) sbr_scanline_v = 0;
    }
    uint8_t sc4 = (uint8_t)(sbr_scanline_v >> 2);
    int bit = ((pwm_byte & sc4) != 0) ? 1 : 0;
    // Scala esponenziale diretta nel buffer (bypassa valueToBuffer che
    // applica *128/vol e satura, rendendo tutti i livelli uguali).
    // vol=1→32767, vol=15→~1036, vol=29→~32, vol>=30→0.
    static const short vol_table[30] = {
      32767, 24601, 18467, 13867, 10413,  7820,
       5872,  4410,  3312,  2487,  1867,  1402,
       1053,   791,   594,   446,   335,   251,
        189,   142,   107,    80,    60,    45,
         34,    25,    19,    14,    11,     8
    };
    int16_t amplitude = (volumeSetting >= 30) ? 0 : vol_table[volumeSetting - 1];
    int16_t sample = bit ? amplitude : -amplitude;
    snd_buffer[2 * i]     = sample;
    snd_buffer[2 * i + 1] = sample;
  }
}
#endif // ENABLE_SBRKOUT