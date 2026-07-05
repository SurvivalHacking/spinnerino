// ============================================================================
// SPINNERINO P4 - machines/spaceinvaders/spaceinvaders.cpp
//
// Space Invaders (Midway 1978)
// CPU: Intel 8080 @ 2 MHz emulato su core Z80 (8080 e' subset di Z80)
// Schermo arcade portrait 224w x 256h, framebuffer 1bpp 7KB a 0x2400-0x3FFF
//
// Memory map:
//   0x0000-0x1FFF  ROM (8 KB = 4 x 2 KB)
//   0x2000-0x23FF  RAM lavoro (1 KB)
//   0x2400-0x3FFF  VRAM (7 KB, 1bpp 256x224 originale)
//   0x4000-0x5FFF  RAM mirror
//
// I/O ports:
//   IN  1: COIN, P1 START, P1 FIRE/LEFT/RIGHT
//   IN  2: DIP, P2 controls, TILT
//   IN  3: shift register result
//   OUT 2: shift amount
//   OUT 3: sound effects 1
//   OUT 4: shift data
//   OUT 5: sound effects 2
//   OUT 6: watchdog
//
// RENDERING NOTE (DIVERSO DA Galagino V3 originale):
// SPINNERINO P4 ha framebuffer landscape 256w x 224h (28 strip da 8 px),
// mentre Space Invaders e' arcade portrait 224w x 256h. La rotazione
// hardware MV del display ST7789 ruota poi il landscape framebuffer di 90°
// per l'utente (vedi boot_menu.cpp). Per restituire arcade portrait corretto
// rendo "trasposto" rispetto alla pipeline Galagino:
//   - frame_buffer X (0..255) <-> arcade_y (verticale arcade)
//   - frame_buffer Y (0..223) <-> arcade_x (orizzontale arcade), reversed
// ============================================================================
#include "spaceinvaders.h"

#ifdef ENABLE_SPACE

// RAM mapping nel buffer condiviso `memory[]`:
//   memory[0x0000..0x1FFF] copre indirizzi 0x2000-0x3FFF (RAM + VRAM)
//   memory[0x0400] = VRAM start (indirizzo 0x2400)
#define RAM_OFFSET   0x2000
#define VRAM_OFFSET  0x0400

// Color overlay (RGB565 byte-swapped per ESP32 SPI BE)
#define COL_WHITE  0xFFFF
#define COL_GREEN  0xE007
#define COL_RED    0x00F8

// 8080 @ 2 MHz, 60 fps -> ~33333 T-states/frame, split in 2 half-frame
// con interrupt RST 08 (mid-screen) e RST 10 (vblank)
#define INST_PER_HALFFRAME  2083

void SpaceInvaders::reset() {
  machineBase::reset();
  shift_data = 0;
  shift_amount = 0;
}

unsigned char SpaceInvaders::opZ80(unsigned short Addr) {
  Addr &= 0x3FFF;  // mirror 0x4000+ -> 0x0000+
  if (Addr < 0x2000)
    return pgm_read_byte(&spaceinvaders_rom[Addr]);
  return memory[Addr - RAM_OFFSET];
}

unsigned char SpaceInvaders::rdZ80(unsigned short Addr) {
  Addr &= 0x3FFF;
  if (Addr < 0x2000)
    return pgm_read_byte(&spaceinvaders_rom[Addr]);
  return memory[Addr - RAM_OFFSET];
}

void SpaceInvaders::wrZ80(unsigned short Addr, unsigned char Value) {
  Addr &= 0x3FFF;
  if (Addr < 0x2000) return;  // ROM area
  memory[Addr - RAM_OFFSET] = Value;
  if (!game_started && Addr >= 0x2400 && Addr < 0x4000) {
    if (Value != 0) game_started = 1;
  }
}

