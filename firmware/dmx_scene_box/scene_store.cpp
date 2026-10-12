#include "scene_store.h"
#include <LittleFS.h>

static String g_trig[EQ_PIN_16 + 1];

// ---------- defaults written on a fresh device ----------
static const char DEFAULT_TRIGGERS[] =
  "# input -> scene file. Inputs 1-14. 15 = override, 16 = setup switch (fixed).\n"
  "9  red\n"
  "10 green\n"
  "11 blue\n"
  "12 white\n"
  "13 off\n";

static const char DEFAULT_DESCRIPTION[] =
  "# DMX Scene Box device description. Read this first.\n"
  "device: DMX Scene Box on EQSP32CE\n"
  "dmx_universe: 1\n"
  "refresh_hz: 40\n"
  "fixtures:\n"
  "  - name: bench RGB strip (PX24500 decoder, address 1)\n"
  "    channels: {red: 1, green: 2, blue: 3}\n"
  "scene_file_format: |\n"
  "  Plain text, one statement per line. '#' starts a comment.\n"
  "  fade <ms>         optional, crossfade time from the previous state\n"
  "  base keep         optional, unlisted channels keep their current value (default: 0)\n"
  "  <ch> <value>      channel 1-512, value 0-255\n"
  "  <ch>-<ch> <value> range\n"
  "  beep <hz> [ms]    optional, up to 8 lines, played in order when the scene fires (onboard buzzer)\n"
  "  Example: 'fade 500' then '1-3 255'\n"
  "dynamic_scene_format: |\n"
  "  A .scene file whose FIRST line starts with '--' is a Lua 5.4 script, ticked by the device every 20 ms.\n"
  "  function update(t)   required. t = milliseconds since the scene started. Compute the look from t, never sleep or count ticks.\n"
  "  function setup()     optional, runs once first.\n"
  "  dmx.set(ch, v)       set channel 1-512 to 0-255 in the working frame (frame starts all 0, persists between ticks)\n"
  "  dmx.fill(lo, hi, v)  set a range\n"
  "  dmx.get(ch)          read the working frame\n"
  "  dmx.beep(hz, ms)     onboard buzzer\n"
  "  log(...)             print to the serial console\n"
  "  math.* string.* table.* available; no io, os, files or network. Limits: 64 kB heap, 400k instructions per update().\n"
  "  A runtime error stops the script and freezes the last frame; the next trigger clears it.\n"
  "  Example wave: function update(t) for s=1,8 do dmx.set(s, 128+127*math.sin(t/400 - s*0.6)) end end\n"
  "trigger_map: triggers.txt, lines '<input> <scene>', inputs 1-14, scene name with or without .scene\n"
  "fixed_inputs: {15: manual override -> override.scene or all channels 255, 16: setup mode switch}\n"
  "api:\n"
  "  base: http://<device-ip>/api   (only while in setup mode)\n"
  "  GET    /api/status             JSON state\n"
  "  GET    /api/files              JSON list of files\n"
  "  GET    /api/files/{name}       download file\n"
  "  PUT    /api/files/{name}       upload whole file: raw body, header Content-Type: text/plain. editing.scene is applied live.\n"
  "  example: curl -X PUT -H 'Content-Type: text/plain' --data-binary @foyer.scene http://<ip>/api/files/foyer.scene\n"
  "  DELETE /api/files/{name}       delete\n"
  "  POST   /api/play/{name}        play a scene now (preview)\n"
  "  GET    /api/probe?ch=N[&to=M][&v=255]   survey helper: channels N..M to v, everything else 0, no file needed\n"
  "workflow: edit editing.scene until it looks right, then PUT the same text as <name>.scene and map it in triggers.txt\n";

static void writeDefault(const char* path, const char* body) {
  if (LittleFS.exists(path)) return;
  File f = LittleFS.open(path, "w");
  if (f) { f.print(body); f.close(); Serial.printf("[store] created default %s\n", path); }
}

