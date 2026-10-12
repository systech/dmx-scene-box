// File store (LittleFS) + static scene parser + trigger map.
#pragma once
#include <Arduino.h>
#include <FS.h>
#include "config.h"

#define SCENE_MAX_BEEPS 8

struct Beep { uint16_t freq; uint16_t ms; };

struct SceneData {
  uint8_t  vals[DMX_CHANNELS + 1];
  bool     set[DMX_CHANNELS + 1];
  uint16_t fadeMs = DEFAULT_FADE_MS;
  bool     keepUnlisted = false;   // "base keep": unlisted channels stay as they are
  bool     isScript = false;       // Lua scene: first line starts with "--"
  Beep     beeps[SCENE_MAX_BEEPS];
  uint8_t  nBeeps = 0;
};

namespace Store {
  bool begin();                                  // mount, create defaults
  bool validName(const String& name);            // "foyer.scene" style, no slashes
  String normalizeSceneName(String name);        // adds ".scene" if no extension
  bool exists(const String& name);
  bool readFile(const String& name, String& out);
  bool writeFile(const String& name, const String& data);
  bool removeFile(const String& name);
  String listJson();                             // [{"name":..,"size":..},..]
  size_t totalBytes();
  size_t usedBytes();

  // Parse a scene file body. Returns false with err filled on syntax error.
  bool parseScene(const String& text, SceneData& out, String& err);

  // Trigger map: input number -> scene file name. Unmapped -> "".
  void loadTriggers();
  String sceneForInput(int input);
  String triggersSummary();
}
