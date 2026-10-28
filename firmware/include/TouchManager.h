#ifndef EINKDASH_TOUCH_MANAGER_H
#define EINKDASH_TOUCH_MANAGER_H

#include <Arduino.h>
#include <Wire.h>

struct TouchPointEink {
  int16_t x = -1;  // coordenada horizontal da tela: 0..799
  int16_t y = -1;  // coordenada vertical da tela: 0..479
  uint16_t rawX = 0;
  uint16_t rawY = 0;
};

class TouchManager {
 public:
  // GPIO confirmados pelo exemplo original Good Display ESP32-L/FTS02:
  // A4 = GPIO32 (SDA), A5 = GPIO33 (SCL).
  bool begin(int sda = 32, int scl = 33);
  bool available() const { return _address != 0; }
  void setTransform(bool swapXY, bool mirrorX, bool mirrorY);
  bool readTap(TouchPointEink& point);  // um evento por toque, com debounce

 private:
  uint8_t _address = 0;
  uint16_t _maxX = 800;
  uint16_t _maxY = 480;
  bool _swapXY = false;
  bool _mirrorX = false;
  bool _mirrorY = false;
  bool _pressed = false;
  uint32_t _lastTapMs = 0;
  uint32_t _lastTouchReportMs = 0;

  bool readReg(uint16_t reg, uint8_t* data, size_t size);
  bool writeReg8(uint16_t reg, uint8_t value);
};

#endif
