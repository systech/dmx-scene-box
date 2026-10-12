/*
 * dmx_scene_box.ino — DMX Scene Box: a file-driven DMX512 scene controller.
 *
 * Normal mode : drives DMX from the active scene, inputs 1-14 fire scenes from
 *               triggers.txt (rising edge, latest wins), input 15 = manual override,
 *               nothing listens on the network.
 * Setup mode  : input 16 held high. HTTP file API on port 80; editing.scene is
 *               played live and re-applied on every upload.
 * Boot        : boot.scene if present, else last scene (NVS), else all zero.
 *
 * Toolchain: ESP32 core 2.0.17, EQSP32 lib 1.5.2, RadioLib 7.7.1,
 *            board ESP32S3 Dev Module, 8M flash, "8M with spiffs". See README.
 */

#include <Arduino.h>
#include <EQSP32.h>
#include <Preferences.h>
#include "config.h"
#include "dmx_engine.h"
#include "scene_store.h"
#include "api_server.h"
#include "script_engine.h"
// Optional Wi-Fi: copy wifi_secrets.example.h to wifi_secrets.h and fill it in.
// Without it the box uses Ethernet only (DHCP).
#if __has_include("wifi_secrets.h")
  #include "wifi_secrets.h"
#endif

EQSP32      eqsp32;
DmxEngine   dmx;
Preferences prefs;

static const String NAME_EDITING  = String(FILE_EDITING).substring(1);
static const String NAME_BOOT     = String(FILE_BOOT).substring(1);
static const String NAME_OVERRIDE = String(FILE_OVERRIDE).substring(1);

static String   g_activeScene;       // file name currently on the wire
static bool     g_setupMode = false;
static uint32_t g_setupCandidateSince = 0;
static int      g_setupLevelSeen = -1;
static EQTimer  tReport(5000);

// ---------- beeper: small queue, served from loop() ----------
static Beep     g_beepQ[16];
static volatile uint8_t g_beepHead = 0, g_beepTail = 0;
static uint32_t g_beepUntil = 0;
static portMUX_TYPE g_beepMux = portMUX_INITIALIZER_UNLOCKED;

static void beepEnqueue(uint32_t freq, uint32_t ms) {
  portENTER_CRITICAL(&g_beepMux);
  uint8_t next = (g_beepHead + 1) % 16;
  if (next != g_beepTail) { g_beepQ[g_beepHead] = { (uint16_t)freq, (uint16_t)ms }; g_beepHead = next; }
  portEXIT_CRITICAL(&g_beepMux);
}

static void beepService() {
  if (millis() < g_beepUntil) return;
  Beep b; bool have = false;
  portENTER_CRITICAL(&g_beepMux);
  if (g_beepTail != g_beepHead) { b = g_beepQ[g_beepTail]; g_beepTail = (g_beepTail + 1) % 16; have = true; }
  portEXIT_CRITICAL(&g_beepMux);
  if (have) { eqsp32.buzzerOn(b.freq, b.ms); g_beepUntil = millis() + b.ms + 30; }
}

// ---------- scene playback ----------
static bool playScene(const String& nameIn, String& err) {
  String name = Store::normalizeSceneName(nameIn);
  String text;
  if (!Store::readFile(name, text)) { err = "no such scene: " + name; return false; }
  SceneData sd;
  if (!Store::parseScene(text, sd, err)) { err = name + ": " + err; return false; }

  if (sd.isScript) {
    if (!Script::start(text, name, err)) { err = name + ": " + err; return false; }
    g_activeScene = name;
    return true;
  }

  Script::stop();
  uint8_t target[DMX_CHANNELS + 1];
  if (sd.keepUnlisted) dmx.getCurrent(target); else memset(target, 0, sizeof(target));
  for (int c = 1; c <= DMX_CHANNELS; c++) if (sd.set[c]) target[c] = sd.vals[c];
  dmx.setTarget(target, sd.fadeMs);
  for (int i = 0; i < sd.nBeeps; i++) beepEnqueue(sd.beeps[i].freq, sd.beeps[i].ms);
  g_activeScene = name;
  Serial.printf("[scene] %s (fade %u ms)\n", name.c_str(), sd.fadeMs);
  return true;
}

static void probe(int lo, int hi, int value) {
  Script::stop();
  uint8_t target[DMX_CHANNELS + 1];
  memset(target, 0, sizeof(target));
  for (int c = lo; c <= hi; c++) target[c] = (uint8_t)value;
  dmx.setTarget(target, 0);
  g_activeScene = "(probe " + String(lo) + "-" + String(hi) + ")";
  Serial.println("[scene] " + g_activeScene);
}

static void rememberScene(const String& name) {
  if (prefs.getString("last", "") != name) prefs.putString("last", name);
}

static void playOverride() {
  String err;
  if (Store::exists(NAME_OVERRIDE) && playScene(NAME_OVERRIDE, err)) return;
  Script::stop();
  uint8_t full[DMX_CHANNELS + 1];
  memset(full, 255, sizeof(full));
  full[0] = 0;
  dmx.setTarget(full, 0);
  g_activeScene = "(override: all 255)";
  Serial.println("[scene] override -> all channels 255");
}

