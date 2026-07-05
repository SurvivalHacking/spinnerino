#ifndef MACHINEBASE_H
#define MACHINEBASE_H

#include "Arduino.h"
#include "../cpus/z80/Z80.h"
#include "../cpus/i8048/i8048.h"
#include "../emulation/input.h"

#ifdef LED_PIN
#include <FastLED.h>
#define NUM_LEDS  7

#define LED_BLACK    CRGB::Black
#define LED_RED      CRGB::Red
#define LED_GREEN    CRGB::Green
#define LED_BLUE     CRGB::Blue
#define LED_YELLOW   CRGB::Yellow
#define LED_MAGENTA  CRGB::Magenta
#define LED_CYAN     CRGB::Cyan
#define LED_WHITE    CRGB::White
#endif

#if defined(ENABLE_PANG)
  // Pang mappa palette+color+vram+obj+work in 0..0x57FF -> serve 0x5800 (22528).
  // (>= di tutti gli altri: 16384/8192, quindi copre l'intero set.)
  #define RAMSIZE   (0x5800)
#elif defined(ENABLE_ARKANOID) || defined(ENABLE_ROADFIGHTER) || defined(ENABLE_GOINDOL)
  // Road Fighter indirizza RAM/IO/VRAM in 0x0000-0x3FFF (16 KB) → richiede 16384.
  // Goindol mappa work+2x videoram+2x area in 0..0x27FF -> serve 16384.
  #define RAMSIZE   (16384)
#elif (defined(ENABLE_MOTORACE))
  #define RAMSIZE   (8192 + 1024 + 128)
#else
  #define RAMSIZE   (8192)
#endif

struct sprite_S {
  unsigned short code;
  unsigned char color, flags;
  short x, y;

  //bombjack
  unsigned char color_block; 
  char is_32x32;
  char flip_x;
  char flip_y;
};

enum {
  MCH_MENU = 0,
  #ifdef ENABLE_GALAGA  
    MCH_GALAGA,
  #endif 	
  #ifdef ENABLE_GALAXIAN  
    MCH_GALAXIAN,
  #endif 	
  #ifdef ENABLE_SPACE
    MCH_SPACE,
  #endif
  #ifdef ENABLE_MOTORACE
    MCH_MOTORACE,
  #endif
  #ifdef ENABLE_ARKANOID
    MCH_ARKANOID,
  #endif
  #ifdef ENABLE_ARKANOID2
    MCH_ARKANOID2,
  #endif
  #ifdef ENABLE_GIGAS
    MCH_GIGAS,
  #endif
  #ifdef ENABLE_GIGAS2
    MCH_GIGAS2,
  #endif
  #ifdef ENABLE_GOINDOL
    MCH_GOINDOL,
  #endif
  #ifdef ENABLE_SBRKOUT
    MCH_SBRKOUT,
  #endif
  #ifdef ENABLE_BOMBBEE
    MCH_BOMBBEE,
  #endif
  #ifdef ENABLE_PHOENIX
    MCH_PHOENIX,
  #endif
  #ifdef ENABLE_GYRUSS
    MCH_GYRUSS,
  #endif
  #ifdef ENABLE_ROADFIGHTER
    MCH_ROADFIGHTER,
  #endif
  #ifdef ENABLE_PANG
    MCH_PANG,
  #endif
};


// one inst at 3Mhz ~ 500k inst/sec = 500000/60 inst per frame
#define INST_PER_FRAME 300000/60/4 //=1250

#ifdef LED_PIN
  typedef const CRGB (*MenuLedType)[12][NUM_LEDS];
#endif

class machineBase
{
public:
    machineBase() { }
    virtual ~machineBase() { }

    virtual void init(Input *input, unsigned short *framebuffer, sprite_S *spritebuffer, unsigned char *memorybuffer) {
      this->input = input;
      this->frame_buffer = framebuffer; 
      this->sprite = spritebuffer;
      this->memory = memorybuffer;
      memset(soundregs, 0, sizeof(soundregs)); 
     }

    virtual void reset() {
      for(current_cpu = 0; current_cpu < sizeof(cpu) / sizeof(Z80); current_cpu++)
        ResetZ80(&cpu[current_cpu]);

      memset(memory, 0, RAMSIZE);
      memset(soundregs, 0, sizeof(soundregs)); 

      for (int chip = 0; chip < 4; chip++) {
        for (int c = 0; c < 4; c++) {
          sn_period[chip][c] = 0;
          sn_volume[chip][c] = 15; // Muto
          sn_min_volume[chip][c] = 15;
          sn_hold[chip][c] = 0;
        }
      }
      current_cpu = 0;
      game_started = 0;
      Z80_i8080 = 0;   // default: flag P/V = overflow (Z80). I giochi 8080 lo forzano a 1.
    }

    virtual signed char machineType() { return MCH_MENU; } 
    virtual signed char videoFlipY() { return 0; }
    virtual signed char videoFlipX() { return 0; }
    virtual signed char useVideoHalfRate() { return 0; }
    // Active row range for 240px displays: if >= 0, render 30 rows starting here at full 8px (no scaling)
    virtual int videoActiveStart() { return -1; }
    virtual bool isLandscape() { return false; }
    virtual bool hasOpaqueBG() { return false; }  // true = skip memset before render_row
    virtual bool freeRunEmulation() { return false; }  // true = emulation runs on fixed timer, not vblank-locked

