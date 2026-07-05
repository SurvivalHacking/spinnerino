#ifndef ARKANOID2_H
#define ARKANOID2_H

#include "../machineBase.h"

#ifdef ENABLE_ARKANOID2

// Arkanoid 2 - Revenge of Doh (Taito 1987, set arknoid2b)
// Hardware: 2x Z80 (main + sub) + YM2203 + Seta X1-001 + uPD4701 paddle
// Display: 256x224 portrait (ROT90 cabinet view)
//
// FASE 1B-1: SD ROM load + Z80 main emulation + schermo nero.
// Sub Z80, sprite (X1-001), audio (YM2203), paddle EC11 nelle fasi successive.

class Arkanoid2 : public machineBase
{
public:
  Arkanoid2() {}
  ~Arkanoid2() {}

  signed char machineType()       override { return MCH_ARKANOID2; }
  signed char videoFlipY()        override { return 0; }
  signed char videoFlipX()        override { return 0; }
  signed char useVideoHalfRate()  override { return 0; }
  bool        isLandscape()       override { return false; }

  void init(Input *input, unsigned short *framebuffer,
            sprite_S *spritebuffer, unsigned char *memorybuffer) override;
  void reset()    override;
  void freeRoms();

  unsigned char rdZ80(unsigned short Addr) override;
  void          wrZ80(unsigned short Addr, unsigned char Value) override;
  unsigned char opZ80(unsigned short Addr) override;
  unsigned char inZ80(unsigned short Port) override;
  void          outZ80(unsigned short Port, unsigned char Value) override;

  void run_frame()     override;
  void prepare_frame() override;
  void render_row(short row) override;

  const signed char    *waveRom(unsigned char value) override { return 0; }
  const unsigned short *logo(void) override;

protected:
  void blit_tile(short row, char col)            override {}
  void blit_sprite(short row, unsigned char s)   override {}

private:
  // ROM caricate da SD (PSRAM)
  const unsigned char *rom_main    = nullptr;   // 64 KB main Z80
  const unsigned char *rom_sub     = nullptr;   // 64 KB sub Z80 (audio CPU)
  const unsigned short *palette    = nullptr;   // 512 short
  const unsigned char *gfx_decoded = nullptr;   // 1 MB pre-decoded sprite GFX

  // Memory map main Z80:
  //   0x8000-0xBFFF banked (16 KB di RAM in bank 0,1; ROM in bank 2..7)
  unsigned char *main_ram   = nullptr;          // 16 KB (banks 0,1)
  unsigned char *sub_ram    = nullptr;          // 4 KB (sub 0xD000-0xDFFF)
  unsigned char *shared_ram = nullptr;          // 4 KB (0xE000-0xEFFF)

  // Bank registers
  unsigned char main_bank = 0;                  // bits 0..2 di 0xF600
  unsigned char sub_bank  = 0;                  // 0..3, scritto a 0xA000
  bool          sub_running = true;             // bit 4 di 0xF600

  // X1-001 sprite engine RAM
  unsigned short *spritecode = nullptr;         // 0x2000 entries (8 KB)
  unsigned char  *spriteylow = nullptr;         // 0x300 byte
  unsigned char  sprite_ctrl[4] = {0, 0, 0, 0}; // 0xF300-0xF303
  unsigned char  sprite_bg_flag = 0;            // 0xF400

  // Palette 512 RGB565 (caricata da SD una volta in init/reset)
  unsigned short palette_rgb565[512] = {0};

  // Scratch full-frame 256x224 RGB565 (112 KB) per rendering pre-rotato
  unsigned short *frame_scratch = nullptr;

  uint32_t frame_counter = 0;

  // MCU 8042 stub (port da MAME arknoid2_state). Senza questo il main
  // resta bloccato in attesa che l'MCU risponda al protocollo handshake.
  int            mcu_initializing = 3;
  int            mcu_command = 0;
  unsigned char  mcu_credits = 0;
  int            mcu_readcredits = 0;
  unsigned char  mcu_reportcoin = 0;
  unsigned char  mcu_coinage[4] = {1, 1, 1, 1};
  int            mcu_coinage_init = 0;
  unsigned char  mcu_coins_a = 0;
  unsigned char  mcu_coins_b = 0;
  unsigned char  mcu_insertcoin = 0;

  void mcu_reset_state();
  void mcu_handle_coins(int coin);

  // YM2203 register address latch (sub 0xB000)
  unsigned char  ym2203_addr = 0;

  // Paddle uPD4701 (12-bit signed delta accumulato).
  // Aggiornato in prepare_frame() da EC11 + joystick LEFT/RIGHT.
  int paddle_pos = 0;

public:
  // FM chip (sorgente SH: ym2203_state*). P4 multi NON usa fm.c -> nullptr.
  // audio.cpp ha "if (a2->ym2203_chip)" quindi salta il render FM, solo PSG.
  void *ym2203_chip = nullptr;

  // YM2203 FM register file + key-on tracking (per audio.cpp FM render).
  unsigned char ym_regs[1][256] = {{0}};
  unsigned char fm_keyed[1][3]  = {{0, 0, 0}};

  // YM2203 timer state (regs 0x24-0x27 + status 0xB000 bit 0/1).
  // Necessario per la corretta velocità BGM: in MAME il timer scatta a
  // tempo preciso e genera IRQ al sub Z80 → suona la nota successiva.
  int  timer_a_period = 0;    // 10-bit (regs 0x24/0x25)
  int  timer_b_period = 0;    // 8-bit (reg 0x26)
  int  timer_a_count  = 0;
  int  timer_b_count  = 0;
  bool timer_a_started = false;
  bool timer_a_enabled = false;
  bool timer_b_started = false;
  bool timer_b_enabled = false;
  bool timer_a_overflow = false;
  bool timer_b_overflow = false;
};

#endif // ENABLE_ARKANOID2
#endif // ARKANOID2_H
