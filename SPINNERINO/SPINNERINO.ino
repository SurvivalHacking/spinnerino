/*
 * ============================================================================
 * SPINNERINO V1.0 - 21/05/2026
 * by Marco Prunca and Davide Gatti www.survivalhacking.it
 *
 * (2026-07-01: PANG free-run 57.42Hz OK + render stretch pieno + volume gain x2 reale)
 * ============================================================================
 *
 * Arduino IDE settings (esp32 core >= 3.2.0):
 *   Board:                 ESP32P4 Dev Module
 *   Flash Size:            32MB
 *   PSRAM:                 Enabled
 *   Partition Scheme:      32M Flash (13Mb App/6.75Mb SPIFFS)
 *   USB Mode:              Hardware CDC and JTAG
 *   USB CDC On Boot:       Disabled
 *   Upload Speed:          921600
 *
 * ----------------------------------------------------------------------------
 *   COMANDI:
 *
 *   Gioca:            -Ruota l'encoder/paddle/spinner. 
 *                     -Premi manopola (COIN) encoder per inserire credito. 
 *                     -Premi pulsante FIRE per avviare il gioco.
 *
 *   REGOLA VOLUME:    Tieni premuto FIRE per ~3 secondi
 *                     -> compare la barra volume verde. Poi RUOTA l'encoder:
 *                     orario (destra) = alza, antiorario (sinistra) = abbassa.
 *                     RILASCIA FIRE per confermare e salvare (NVS). Durante la
 *                     regolazione il paddle resta fermo.
 *
 *   ESCI DAL GIOCO:   tieni premuto (COIN) per ~3 secondi -> torna al menu giochi.
 *
 *   IMPOSTAZIONI:     nel menu giochi, dopo l'ultimo gioco c'e' la voce
 *                     IMPOSTAZIONI per regolare la sensibilita' dell'encoder
 *                     (spinner) per ogni gioco.
 *
 * ----------------------------------------------------------------------------
 *
 * Dove trovare i file da inserire nella cartella ROM per la conversione:
 * [Galaga (Namco Rev. B ROM)](https://www.google.com/search?q=galaga.zip+arcade+rom)
 * [Galaxian](https://www.google.com/search?q=glaxian.zip+arcade+rom)
 * [SpaceInvaders](https://www.google.com/search?q=invaders.zip+arcade+rom)
 * [Arkanoid](https://www.google.com/search?q=arkangc.zip+arcade+rom)
 * [Arkanoid2](https://www.google.com/search?q=arknoid2.zip+arcade+rom)
 * [Gigas Bootleg](https://www.google.com/search?q=gigasb.zip+arcade+rom)
 * [Gigas2 Bootleg](https://www.google.com/search?q=gigasm2b.zip+arcade+rom)
 * [Super Breakout](https://www.google.com/search?q=sbrkout.zip+arcade+rom)
 * [Moto Race USA](https://www.google.com/search?q=motorace.zip+arcade+rom)
 * [Phoenix](https://www.google.com/search?q=phoenix.zip+arcade+rom)
 * [Goindol](https://www.google.com/search?q=goindol.zip+arcade+rom)
 * [Gyruss](https://www.google.com/search?q=gyruss.zip+arcade+rom)
 * [Road Fighters](https://www.google.com/search?q=roadf2.zip+arcade+rom)
 * [Bomb Bee](https://www.google.com/search?q=bombbee.zip+arcade+rom)
 * [Pang / Buster Bros](https://www.google.com/search?q=pang.zip+arcade+rom)   
 *
 * Una volta recuperati tutti i file ZIP inseriteli nella cartella SPINNERINO\ROMS
 * Per convertire le ROM:
 * - WINDOWS: Lanciare il file convert_all.bat nella cartella \ROM_CONVERTER (serve avere installato python)
 * - MACOS: Lanciare il file .\convert.all.sh nella cartella \ROM_CONVERTER (nel caso rendere prima eseguibile con il comendo chmod +x conv_all.sh)
 *
 * ============================================================================
 */

#include <Arduino.h>
#include "esp_task_wdt.h"
#include "config.h"
#include "machines/machineBase.h"
#include "emulation/audio.h"
#include "emulation/video.h"
#include "emulation/input.h"
#include "emulation/menu.h"
#include "emulation/boot_menu.h"
#include "emulation/emulation.h"