    virtual unsigned char rdZ80(unsigned short Addr) { return 0xff; }
    virtual void wrZ80(unsigned short Addr, unsigned char Value) { };
    virtual void outZ80(unsigned short Port, unsigned char Value) { };
    virtual unsigned char opZ80(unsigned short Addr) { return 0x00; }
    virtual unsigned char inZ80(unsigned short Port) { return 0x00; }

    virtual void wrI8048_port(struct i8048_state_S *state, unsigned char port, unsigned char pos) { }
    virtual unsigned char rdI8048_port(struct i8048_state_S *state, unsigned char port) { return 0x00; };
    virtual unsigned char rdI8048_xdm(struct i8048_state_S *state, unsigned char addr)  { return 0x00; };
    virtual void wrI8048_xdm(struct i8048_state_S *state, unsigned char addr, unsigned char data) { };
    virtual unsigned char rdI8048_rom(struct i8048_state_S *state, unsigned short addr) { return 0x00; };

    virtual void run_frame(void) { };
    virtual void prepare_frame(void) { };
    virtual void render_row(short row) { };
    
    virtual const signed char *waveRom(unsigned char value) { return 0; }
    virtual const unsigned short *logo(void) { return 0; };
    virtual bool hasNamcoAudio() { return false; }

    // Durata in millisecondi dell'hold direzionale BUTTON_LEFT/RIGHT generato
    // dall'EC11 per ogni click. 0 = nessun hold (default, OK per giochi che
    // leggono IN1 una volta per frame o usano paddle continuo come Arkanoid).
    // Giochi che leggono IN1 molte volte per frame dal Z80 (Space Invaders,
    // Galaga, MotoRace) ridefiniscono a ~100 ms per non perdere gli input.
    virtual int ec11PulseHoldMs() { return 0; }

    // Sensibilita' EC11 per-gioco, livello 1..10 (5 = default di fabbrica).
    // Regolata dal menu IMPOSTAZIONI nel carosello, persistita in NVS per
    // machineType(). Scala l'hold direzionale (qui sotto) e lo step encoder
    // paddle/dial (applicato in input.cpp via ec11_apply_sensitivity()).
    int ec11SensLevel = 5;
    // Hold direzionale effettivo = base * livello / 5. A livello 5 = invariato.
    // Min 15 ms quando il gioco usa l'hold (base>0), per non azzerarlo.
    int ec11HoldMsEff() {
      int base = ec11PulseHoldMs();
      if (base <= 0) return 0;
      int v = (base * ec11SensLevel) / 5;
      return v < 15 ? 15 : v;
    }

    // Disabilita il vol mode (hold FIRE 3s) per giochi dove FIRE viene tenuto
    // premuto a lungo come parte del gameplay normale (es. MotoRace dove
    // FIRE = acceleratore). Default false = vol mode abilitato.
    virtual bool disableVolMode() { return false; }

#if defined(ENABLE_SBRKOUT)
    // M6502 (fake6502): dispatcher in emulation.cpp inoltra qui.
    // Wrapped in SBRKOUT per non alterare la vtable quando 6502 non serve
    // (mantiene binary bit-identico per macchine Z80-only).
    virtual unsigned char rd6502(unsigned short Addr) { return 0xff; }
    virtual void          wr6502(unsigned short Addr, unsigned char Value) { }
    virtual unsigned char op6502(unsigned short Addr) { return rd6502(Addr); }
#endif
#ifdef LED_PIN
    virtual void menuLeds(CRGB *leds) { memcpy(leds, menu_leds, NUM_LEDS*sizeof(CRGB)); };
    virtual void gameLeds(CRGB *leds) { memcpy(leds, menu_leds, NUM_LEDS*sizeof(CRGB)); };
#endif
    char game_started;	
    unsigned char soundregs[80];
    
    //Mr.Do!
    // SN76489: fino a 4 chip (Gigas ne ha 4 @ 0xFC00-0xFC03), 4 canali ciascuno
    // (3 tono + 1 rumore). Estesi da [2][4] a [4][4] per supporto Gigas.
    int sn_period[4][4];
    int sn_volume[4][4];
    int sn_min_volume[4][4]; // latched min volume per audio render cycle
    int sn_hold[4][4];       // hold counter: keep sound active for N render cycles
protected:
    virtual void blit_tile(short row, char col) { }
    virtual void blit_sprite(short row, unsigned char s) { }
	
    Input *input;
    Z80 cpu[3];
    char irq_enable[3];
    char current_cpu;
    unsigned char irq_ptr;

    int active_sprites;
    sprite_S *sprite;
    unsigned short *frame_buffer;
    unsigned char *memory;

private:	
#ifdef LED_PIN
    const CRGB menu_leds[7] = { LED_BLACK, LED_BLACK, LED_BLACK, LED_BLACK, LED_BLACK, LED_BLACK, LED_BLACK };
#endif
};

#endif