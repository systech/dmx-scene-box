#include "dmx_engine.h"

void DmxEngine::begin(int txPin, int rxPin, int enPin) {
  _tx = txPin; _rx = rxPin; _en = enPin;
  _mutex = xSemaphoreCreateMutex();
  memset(_frame, 0, sizeof(_frame));
  memset(_from, 0, sizeof(_from));
  memset(_to, 0, sizeof(_to));

  pinMode(_en, OUTPUT);
  digitalWrite(_en, HIGH);                 // transmit-only: driver permanently enabled
  _uart = &Serial2;
  _uart->setTxBufferSize(DMX_CHANNELS + 16);
  _uart->begin(DMX_BAUD, SERIAL_8N2, _rx, _tx);

  xTaskCreatePinnedToCore(taskEntry, "dmx", 4096, this, 3, nullptr, 1);
}

void DmxEngine::setTarget(const uint8_t* vals, uint16_t fadeMs) {
  xSemaphoreTake(_mutex, portMAX_DELAY);
  memcpy(_from, _frame, sizeof(_from));
  memcpy(_to, vals, sizeof(_to));
  _to[0] = 0;                              // start code always 0
  _fadeMs = fadeMs;
  _fadeStart = millis();
  _fading = fadeMs > 0;
  if (!_fading) memcpy(_frame, _to, sizeof(_frame));
  xSemaphoreGive(_mutex);
}

void DmxEngine::getCurrent(uint8_t* out) {
  xSemaphoreTake(_mutex, portMAX_DELAY);
  memcpy(out, _frame, DMX_CHANNELS + 1);
  xSemaphoreGive(_mutex);
}

void DmxEngine::computeFrame() {
  xSemaphoreTake(_mutex, portMAX_DELAY);
  if (_fading) {
    uint32_t el = millis() - _fadeStart;
    if (el >= _fadeMs) {
      memcpy(_frame, _to, sizeof(_frame));
      _fading = false;
    } else {
      uint32_t k = (el * 256) / _fadeMs;   // 0..255
      for (int i = 1; i <= DMX_CHANNELS; i++) {
        int d = (int)_to[i] - (int)_from[i];
        _frame[i] = (uint8_t)((int)_from[i] + (d * (int)k) / 256);
      }
    }
  }
  xSemaphoreGive(_mutex);
}

void DmxEngine::sendFrame() {
  // Break + mark-after-break: one 0x00 byte at a slower baud rate.
  _uart->updateBaudRate(DMX_BREAK_BAUD);
  _uart->write((uint8_t)0x00);
  _uart->flush();
  _uart->updateBaudRate(DMX_BAUD);
  _uart->write(_frame, DMX_CHANNELS + 1);
  _uart->flush();
  _frames++;
}

void DmxEngine::taskEntry(void* arg) { static_cast<DmxEngine*>(arg)->task(); }

void DmxEngine::task() {
  TickType_t last = xTaskGetTickCount();
  for (;;) {
    computeFrame();
    sendFrame();
    vTaskDelayUntil(&last, pdMS_TO_TICKS(DMX_FRAME_MS));
  }
}