// CPU cores (Z80 sempre, M6502 solo se Super breakout attivo, M6803 per MotoRace)
#include "cpus/z80/Z80.c"
#if defined(ENABLE_SBRKOUT)
#include "cpus/m6502/fake6502.c"
#endif
#ifdef ENABLE_MOTORACE
#include "cpus/m6803/m6803.c"
#endif
#ifdef ENABLE_GYRUSS
#include "cpus/m6809/m6809.c"   // M6809 sub-CPU (Konami-1, versione function-pointer)
#include "cpus/i8048/i8048.c"   // i8039 MCU drum/percussioni Gyruss
#endif
#ifdef ENABLE_ARKANOID
#include "machines/arkanoid/arkanoid.h"
#include "machines/arkanoid/arkanoid.cpp"
#endif
// YM2203 FM emulator (MAME fm.c). Condiviso da Arkanoid 2 e Goindol: una sola
// copia in flash, inclusa PRIMA dei .cpp che la usano (fm.h dichiara le API).
#if defined(ENABLE_ARKANOID2) || defined(ENABLE_GOINDOL)
#include "machines/arkanoid2/ym2203/fm.h"
#include "machines/arkanoid2/ym2203/fm.c"
#endif
#ifdef ENABLE_ARKANOID2
#include "machines/arkanoid2/arkanoid2.h"
#include "machines/arkanoid2/arkanoid2.cpp"
#endif
#ifdef ENABLE_GIGAS
#include "machines/gigas/gigas.h"
#include "machines/gigas/gigas.cpp"
#endif
#ifdef ENABLE_GIGAS2
#include "machines/gigas2/gigas2.h"
#include "machines/gigas2/gigas2.cpp"
#endif
#ifdef ENABLE_GOINDOL
#include "machines/goindol/goindol.h"
#include "machines/goindol/goindol.cpp"
#endif
#ifdef ENABLE_SBRKOUT
#include "machines/sbrkout/sbrkout.h"
#include "machines/sbrkout/sbrkout.cpp"
#endif
#ifdef ENABLE_BOMBBEE
#include "machines/bombbee/bombbee.h"
#include "machines/bombbee/bombbee.cpp"
#endif
#ifdef ENABLE_SPACE
#include "machines/spaceinvaders/spaceinvaders.h"
#include "machines/spaceinvaders/spaceinvaders.cpp"
#endif
#ifdef ENABLE_GALAGA
#include "machines/galaga/galaga.h"
#include "machines/galaga/galaga.cpp"
#endif
#ifdef ENABLE_MOTORACE
#include "machines/motorace/motorace.h"
#include "machines/motorace/motorace.cpp"
#endif
#ifdef ENABLE_PHOENIX
#include "machines/phoenix/phoenix.h"
#include "machines/phoenix/phoenix.cpp"
#endif
#ifdef ENABLE_GALAXIAN
#include "machines/galaxian/galaxian.h"
#include "machines/galaxian/galaxian.cpp"
#endif
#ifdef ENABLE_GYRUSS
#include "machines/gyruss/gyruss.h"
#include "machines/gyruss/gyruss.cpp"
#endif
#ifdef ENABLE_ROADFIGHTER
#include "machines/roadfighter/roadfighter.h"
#include "machines/roadfighter/roadfighter.cpp"
#endif
#ifdef ENABLE_PANG
// Struttura split come goindol: .h dichiarazioni, .cpp implementazione+asset.
#include "machines/pang/pang.h"
#include "machines/pang/pang.cpp"
#endif

#include <Preferences.h>
static Preferences volPrefs;

// Splash screen 240x320 portrait (PROGMEM, ~153 KB)
#include "splash_logo.h"

// ============================================================================
// Globali richiesti dal framework
// ============================================================================
unsigned short *frame_buffer  = nullptr;
sprite_S       *sprite_buffer = nullptr;
unsigned char  *memory        = nullptr;
machineBase    *currentMachine = nullptr;

Audio audio;
Video video;
Input input;
Menu  menu;
BootMenu bootMenu;
SettingsMenu settingsMenu;

// Lista macchine (multi-machine via menu carousel)
#ifdef ENABLE_ARKANOID
Arkanoid arkanoid;
#endif
#ifdef ENABLE_ARKANOID2
Arkanoid2 arkanoid2;
#endif
#ifdef ENABLE_GIGAS
Gigas gigas;
#endif
#ifdef ENABLE_GIGAS2
Gigas2 gigas2;
#endif
#ifdef ENABLE_GOINDOL
Goindol goindol;
#endif
#ifdef ENABLE_SBRKOUT
Sbrkout sbrkout;
#endif
#ifdef ENABLE_BOMBBEE
BombBee bombbee;
#endif
#ifdef ENABLE_SPACE
SpaceInvaders spaceinvaders;
#endif
#ifdef ENABLE_GALAGA
galaga galagaInst;
#endif
#ifdef ENABLE_MOTORACE
motorace motoraceInst;
#endif
#ifdef ENABLE_PHOENIX
Phoenix phoenix;
#endif
#ifdef ENABLE_GALAXIAN
galaxian galaxianInst;
#endif
#ifdef ENABLE_GYRUSS
gyruss gyrussInst;
#endif
#ifdef ENABLE_ROADFIGHTER
Roadfighter roadfighter;
#endif
#ifdef ENABLE_PANG
pang pangInst;
// Bridge audio: audio.cpp (Audio::pang_render_buffer) chiama questa funzione,
// definita qui dove pang.h e' incluso (gli asset non vengono duplicati in audio.cpp).
void pang_audio_fill(int16_t *out, int frames) { pangInst.renderAudio(out, frames); }
#endif
// Ordine di visualizzazione (boot menu + menu impostazioni spinner), richiesto
// utente 2026-06-06: Arkanoid, Arkanoid 2, Gigas, Gigas 2, Goindol, Super Breakout,
// Space Invaders, Galaga, MotoRace USA, Phoenix, Galaxian.
// (Goindol aggiunto 2026-06-30 dopo Gigas 2: famiglia SunA, dial/spinner; da SPINNERINO WIFI.)
// Entrambi i menu (BootMenu/SettingsMenu in boot_menu.cpp) indicizzano questo
// array per posizione, quindi quest'ordine e' l'unica sorgente di verita'.

