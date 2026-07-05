#include "Arduino.h"
#include "emulation/emulation.h"
#include "emulation/input.h"
#include "machines/machineBase.h"
#include "esp_task_wdt.h"

// including of "../machines/machineBase.h" in "emulation.h" not possible
TaskHandle_t emulationTaskHandle;
extern machineBase *currentMachine;
extern Input input;

void emulation_start() {
  currentMachine->reset();
  // Priority 1 instead of 2 to give Bluetooth more CPU time
  xTaskCreatePinnedToCore(emulation_task, "emulation task", 4096, NULL, 1, &emulationTaskHandle, ARDUINO_RUNNING_CORE == 0 ? 1 : 0);
}

void emulation_stop() {
  if (emulationTaskHandle == NULL)
    return;
  
  input.disable(); // disable input read from nunchuck
  
  vTaskDelete(emulationTaskHandle);
  emulationTaskHandle = NULL;
  currentMachine->reset();  // clear sound output

  input.enable(); // enable input read from nunchuck
}

void emulation_notifyGive() {
  if (emulationTaskHandle == NULL)
    return;

  xTaskNotifyGive(emulationTaskHandle);
}

void emulation_task(void *p) {
  // Watchdog already disabled globally via esp_task_wdt_deinit() in setup()

  while(1) {
    emulation_frame();

    // Small yield to allow Bluetooth stack to process
    taskYIELD();
  }
}

void emulation_frame() {
  // It may happen that the emulation runs too slow. It will then miss the
  // vblank notification and in turn will miss a frame and significantly
  // slow down. This risk is only given with Galaga as the emulation of
  // all three CPUs takes nearly 13ms. The 60hz vblank rate is in turn 
  // 16.6 ms.  
#if 0
  static int counter;
  static unsigned long time = millis();
  
  if (counter % 10 == 0) {
    // good time: 160ms
    unsigned long now = millis();
    printf("%2d: %dms\n", (int)currentMachine->machineType(),  now - time);
    time = now;
  }
  counter++;
#endif
   
  static uint32_t freeRunNextUs = 0;

  currentMachine->run_frame();

  // Wait for signal from video task to emulate a 60Hz frame rate. Don't do
  // this unless the game has actually started to speed up the boot process
  // a little bit.
  if(currentMachine->game_started) {
    if (currentMachine->freeRunEmulation()) {
      // Emulazione DISACCOPPIATA dal render: la logica di gioco gira a frequenza
      // reale (timer fisso 57.42 Hz), NON legata al notify del render. Serve ai
      // giochi il cui render e' troppo pesante per tenere i 60fps (es. PANG,
      // render 384x240 a 2 passaggi): con lo schema vblank-locked il gioco era
      // costretto alla velocita' del render = rallentatore. Il render (core 0)
      // ora mostra i frame in modo asincrono, a qualunque fps riesca.
      uint32_t now = micros();
      if (freeRunNextUs == 0) freeRunNextUs = now;
      freeRunNextUs += 17422;                                  // periodo 57.42 Hz
      while ((int32_t)(micros() - freeRunNextUs) < 0) vTaskDelay(1);  // attende cedendo la CPU
      if ((int32_t)(micros() - freeRunNextUs) > 100000) freeRunNextUs = micros(); // resync se molto indietro
    } else {
      ulTaskNotifyTake(1, portMAX_DELAY);
      // Give Bluetooth stack time to process after each frame
      vTaskDelay(1);
    }
  }
  else {
    freeRunNextUs = 0;               // reset pacing free-run per il prossimo gioco
    vTaskDelay(1); // give a millisecond delay to make the watchdog happy
  }
}

unsigned char OpZ80_INL(unsigned short Addr) {
  return currentMachine->opZ80(Addr);
}

void OutZ80(unsigned short Port, unsigned char Value) {
  currentMachine->outZ80(Port, Value);
}
  
unsigned char InZ80(unsigned short Port) {
  return currentMachine->inZ80(Port);
}

void WrZ80(unsigned short Addr, unsigned char Value) {
  currentMachine->wrZ80(Addr, Value);
}

unsigned char RdZ80(unsigned short Addr) {
  return currentMachine->rdZ80(Addr);
}

void PatchZ80(Z80 *R) {
}

void i8048_port_write(i8048_state_S *state, unsigned char port, unsigned char pos) {
  currentMachine->wrI8048_port(state, port, pos);
}

unsigned char i8048_port_read(i8048_state_S *state, unsigned char port) {
  return currentMachine->rdI8048_port(state, port);
}

unsigned char i8048_rom_read(i8048_state_S *state, unsigned short addr) {
  return currentMachine->rdI8048_rom(state, addr);
}

unsigned char i8048_xdm_read(i8048_state_S *state, unsigned char addr) {
  return currentMachine->rdI8048_xdm(state, addr);
}

void i8048_xdm_write(i8048_state_S *state, unsigned char addr, unsigned char data) {
  currentMachine->wrI8048_xdm(state, addr, data);
}

#if defined(ENABLE_SBRKOUT)
// ── M6502 (fake6502) dispatcher — quando Sbrkout e' attivo ──
// fake6502.c dichiara extern senza extern "C" → linkage C++. Manteniamo C++.
IRAM_ATTR uint8_t read6502(uint16_t address) {
  return currentMachine->rd6502((unsigned short)address);
}
IRAM_ATTR void write6502(uint16_t address, uint8_t value) {
  currentMachine->wr6502((unsigned short)address, (unsigned char)value);
}
IRAM_ATTR uint8_t opread6502(uint16_t address) {
  return currentMachine->op6502((unsigned short)address);
}
#endif // ENABLE_SBRKOUT