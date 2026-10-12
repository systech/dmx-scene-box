// DMX output engine: owns the universe, runs in its own task, fades between states.
#pragma once
#include <Arduino.h>
#include "config.h"

class DmxEngine {
public:
  void begin(int txPin, int rxPin, int enPin);
  // Set a new target universe (513 bytes incl. start code at [0]); fade over fadeMs.
  void setTarget(const uint8_t* vals, uint16_t fadeMs);
  // Snapshot of what is on the wire right now.
  void getCurrent(uint8_t* out513);
  uint32_t framesSent() const { return _frames; }

private:
  static void taskEntry(void* arg);
  void task();
  void computeFrame();
  void sendFrame();

  HardwareSerial* _uart = nullptr;
  int _tx = -1, _rx = -1, _en = -1;
  uint8_t  _frame[DMX_CHANNELS + 1];   // what goes on the wire
  uint8_t  _from[DMX_CHANNELS + 1];    // fade start
  uint8_t  _to[DMX_CHANNELS + 1];      // fade target
  uint32_t _fadeStart = 0, _fadeMs = 0;
  bool     _fading = false;
  uint32_t _frames = 0;
  SemaphoreHandle_t _mutex = nullptr;
};