machineBase *machines[] = {
#ifdef ENABLE_ARKANOID
  &arkanoid,
#endif
#ifdef ENABLE_ARKANOID2
  &arkanoid2,
#endif
#ifdef ENABLE_GIGAS
  &gigas,
#endif
#ifdef ENABLE_GIGAS2
  &gigas2,
#endif
#ifdef ENABLE_GOINDOL
  &goindol,
#endif
#ifdef ENABLE_SBRKOUT
  &sbrkout,
#endif
#ifdef ENABLE_BOMBBEE
  &bombbee,
#endif
#ifdef ENABLE_SPACE
  &spaceinvaders,
#endif
#ifdef ENABLE_GALAGA
  &galagaInst,
#endif
#ifdef ENABLE_MOTORACE
  &motoraceInst,
#endif
#ifdef ENABLE_PHOENIX
  &phoenix,
#endif
#ifdef ENABLE_GALAXIAN
  &galaxianInst,
#endif
#ifdef ENABLE_GYRUSS
  &gyrussInst,
#endif
#ifdef ENABLE_ROADFIGHTER
  &roadfighter,
#endif
#ifdef ENABLE_PANG
  &pangInst,
#endif
};
const int machinesCount = sizeof(machines) / sizeof(machines[0]);

static bool in_menu = true;     // true finche' il menu boot non chiude
static bool in_settings = false; // true mentre il menu IMPOSTAZIONI e' aperto

// Helper: salva la sensibilita' EC11 di tutti i giochi su NVS (per machineType).
static void save_ec11_sens() {
  Preferences sp;
  sp.begin("ec11sens", false);
  for (int i = 0; i < machinesCount; i++) {
    char k[8]; snprintf(k, sizeof(k), "t%d", (int)machines[i]->machineType());
    sp.putUChar(k, (uint8_t)machines[i]->ec11SensLevel);
  }
  sp.end();
}

// Strip buffer 320x8 condiviso tra game render e BootMenu.
// DEVE essere dichiarato prima di setup() per essere visibile in scope.
unsigned short out_strip[TFT_FB_W * 8];

// ============================================================================
// Splash screen: blit immagine 240x320 portrait, attendi N ms o skip su FIRE.
// Presuppone che video.setOrientation(true) sia gia' stata chiamata.
// ============================================================================
static void show_splash(unsigned long duration_ms) {
  // Splash in LANDSCAPE 320x232 (pre-ruotata 90 anti-orario in Python).
  // SPLASH_W=320, SPLASH_H=232. Pipeline: 29 strip da 8 righe x 320 col.
  for (int s = 0; s < SPLASH_H / 8; s++) {
    memcpy(out_strip,
           splash_logo + s * 8 * SPLASH_W,
           SPLASH_W * 8 * sizeof(unsigned short));
    video.write(out_strip, SPLASH_W * 8);
  }

  // Mute dell'emulazione durante lo splash (il jingle ha priorita' sul mute):
  // dopo la melodia resta silenzio pulito, e il menu boot e' silenzioso.
  audio.setMute(true);
  // Jingle d'avvio (suonato dall'audio_task via transmit(), si auto-ferma).
  audio.startSplashMelody();

  // Attesa con skip su FIRE
  unsigned long t0 = millis();
  while (millis() - t0 < duration_ms) {
    if (input.buttons_get() & BUTTON_FIRE) break;
    delay(50);
  }

  audio.stopMelody();   // ferma il jingle se ancora in corso (skip FIRE)
}

