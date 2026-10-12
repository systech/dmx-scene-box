/*
 * hardware_check.ino — wiring smoke test. Flash this first; no network, no files.
 *
 * - Sends a 512-channel DMX frame every 25 ms (40 Hz) on the EQSP32 RS485 port.
 * - Channels 1..3 drive the PX24500 (R, G, B) through a slow colour cycle so
 *   the LED strip shows the link is alive.
 * - Inputs 9..16 (DIO) are read as digital inputs; a rising edge on input N
 *   freezes the colour to a fixed scene for that input (N=9 -> red, 10 -> green,
 *   11 -> blue, 12 -> white, others -> off) until the next edge. Edge-triggered,
 *   latest wins, exactly as the spec describes.
 * - Everything is printed on the USB serial monitor at 115200.
 *
 * Toolchain: ESP32 core 2.0.17, EQSP32 lib 1.5.2, board ESP32S3 Dev Module,
 *            8M flash, partition "8M with spiffs".
 */

#include <Arduino.h>
#include <EQSP32.h>

#define DMX_BAUD      250000
#define DMX_BREAK_US  120
#define DMX_MAB_US    12
#define DMX_FRAME_MS  25
#define DMX_CHANNELS  512

EQSP32 eqsp32;
HardwareSerial& DMX_SERIAL = Serial2;
int dmxTx, dmxRx, dmxEn;
uint8_t dmxFrame[DMX_CHANNELS + 1];
EQTimer tFrame(DMX_FRAME_MS);
EQTimer tReport(1000);

int  activeScene = 0;        // 0 = colour cycle, 9..16 = fixed scene from that input
uint32_t cycleT0 = 0;

void dmxBeginUart() {
  DMX_SERIAL.begin(DMX_BAUD, SERIAL_8N2, dmxRx, dmxTx, false, 1);
}

void dmxSendFrame(uint8_t* packet, int size) {
  digitalWrite(dmxEn, HIGH);
  DMX_SERIAL.flush();
  DMX_SERIAL.end();
  pinMode(dmxTx, OUTPUT);
  digitalWrite(dmxTx, LOW);
  delayMicroseconds(DMX_BREAK_US);
  digitalWrite(dmxTx, HIGH);
  delayMicroseconds(DMX_MAB_US);
  dmxBeginUart();
  DMX_SERIAL.write(packet, size);
  DMX_SERIAL.flush();
  digitalWrite(dmxEn, LOW);
}

// Simple RGB colour wheel, 0..1535 -> r,g,b
void wheel(uint32_t pos, uint8_t& r, uint8_t& g, uint8_t& b) {
  pos %= 1536;
  uint8_t seg = pos / 256, x = pos % 256;
  switch (seg) {
    case 0: r = 255;     g = x;       b = 0;       break;
    case 1: r = 255 - x; g = 255;     b = 0;       break;
    case 2: r = 0;       g = 255;     b = x;       break;
    case 3: r = 0;       g = 255 - x; b = 255;     break;
    case 4: r = x;       g = 0;       b = 255;     break;
    default:r = 255;     g = 0;       b = 255 - x; break;
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== DMX Scene Box hardware check ===");

  eqsp32.begin(true);                      // verbose system messages

  dmxTx = eqsp32.getPin(EQ_RS485_TX);
  dmxRx = eqsp32.getPin(EQ_RS485_RX);
  dmxEn = eqsp32.getPin(EQ_RS485_EN);
  pinMode(dmxEn, OUTPUT);
  digitalWrite(dmxEn, LOW);
  dmxBeginUart();
  memset(dmxFrame, 0, sizeof(dmxFrame));   // byte 0 = start code 0

  for (int p = EQ_PIN_9; p <= EQ_PIN_16; p++) eqsp32.pinMode(p, DIN);

  Serial.printf("RS485 pins tx=%d rx=%d en=%d, DMX at 250k, %d ms/frame\n",
                dmxTx, dmxRx, dmxEn, DMX_FRAME_MS);
  cycleT0 = millis();
  tFrame.start();
  tReport.start();
}

void loop() {
  // Inputs: rising edge on any DIO selects that scene (latest wins)
  for (int p = EQ_PIN_9; p <= EQ_PIN_16; p++) {
    if (eqsp32.readPin(p, ON_RISING)) {
      activeScene = p;
      Serial.printf("edge on input %d -> scene %d\n", p, p);
    }
  }

  if (tFrame.isExpired()) {
    tFrame.reset();
    uint8_t r = 0, g = 0, b = 0;
    switch (activeScene) {
      case 0:  wheel((millis() - cycleT0) / 8, r, g, b); break;  // ~12 s per cycle
      case 9:  r = 255; break;
      case 10: g = 255; break;
      case 11: b = 255; break;
      case 12: r = g = b = 255; break;
      default: break;                                              // off
    }
    dmxFrame[1] = r; dmxFrame[2] = g; dmxFrame[3] = b;
    dmxSendFrame(dmxFrame, DMX_CHANNELS + 1);
  }

  if (tReport.isExpired()) {
    tReport.reset();
    Serial.printf("scene=%d rgb=%3d,%3d,%3d inputs 9..16:", activeScene,
                  dmxFrame[1], dmxFrame[2], dmxFrame[3]);
    for (int p = EQ_PIN_9; p <= EQ_PIN_16; p++) Serial.printf(" %d", eqsp32.readPin(p));
    Serial.println();
  }

  delay(2);
}
