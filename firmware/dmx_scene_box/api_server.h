// HTTP file API, active only in setup mode.
#pragma once
#include <Arduino.h>
#include <functional>

namespace Api {
  typedef std::function<bool(const String& sceneName, String& err)> PlayFn;
  typedef std::function<String()> StatusFn;
  typedef std::function<void(int lo, int hi, int value)> ProbeFn;

  void begin(PlayFn play, StatusFn status, ProbeFn probe);  // registers routes, does not listen yet
  void start();                              // listen
  void stop();                               // stop listening
  bool running();
  void handle();                             // call from loop()
}
