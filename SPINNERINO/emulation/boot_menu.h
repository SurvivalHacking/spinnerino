#ifndef BOOT_MENU_H
#define BOOT_MENU_H

#include <Arduino.h>
#include "input.h"
#include "../machines/machineBase.h"

// ============================================================================
// BootMenu — carousel orizzontale per selezione gioco a boot.
//
// Layout (320x232 utili, ROT270 portrait fisica):
//   y=  0..15   "ARCADE COLLECTION" (titolo, font 8x8)
//   y= 32..127  area logo 224x96 (centrato x=48..271, scroll horizontal)
//   y=144..151  nome gioco (font 8x8)
//   y=168..175  pagination "<  N / TOT  >"
//   y=200..207  hint "SPIN: change   FIRE: start"
//
// Input: EC11 ruota = scroll giochi (loop infinito), FIRE = avvia.
// Animazione scroll fluida (~10 frame), durante l'animazione l'input e'
// ignorato.
// ============================================================================
class BootMenu {
public:
  void init(Input *input, machineBase **machines, int count,
            unsigned short *out_strip, int start_idx = 0);
  void update();        // gestisce input + tick animazione (chiamare ogni frame)
  void render();        // disegna a video (chiamare ogni frame)
  bool finished() const { return _finished; }
  int  selectedIndex() const { return _current_idx; }
  void attract_resetTimer() { _attract_t0 = millis(); _attract_armed = true; }

private:
  Input         *_input;
  machineBase  **_machines;
  int            _count;
  unsigned short *_out_strip;     // buffer 320x8 condiviso per write strip

  int  _current_idx;              // indice gioco al centro
  int  _next_idx;                 // indice durante transizione
  int  _anim_frame;               // 0 = idle, 1..N = in transizione
  int  _anim_dir;                 // -1 = scroll a sinistra, +1 = a destra
  bool _finished;                 // true quando l'utente ha premuto FIRE

  unsigned char _last_buttons;    // edge detection FIRE
  unsigned long _attract_t0;      // master attract menu timeout origin
  bool          _attract_armed;   // true = attract pending, false = disabled

  // Render helpers
  void draw_strip(int strip_y);   // riempie _out_strip con la strip globale y
  void draw_text(int strip_y, int x, int y_in_strip,
                 const char *text, unsigned short color);
  void draw_logo(int strip_y, int x_offset, const unsigned short *logo);
  void draw_settings_card(int strip_y, int x_offset);  // "card" voce IMPOSTAZIONI
  const char *machine_name(int idx);
};

// ============================================================================
// SettingsMenu — schermata IMPOSTAZIONI: lista di tutti i giochi con slider
// per la sensibilita' EC11 (livello 1..10, 5 = fabbrica). Si apre dalla voce
// "IMPOSTAZIONI" del carosello BootMenu.
//
// Controlli:
//   SPIN (EC11)  : in navigazione scorre i giochi; in modifica cambia il livello
//   FIRE         : entra/esce dalla modalita' modifica del gioco selezionato
//   START        : salva ed esce (torna al menu giochi)
//
// Il livello vive in machines[i]->ec11SensLevel (applicato live); il salvataggio
// su NVS lo fa il chiamante (.ino) quando finished() diventa true.
// ============================================================================
class SettingsMenu {
public:
  void init(Input *input, machineBase **machines, int count,
            unsigned short *out_strip);
  void update();
  void render();
  bool finished() const { return _finished; }

private:
  Input         *_input;
  machineBase  **_machines;
  int            _count;
  unsigned short *_out_strip;

  int  _sel;            // riga/gioco selezionato (0.._count-1)
  bool _edit;           // false = naviga lista, true = modifica livello
  int  _scroll;         // prima riga visibile (per liste lunghe)
  bool _finished;       // true = salva ed esci
  unsigned char _last_buttons;

  void draw_strip(int strip_y);
};

#endif // BOOT_MENU_H
