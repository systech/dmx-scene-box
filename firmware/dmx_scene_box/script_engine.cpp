#include "script_engine.h"
#include "config.h"
#include "src/lua/lua.hpp"

// ---- limits: a bad script must never take the box down ----
#define SCRIPT_TICK_MS        20
#define SCRIPT_MEM_LIMIT      (64 * 1024)   // bytes of Lua heap
#define SCRIPT_MAX_INSTR      400000        // VM instructions per update() call
#define SCRIPT_HOOK_EVERY     1000
#define SCRIPT_TASK_STACK     12288

static DmxEngine*        g_dmx = nullptr;
static Script::BeepFn    g_beep = nullptr;
static lua_State*        L = nullptr;
static SemaphoreHandle_t g_lock = nullptr;
static bool              g_running = false;
static String            g_name, g_err;
static uint32_t          g_t0 = 0, g_ticks = 0;
static size_t            g_mem = 0;
static uint32_t          g_instr = 0;
static uint8_t           g_frame[DMX_CHANNELS + 1];

// ---- bounded allocator ----
static void* l_alloc(void*, void* ptr, size_t osize, size_t nsize) {
  if (nsize == 0) { if (ptr) { g_mem -= osize; free(ptr); } return nullptr; }
  size_t newTotal = g_mem + nsize - (ptr ? osize : 0);
  if (newTotal > SCRIPT_MEM_LIMIT) return nullptr;        // Lua raises "not enough memory"
  void* p = realloc(ptr, nsize);
  if (p) g_mem = newTotal;
  return p;
}

// ---- instruction budget per tick ----
static void l_hook(lua_State* S, lua_Debug*) {
  g_instr += SCRIPT_HOOK_EVERY;
  if (g_instr > SCRIPT_MAX_INSTR) luaL_error(S, "script too slow: over %d instructions in one update()", SCRIPT_MAX_INSTR);
}

// ---- dmx.* vocabulary ----
static int clampi(lua_Integer v, lua_Integer lo, lua_Integer hi) { return v < lo ? lo : (v > hi ? hi : v); }

static int l_set(lua_State* S) {
  lua_Integer ch = luaL_checkinteger(S, 1);
  lua_Integer v  = (lua_Integer)luaL_checknumber(S, 2);
  if (ch < 1 || ch > DMX_CHANNELS) return luaL_error(S, "dmx.set: channel %d out of 1-%d", (int)ch, DMX_CHANNELS);
  g_frame[ch] = (uint8_t)clampi(v, 0, 255);
  return 0;
}
static int l_fill(lua_State* S) {
  lua_Integer lo = luaL_checkinteger(S, 1), hi = luaL_checkinteger(S, 2);
  lua_Integer v  = (lua_Integer)luaL_checknumber(S, 3);
  if (lo < 1 || hi > DMX_CHANNELS || lo > hi) return luaL_error(S, "dmx.fill: bad range %d-%d", (int)lo, (int)hi);
  uint8_t b = (uint8_t)clampi(v, 0, 255);
  for (lua_Integer c = lo; c <= hi; c++) g_frame[c] = b;
  return 0;
}
static int l_get(lua_State* S) {
  lua_Integer ch = luaL_checkinteger(S, 1);
  if (ch < 1 || ch > DMX_CHANNELS) return luaL_error(S, "dmx.get: channel %d out of 1-%d", (int)ch, DMX_CHANNELS);
  lua_pushinteger(S, g_frame[ch]);
  return 1;
}
static int l_beep(lua_State* S) {
  lua_Integer f = luaL_checkinteger(S, 1), ms = luaL_optinteger(S, 2, 100);
  if (g_beep) g_beep((uint32_t)clampi(f, 50, 20000), (uint32_t)clampi(ms, 1, 5000));
  return 0;
}
static int l_log(lua_State* S) {
  int n = lua_gettop(S);
  String out = "[lua] ";
  for (int i = 1; i <= n; i++) {
    size_t len; const char* s = luaL_tolstring(S, i, &len);
    out += s; if (i < n) out += " ";
    lua_pop(S, 1);
  }
  Serial.println(out);
  return 0;
}

static const luaL_Reg dmxlib[] = {
  {"set", l_set}, {"fill", l_fill}, {"get", l_get}, {"beep", l_beep}, {nullptr, nullptr}
};