// ============================================================================
// Audio task: gira sull'altro core e tiene il DMA I2S sempre pieno.
// IMPORTANTE: con buffer DMA = 1536 sample (48 ms) e frame 60 fps (16.67 ms),
// se transmit() viene chiamata solo a fine frame il DMA si svuota -> click
// periodici udibili come distorsione. Galagino chiama transmit() 6 volte per
// frame; qui usiamo un task dedicato che lo fa ogni 2 ms.
// ============================================================================
static void audio_task(void *param) {
  Audio *aud = (Audio*)param;
  for (;;) {
    aud->transmit();
    // pdMS_TO_TICKS arrotonda a 1 tick minimo (con tickrate 100 Hz default
    // 2/portTICK_PERIOD_MS = 0 → busy loop → starve IDLE → TWDT 5s freeze).
    // Phoenix col rendering audio leggero scatena il bug; altri giochi
    // mascheravano perche' transmit() durava di piu'.
    vTaskDelay(pdMS_TO_TICKS(2));
    if (pdMS_TO_TICKS(2) == 0) vTaskDelay(1);   // safety net se tickrate insolito
  }
}

// ============================================================================
// Callback Input -> Audio/Video
// ============================================================================
static void onVolumeUpDown(bool up, bool down)     { audio.volumeUpDown(up, down); }
static void onBrightnessUpDown(bool up, bool down) { /* opt: video.setBrightness */ }
static void onDoReset()         { if (currentMachine) currentMachine->reset(); }
static void onDoAttractReset()  { /* opt */ }

// ============================================================================
// setup / loop
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(50);
  Serial.println(F("\n[SPINNERINO SH] boot - Marco Prunca & Davide Gatti"));

  setCpuFrequencyMhz(360);   // ESP32-P4 default max

  // Framebuffer: 256 px wide × 8 row × 2 byte (1 strip per render)
  frame_buffer  = (unsigned short*)malloc(256 * 8 * 2);
  sprite_buffer = (sprite_S*)malloc(128 * sizeof(sprite_S));
  memory        = (unsigned char*)malloc(RAMSIZE);

  if (!frame_buffer || !sprite_buffer || !memory) {
    Serial.println(F("ALLOC FAIL"));
    while (1) delay(1000);
  }

  currentMachine = machines[0];

  for (int i = 0; i < machinesCount; i++)
    machines[i]->init(&input, frame_buffer, sprite_buffer, memory);

  // Sensibilita' EC11 per-gioco da NVS (chiave per machineType, default livello 5).
  {
    Preferences sp;
    sp.begin("ec11sens", true);
    for (int i = 0; i < machinesCount; i++) {
      char k[8]; snprintf(k, sizeof(k), "t%d", (int)machines[i]->machineType());
      uint8_t lv = sp.getUChar(k, 5);
      if (lv >= 1 && lv <= 10) machines[i]->ec11SensLevel = lv;
    }
    sp.end();
  }

  audio.init();
  audio.start(currentMachine);

  input.init();

  // Volume da NVS (con default)
  volPrefs.begin("arkanoid", true);
  uint8_t savedVol = volPrefs.getUChar("volume", 5);
  volPrefs.end();
  if (savedVol >= 1 && savedVol <= 30)
    audio.volumeSetting = savedVol;

  input.onVolumeUpDown(onVolumeUpDown);
  input.onBrightnessUpDown(onBrightnessUpDown);
  input.onDoReset(onDoReset);
  input.onDoAttractReset(onDoAttractReset);
  input.onVolumeCommit([]() {
    volPrefs.begin("arkanoid", false);
    volPrefs.putUChar("volume", (uint8_t)audio.volumeSetting);
    volPrefs.end();
    Serial.printf("[VOL] saved %d to NVS\n", audio.volumeSetting);
  });

  menu.init(&input, machines, machinesCount, frame_buffer);
  bootMenu.init(&input, machines, machinesCount, out_strip);

  // Audio task su CORE 0 (insieme ad Arduino main+video).
  // Phoenix e' polling-only: Z80 polla VBLANK su 0x7800 a 5.5MHz, audio sullo
  // stesso core dell'emulazione lo starva (game bloccato HUD INSERT COIN).
  // Separazione fisica: audio core 0, emulation core 1.
  // NOTA (fix bisect 2026-05-09): la slowness di Arkanoid 2/Galaga/Motorace
  // NON era causata dal pin audio_task su core 0, ma dalla saturazione SRAM
  // interna in Phoenix::init() (~268 KB allocate al boot per TUTTE le machine
  // → stack FreeRTOS in PSRAM → 5x lenti). Risolto in phoenix.cpp spostando
  // l'allocazione da init() a reset() (lazy, solo quando Phoenix selezionato).
  // Stack 8KB per YM2203 update_one().
  xTaskCreatePinnedToCore(audio_task, "aud", 8192, &audio, 5, NULL, 0);

  video.begin();
  Serial.println(F("[BOOT] video.begin done"));

  // Splash + menu restano in LANDSCAPE come il game (per ora).
  // L'immagine splash_logo.h e' stata pre-ruotata 90 anti-orario in Python
  // per apparire diritta sul display portrait fisico (vedi convert_splash.py).
  show_splash(4300);                  // splash, copre il jingle (~3.8s), FIRE per skip
  Serial.println(F("[BOOT] splash done -> boot menu"));
}

