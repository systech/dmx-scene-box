#include "api_server.h"
#include <WebServer.h>
#include <uri/UriBraces.h>
#include "config.h"
#include "scene_store.h"
#include "script_engine.h"

static WebServer server(HTTP_PORT);
static bool g_running = false;
static Api::PlayFn g_play;
static Api::StatusFn g_status;
static Api::ProbeFn g_probe;

static void handleIndex() {
  String h = "<!doctype html><title>DMX Scene Box</title><body style=\"font-family:sans-serif\">"
             "<h2>DMX Scene Box " FW_VERSION " &mdash; setup mode</h2><pre>" + g_status() + "</pre><ul>";
  String list = Store::listJson();
  int p = 0;
  while ((p = list.indexOf("\"name\":\"", p)) >= 0) {
    p += 8;
    int e = list.indexOf('"', p);
    String n = list.substring(p, e);
    h += "<li><a href=\"/api/files/" + n + "\">" + n + "</a>";
    if (n.endsWith(SCENE_EXT)) h += " &nbsp;<a href=\"/api/play/" + n + "\">[play]</a>";
    h += "</li>";
  }
  h += "</ul><p>API: see <a href=\"/api/files/description.yaml\">description.yaml</a></p></body>";
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "text/html", h);
}

static const String NAME_TRIGGERS = String(FILE_TRIGGERS).substring(1);
static const String NAME_EDITING  = String(FILE_EDITING).substring(1);

static void sendText(int code, const String& body) {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(code, "text/plain; charset=utf-8", body);
}
static void sendJson(int code, const String& body) {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(code, "application/json", body);
}

static void handleProbe() {
  if (!server.hasArg("ch")) { sendText(400, "usage: /api/probe?ch=N[&to=M][&v=255]\n"); return; }
  int lo = server.arg("ch").toInt();
  int hi = server.hasArg("to") ? server.arg("to").toInt() : lo;
  int v  = server.hasArg("v")  ? server.arg("v").toInt()  : 255;
  if (lo < 1 || hi > DMX_CHANNELS || lo > hi) { sendText(400, "channel range out of 1-512\n"); return; }
  if (v < 0 || v > 255) { sendText(400, "v out of 0-255\n"); return; }
  g_probe(lo, hi, v);
  sendText(200, "probe " + String(lo) + "-" + String(hi) + " = " + String(v) + ", all other channels 0\n");
}

static void handleStatus() { sendJson(200, g_status()); }
static void handleList()   { sendJson(200, Store::listJson()); }

static void handleGetFile() {
  String name = server.pathArg(0);
  if (!Store::validName(name)) { sendText(400, "bad name\n"); return; }
  String body;
  if (!Store::readFile(name, body)) { sendText(404, "not found\n"); return; }
  sendText(200, body);
}

static void handlePutFile() {
  String name = server.pathArg(0);
  if (!Store::validName(name)) { sendText(400, "bad name\n"); return; }
  if (!server.hasArg("plain")) {
    sendText(400, "empty body: send the file as the raw request body with header Content-Type: text/plain\n");
    return;
  }
  String body = server.arg("plain");
  if (body.length() > MAX_FILE_BYTES) { sendText(413, "too large\n"); return; }

  // Validate scenes before storing, so a broken file never lands on the device.
  if (name.endsWith(SCENE_EXT)) {
    SceneData sd;
    String err;
    if (!Store::parseScene(body, sd, err)) { sendText(422, "scene error: " + err + "\n"); return; }
    if (sd.isScript && !Script::check(body, name, err)) { sendText(422, "script error: " + err + "\n"); return; }
  }
  if (!Store::writeFile(name, body)) { sendText(500, "write failed\n"); return; }
  Serial.printf("[api] wrote %s (%u bytes)\n", name.c_str(), body.length());

  String note = "ok\n";
  if (name == NAME_TRIGGERS) {
    Store::loadTriggers();
    note = "ok, triggers reloaded: " + Store::triggersSummary() + "\n";
  }
  if (name == NAME_EDITING) {
    String err;
    if (g_play(name, err)) note = "ok, playing\n";
    else note = "stored, play failed: " + err + "\n";
  }
  sendText(200, note);
}

static void handleDeleteFile() {
  String name = server.pathArg(0);
  if (!Store::validName(name)) { sendText(400, "bad name\n"); return; }
  if (!Store::exists(name)) { sendText(404, "not found\n"); return; }
  if (!Store::removeFile(name)) { sendText(500, "delete failed\n"); return; }
  if (name == NAME_TRIGGERS) Store::loadTriggers();
  sendText(200, "deleted\n");
}

static void handlePlay() {
  String name = Store::normalizeSceneName(server.pathArg(0));
  if (!Store::validName(name)) { sendText(400, "bad name\n"); return; }
  String err;
  if (!g_play(name, err)) { sendText(422, err + "\n"); return; }
  sendText(200, "playing " + name + "\n");
}

static void handleNotFound() {
  if (server.method() == HTTP_OPTIONS) {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Access-Control-Allow-Methods", "GET,PUT,POST,DELETE,OPTIONS");
    server.sendHeader("Access-Control-Allow-Headers", "*");
    server.send(204);
    return;
  }
  sendText(404, "no such endpoint\n");
}

void Api::begin(PlayFn play, StatusFn status, ProbeFn probe) {
  g_play = play;
  g_status = status;
  g_probe = probe;
  server.on("/", HTTP_GET, handleIndex);
  server.on("/api/probe", HTTP_GET, handleProbe);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/files", HTTP_GET, handleList);
  server.on(UriBraces("/api/files/{}"), HTTP_GET, handleGetFile);
  server.on(UriBraces("/api/files/{}"), HTTP_PUT, handlePutFile);
  server.on(UriBraces("/api/files/{}"), HTTP_POST, handlePutFile);
  server.on(UriBraces("/api/files/{}"), HTTP_DELETE, handleDeleteFile);
  server.on(UriBraces("/api/play/{}"), HTTP_POST, handlePlay);
  server.on(UriBraces("/api/play/{}"), HTTP_GET, handlePlay);   // convenience for browsers
  server.onNotFound(handleNotFound);
}

void Api::start() {
  if (g_running) return;
  server.begin();
  g_running = true;
  Serial.println("[api] listening on port " + String(HTTP_PORT));
}

void Api::stop() {
  if (!g_running) return;
  server.stop();
  g_running = false;
  Serial.println("[api] stopped");
}

bool Api::running() { return g_running; }
void Api::handle() { if (g_running) server.handleClient(); }
