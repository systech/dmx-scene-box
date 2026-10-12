// Dynamic scenes: a Lua script ticked by the device clock.
//
// Script contract (see description.yaml on the device):
//   function update(t)   -- called every SCRIPT_TICK_MS, t = ms since the scene started
//   function setup()     -- optional, once, before the first update
//   dmx.set(ch, v)  dmx.fill(lo, hi, v)  dmx.get(ch)  dmx.beep(freq, ms)  log(...)
// The working frame starts at all zero and persists between ticks.
#pragma once
#include <Arduino.h>
#include "dmx_engine.h"

namespace Script {
  typedef void (*BeepFn)(uint32_t freq, uint32_t ms);

  void begin(DmxEngine* dmx, BeepFn beep);
  // Compile + run setup(). On failure returns false with err set and nothing running.
  bool start(const String& code, const String& name, String& err);
  // Syntax-check only (no execution). Used by the upload API.
  bool check(const String& code, const String& name, String& err);
  void stop();
  bool running();
  String name();
  String lastError();
  uint32_t ticks();
  size_t memoryUsed();
}