// ============================================================================
// TEST_PATTERN: bypassa Arkanoid e disegna un pattern noto per verificare
// display, orientamento, ordine RGB/BGR, mappatura pixel.
// Commenta la riga sotto per tornare al render normale.
// ============================================================================
//#define TEST_PATTERN

#ifdef TEST_PATTERN
// Colori RGB565 byte-swapped per ESP32 SPI BE
static const unsigned short C_BLACK   = 0x0000;
static const unsigned short C_WHITE   = 0xFFFF;
static const unsigned short C_RED     = 0x00F8;  // RGB565: F800 -> swap
static const unsigned short C_GREEN   = 0xE007;  // 07E0 -> swap
static const unsigned short C_BLUE    = 0x1F00;  // 001F -> swap
static const unsigned short C_YELLOW  = 0xE0FF;  // FFE0
static const unsigned short C_CYAN    = 0xFF07;  // 07FF
static const unsigned short C_MAGENTA = 0x1FF8;  // F81F

static void draw_test_pattern_strip(int strip_row) {
  // strip_row 0..27, copre 8 righe pixel (y = strip_row*8 .. strip_row*8+7)
  // Layout 256x224:
  //   row 0..1   (y 0-15)   : barre di colore (R,G,B,W,Y,C,M,K)
  //   row 2..3   (y 16-31)  : sfumatura R/G/B grayscale
  //   row 4..25  (y 32-207) : scacchiera 8x8 con cornice 1px bianca
  //   row 26..27 (y 208-223): scritta "ARKANOID TEST" semplice -> usiamo barre

  for (int y = 0; y < 8; y++) {
    unsigned short *line = frame_buffer + y * 256;
    int gy = strip_row * 8 + y;

    // Cornice bianca esterna 1px
    bool border = (gy == 0) || (gy == 223);

    if (strip_row < 2) {
      // Barre colore (8 barre x 32 px = 256)
      for (int x = 0; x < 256; x++) {
        unsigned short c;
        switch (x / 32) {
          case 0: c = C_RED; break;
          case 1: c = C_GREEN; break;
          case 2: c = C_BLUE; break;
          case 3: c = C_WHITE; break;
          case 4: c = C_YELLOW; break;
          case 5: c = C_CYAN; break;
          case 6: c = C_MAGENTA; break;
          default: c = C_BLACK; break;
        }
        if (border || x == 0 || x == 255) c = C_WHITE;
        line[x] = c;
      }
    } else if (strip_row < 4) {
      // Sfumatura grayscale (256 step)
      for (int x = 0; x < 256; x++) {
        uint8_t g = (uint8_t)x;
        uint16_t rgb = ((g & 0xF8) << 8) | ((g & 0xFC) << 3) | (g >> 3);
        unsigned short bs = (rgb >> 8) | (rgb << 8);
        if (border || x == 0 || x == 255) bs = C_WHITE;
        line[x] = bs;
      }
    } else {
      // Scacchiera 16x16 + cornice
      for (int x = 0; x < 256; x++) {
        bool side = (x == 0) || (x == 255);
        bool checker = ((x / 16) ^ (gy / 16)) & 1;
        line[x] = (border || side) ? C_WHITE : (checker ? C_BLACK : C_BLUE);
      }
    }
  }
}

static void render_one_frame() {
  for (int row = 0; row < 28; row++) {
    draw_test_pattern_strip(row);
    video.write(frame_buffer, 256 * 8);
  }
}
#else
// ============================================================================
// Render loop: window FULL 320x240 con padding nero, game 256x224 centrato.
// Indipendente da offset HW del pannello ST7789.
// ============================================================================
// (out_strip dichiarato sopra, vicino alle altre globali)

// Forward declarations per overlay volume (definizioni piu' avanti)
extern bool input_is_vol_mode_active(void);
static void draw_volume_overlay(unsigned short *strip_buf, int row, int vol_setting);

// DEBUG_BORDER disabilitato (era confondente)
//#define DEBUG_BORDER

static inline void apply_border_to_strip(int strip_y_global) {
  for (int r = 0; r < 8; r++) {
    unsigned short *line = out_strip + r * TFT_FB_W;
    int gy = strip_y_global + r;
    if (gy < 4 || gy >= TFT_FB_H - 4) {
      for (int x = 0; x < TFT_FB_W; x++) line[x] = 0x1F00; // BLU
    } else {
      for (int x = 0; x < 4; x++) line[x] = 0x00F8;        // ROSSO sx
      for (int x = TFT_FB_W - 4; x < TFT_FB_W; x++) line[x] = 0xE007; // VERDE dx
    }
  }
}