bool Store::begin() {
  if (!LittleFS.begin(true, "/fs", 10, FS_PARTITION)) {
    Serial.println("[store] LittleFS mount FAILED");
    return false;
  }
  Serial.printf("[store] LittleFS %u/%u bytes used\n", (unsigned)usedBytes(), (unsigned)totalBytes());
  writeDefault(FILE_TRIGGERS, DEFAULT_TRIGGERS);
  writeDefault(FILE_DESCRIPTION, DEFAULT_DESCRIPTION);
  writeDefault("/red.scene",   "fade 300\n1 255\n2 0\n3 0\n");
  writeDefault("/green.scene", "fade 300\n1 0\n2 255\n3 0\n");
  writeDefault("/blue.scene",  "fade 300\n1 0\n2 0\n3 255\n");
  writeDefault("/white.scene", "fade 300\n1-3 255\n");
  writeDefault("/off.scene",   "fade 1000\n1-3 0\n");
  writeDefault(FILE_EDITING,   "# live editing scene\n1 40\n2 40\n3 40\n");
  writeDefault("/wheel.scene",
    "-- colour wheel on channels 1-3, one full cycle every 12 s\n"
    "local function wheel(pos)\n"
    "  pos = pos % 1536\n"
    "  local seg, x = pos // 256, pos % 256\n"
    "  if seg == 0 then return 255, x, 0\n"
    "  elseif seg == 1 then return 255 - x, 255, 0\n"
    "  elseif seg == 2 then return 0, 255, x\n"
    "  elseif seg == 3 then return 0, 255 - x, 255\n"
    "  elseif seg == 4 then return x, 0, 255\n"
    "  else return 255, 0, 255 - x end\n"
    "end\n"
    "\n"
    "function setup()\n"
    "  dmx.beep(1200, 60)\n"
    "end\n"
    "\n"
    "function update(t)\n"
    "  local r, g, b = wheel(t // 8)\n"
    "  dmx.set(1, r); dmx.set(2, g); dmx.set(3, b)\n"
    "end\n");
  loadTriggers();
  return true;
}