static String statusJson() {
  uint8_t cur[DMX_CHANNELS + 1];
  dmx.getCurrent(cur);
  String s = "{";
  s += "\"fw\":\"" FW_NAME " " FW_VERSION "\",";
  s += "\"mode\":\"" + String(g_setupMode ? "setup" : "normal") + "\",";
  s += "\"scene\":\"" + g_activeScene + "\",";
  s += "\"ip\":\"" + eqsp32.localIP() + "\",";
  s += "\"wifi\":" + String(eqsp32.getWiFiStatus() == EQ_WF_CONNECTED ? "true" : "false") + ",";
  s += "\"eth\":" + String(eqsp32.getEthernetStatus() == EQ_ETH_CONNECTED ? "true" : "false") + ",";
  s += "\"uptime_s\":" + String(millis() / 1000) + ",";
  s += "\"frames\":" + String(dmx.framesSent()) + ",";
  s += "\"script\":{\"running\":" + String(Script::running() ? "true" : "false") +
       ",\"ticks\":" + String(Script::ticks()) + ",\"lua_heap\":" + String(Script::memoryUsed()) +
       ",\"error\":\"" + Script::lastError() + "\"},";
  s += "\"heap_free\":" + String(ESP.getFreeHeap()) + ",";
  s += "\"fs_used\":" + String(Store::usedBytes()) + ",\"fs_total\":" + String(Store::totalBytes()) + ",";
  s += "\"ch1_8\":[";
  for (int c = 1; c <= 8; c++) { s += String(cur[c]); if (c < 8) s += ","; }
  s += "],\"triggers\":\"" + Store::triggersSummary() + "\"}";
  return s;
}

// ---------- mode switching ----------
static void enterSetup() {
  g_setupMode = true;
  Serial.println("[mode] SETUP");
  Api::start();
  String err;
  if (!playScene(NAME_EDITING, err)) Serial.println("[mode] " + err);
}

static void leaveSetup() {
  g_setupMode = false;
  Api::stop();
  Serial.println("[mode] NORMAL");
  String last = prefs.getString("last", "");
  String err;
  if (Store::exists(NAME_BOOT)) playScene(NAME_BOOT, err);
  else if (last.length() && !playScene(last, err)) Serial.println("[mode] " + err);
}

// Setup switch is a level: debounce it over SETUP_DEBOUNCE_MS.
static void pollSetupSwitch() {
  int lvl = eqsp32.readPin(IN_SETUP_MODE) ? 1 : 0;
  if (lvl != g_setupLevelSeen) {
    g_setupLevelSeen = lvl;
    g_setupCandidateSince = millis();
    return;
  }
  if ((bool)lvl != g_setupMode && millis() - g_setupCandidateSince >= SETUP_DEBOUNCE_MS) {
    if (lvl) enterSetup(); else leaveSetup();
  }
}

// ---------- setup / loop ----------
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== " FW_NAME " " FW_VERSION " ===");

  EQSP32Configs cfg;
#ifdef WIFI_SSID
  cfg.wifiSSID = WIFI_SSID;
  cfg.wifiPassword = WIFI_PASSWORD;
#endif
  cfg.userDevName = FW_NAME;
  cfg.bleBroadcastingMins = 1;
  eqsp32.begin(cfg, false);

  for (int p = EQ_PIN_1; p <= EQ_PIN_16; p++) eqsp32.pinMode(p, DIN);

  dmx.begin(eqsp32.getPin(EQ_RS485_TX), eqsp32.getPin(EQ_RS485_RX), eqsp32.getPin(EQ_RS485_EN));
  Store::begin();
  prefs.begin("cdmx", false);
  Script::begin(&dmx, beepEnqueue);
  Api::begin(playScene, statusJson, probe);

  // Boot scene
  String err;
  if (Store::exists(NAME_BOOT)) playScene(NAME_BOOT, err);
  else {
    String last = prefs.getString("last", "");
    if (last.length() == 0 || !playScene(last, err)) {
      Serial.println("[boot] no boot/last scene, universe at 0");
      g_activeScene = "(none)";
    }
  }

  // Arm edge detection so an input that is already high does not fire at boot.
  for (int p = IN_FIRST; p <= IN_OVERRIDE; p++) eqsp32.readPin(p, ON_RISING);
  g_setupLevelSeen = eqsp32.readPin(IN_SETUP_MODE) ? 1 : 0;
  g_setupCandidateSince = millis();
  if (g_setupLevelSeen) enterSetup();

  tReport.start();
}

void loop() {
  Api::handle();
  pollSetupSwitch();
  beepService();

  // Trigger inputs: rising edge, latest wins, in both modes.
  for (int p = IN_FIRST; p <= IN_LAST_TRIGGER; p++) {
    if (eqsp32.readPin(p, ON_RISING)) {
      String scene = Store::sceneForInput(p);
      Serial.printf("[input] %d rising -> %s\n", p, scene.length() ? scene.c_str() : "(unmapped)");
      if (scene.length()) {
        String err;
        if (playScene(scene, err)) { if (!g_setupMode) rememberScene(scene); }
        else Serial.println("[input] " + err);
      }
    }
  }
  if (eqsp32.readPin(IN_OVERRIDE, ON_RISING)) { Serial.println("[input] 15 override"); playOverride(); }

  if (tReport.isExpired()) {
    tReport.reset();
    Serial.printf("[hb] mode=%s scene=%s ip=%s frames=%lu heap=%u script=%s ticks=%lu lua=%u\n",
                  g_setupMode ? "setup" : "normal", g_activeScene.c_str(),
                  eqsp32.localIP().c_str(), (unsigned long)dmx.framesSent(), ESP.getFreeHeap(),
                  Script::running() ? "run" : "-", (unsigned long)Script::ticks(), (unsigned)Script::memoryUsed());
  }
  delay(2);
}