static void render_one_frame() {
  currentMachine->prepare_frame();

  // Riga di padding nero (8 righe) per top/bottom
  static bool padding_inited = false;
  if (!padding_inited) {
    memset(out_strip, 0, sizeof(out_strip));
    padding_inited = true;
  }

  // 1) Top padding: scritto in chunks di max 8 righe (out_strip e' 8 righe)
  if (TFT_Y_OFFSET > 0) {
    memset(out_strip, 0, TFT_FB_W * 8 * sizeof(unsigned short));
    int rem = TFT_Y_OFFSET;
    while (rem > 0) {
      int chunk = (rem > 8) ? 8 : rem;
      video.write(out_strip, TFT_FB_W * chunk);
      rem -= chunk;
    }
  }

  // 2) Game: 28 strip di 8 righe ciascuna, ognuna con padding laterale
  for (int row = 0; row < 28; row++) {
    if (!currentMachine->hasOpaqueBG())
      memset(frame_buffer, 0, 256 * 8 * 2);
    currentMachine->render_row(row);

    // Compone strip 320x8: padding sx + game 256 + padding dx
    for (int r = 0; r < 8; r++) {
      unsigned short *dst = out_strip + r * TFT_FB_W;
      // padding sinistro
      memset(dst, 0, TFT_X_OFFSET * sizeof(unsigned short));
      // game (256 px)
      memcpy(dst + TFT_X_OFFSET,
             frame_buffer + r * 256,
             256 * sizeof(unsigned short));
      // padding destro
      int right_pad = TFT_FB_W - TFT_X_OFFSET - 256;
      if (right_pad > 0)
        memset(dst + TFT_X_OFFSET + 256, 0, right_pad * sizeof(unsigned short));
    }
#ifdef DEBUG_BORDER
    apply_border_to_strip(TFT_Y_OFFSET + row * 8);
#endif
    // Overlay barra volume non bloccante (sopra il game in user portrait alto)
    if (input_is_vol_mode_active()) {
      draw_volume_overlay(out_strip, row, audio.volumeSetting);
    }
    video.write(out_strip, TFT_FB_W * 8);
  }

  // 3) Bottom padding (chunks di max 8 righe)
  int bottom_pad = TFT_FB_H - TFT_Y_OFFSET - 224;
  if (bottom_pad > 0) {
    memset(out_strip, 0, TFT_FB_W * 8 * sizeof(unsigned short));
    int rem = bottom_pad;
    while (rem > 0) {
      int chunk = (rem > 8) ? 8 : rem;
      video.write(out_strip, TFT_FB_W * chunk);
      rem -= chunk;
    }
  }

  // audio.transmit() ora gestita da audio_task() su core dedicato (vedi setup)
  emulation_notifyGive();
}
#endif // TEST_PATTERN

// Debug Serial ogni 60 frame (~1s)
extern volatile int16_t ec11_paddle_x;
class ArkanoidDebug : public Arkanoid {
public:
  Z80 *get_cpu() { return &cpu[0]; }
};
static void debug_tick() {
  static uint32_t fc = 0;
  static int last_start = -1, last_fire = -1, last_a = -1, last_b = -1;
  fc++;

  int ps = digitalRead(BTN_START);
  int ec_sw = digitalRead(EC11_PIN_SW);
  int ec_a = digitalRead(EC11_PIN_A);
  int ec_b = digitalRead(EC11_PIN_B);

  // Edge detection: stampa subito ogni cambiamento
  if (ps != last_start) {
    Serial.printf("[%lu] *** START %d -> %d\n", fc, last_start, ps);
    last_start = ps;
  }
  if (ec_sw != last_fire) {
    Serial.printf("[%lu] *** FIRE %d -> %d\n", fc, last_fire, ec_sw);
    last_fire = ec_sw;
  }
  if (ec_a != last_a) {
    Serial.printf("[%lu] *** EC_A %d -> %d (paddle=%d)\n", fc, last_a, ec_a, (int)ec11_paddle_x);
    last_a = ec_a;
  }
  if (ec_b != last_b) {
    Serial.printf("[%lu] *** EC_B %d -> %d (paddle=%d)\n", fc, last_b, ec_b, (int)ec11_paddle_x);
    last_b = ec_b;
  }

  // Heartbeat ogni 3 secondi
  if ((fc % 180) == 0) {
    int vnz = 0;
    for (int i = 0; i < 0x800; i++) if (memory[0x1000 + i]) vnz++;
    Z80 *cpu = ((ArkanoidDebug*)&arkanoid)->get_cpu();
    Serial.printf("[%lu] PC=%04X paddle=%d vram=%d  pins ST=%d FIRE=%d A%d B%d\n",
                  fc, cpu->PC.W, (int)ec11_paddle_x, vnz, ps, ec_sw, ec_a, ec_b);
  }
}