bool Store::validName(const String& n) {
  if (n.length() == 0 || n.length() > MAX_NAME_LEN) return false;
  if (n[0] == '.') return false;
  for (size_t i = 0; i < n.length(); i++) {
    char c = n[i];
    if (!(isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.')) return false;
  }
  return true;
}

String Store::normalizeSceneName(String n) {
  n.trim();
  if (n.indexOf('.') < 0) n += SCENE_EXT;
  return n;
}

static String fsPath(const String& name) { return "/" + name; }

bool Store::exists(const String& name) { return LittleFS.exists(fsPath(name)); }

bool Store::readFile(const String& name, String& out) {
  File f = LittleFS.open(fsPath(name), "r");
  if (!f || f.isDirectory()) return false;
  out = f.readString();
  f.close();
  return true;
}

bool Store::writeFile(const String& name, const String& data) {
  File f = LittleFS.open(fsPath(name), "w");
  if (!f) return false;
  size_t n = f.print(data);
  f.close();
  return n == data.length();
}

bool Store::removeFile(const String& name) { return LittleFS.remove(fsPath(name)); }

String Store::listJson() {
  String s = "[";
  File root = LittleFS.open("/");
  File f = root.openNextFile();
  bool first = true;
  while (f) {
    if (!f.isDirectory()) {
      if (!first) s += ",";
      first = false;
      String n = f.name();
      if (n.startsWith("/")) n = n.substring(1);
      s += "{\"name\":\"" + n + "\",\"size\":" + String(f.size()) + "}";
    }
    f = root.openNextFile();
  }
  s += "]";
  return s;
}

size_t Store::totalBytes() { return LittleFS.totalBytes(); }
size_t Store::usedBytes()  { return LittleFS.usedBytes(); }

// ---------- scene parser ----------
static bool parseInt(const String& s, long& v) {
  if (s.length() == 0) return false;
  char* end;
  v = strtol(s.c_str(), &end, 10);
  return *end == '\0';
}

bool Store::parseScene(const String& text, SceneData& out, String& err) {
  memset(out.vals, 0, sizeof(out.vals));
  memset(out.set, 0, sizeof(out.set));
  out.fadeMs = DEFAULT_FADE_MS;
  out.keepUnlisted = false;
  out.isScript = false;

  int lineNo = 0, start = 0;
  while (start < (int)text.length()) {
    int nl = text.indexOf('\n', start);
    if (nl < 0) nl = text.length();
    String line = text.substring(start, nl);
    start = nl + 1;
    lineNo++;

    if (lineNo == 1 && line.startsWith("--")) { out.isScript = true; return true; }

    int hash = line.indexOf('#');
    if (hash >= 0) line = line.substring(0, hash);
    line.replace('=', ' ');
    line.replace(':', ' ');
    line.replace('\t', ' ');
    line.replace('\r', ' ');
    line.trim();
    if (line.length() == 0) continue;

    int sp = line.indexOf(' ');
    String a = sp < 0 ? line : line.substring(0, sp);
    String b = sp < 0 ? ""   : line.substring(sp + 1);
    b.trim();

    if (a == "fade") {
      long v;
      if (!parseInt(b, v) || v < 0 || v > 60000) { err = "line " + String(lineNo) + ": bad fade"; return false; }
      out.fadeMs = v;
      continue;
    }
    if (a == "base") {
      if (b == "keep") out.keepUnlisted = true;
      else if (b == "zero") out.keepUnlisted = false;
      else { err = "line " + String(lineNo) + ": base must be keep or zero"; return false; }
      continue;
    }
    if (a == "beep") {
      int sp2 = b.indexOf(' ');
      long f, ms = 100;
      if (!parseInt(sp2 < 0 ? b : b.substring(0, sp2), f) || f < 50 || f > 20000) { err = "line " + String(lineNo) + ": beep needs <freq 50-20000> [ms]"; return false; }
      if (sp2 >= 0) { String m = b.substring(sp2 + 1); m.trim(); if (!parseInt(m, ms) || ms < 1 || ms > 5000) { err = "line " + String(lineNo) + ": beep ms out of 1-5000"; return false; } }
      if (out.nBeeps < SCENE_MAX_BEEPS) { out.beeps[out.nBeeps].freq = f; out.beeps[out.nBeeps].ms = ms; out.nBeeps++; }
      continue;
    }

    long lo, hi, val;
    int dash = a.indexOf('-');
    if (dash > 0) {
      if (!parseInt(a.substring(0, dash), lo) || !parseInt(a.substring(dash + 1), hi)) {
        err = "line " + String(lineNo) + ": bad range"; return false;
      }
    } else {
      if (!parseInt(a, lo)) { err = "line " + String(lineNo) + ": bad channel '" + a + "'"; return false; }
      hi = lo;
    }
    if (!parseInt(b, val)) { err = "line " + String(lineNo) + ": bad value '" + b + "'"; return false; }
    if (lo < 1 || hi > DMX_CHANNELS || lo > hi) { err = "line " + String(lineNo) + ": channel out of 1-512"; return false; }
    if (val < 0 || val > 255) { err = "line " + String(lineNo) + ": value out of 0-255"; return false; }
    for (long c = lo; c <= hi; c++) { out.vals[c] = (uint8_t)val; out.set[c] = true; }
  }
  return true;
}

// ---------- trigger map ----------
void Store::loadTriggers() {
  for (int i = 0; i <= EQ_PIN_16; i++) g_trig[i] = "";
  String text;
  if (!readFile(String(FILE_TRIGGERS).substring(1), text)) { Serial.println("[store] no triggers.txt"); return; }
  int start = 0;
  while (start < (int)text.length()) {
    int nl = text.indexOf('\n', start);
    if (nl < 0) nl = text.length();
    String line = text.substring(start, nl);
    start = nl + 1;
    int hash = line.indexOf('#');
    if (hash >= 0) line = line.substring(0, hash);
    line.replace('=', ' ');
    line.replace('\t', ' ');
    line.replace('\r', ' ');
    line.trim();
    if (line.length() == 0) continue;
    int sp = line.indexOf(' ');
    if (sp < 0) continue;
    long in;
    if (!parseInt(line.substring(0, sp), in)) continue;
    if (in < IN_FIRST || in > IN_LAST_TRIGGER) continue;
    String scene = line.substring(sp + 1);
    scene.trim();
    g_trig[in] = normalizeSceneName(scene);
  }
  Serial.println("[store] triggers: " + triggersSummary());
}

String Store::sceneForInput(int input) {
  if (input < 0 || input > EQ_PIN_16) return "";
  return g_trig[input];
}

String Store::triggersSummary() {
  String s;
  for (int i = IN_FIRST; i <= IN_LAST_TRIGGER; i++)
    if (g_trig[i].length()) s += String(i) + "->" + g_trig[i] + " ";
  return s;
}
