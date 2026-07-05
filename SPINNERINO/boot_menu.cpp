// ============================================================================
// boot_menu.cpp — BootMenu carousel con ROTAZIONE SOFTWARE (90 antiorario)
//
// Il display lavora in MAC=0x60 landscape (320x232) ma l'utente lo vede in
// portrait fisico. Il menu e' progettato in COORDINATE LOGICHE PORTRAIT
// 240x320 e ogni pixel viene mappato in landscape via:
//
//   x_landscape = y_portrait
//   y_landscape = 239 - x_portrait        (visibili: y_l in [0..231])
//
// Cosi' l'utente vede il menu DIRITTO sul display portrait fisico.
// Lo scroll orizzontale (x_p change) appare orizzontale anche all'utente.
//
// Layout PORTRAIT 240x320 (area utile: x_p in [8..239] = 232 visibili):
//   y_p=  20.. 27   "ARCADE COLLECTION" (font 8x8, ambra)
//   y_p=  60..155   logo 224x96 centrato (x_p = 8..231)
//   y_p= 175..182   nome gioco (bianco)
//   y_p= 200..207   pagination "<  N / TOT  >" (ciano)
//   y_p= 290..297   hint "SPIN: CHANGE   FIRE: START" (ciano)
// ============================================================================
#include "emulation/boot_menu.h"
#include "emulation/video.h"
#include "config.h"
#include "machines/arkanoid/arkanoid_logo.h"

extern Video video;

// Coordinate logiche portrait
#define MENU_W   240          // larghezza canvas portrait
#define MENU_H   320          // altezza canvas portrait
#define LOGO_W   224
#define LOGO_H   96
#define LOGO_X     8          // partenza x_p del logo (centrato in area utile)
#define LOGO_Y    60          // partenza y_p del logo
#define ANIM_FRAMES   10

#define Y_TITLE   20
#define Y_NAME   175
#define Y_PAGIN  200
#define Y_HINT   290

// Mappa coords portrait -> coords landscape strip-relative.
// Ritorna true se il pixel cade nella strip landscape corrente.
// strip[] e' il buffer 320x8 landscape (TFT_FB_W * 8).
static inline void put_pixel_p(unsigned short *strip, int strip_y_l,
                               int x_p, int y_p, unsigned short color) {
  int x_l = y_p;
  int y_l = 239 - x_p;
  if (x_l < 0 || x_l >= TFT_FB_W) return;
  if (y_l < 0 || y_l >= TFT_FB_H) return;     // fuori area visibile
  int y_in = y_l - strip_y_l;
  if (y_in < 0 || y_in >= 8) return;          // non in questa strip
  strip[y_in * TFT_FB_W + x_l] = color;
}

