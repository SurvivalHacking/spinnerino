#include "emulation/input.h"
#include "machines/machineBase.h"
#include <Preferences.h>

// Per leggere ec11PulseHoldMs() dalla machine attiva
extern machineBase *currentMachine;

static Preferences prefs;

// Core che possiede la macchina a stati input (timing FIRE, vol mode, sequenza
// COIN/START, hold back-to-menu). Catturato in Input::init() = core del main
// loop Arduino. Le chiamate a buttons_get() dal core di EMULAZIONE restituiscono
// solo lo snapshot gia' calcolato, senza rieseguire la FSM: cosi' il micro-switch
// EC11 (FIRE) e' campionato a ~60 Hz dal solo main loop e non ad alta frequenza
// dal core emulazione (che catturava i glitch meccanici del pulsante, rompendo
// l'attivazione/uscita della regolazione volume su Arkanoid 2, Gyruss, ecc.).
static volatile int s_fsm_core = -1;

// --- Stato paddle (esposto a machines/arkanoid/arkanoid.cpp) ---
// IMPORTANTE: arkangc check boot a $042D vuole D018=0 al primo read,
// altrimenti stampa "BAD HARDWARE". MAME inizia IPT_DIAL a 0 e poi accumula.
volatile int16_t ec11_paddle_x = 0;
// Dial counter free-running (wraps mod 256) per giochi con dial relativo
// (es. Gigas). A differenza di ec11_paddle_x che e' clamped 0..255 per
// paddle assoluto stile Arkanoid, qui non clampiamo: il game legge il
// counter due volte e calcola il delta autonomamente.
volatile uint8_t ec11_dial_counter = 0;
static volatile uint8_t ec11_ab_prev = 0;

// Step encoder regolabili per-gioco dal menu IMPOSTAZIONI (sensibilita' EC11).
// Valori a livello 5 (default di fabbrica) = i vecchi costanti hardcoded:
//   paddle_x step = EC11_PADDLE_STEP (4), dial_counter step = 6.
// ec11_apply_sensitivity(level) li riscala quando si avvia un gioco.
volatile int g_ec11_paddle_step = EC11_PADDLE_STEP;   // base 4 (Arkanoid paddle)
volatile int g_ec11_dial_step   = 6;                  // base 6 (dial free-running)

void ec11_apply_sensitivity(int level) {
  if (level < 1)  level = 1;
  if (level > 10) level = 10;
  int ps = (EC11_PADDLE_STEP * level) / 5;  if (ps < 1) ps = 1;
  int ds = (6 * level) / 5;                 if (ds < 1) ds = 1;
  g_ec11_paddle_step = ps;
  g_ec11_dial_step   = ds;
}

// Quando true (durante regolazione volume), gli step encoder NON aggiornano
// il paddle: il pulse viene comunque generato per il consumo lato volume.
static volatile bool paddle_locked = false;

// Sequenza COIN -> gap -> START attivata alla pressione di START
//   0..80 ms   : BUTTON_COIN attivo
//   80..160 ms : gap
//   160..400 ms: BUTTON_START
static uint32_t start_seq_t0 = 0;        // millis() di inizio, 0 = idle

// Hold lungo START (>= 3s) -> torna al menu boot.
// Esposto via input_consume_back_to_menu() per essere consumato dal loop.
static uint32_t btn_start_hold_t0 = 0;
static volatile bool back_to_menu_request = false;
static const uint32_t BACK_TO_MENU_HOLD_MS = 3000;

bool input_consume_back_to_menu() {
  bool v = back_to_menu_request;
  back_to_menu_request = false;
  return v;
}

void input_reset_button_state() {
  // Reset di tutti gli state interni dei bottoni quando si rientra nel menu.
  start_seq_t0 = 0;
  btn_start_hold_t0 = 0;
  back_to_menu_request = false;
}