// =====================================================================
// TEST_AUDIO: bypassa l'emulazione e genera una sinusoide pura sul DAC.
// Decommenta per diagnosticare l'hardware audio (ES8311 + NS4150B + speaker):
//   - tono pulito a 440 Hz -> HW OK, problema e' nell'emulazione AY
//   - rumore/distorsione    -> HW (SD pin, gain, alimentazione, GND)
// Commenta per tornare al gioco normale.
// =====================================================================
//#define TEST_AUDIO

// Forward declarations dichiarate in input.cpp
extern bool input_consume_back_to_menu();
extern void input_reset_button_state();
extern bool input_is_vol_mode_active();

// ============================================================================
// Overlay barra volume (non bloccante): chiamata per ogni strip durante il
// render del game. Disegna nei pixel landscape che corrispondono alla parte
// alta del display in user portrait (= prime 16 colonne x_l del frame_buffer
// landscape, dentro il padding TFT_X_OFFSET=32 che e' normalmente nero).
//
// User view portrait (barra centrata orizzontalmente, lunga 112 px):
//   user_y_p ∈ [0..15]    -> barra (alta 16 px)
//   user_x_p ∈ [64..175]  -> lunghezza barra (112 px utili centrati in 240)
//
// Mapping a coords landscape (boot_menu): x_l = user_y_p, y_l = 239 - user_x_p
//   x_l ∈ [0..15]    (16 colonne landscape, dentro padding sx)
//   y_l ∈ [64..175]  (112 righe centrate, fuori range = padding nero)
//
// Volume range [1..30]: 1 = max, 30 = min. Inverso visivo:
//   fill_px = 112 * (31 - vol) / 30   ∈ [3..112]
// ============================================================================
static void draw_volume_overlay(unsigned short *strip_buf, int row, int vol_setting) {
  const int BAR_LEN  = 112;                       // metà di 224 (=larghezza precedente)
  const int BAR_Y_LO = (224 - BAR_LEN) / 2;       // 56  -> centratura (224-112)/2
  const int BAR_Y_HI = BAR_Y_LO + BAR_LEN - 1;    // 167

  int fill_px = (BAR_LEN * (31 - vol_setting)) / 30;
  if (fill_px < 0)        fill_px = 0;
  if (fill_px > BAR_LEN)  fill_px = BAR_LEN;

  for (int r = 0; r < 8; r++) {
    int y_l = row * 8 + r;
    if (y_l >= 224) break;

    // Fuori range barra: lascia il pixel nero (padding originale del game)
    if (y_l < BAR_Y_LO || y_l > BAR_Y_HI) continue;

    // Fill verde da sinistra-user (alto y_l) verso destra-user (basso y_l)
    bool fill = (y_l > BAR_Y_HI - fill_px);
    // Bordi sinistro/destro user = estremi della barra
    bool border_lr = (y_l == BAR_Y_LO) || (y_l == BAR_Y_HI);

    // Soglia "rossa" vicino al massimo: ultimi ~10 px del fill (verso il max
    // utente = bassi y_l = vicini a BAR_Y_LO) sono rossi, gli altri verdi.
    const int RED_ZONE = 10;
    bool fill_red = fill && (y_l <= BAR_Y_LO + RED_ZONE - 1);

    unsigned short *line = strip_buf + r * TFT_FB_W;

    for (int x_l = 0; x_l < 16; x_l++) {
      // Bordi top/bottom user = x_l 0 e 15
      bool border_tb = (x_l == 0) || (x_l == 15);

      unsigned short color;
      if (border_tb || border_lr) color = 0xFFFF;        // bianco bordo
      else if (fill_red)          color = 0x00F8;        // rosso (RGB565 byte-swap di 0xF800)
      else if (fill)              color = 0xE007;        // verde fill (RGB565 byte-swap)
      else                        color = 0x0000;        // nero background interno

      line[x_l] = color;
    }
  }
}

// ============================================================================
// Termina il gioco corrente e torna al menu boot (hold START 4s).
// ============================================================================
// Attract mode: tracker timeout in game, indice prossimo gioco per rotazione.
static unsigned long game_attract_t0 = 0;     // 0 = disarmato
static int           attract_next_idx = 0;    // gioco proposto al prossimo return menu

static void return_to_menu(int next_idx = -1) {
  Serial.println(F("[BOOT] back to menu"));
  audio.setMute(true);               // mute prima di stop per evitare click/loop
  emulation_stop();                  // ferma il task emulation, resetta machine
  audio.start(currentMachine);       // ri-azzera stati audio residui (es. Galaxian VCO sweep)
  input_reset_button_state();        // pulisce sequenza COIN/START e hold tracker
  int start = (next_idx >= 0) ? next_idx : 0;
  bootMenu.init(&input, machines, machinesCount, out_strip, start);
  in_menu = true;
  game_attract_t0 = 0;               // disarma attract finche' non riparte un gioco
}