// ----------------------------------------------------------------------------
// Font 8x8 minimale
// ----------------------------------------------------------------------------
static const unsigned char FONT8[][8] PROGMEM = {
  {0x18,0x3C,0x66,0x66,0x7E,0x66,0x66,0x00}, // A
  {0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0x00}, // B
  {0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0x00}, // C
  {0x78,0x6C,0x66,0x66,0x66,0x6C,0x78,0x00}, // D
  {0x7E,0x60,0x60,0x78,0x60,0x60,0x7E,0x00}, // E
  {0x7E,0x60,0x60,0x78,0x60,0x60,0x60,0x00}, // F
  {0x3C,0x66,0x60,0x6E,0x66,0x66,0x3C,0x00}, // G
  {0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x00}, // H
  {0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, // I
  {0x1E,0x0C,0x0C,0x0C,0x0C,0x6C,0x38,0x00}, // J
  {0x66,0x6C,0x78,0x70,0x78,0x6C,0x66,0x00}, // K
  {0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0x00}, // L
  {0x63,0x77,0x7F,0x6B,0x63,0x63,0x63,0x00}, // M
  {0x66,0x76,0x7E,0x7E,0x6E,0x66,0x66,0x00}, // N
  {0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00}, // O
  {0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0x00}, // P
  {0x3C,0x66,0x66,0x66,0x66,0x3C,0x0E,0x00}, // Q
  {0x7C,0x66,0x66,0x7C,0x78,0x6C,0x66,0x00}, // R
  {0x3C,0x66,0x60,0x3C,0x06,0x66,0x3C,0x00}, // S
  {0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x00}, // T
  {0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00}, // U
  {0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0x00}, // V
  {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00}, // W
  {0x66,0x66,0x3C,0x18,0x3C,0x66,0x66,0x00}, // X
  {0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0x00}, // Y
  {0x7E,0x06,0x0C,0x18,0x30,0x60,0x7E,0x00}, // Z
  {0x3C,0x66,0x6E,0x76,0x66,0x66,0x3C,0x00}, // 0
  {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00}, // 1
  {0x3C,0x66,0x06,0x1C,0x30,0x60,0x7E,0x00}, // 2
  {0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0x00}, // 3
  {0x0C,0x1C,0x3C,0x6C,0x7E,0x0C,0x0C,0x00}, // 4
  {0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00}, // 5
  {0x3C,0x66,0x60,0x7C,0x66,0x66,0x3C,0x00}, // 6
  {0x7E,0x06,0x0C,0x18,0x18,0x18,0x18,0x00}, // 7
  {0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00}, // 8
  {0x3C,0x66,0x66,0x3E,0x06,0x66,0x3C,0x00}, // 9
  {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // ' '
  {0x03,0x06,0x0C,0x18,0x30,0x60,0xC0,0x00}, // '/'
  {0x00,0x00,0x18,0x00,0x00,0x18,0x00,0x00}, // ':'
  {0x00,0x0C,0x18,0x30,0x18,0x0C,0x00,0x00}, // '<'
  {0x00,0x30,0x18,0x0C,0x18,0x30,0x00,0x00}, // '>'
  {0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x00}, // '!'
  {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00}, // '.'
  {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00}, // '-'
  {0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0x00}, // '_'
};

static int char_to_idx(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a';
  if (c >= '0' && c <= '9') return 26 + (c - '0');
  switch (c) {
    case ' ': return 36;
    case '/': return 37;
    case ':': return 38;
    case '<': return 39;
    case '>': return 40;
    case '!': return 41;
    case '.': return 42;
    case '-': return 43;
    case '_': return 44;
  }
  return 36;
}

static int text_width(const char *s) { return (int)strlen(s) * 8; }

// Disegna un char 8x8 a coords portrait (x_p, y_p) con rotazione software.
static void blit_char_rot(unsigned short *strip, int strip_y_l,
                          int x_p, int y_p, char ch, unsigned short color) {
  int idx = char_to_idx(ch);
  for (int r = 0; r < 8; r++) {
    unsigned char row = pgm_read_byte(&FONT8[idx][r]);
    int y_p_pixel = y_p + r;
    int x_l_for_this_row = y_p_pixel;
    if (x_l_for_this_row < 0 || x_l_for_this_row >= TFT_FB_W) continue;
    for (int c = 0; c < 8; c++) {
      if (row & (0x80 >> c)) {
        int x_p_pixel = x_p + c;
        int y_l = 239 - x_p_pixel;
        if (y_l < 0 || y_l >= TFT_FB_H) continue;
        int y_in = y_l - strip_y_l;
        if (y_in < 0 || y_in >= 8) continue;
        strip[y_in * TFT_FB_W + x_l_for_this_row] = color;
      }
    }
  }
}

// ----------------------------------------------------------------------------
// BootMenu
// ----------------------------------------------------------------------------
void BootMenu::init(Input *input, machineBase **machines, int count,
                    unsigned short *out_strip, int start_idx) {
  _input        = input;
  _machines     = machines;
  _count        = count;
  _out_strip    = out_strip;
  if (count > 0) {
    _current_idx = ((start_idx % count) + count) % count;
  } else {
    _current_idx = 0;
  }
  _next_idx     = _current_idx;
  _anim_frame   = 0;
  _anim_dir     = 0;
  _finished     = false;
  _last_buttons = 0xFF;
  _attract_t0   = millis();
  _attract_armed = true;
}

void BootMenu::update() {
  if (_finished || _count <= 0) return;
  unsigned char b = _input->buttons_get();

  if (_anim_frame > 0) {
    _anim_frame++;
    if (_anim_frame > ANIM_FRAMES) {
      _current_idx = _next_idx;
      _anim_frame  = 0;
      _anim_dir    = 0;
    }
    _last_buttons = b;
    return;
  }

  if ((b & BUTTON_FIRE) && !(_last_buttons & BUTTON_FIRE)) {
    _finished = true;
    _attract_armed = false;
    _last_buttons = b;
    return;
  }
  // Slot totali = giochi + 1 voce IMPOSTAZIONI (indice == _count).
  int slots = _count + 1;
  if ((b & BUTTON_RIGHT) && !(_last_buttons & BUTTON_RIGHT)) {
    _anim_dir   = -1;
    _next_idx   = (_current_idx + 1) % slots;
    _anim_frame = 1;
    _attract_t0 = millis();        // user input → reset attract timer
  } else if ((b & BUTTON_LEFT) && !(_last_buttons & BUTTON_LEFT)) {
    _anim_dir   = +1;
    _next_idx   = (_current_idx - 1 + slots) % slots;
    _anim_frame = 1;
    _attract_t0 = millis();
  }
  _last_buttons = b;

#ifdef MASTER_ATTRACT_MENU_TIMEOUT
  // Master attract: dopo X ms di idle nel menu, autostart del gioco corrente.
  // Stesso pattern di FULL33/menu.cpp (Menu::handle()). NON su voce IMPOSTAZIONI.
  if (_attract_armed && _current_idx < _count &&
      (millis() - _attract_t0) > MASTER_ATTRACT_MENU_TIMEOUT) {
    _finished = true;
    _attract_armed = false;
  }
#endif
}

// Nome gioco per machineType (condiviso BootMenu + SettingsMenu).
static const char *name_for_type(signed char t) {
  switch (t) {
#ifdef ENABLE_ARKANOID
    case MCH_ARKANOID: return "ARKANOID";
#endif
#ifdef ENABLE_ARKANOID2
    case MCH_ARKANOID2: return "ARKANOID 2 WIP";
#endif
#ifdef ENABLE_GIGAS
    case MCH_GIGAS:     return "GIGAS";
#endif
#ifdef ENABLE_GIGAS2
    case MCH_GIGAS2:    return "GIGAS MARK II";
#endif
#ifdef ENABLE_GOINDOL
    case MCH_GOINDOL:   return "GOINDOL";
#endif
#ifdef ENABLE_SBRKOUT
    case MCH_SBRKOUT:   return "SUPER BREAKOUT";
#endif
#ifdef ENABLE_BOMBBEE
    case MCH_BOMBBEE:   return "BOMB BEE";
#endif
#ifdef ENABLE_SPACE
    case MCH_SPACE:    return "SPACE INVADERS";
#endif
#ifdef ENABLE_GALAGA
    case MCH_GALAGA:   return "GALAGA";
#endif
#ifdef ENABLE_MOTORACE
    case MCH_MOTORACE: return "MOTORACE USA";
#endif
#ifdef ENABLE_PHOENIX
    case MCH_PHOENIX:  return "PHOENIX";
#endif
#ifdef ENABLE_GALAXIAN
    case MCH_GALAXIAN: return "GALAXIAN";
#endif
#ifdef ENABLE_GYRUSS
    case MCH_GYRUSS:   return "GYRUSS";
#endif
#ifdef ENABLE_ROADFIGHTER
    case MCH_ROADFIGHTER: return "ROAD FIGHTER WIP";
#endif
#ifdef ENABLE_PANG
    case MCH_PANG:     return "PANG";
#endif
    default:           return "GAME";
  }
}

const char *BootMenu::machine_name(int idx) {
  if (!_machines || idx < 0) return "?";
  if (idx == _count) return "IMPOSTAZIONI";        // voce extra (settings)
  if (idx > _count)  return "?";
  return name_for_type(_machines[idx]->machineType());
}

// Wrapper compatibili con l'interfaccia originale (non piu' usate per il
// rendering portrait, le ridichiariamo per soddisfare il .h).
void BootMenu::draw_text(int strip_y_global, int x, int y_global,
                         const char *text, unsigned short color) {
  for (int i = 0; text[i]; i++) {
    blit_char_rot(_out_strip, strip_y_global, x + i * 8, y_global, text[i], color);
  }
}

// Disegna porzione del logo 224x96 con rotazione software.
// x_offset = x_p del logo nel canvas portrait (puo' essere negativo per slide).
void BootMenu::draw_logo(int strip_y_global, int x_offset,
                         const unsigned short *logo) {
  // Determina quali colonne del canvas portrait cadono nella strip landscape.
  // Ogni col portrait x_p diventa row landscape y_l = 239 - x_p.
  // Ci interessano x_p tali che y_l in [strip_y..strip_y+8).
  // => 239 - x_p in [strip_y..strip_y+8)  =>  x_p in (231-strip_y..239-strip_y].
  int x_p_min = 232 - strip_y_global;             // inclusivo
  int x_p_max = 239 - strip_y_global;             // inclusivo
  if (x_p_min < 0)        x_p_min = 0;
  if (x_p_max >= MENU_W)  x_p_max = MENU_W - 1;

  // Range del logo in coords portrait
  int logo_x_min = x_offset;
  int logo_x_max = x_offset + LOGO_W - 1;

  for (int x_p = x_p_min; x_p <= x_p_max; x_p++) {
    if (x_p < logo_x_min || x_p > logo_x_max) continue;
    int logo_col = x_p - x_offset;                // 0..223
    int y_l = 239 - x_p;
    int y_in = y_l - strip_y_global;
    if (y_in < 0 || y_in >= 8) continue;
    unsigned short *dst_row = _out_strip + y_in * TFT_FB_W;
    // Per ogni y_p della colonna: y_p in [LOGO_Y..LOGO_Y+LOGO_H)
    // x_l = y_p
    for (int row = 0; row < LOGO_H; row++) {
      int y_p_pixel = LOGO_Y + row;
      int x_l = y_p_pixel;
      if (x_l < 0 || x_l >= TFT_FB_W) continue;
      // Pixel sorgente: logo[row * LOGO_W + logo_col]
      dst_row[x_l] = logo[row * LOGO_W + logo_col];
    }
  }
}

void BootMenu::draw_strip(int strip_y_global) {
  // Sfondo nero
  memset(_out_strip, 0, TFT_FB_W * 8 * sizeof(unsigned short));

  // Posizione X portrait del logo (per scroll orizzontale durante animazione)
  int x_current = LOGO_X;
  int x_next    = 0;
  bool anim_active = (_anim_frame > 0);
  if (anim_active) {
    int dx = (MENU_W * _anim_frame) / ANIM_FRAMES;
    x_current = LOGO_X + _anim_dir * dx;
    x_next    = x_current - _anim_dir * MENU_W;
  }

  // Entry corrente: logo del gioco, oppure card "IMPOSTAZIONI" (idx == _count).
  if (_current_idx < _count) {
    const unsigned short *logo_cur = _machines[_current_idx]->logo();
    if (logo_cur) draw_logo(strip_y_global, x_current, logo_cur);
  } else {
    draw_settings_card(strip_y_global, x_current);
  }
  if (anim_active) {
    if (_next_idx < _count) {
      const unsigned short *logo_nxt = _machines[_next_idx]->logo();
      if (logo_nxt) draw_logo(strip_y_global, x_next, logo_nxt);
    } else {
      draw_settings_card(strip_y_global, x_next);
    }
  }

  unsigned short C_WHITE = 0xFFFF;
  unsigned short C_CYAN  = 0xFF07;
  unsigned short C_AMBER = 0x60FE;

  // Title centrato in area utile portrait [8..239] (= 232 px utili)
  const char *title = "ARCADE COLLECTION";
  int title_x = 8 + (232 - text_width(title)) / 2;
  draw_text(strip_y_global, title_x, Y_TITLE, title, C_AMBER);

  if (!anim_active) {
    const char *name = machine_name(_current_idx);
    int name_x = 8 + (232 - text_width(name)) / 2;
    draw_text(strip_y_global, name_x, Y_NAME, name, C_WHITE);

    char pag[16];
    snprintf(pag, sizeof(pag), "<  %d / %d  >", _current_idx + 1, _count + 1);
    int pag_x = 8 + (232 - text_width(pag)) / 2;
    draw_text(strip_y_global, pag_x, Y_PAGIN, pag, C_CYAN);
  }

  const char *hint = "SPIN: CHANGE   FIRE: START";
  int hint_x = 8 + (232 - text_width(hint)) / 2;
  draw_text(strip_y_global, hint_x, Y_HINT, hint, C_CYAN);
}

// Render: strip landscape 320x8, 29 strip per coprire 232 row (TFT_FB_H).
void BootMenu::render() {
  int n_strips = TFT_FB_H / 8;
  for (int s = 0; s < n_strips; s++) {
    draw_strip(s * 8);
    video.write(_out_strip, TFT_FB_W * 8);
  }
}

// Card della voce "IMPOSTAZIONI" nel carosello: cornice + testo nella logo-box,
// scorre con x_offset come un logo durante l'animazione.
void BootMenu::draw_settings_card(int strip_y_global, int x_offset) {
  unsigned short C_AMBER = 0x60FE;
  unsigned short C_WHITE = 0xFFFF;

  int x0 = x_offset + 12;
  int x1 = x_offset + LOGO_W - 13;
  int y0 = LOGO_Y + 6;
  int y1 = LOGO_Y + LOGO_H - 7;
  for (int x_p = x0; x_p <= x1; x_p++) {
    put_pixel_p(_out_strip, strip_y_global, x_p, y0, C_AMBER);
    put_pixel_p(_out_strip, strip_y_global, x_p, y1, C_AMBER);
  }
  for (int y_p = y0; y_p <= y1; y_p++) {
    put_pixel_p(_out_strip, strip_y_global, x0, y_p, C_AMBER);
    put_pixel_p(_out_strip, strip_y_global, x1, y_p, C_AMBER);
  }

  const char *l1 = "IMPOSTAZIONI";
  const char *l2 = "SENSIBILITA";
  const char *l3 = "SPINNER";
  int l1x = x_offset + (LOGO_W - text_width(l1)) / 2;
  int l2x = x_offset + (LOGO_W - text_width(l2)) / 2;
  int l3x = x_offset + (LOGO_W - text_width(l3)) / 2;
  for (int i = 0; l1[i]; i++)
    blit_char_rot(_out_strip, strip_y_global, l1x + i * 8, LOGO_Y + 24, l1[i], C_WHITE);
  for (int i = 0; l2[i]; i++)
    blit_char_rot(_out_strip, strip_y_global, l2x + i * 8, LOGO_Y + 42, l2[i], C_AMBER);
  for (int i = 0; l3[i]; i++)
    blit_char_rot(_out_strip, strip_y_global, l3x + i * 8, LOGO_Y + 60, l3[i], C_AMBER);
}

// ============================================================================
// SettingsMenu — lista giochi + slider sensibilita' EC11 (livello 1..10).
// ============================================================================
void SettingsMenu::init(Input *input, machineBase **machines, int count,
                        unsigned short *out_strip) {
  _input     = input;
  _machines  = machines;
  _count     = count;
  _out_strip = out_strip;
  _sel       = 0;
  _edit      = false;
  _scroll    = 0;
  _finished  = false;
  _last_buttons = 0xFF;
}

void SettingsMenu::update() {
  if (_finished || _count <= 0) return;
  unsigned char b = _input->buttons_get();

  // START = salva ed esci (gestito dal chiamante quando finished()).
  if ((b & BUTTON_START) && !(_last_buttons & BUTTON_START)) {
    _finished = true;
    _last_buttons = b;
    return;
  }

  // FIRE = entra/esce dalla modifica del gioco selezionato.
  if ((b & BUTTON_FIRE) && !(_last_buttons & BUTTON_FIRE)) {
    _edit = !_edit;
  }

  // SPIN: in modifica cambia il livello, altrimenti scorre la lista.
  if ((b & BUTTON_RIGHT) && !(_last_buttons & BUTTON_RIGHT)) {
    if (_edit) {
      if (_machines[_sel]->ec11SensLevel < 10) _machines[_sel]->ec11SensLevel++;
    } else {
      _sel = (_sel + 1) % _count;
    }
  } else if ((b & BUTTON_LEFT) && !(_last_buttons & BUTTON_LEFT)) {
    if (_edit) {
      if (_machines[_sel]->ec11SensLevel > 1) _machines[_sel]->ec11SensLevel--;
    } else {
      _sel = (_sel - 1 + _count) % _count;
    }
  }

  // Scroll per tenere la riga selezionata visibile.
  const int VISIBLE = 11;
  if (_sel < _scroll)             _scroll = _sel;
  if (_sel >= _scroll + VISIBLE)  _scroll = _sel - VISIBLE + 1;

  _last_buttons = b;
}

void SettingsMenu::draw_strip(int strip_y) {
  memset(_out_strip, 0, TFT_FB_W * 8 * sizeof(unsigned short));

  unsigned short C_WHITE = 0xFFFF, C_CYAN = 0xFF07, C_AMBER = 0x60FE,
                 C_GREEN = 0xE007, C_GREY = 0x0842;

  const char *title = "SENSIBILITA SPINNER";
  int tx = 8 + (232 - text_width(title)) / 2;
  for (int i = 0; title[i]; i++)
    blit_char_rot(_out_strip, strip_y, tx + i * 8, 12, title[i], C_AMBER);

  const int ROW0 = 38, ROW_H = 22, VISIBLE = 11;
  const int CURSOR_X = 8, NAME_X = 20, BAR_X = 120, BAR_CELLS = 10, CELL_W = 8;
  const int NUM_X = BAR_X + BAR_CELLS * CELL_W + 6;   // 206 (entro x_p<=239)

  for (int r = 0; r < VISIBLE; r++) {
    int gi = _scroll + r;
    if (gi >= _count) break;
    int y_p = ROW0 + r * ROW_H;
    bool selected = (gi == _sel);
    bool editing  = selected && _edit;
    int  level    = _machines[gi]->ec11SensLevel;
    unsigned short name_col = selected ? C_WHITE : C_CYAN;

    if (selected)
      blit_char_rot(_out_strip, strip_y, CURSOR_X, y_p, '>', C_AMBER);

    char nm[13];
    const char *full = name_for_type(_machines[gi]->machineType());
    int n = 0; for (; full[n] && n < 12; n++) nm[n] = full[n]; nm[n] = 0;
    for (int i = 0; nm[i]; i++)
      blit_char_rot(_out_strip, strip_y, NAME_X + i * 8, y_p, nm[i], name_col);

    for (int c = 0; c < BAR_CELLS; c++) {
      bool fill = (c < level);
      unsigned short cell_col = fill ? (editing ? C_AMBER : C_GREEN) : C_GREY;
      int cx = BAR_X + c * CELL_W;
      for (int yy = 0; yy < 7; yy++)
        for (int xx = 0; xx < CELL_W - 1; xx++)
          put_pixel_p(_out_strip, strip_y, cx + xx, y_p + yy, cell_col);
    }

    char num[4]; snprintf(num, sizeof(num), "%d", level);
    unsigned short num_col = editing ? C_AMBER : name_col;
    for (int i = 0; num[i]; i++)
      blit_char_rot(_out_strip, strip_y, NUM_X + i * 8, y_p, num[i], num_col);
  }

  const char *hint = _edit ? "SPIN: VALORE   FIRE: OK"
                           : "SPIN: SCEGLI   FIRE: MODIFICA";
  int hx = 8 + (232 - text_width(hint)) / 2;
  for (int i = 0; hint[i]; i++)
    blit_char_rot(_out_strip, strip_y, hx + i * 8, 290, hint[i], C_CYAN);
  const char *hint2 = "START: SALVA ED ESCI";
  int h2x = 8 + (232 - text_width(hint2)) / 2;
  for (int i = 0; hint2[i]; i++)
    blit_char_rot(_out_strip, strip_y, h2x + i * 8, 306, hint2[i], C_CYAN);
}

void SettingsMenu::render() {
  int n_strips = TFT_FB_H / 8;
  for (int s = 0; s < n_strips; s++) {
    draw_strip(s * 8);
    video.write(_out_strip, TFT_FB_W * 8);
  }
}