// Vol mode: si attiva quando FIRE viene tenuto premuto >= 3 secondi (per
// evitare conflitti con il fire normale in-game). In vol mode la rotazione
// EC11 cambia il volume e i bottoni LEFT/RIGHT/FIRE NON vengono inoltrati al
// gioco. Si disattiva al rilascio di FIRE.
//
// volatile OBBLIGATORIO: buttons_get() viene chiamata da PIU' CORE per i giochi
// che pollano l'input dal core di emulazione (Arkanoid 2 legge la porta MCU ad
// alta frequenza nel suo run_frame su core 1, oltre al main loop su core 0).
// Senza volatile il core di emulazione vede vol_mode_active in modo NON coerente
// → spesso false → consuma ec11_step_pulse nel ramo non-volume e lo SCARTA →
// il volume non si regola. Stessa convenzione di paddle_locked/ec11_* qui sopra.
static volatile bool vol_mode_active = false;
static volatile bool prev_fire_pressed = false;
static volatile uint32_t fire_press_t0 = 0;
static volatile int8_t vol_accum_shared = 0;   // accumulo step encoder per il volume
static const uint32_t VOL_HOLD_MS = 3000;

bool input_is_vol_mode_active(void) { return vol_mode_active; }

// Per generare BUTTON_LEFT/RIGHT "virtuali" durante menu/attract (1 frame
// per ogni step encoder), oltre al paddle continuo
static volatile int8_t ec11_step_pulse = 0;   // -1/0/+1 consumato dal main

// Hold direzionale dell'EC11 (per-machine via machineBase::ec11PulseHoldMs).
// Dopo ogni click EC11, BUTTON_LEFT o BUTTON_RIGHT resta settato per N
// millisecondi (tipicamente 100 ms = ~6 frame) per giochi che leggono IN1
// molte volte per frame dal Z80 (Space Invaders, Galaga). Per Arkanoid e
// altri giochi paddle, ec11PulseHoldMs() ritorna 0 -> nessun hold attivo.
static uint32_t pulse_hold_until = 0;
static int8_t   pulse_held_sign  = 0;

// --- Quadrature decoder (table-driven, glitch-safe) ---
// new_state = (A<<1)|B, key = (prev<<2)|new
// table: +1 = CW, -1 = CCW, 0 = invalid/no-change
static const int8_t QDEC_TABLE[16] = {
   0, -1, +1,  0,
  +1,  0,  0, -1,
  -1,  0,  0, +1,
   0, +1, -1,  0
};

static IRAM_ATTR void ec11_isr() {
  uint8_t a = digitalRead(EC11_PIN_A) ? 1 : 0;
  uint8_t b = digitalRead(EC11_PIN_B) ? 1 : 0;
  uint8_t cur = (a << 1) | b;
  uint8_t key = (ec11_ab_prev << 2) | cur;
  int8_t d = QDEC_TABLE[key & 0x0F];
  ec11_ab_prev = cur;

  if (d != 0) {
    if (!paddle_locked) {
      int16_t v = ec11_paddle_x + (int16_t)d * g_ec11_paddle_step;
      if (v < 0)   v = 0;
      if (v > 255) v = 255;
      ec11_paddle_x = v;
      // Dial free-running: wrappa mod 256. Step regolabile per-gioco (default 6
      // vs 4 del paddle) per dare piu' "spinta" ai giochi con dial relativo.
      // Congelato (insieme al paddle) quando paddle_locked = true, cioe' in vol
      // mode: cosi' regolando il volume il paddle dei giochi a dial (Arkanoid 2,
      // Gigas, sbrkout) NON vola via.
      ec11_dial_counter = (uint8_t)(ec11_dial_counter + d * g_ec11_dial_step);
    }
    ec11_step_pulse = d;   // sempre: in vol mode viene consumato per il volume
  }
}

