#ifndef VIDEO_H
#define VIDEO_H

#include <Arduino.h>
#include <driver/spi_master.h>
#include "../config.h"

class Video {
public:
  Video();
  void begin(void);
  void write(uint16_t *colors, uint32_t len);
  void flipVertical(char flip);
  void flipHorizontal(char flip);
  void setLandscape(bool enable);
  // Cambia orientamento HW: portrait (240x320 nativo per splash/menu) o
  // landscape (320x240 con MAC=0x60 per il game). Setta MADCTL + addr window.
  void setOrientation(bool portrait);
  bool mirror_x = false;
  bool _landscape = false;
  void setAddrWindow(uint16_t x, uint16_t y, uint16_t w, uint16_t h);
  void write16(uint16_t u16);
  void writeCommand(uint8_t cmd);
  void sendCommand(uint8_t commandByte, uint8_t *dataBytes, uint8_t numDataBytes);
  void write8(uint8_t u8);
  void setBrightness(uint8_t level);  // 0=min, 10=max
  uint8_t brightnessLevel = 7;        // default level (0-10)

private:
  uint8_t _bl_duty = 180;             // PWM duty 0-255 (default ~70%)
  char dma_active;
  spi_device_handle_t handle;
  spi_transaction_t* r_trans;
  spi_transaction_t transaction;
  unsigned char *dma_buffer;  // use a second buffer for dma transfers
};

#endif // VIDEO_H
