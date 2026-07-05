#ifndef _CONFIG_H_
#define _CONFIG_H_

#ifndef ARDUINO_RUNNING_CORE
  #define ARDUINO_RUNNING_CORE 0
#endif

#ifndef TFT_SPICLK
  #define TFT_SPICLK 80000000      // 80 MHz: ST7789 stabile su P4 con HSPI
#endif

// ============================================================================
// BOARD: Waveshare ESP32-P4-WIFI6 (header 40-pin, ROM in flash)
// ============================================================================
#define SPINNERINO_P4_BOARD

// --- ST7789 SPI 320x240 landscape (cablato sul GPIO header 40-pin) ---
#define TFT_MOSI        21
#define TFT_SCLK        20
#define TFT_CS          22
#define TFT_DC          23
#define TFT_RST         26
#define TFT_BL          27       // backlight PWM (LEDC), active HIGH
#define TFT_BL_LEVEL    HIGH

// Pannello ST7789 (no ILI9341)
//#define USE_ILI9341

// MADCTL landscape MV+MX = 0x60 (display fisico portrait, sw rotation)
#ifdef USE_ILI9341
  #define TFT_MAC       0x28
#else
  #define TFT_MAC       0x60
#endif
// ATTENZIONE: il display e' montato in PORTRAIT fisicamente, con MAC=0x60 (MV).
// Quindi gli assi RENDERING e DISPLAY FISICO sono SCAMBIATI.
// Su molti pannelli generic, INVON inverte i colori. Definisci TFT_INVERT
// solo per pannelli IPS che lo richiedono.
//#define TFT_INVERT

#define TFT_HW_X             0
#define TFT_HW_Y             8       // shift HW finale
#define TFT_FB_W             320
#define TFT_FB_H             232     // 240 - TFT_HW_Y
#define TFT_X_OFFSET         32      // padding alto/basso display fisico
#define TFT_Y_OFFSET         0       // 0 = max dx (centrato con MX mirror)
#define TFT_PANEL_X_OFFSET   0
#define TFT_PANEL_Y_OFFSET   0
#define TFT_PANEL_OFFSET     0
#define TFT_SCREEN_W         320
#define TFT_SCREEN_H         240

// --- Audio ES8311 + NS4150B amp (onboard) ---
//   Pin DIVERSI da MAX98357A: I2S MCLK richiesto, PA enable separato
#define I2C_SDA          7        // I2C ES8311
#define I2C_SCL          8
#define ES8311_I2C_ADDR  0x18
#define I2S_MCLK         13       // master clock (richiesto da ES8311 in slave)
#define I2S_BCLK         12       // SCLK
#define I2S_LRC          10       // LRCK / WS
#define I2S_DOUT          9       // ESP32 TX -> codec DSDIN
#define ES8311_PA_PIN    53       // amplifier enable (HIGH = ON)
#define SND_I2S_DIGITAL           // segnala driver digitale (no DAC interno)

// --- Input: EC11 encoder + COIN + START (header 40-pin) ---
#define USE_EC11_PADDLE
#define EC11_PIN_A       32       // quadrature A (ISR)
#define EC11_PIN_B       33       // quadrature B
#define EC11_PIN_SW      48       // COIN / SELECT in menu  - >5sec return to menu
#define BTN_START        47       // FIRE / Lancio palla    - >5sec VOLUME

#define EC11_PADDLE_STEP 4
#define AUTO_COIN_FRAMES 30       // ~0.5s a 60 fps

// LED non disponibile (board Waveshare non ha LED utente standard)
// #define LED_PIN

// ============================================================================
// GAME SELECTION
// ============================================================================
#define ENABLE_ARKANOID
#define ENABLE_ARKANOID2     
#define ENABLE_GIGAS
#define ENABLE_GIGAS2
#define ENABLE_GOINDOL       
#define ENABLE_SPACE         
#define ENABLE_GALAGA        
#define ENABLE_MOTORACE      
#define ENABLE_SBRKOUT
#define ENABLE_BOMBBEE       
#define ENABLE_GALAXIAN
#define ENABLE_GYRUSS        
#define ENABLE_ROADFIGHTER
#define ENABLE_PANG
// #define SINGLE_MACHINE

// Config attract
#define MASTER_ATTRACT_MENU_TIMEOUT  40000      // 40 s prima dell'auto-avvio dal menu
#define MASTER_ATTRACT_GAME_TIMEOUT  60000 * 3  // 3 min prima del ritorno al menu

// Video full frame rate (P4 + 80 MHz SPI)
// #define VIDEO_HALF_RATE

#endif // _CONFIG_H_