void Input::init() {
  s_fsm_core = xPortGetCoreID();   // setup() gira sul core del main loop
  virtual_coin_state = 0;
  virtual_coin_timer = 0;
  reset_timer = 0;
  input_states_last = 0;

  pinMode(EC11_PIN_A,  INPUT_PULLUP);
  pinMode(EC11_PIN_B,  INPUT_PULLUP);
  pinMode(EC11_PIN_SW, INPUT_PULLUP);
  pinMode(BTN_START,   INPUT_PULLUP);

  // stato iniziale quadrature
  uint8_t a = digitalRead(EC11_PIN_A) ? 1 : 0;
  uint8_t b = digitalRead(EC11_PIN_B) ? 1 : 0;
  ec11_ab_prev = (a << 1) | b;

  attachInterrupt(digitalPinToInterrupt(EC11_PIN_A), ec11_isr, CHANGE);
  attachInterrupt(digitalPinToInterrupt(EC11_PIN_B), ec11_isr, CHANGE);
}

void Input::enable()  {}
void Input::disable() {}
void Input::updateBluetooth(void) {}            // no BT su questo board
bool Input::button_y_pressed(void) { return false; }
bool Input::isBluetoothEnabled()   { return false; }

unsigned char Input::buttons_get(void) {
  // Solo il core del main loop esegue la FSM. Le chiamate dal core di emulazione
  // leggono lo snapshot gia' calcolato (niente race ne' campionamento ad alta
  // frequenza del micro-switch). input_states_last e' aggiornato ogni frame dal
  // main loop (SPINNERINO.ino:769), quindi qui e' sempre coerente col frame.
  if (s_fsm_core >= 0 && xPortGetCoreID() != s_fsm_core)
    return input_states_last;

  unsigned char states = 0;
  uint32_t now = millis();

  // ----- Stato pulsante FIRE (EC11 SW) -----
  bool fire_pressed = !digitalRead(EC11_PIN_SW);

  // Debounce del RILASCIO di FIRE per la SOLA macchina a stati del volume.
  // L'invio di BUTTON_FIRE al gioco (piu' sotto) resta istantaneo. Un glitch del
  // micro-switch EC11 mentre premi e ruoti la manopola in vol mode NON deve:
  //   (a) chiudere per sbaglio la regolazione  -> barra che sparisce (Gyruss)
  //   (b) resettare il timer di hold 3s         -> vol mode che non parte (Ark2)
  // Il rilascio viene confermato solo dopo 40 ms continui di FIRE non premuto.
  static uint32_t fire_release_t0 = 0;
  bool fire_stable;
  if (fire_pressed) {
    fire_release_t0 = 0;
    fire_stable = true;
  } else {
    if (fire_release_t0 == 0) fire_release_t0 = now ? now : 1;
    fire_stable = (now - fire_release_t0) < 40;
  }

  // Sensibilita' regolazione volume: 1 unita' di volume ogni N click encoder.
  // Aumenta per regolazione piu' lenta, diminuisci per piu' veloce.
  // vol_accum_shared e' volatile a file-scope (vedi nota in alto): condiviso tra
  // le chiamate da core diversi, non un local static per-core.
  static const int8_t VOL_STEP_THRESH = 4;

  // Tracking durata press FIRE per attivazione vol mode con hold >= 3 s
  // (usa fire_stable: il rilascio e' debounced, vedi sopra)
  if (fire_stable) {
    if (fire_press_t0 == 0) fire_press_t0 = now ? now : 1;
  } else {
    fire_press_t0 = 0;
  }

  // Edge release: se eravamo in vol mode, esci e persisti volume
  if (!fire_stable && prev_fire_pressed) {
    if (vol_mode_active) {
      vol_mode_active = false;
      paddle_locked   = false;
      vol_accum_shared = 0;           // reset accumulatore
      if (_volume_commit_callback) _volume_commit_callback();
    }
  }
  prev_fire_pressed = fire_stable;

  // Attiva vol mode se FIRE tenuto premuto >= VOL_HOLD_MS (3 s),
  // ma solo se la machine corrente non lo disabilita esplicitamente
  // (es. MotoRace dove FIRE = acceleratore va tenuto a lungo).
  bool vol_mode_allowed = currentMachine ? !currentMachine->disableVolMode() : true;
  if (vol_mode_allowed && fire_stable && fire_press_t0 != 0 &&
      (now - fire_press_t0) >= VOL_HOLD_MS && !vol_mode_active) {
    vol_mode_active = true;
    paddle_locked   = true;
  }

  // ----- Pulse encoder -----
  int8_t pulse = ec11_step_pulse;
  ec11_step_pulse = 0;

  // Aggiorna hold direzionale solo se la machine corrente lo richiede
  int hold_ms = currentMachine ? currentMachine->ec11HoldMsEff() : 0;
  if (pulse != 0 && hold_ms > 0) {
    pulse_hold_until = now + (uint32_t)hold_ms;
    pulse_held_sign  = (pulse > 0) ? 1 : -1;
  }

  if (vol_mode_active) {
    // Accumula gli step encoder; emette 1 cambio volume solo ogni
    // VOL_STEP_THRESH click (sensibilita' ridotta per regolazione fine).
    // dx (CW, pulse>0) = alza volume, sx (CCW, pulse<0) = abbassa.
    int8_t acc = vol_accum_shared + pulse;
    while (acc >= VOL_STEP_THRESH) {
      if (_volume_callback) _volume_callback(true, false);
      acc -= VOL_STEP_THRESH;
    }
    while (acc <= -VOL_STEP_THRESH) {
      if (_volume_callback) _volume_callback(false, true);
      acc += VOL_STEP_THRESH;
    }
    vol_accum_shared = acc;
    // In vol mode NON inviare BUTTON_FIRE (evita lancio palla accidentale)
    // e NON usare il pulse come BUTTON_LEFT/RIGHT.
  } else {
    if (fire_pressed) states |= BUTTON_FIRE;
    // Pulse fresco: sempre inviato (giochi senza hold lo usano direttamente)
    if (pulse < 0)    states |= BUTTON_LEFT;
    if (pulse > 0)    states |= BUTTON_RIGHT;
    // Hold: attivo solo per machine con ec11PulseHoldMs() > 0 (SI, Galaga)
    if (hold_ms > 0 && (int32_t)(pulse_hold_until - now) > 0) {
      if (pulse_held_sign < 0) states |= BUTTON_LEFT;
      if (pulse_held_sign > 0) states |= BUTTON_RIGHT;
    }
  }

  // ----- Stato pulsante START + detection hold lungo (-> back to menu) -----
  bool start_pressed = !digitalRead(BTN_START);

  // Hold tracker: parte al press, scatta a >= 4s
  if (start_pressed) {
    if (btn_start_hold_t0 == 0) btn_start_hold_t0 = now ? now : 1;
    if (now - btn_start_hold_t0 >= BACK_TO_MENU_HOLD_MS) {
      back_to_menu_request = true;
      btn_start_hold_t0 = 0;             // armato una volta sola per pressione
    }
  } else {
    btn_start_hold_t0 = 0;
  }

  // ----- Sequenza COIN -> gap -> START (alla pressione di START) -----
  if (start_pressed && start_seq_t0 == 0) {
    start_seq_t0 = now ? now : 1;
  }
  if (start_seq_t0 != 0) {
    uint32_t dt = now - start_seq_t0;
    if      (dt <  80)  states |= BUTTON_COIN;
    else if (dt < 160)  /* gap */;
    else if (dt < 400) {
      states |= BUTTON_START | BUTTON_EXTRA;
      // Centra il paddle alla prima entrata START (racchetta al centro)
      static bool paddle_centered = false;
      if (!paddle_centered) { ec11_paddle_x = 128; paddle_centered = true; }
    }
    else if (dt < 2000 && !start_pressed) start_seq_t0 = 0;
    else if (dt > 5000) start_seq_t0 = 0;
  }

  input_states_last = states;
  return states;
}

Input& Input::onVolumeUpDown(THandlerVolume fn)         { _volume_callback     = fn; return *this; }
Input& Input::onBrightnessUpDown(THandlerBrightness fn) { _brightness_callback = fn; return *this; }
Input& Input::onDoReset(THandlerDoReset fn)             { _doReset_callback    = fn; return *this; }
Input& Input::onDoAttractReset(THandlerDoAttractReset fn){_doAttractReset_callback = fn; return *this;}
Input& Input::onVolumeCommit(THandlerVolumeCommit fn)   { _volume_commit_callback = fn; return *this; }