unsigned char SpaceInvaders::inZ80(unsigned short Port) {
  Port &= 0xFF;
  switch (Port) {
    case 0:
      return 0x0E;  // unused, MAME compat
    case 1: {
      // P1 inputs: bit0 COIN(active high), bit1 P2 START, bit2 P1 START,
      //            bit3=1, bit4 FIRE, bit5 LEFT, bit6 RIGHT
      unsigned char keymask = input->buttons_get();
      unsigned char retval = 0x08;
      if (keymask & BUTTON_COIN) {
        retval |= 0x01;
        if (!last_coin) soundregs[2] = 1;  // trigger coin sound (rising edge)
        last_coin = 1;
      } else {
        last_coin = 0;
      }
      if (keymask & BUTTON_EXTRA) retval |= 0x02;
      if (keymask & BUTTON_START) retval |= 0x04;
      if (keymask & BUTTON_FIRE)  retval |= 0x10;
      if (keymask & BUTTON_LEFT)  retval |= 0x20;
      if (keymask & BUTTON_RIGHT) retval |= 0x40;
      return retval;
    }
    case 2: {
      // DIP + P2 controls
      unsigned char retval = SINV_DIP;
      unsigned char keymask = input->buttons_get();
      if (keymask & BUTTON_FIRE)  retval |= 0x10;
      if (keymask & BUTTON_LEFT)  retval |= 0x20;
      if (keymask & BUTTON_RIGHT) retval |= 0x40;
      return retval;
    }
    case 3:
      return (uint8_t)((shift_data >> (8 - shift_amount)) & 0xFF);
  }
  return 0x00;
}

void SpaceInvaders::outZ80(unsigned short Port, unsigned char Value) {
  Port &= 0xFF;
  switch (Port) {
    case 2:
      shift_amount = Value & 0x07;
      break;
    case 3:
      // Sound effects 1: UFO/Shot/Flash/InvaderDie/ExtPlay
      soundregs[0] = Value;
      break;
    case 4:
      shift_data = (shift_data >> 8) | ((uint16_t)Value << 8);
      break;
    case 5:
      // Sound effects 2: Fleet1/2/3/4 + UFO hit
      soundregs[1] = Value;
      break;
    case 6:
      break; // watchdog
  }
}

void SpaceInvaders::run_frame(void) {
  current_cpu = 0;
  for (int i = 0; i < INST_PER_HALFFRAME; i++) StepZ80(&cpu[0]);
  IntZ80(&cpu[0], INT_RST08);
  for (int i = 0; i < INST_PER_HALFFRAME; i++) StepZ80(&cpu[0]);
  IntZ80(&cpu[0], INT_RST10);
}

void SpaceInvaders::prepare_frame(void) {
  active_sprites = 0;  // SI ha solo bitmap, niente sprite HW
}

// Color overlay bands sul player view (verticale arcade)
unsigned short SpaceInvaders::get_pixel_color(int y) {
  if (y < 32)  return COL_WHITE;
  if (y < 64)  return COL_RED;
  if (y < 184) return COL_WHITE;
  if (y < 240) return COL_GREEN;
  return COL_WHITE;
}

// Render trasposto vs Galagino V3 originale per allineare al framebuffer
// landscape SPINNERINO. Il display MV-rotated mostra all'utente arcade
// portrait nativo (224w x 256h).
//
// frame_buffer[sr * 256 + fx]:
//   sr ∈ [0..7]   -> y_fb = row*8+sr ∈ [0..223]  -> arcade_x = 223 - y_fb
//   fx ∈ [0..255]                                 -> arcade_y = fx
//
// VRAM: byte per riga arcade (32 byte * 224 righe), bit0 = top pixel.
//   y_orig = arcade_x;   x_orig = 255 - arcade_y
//   byte = vram[y_orig * 32 + x_orig / 8]
//   bit  = x_orig & 7
void SpaceInvaders::render_row(short row) {
  if (row < 0 || row >= 28) return;

  unsigned char *vram = memory + VRAM_OFFSET;

  for (int sr = 0; sr < 8; sr++) {
    int y_fb = row * 8 + sr;
    int player_x = 223 - y_fb;
    if (player_x < 0 || player_x >= 224) continue;

    unsigned char *vram_row = vram + player_x * 32;
    unsigned short *dst = frame_buffer + sr * 256;

    for (int fx = 0; fx < 256; fx++) {
      int player_y = fx;
      int x_orig = 255 - player_y;
      int byte_col = x_orig >> 3;
      int bit_pos  = x_orig & 7;
      if (vram_row[byte_col] & (1 << bit_pos)) {
        dst[fx] = get_pixel_color(player_y);
      }
    }
  }
}

const unsigned short *SpaceInvaders::logo(void) {
  return spaceinvaders_logo;
}

#endif // ENABLE_SPACE