static void openLibs(lua_State* S) {
  luaL_requiref(S, LUA_GNAME, luaopen_base, 1);   lua_pop(S, 1);
  luaL_requiref(S, LUA_MATHLIBNAME, luaopen_math, 1);     lua_pop(S, 1);
  luaL_requiref(S, LUA_STRLIBNAME, luaopen_string, 1);    lua_pop(S, 1);
  luaL_requiref(S, LUA_TABLIBNAME, luaopen_table, 1);     lua_pop(S, 1);
  luaL_newlib(S, dmxlib);
  lua_pushinteger(S, DMX_CHANNELS); lua_setfield(S, -2, "channels");
  lua_pushinteger(S, SCRIPT_TICK_MS); lua_setfield(S, -2, "tick_ms");
  lua_setglobal(S, "dmx");
  lua_pushcfunction(S, l_log); lua_setglobal(S, "log");
  lua_pushcfunction(S, l_log); lua_setglobal(S, "print");
  // no io, os, package, debug, dofile, load from files
  lua_pushnil(S); lua_setglobal(S, "dofile");
  lua_pushnil(S); lua_setglobal(S, "loadfile");
}

static void closeState() {
  if (L) { lua_close(L); L = nullptr; }
  g_running = false;
  g_mem = 0;
}

// ---- task ----
static void scriptTask(void*) {
  TickType_t last = xTaskGetTickCount();
  for (;;) {
    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (g_running && L) {
      g_instr = 0;
      lua_getglobal(L, "update");
      lua_pushinteger(L, (lua_Integer)(millis() - g_t0));
      if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
        g_err = lua_tostring(L, -1);
        Serial.println("[script] " + g_name + " stopped: " + g_err);
        lua_pop(L, 1);
        g_running = false;          // frame stays as it was: the hall never goes black because of a typo
      } else {
        g_ticks++;
        g_dmx->setTarget(g_frame, 0);
      }
    }
    xSemaphoreGive(g_lock);
    vTaskDelayUntil(&last, pdMS_TO_TICKS(SCRIPT_TICK_MS));
  }
}

void Script::begin(DmxEngine* dmx, BeepFn beep) {
  g_dmx = dmx; g_beep = beep;
  g_lock = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(scriptTask, "lua", SCRIPT_TASK_STACK, nullptr, 2, nullptr, 1);
}

bool Script::start(const String& code, const String& name, String& err) {
  xSemaphoreTake(g_lock, portMAX_DELAY);
  closeState();
  memset(g_frame, 0, sizeof(g_frame));
  g_name = name; g_err = ""; g_ticks = 0;

  L = lua_newstate(l_alloc, nullptr);
  if (!L) { err = "cannot create Lua state"; xSemaphoreGive(g_lock); return false; }
  openLibs(L);
  lua_sethook(L, l_hook, LUA_MASKCOUNT, SCRIPT_HOOK_EVERY);

  bool ok = true;
  g_instr = 0;
  if (luaL_loadbufferx(L, code.c_str(), code.length(), name.c_str(), "t") != LUA_OK ||
      lua_pcall(L, 0, 0, 0) != LUA_OK) {
    err = lua_tostring(L, -1); ok = false;
  }
  if (ok) {
    lua_getglobal(L, "update");
    if (!lua_isfunction(L, -1)) { err = "no update(t) function"; ok = false; }
    lua_pop(L, 1);
  }
  if (ok) {
    lua_getglobal(L, "setup");
    if (lua_isfunction(L, -1)) {
      g_instr = 0;
      if (lua_pcall(L, 0, 0, 0) != LUA_OK) { err = String("setup: ") + lua_tostring(L, -1); ok = false; }
    } else lua_pop(L, 1);
  }
  if (!ok) { g_err = err; closeState(); xSemaphoreGive(g_lock); return false; }

  g_t0 = millis();
  g_running = true;
  Serial.printf("[script] %s running, %u bytes Lua heap\n", name.c_str(), (unsigned)g_mem);
  xSemaphoreGive(g_lock);
  return true;
}

bool Script::check(const String& code, const String& name, String& err) {
  // Separate throwaway state with the plain allocator: does not touch the running script.
  lua_State* T = luaL_newstate();
  if (!T) { err = "cannot create Lua state"; return false; }
  bool ok = luaL_loadbufferx(T, code.c_str(), code.length(), name.c_str(), "t") == LUA_OK;
  if (!ok) err = lua_tostring(T, -1);
  else if (code.indexOf("function update") < 0 && code.indexOf("update =") < 0 && code.indexOf("update=") < 0) {
    ok = false; err = "no update(t) function";
  }
  lua_close(T);
  return ok;
}

void Script::stop() {
  xSemaphoreTake(g_lock, portMAX_DELAY);
  if (L) Serial.println("[script] " + g_name + " stopped");
  closeState();
  xSemaphoreGive(g_lock);
}

bool     Script::running()    { return g_running; }
String   Script::name()       { return g_name; }
String   Script::lastError()  { return g_err; }
uint32_t Script::ticks()      { return g_ticks; }
size_t   Script::memoryUsed() { return g_mem; }