// ============================================================================
// Esce dal menu di selezione e avvia il gioco scelto: setup macchina + audio
// + flip video + task emulation.
// ============================================================================
extern void ec11_apply_sensitivity(int level);   // input.cpp

static void start_selected_game(int idx) {
  currentMachine = machines[idx];
  currentMachine->reset();
  // Applica la sensibilita' EC11 del gioco (step encoder paddle/dial).
  // L'hold direzionale e' gia' scalato in input.cpp via ec11HoldMsEff().
  ec11_apply_sensitivity(currentMachine->ec11SensLevel);
  audio.setMute(false);              // riattiva audio quando avvia il gioco
  audio.start(currentMachine);

  if (currentMachine->videoFlipY() && !currentMachine->videoFlipX())
    video.flipVertical(1);
  else if (!currentMachine->videoFlipY() && currentMachine->videoFlipX())
    video.flipHorizontal(1);
  // Caso entrambi (rot 180) → gestito in software dalla machine

  emulation_start();
  Serial.printf("[BOOT] start machine %d (type=%d)\n", idx, currentMachine->machineType());

  // Arma timer attract game: dopo MASTER_ATTRACT_GAME_TIMEOUT torna al menu
  // e propone il gioco successivo (rotazione carosello).
  attract_next_idx = (idx + 1) % machinesCount;
  game_attract_t0 = millis();
}

void loop() {
#ifdef TEST_AUDIO
  audio.testSine(440, 8000);
  return;
#endif

  if (in_settings) {
    settingsMenu.update();
    settingsMenu.render();
    if (settingsMenu.finished()) {
      save_ec11_sens();                 // persisti i livelli su NVS
      input_reset_button_state();       // pulisci sequenza START/COIN
      in_settings = false;
      in_menu     = true;
      bootMenu.init(&input, machines, machinesCount, out_strip);
    }
    delay(16);
    return;
  }

  if (in_menu) {
    bootMenu.update();
    bootMenu.render();
    if (bootMenu.finished()) {
      int sel = bootMenu.selectedIndex();
      if (sel >= machinesCount) {
        // Voce "IMPOSTAZIONI" selezionata → apri il menu sensibilita' EC11.
        in_menu     = false;
        in_settings = true;
        settingsMenu.init(&input, machines, machinesCount, out_strip);
        input_reset_button_state();
      } else {
        in_menu = false;
        start_selected_game(sel);
      }
    }
    delay(16);   // ~60 fps cap del menu
    return;
  }

  // Hold START 3s -> torna al menu boot
  // CRITICO: chiama buttons_get() ad ogni loop per assicurare che la logica
  // di hold tracker giri anche se la machine corrente non legge IN0 spesso
  // (es. Gigas in test mode legge IN0 raramente).
  unsigned char in_btn = input.buttons_get();
  if (input_consume_back_to_menu()) {
    return_to_menu();
    return;
  }

#ifdef MASTER_ATTRACT_GAME_TIMEOUT
  // Reset attract timer su qualsiasi input dell'utente (tasti o rotazione EC11);
  // se nessun input per MASTER_ATTRACT_GAME_TIMEOUT ms, torna a menu con
  // prossimo gioco in rotazione.
  {
    extern volatile uint8_t ec11_dial_counter;
    static uint8_t last_dial = 0;
    uint8_t cur_dial = ec11_dial_counter;
    if (in_btn || cur_dial != last_dial) game_attract_t0 = millis();
    last_dial = cur_dial;
  }
  if (game_attract_t0 && (millis() - game_attract_t0) > MASTER_ATTRACT_GAME_TIMEOUT) {
    Serial.printf("[ATTRACT] game timeout, rotate to machine %d\n", attract_next_idx);
    return_to_menu(attract_next_idx);
    return;
  }
#endif

  unsigned long t0 = micros();
  render_one_frame();
  uint32_t render_us = micros() - t0;
  debug_tick();

  // Diagnostica: media tempo render ogni 60 frame (1 sec) + max picco
  static uint32_t accum_render = 0, max_render = 0, frame_cnt = 0;
  accum_render += render_us;
  if (render_us > max_render) max_render = render_us;
  if (++frame_cnt >= 60) {
    Serial.printf("[R] render_us avg=%lu max=%lu (budget 16667)\n",
                  accum_render / 60, max_render);
    accum_render = 0; max_render = 0; frame_cnt = 0;
  }

  // Frame cap a ~60 fps (16 ms)
  unsigned long t1 = render_us / 1000;
  if (t1 < 16) delay(16 - t1);
}
