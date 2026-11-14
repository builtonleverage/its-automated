// It's Automated - a tiny always-on bridge for WiZ smart lamps.
//
// An ESP32 joins your Wi-Fi, serves a phone-friendly web app, talks to WiZ
// bulbs over their local UDP API (port 38899) and runs routines by itself:
// fixed times, sunrise / sunset, scenes, fades and sleep timers.
//
// No cloud, no account, no hub.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <WiFiUdp.h>
#include <time.h>

#include "sun.h"
#include "web_assets.h"

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

static const char *HOSTNAME = "its-automated";
static const char *SETUP_AP = "Its-Automated-Setup";
static const char *CONFIG_PATH = "/config.json";
static const uint16_t WIZ_PORT = 38899;
static const uint16_t LOCAL_UDP_PORT = 38900;
static const int MAX_LAMPS = 8;
static const int MAX_ROUTINES = 40;
static const size_t MAX_BODY = 16384;
static const int LED_PIN = 2;

WebServer server(80);
WiFiUDP udp;
JsonDocument cfg; // settings, lamps, routines, presets (persisted)

// Live state per lamp. Not persisted.
struct LampRt {
  String mac;
  bool online = false;
  bool state = false;
  int dimming = 0;
  int temp = 0;
  int r = -1, g = -1, b = -1;
  int sceneId = 0;
  uint32_t lastPoll = 0;
  bool polled = false;
  // fade engine
  bool fading = false;
  int fadeFrom = 0, fadeTo = 0, fadeLastDim = -1;
  uint32_t fadeStart = 0, fadeDur = 0, fadeLastStep = 0;
  bool fadeOffAtEnd = false;
};
LampRt rt[MAX_LAMPS];

uint32_t lastDiscover = 0;
long lastMinuteStamp = 0;
String lastRunName;
time_t lastRunAt = 0;

// ---------------------------------------------------------------- config

void seedDefaults() {
  if (!cfg["settings"].is<JsonObject>()) {
    JsonObject s = cfg["settings"].to<JsonObject>();
    s["name"] = "Home";
    s["city"] = "Hyderabad";
    s["lat"] = 17.385;
    s["lon"] = 78.4867;
    s["tzOffsetMin"] = 330;
  }
  if (!cfg["lamps"].is<JsonArray>()) cfg["lamps"].to<JsonArray>();
  if (!cfg["routines"].is<JsonArray>()) cfg["routines"].to<JsonArray>();
  if (!cfg["presets"].is<JsonArray>()) {
    JsonArray p = cfg["presets"].to<JsonArray>();
    struct { const char *id, *name; int dim, temp; } seed[] = {
        {"focus", "Focus", 100, 5000}, {"relax", "Relax", 50, 2700}, {"movie", "Movie", 20, 2700}, {"night", "Night", 10, 2700}};
    for (auto &d : seed) {
      JsonObject o = p.add<JsonObject>();
      o["id"] = d.id;
      o["name"] = d.name;
      o["dimming"] = d.dim;
      o["temp"] = d.temp;
    }
  }
}

void loadCfg() {
  cfg.clear();
  File f = LittleFS.open(CONFIG_PATH, "r");
  if (f) {
    DeserializationError err = deserializeJson(cfg, f);
    f.close();
    if (err) {
      Serial.printf("config parse failed: %s\n", err.c_str());
      cfg.clear();
    }
  }
  seedDefaults();
}

// Write to flash, then reload so the in-memory document stays compact.
bool saveCfg() {
  File f = LittleFS.open(CONFIG_PATH, "w");
  if (!f) return false;
  size_t n = serializeJson(cfg, f);
  f.close();
  loadCfg();
  return n > 0;
}

int tzOffsetMin() { return cfg["settings"]["tzOffsetMin"] | 330; }

// ---------------------------------------------------------------- helpers

String hhmm(int minutes) {
  char b[6];
  minutes = ((minutes % 1440) + 1440) % 1440;
  snprintf(b, sizeof(b), "%02d:%02d", minutes / 60, minutes % 60);
  return String(b);
}

bool timeSynced() { return time(nullptr) > 1700000000; }

// Local wall-clock time, using the configured offset (no DST handling).
void localNow(struct tm &out, time_t &localEpoch) {
  localEpoch = time(nullptr) + (time_t)tzOffsetMin() * 60;
  gmtime_r(&localEpoch, &out);
}

LampRt &rtFor(const String &mac) {
  for (auto &l : rt)
    if (l.mac == mac) return l;
  for (auto &l : rt)
    if (l.mac.length() == 0) {
      l.mac = mac;
      return l;
    }
  rt[0] = LampRt();
  rt[0].mac = mac;
  return rt[0];
}

JsonObject findLamp(const String &mac) {
  for (JsonObject l : cfg["lamps"].as<JsonArray>())
    if (mac == (l["mac"] | "")) return l;
  return JsonObject();
}

// ---------------------------------------------------------------- WiZ UDP

void udpDrain() {
  char trash[64];
  while (udp.parsePacket() > 0)
    while (udp.available()) udp.read(trash, sizeof(trash));
}

// Send one JSON message to a bulb and wait for its reply.
bool wizRequest(const IPAddress &ip, const String &msg, JsonDocument *out, uint16_t timeoutMs = 350, uint8_t tries = 2) {
  static char buf[1024];
  for (uint8_t t = 0; t < tries; t++) {
    udpDrain();
    udp.beginPacket(ip, WIZ_PORT);
    udp.write((const uint8_t *)msg.c_str(), msg.length());
    if (!udp.endPacket()) {
      delay(20);
      continue;
    }
    uint32_t start = millis();
    while (millis() - start < timeoutMs) {
      int n = udp.parsePacket();
      if (n > 0) {
        int len = udp.read(buf, sizeof(buf) - 1);
        if (len < 0) len = 0;
        buf[len] = 0;
        if (udp.remoteIP() == ip) {
          if (!out) return true;
          out->clear();
          if (!deserializeJson(*out, buf)) return true;
        }
      }
      delay(4);
    }
  }
  return false;
}

// Broadcast on the LAN and collect every WiZ device that answers.
// addNew=false only refreshes the IP of lamps we already know (DHCP changes).
int discover(bool addNew) {
  lastDiscover = millis();
  IPAddress local = WiFi.localIP(), mask = WiFi.subnetMask(), bcast;
  for (int i = 0; i < 4; i++) bcast[i] = local[i] | (uint8_t)~mask[i];

  String reg = String("{\"method\":\"registration\",\"params\":{\"phoneMac\":\"AAAAAAAAAAAA\",\"register\":false,\"phoneIp\":\"") +
               local.toString() + "\",\"id\":\"1\"}}";
  const char *pilot = "{\"method\":\"getPilot\",\"params\":{}}";

  udpDrain();
  int found = 0;
  bool changed = false;
  static char buf[1024];
  for (int round = 0; round < 2; round++) {
    udp.beginPacket(bcast, WIZ_PORT);
    if (round == 0) udp.write((const uint8_t *)reg.c_str(), reg.length());
    else udp.write((const uint8_t *)pilot, strlen(pilot));
    udp.endPacket();

    uint32_t start = millis();
    while (millis() - start < 900) {
      int n = udp.parsePacket();
      if (n <= 0) {
        delay(5);
        continue;
      }
      int len = udp.read(buf, sizeof(buf) - 1);
      if (len <= 0) continue;
      buf[len] = 0;
      JsonDocument d;
      if (deserializeJson(d, buf)) continue;
      String mac = d["result"]["mac"] | "";
      if (mac.length() == 0) continue;
      String ip = udp.remoteIP().toString();

      JsonObject lamp = findLamp(mac);
      if (lamp.isNull()) {
        JsonArray lamps = cfg["lamps"].as<JsonArray>();
        if (!addNew || (int)lamps.size() >= MAX_LAMPS) continue;
        lamp = lamps.add<JsonObject>();
        lamp["mac"] = mac;
        lamp["ip"] = ip;
        String name = "Lamp";
        if (lamps.size() > 1) name += String(" ") + String((int)lamps.size());
        lamp["name"] = name;
        changed = true;
        found++;
      } else {
        if (ip != (lamp["ip"] | "")) {
          lamp["ip"] = ip;
          changed = true;
        }
        if (round == 0) found++;
      }
    }
  }
  if (changed) saveCfg();
  return found;
}

bool lampIp(JsonObjectConst lamp, IPAddress &ip) { return ip.fromString(lamp["ip"] | ""); }

// Ask a bulb for its current state and cache it.
bool pollLamp(JsonObjectConst lamp) {
  LampRt &s = rtFor(lamp["mac"] | "");
  s.lastPoll = millis();
  s.polled = true;
  IPAddress ip;
  JsonDocument resp;
  bool ok = lampIp(lamp, ip) && wizRequest(ip, "{\"method\":\"getPilot\",\"params\":{}}", &resp, 350, s.online ? 2 : 1) &&
            resp["result"].is<JsonObject>();
  if (!ok) {
    s.online = false;
    return false;
  }
  JsonObject r = resp["result"];
  s.online = true;
  s.state = r["state"] | false;
  s.dimming = r["dimming"] | 0;
  s.temp = r["temp"] | 0;
  s.sceneId = r["sceneId"] | 0;
  if (r["r"].is<int>()) {
    s.r = r["r"];
    s.g = r["g"] | 0;
    s.b = r["b"] | 0;
  } else {
    s.r = s.g = s.b = -1;
  }
  return true;
}

bool sendPilot(JsonObjectConst lamp, JsonDocument &msg) {
  IPAddress ip;
  if (!lampIp(lamp, ip)) return false;
  String out;
  serializeJson(msg, out);
  JsonDocument resp;
  LampRt &s = rtFor(lamp["mac"] | "");
  bool ok = wizRequest(ip, out, &resp, 400, 3) && (resp["result"]["success"] | false);
  s.lastPoll = 0; // force a fresh read on the next state request
  s.polled = false;
  if (!ok && !resp["error"].isNull()) Serial.printf("wiz error: %s\n", out.c_str());
  return ok;
}

bool sendDimming(JsonObjectConst lamp, int dim) {
  JsonDocument d;
  d["method"] = "setPilot";
  d["params"]["dimming"] = constrain(dim, 10, 100);
  return sendPilot(lamp, d);
}

bool sendOff(JsonObjectConst lamp) {
  JsonDocument d;
  d["method"] = "setPilot";
  d["params"]["state"] = false;
  return sendPilot(lamp, d);
}

// ---------------------------------------------------------------- actions

void startFade(LampRt &s, int from, int to, int minutes, bool offAtEnd) {
  s.fading = true;
  s.fadeFrom = from;
  s.fadeTo = to;
  s.fadeLastDim = from;
  s.fadeStart = millis();
  s.fadeLastStep = millis();
  s.fadeDur = (uint32_t)constrain(minutes, 1, 240) * 60000UL;
  s.fadeOffAtEnd = offAtEnd;
}

// Copy the "look" part of an action (scene, colour or white temperature).
void addLook(JsonObjectConst a, JsonObject p) {
  int scene = a["sceneId"] | 0;
  if (scene > 0) {
    p["sceneId"] = constrain(scene, 1, 35);
    if (a["speed"].is<int>()) p["speed"] = constrain(a["speed"].as<int>(), 20, 200);
  } else if (a["r"].is<int>()) {
    p["r"] = constrain(a["r"].as<int>(), 0, 255);
    p["g"] = constrain(a["g"] | 0, 0, 255);
    p["b"] = constrain(a["b"] | 0, 0, 255);
  } else if (a["temp"].is<int>()) {
    p["temp"] = constrain(a["temp"].as<int>(), 2200, 6500);
  }
}

// action: { power, dimming, temp | r,g,b | sceneId, fadeMin }
bool applyToLamp(JsonObjectConst lamp, JsonObjectConst a) {
  LampRt &s = rtFor(lamp["mac"] | "");
  s.fading = false; // any new command cancels a running fade
  bool power = a["power"] | true;
  int fadeMin = a["fadeMin"] | 0;
  int dim = a["dimming"] | 0;

  if (!power) {
    if (fadeMin > 0) {
      if (!pollLamp(lamp) || !s.state) return s.online; // already off
      startFade(s, s.dimming, 10, fadeMin, true);
      return true;
    }
    return sendOff(lamp);
  }

  JsonDocument d;
  d["method"] = "setPilot";
  JsonObject p = d["params"].to<JsonObject>();
  p["state"] = true;
  addLook(a, p);

  if (fadeMin > 0) {
    int target = constrain(dim > 0 ? dim : 100, 10, 100);
    bool on = pollLamp(lamp) && s.state;
    int from = on ? constrain(s.dimming, 10, 100) : 10;
    p["dimming"] = from;
    bool ok = sendPilot(lamp, d);
    if (ok && from != target) startFade(s, from, target, fadeMin, false);
    return ok;
  }
  if (dim > 0) p["dimming"] = constrain(dim, 10, 100);
  return sendPilot(lamp, d);
}

// target: "all" or a lamp MAC. Returns how many lamps accepted the command.
int applyAction(const String &target, JsonObjectConst a) {
  int ok = 0;
  for (JsonObject lamp : cfg["lamps"].as<JsonArray>()) {
    if (target != "all" && target.length() && target != (lamp["mac"] | "")) continue;
    if (applyToLamp(lamp, a)) ok++;
  }
  return ok;
}

void fadeTick() {
  for (auto &s : rt) {
    if (!s.fading) continue;
    uint32_t now = millis();
    uint32_t elapsed = now - s.fadeStart;
    bool done = elapsed >= s.fadeDur;
    if (!done && now - s.fadeLastStep < 2000) continue;
    JsonObject lamp = findLamp(s.mac);
    if (lamp.isNull()) {
      s.fading = false;
      continue;
    }
    float frac = done ? 1.0f : (float)elapsed / (float)s.fadeDur;
    int dim = s.fadeFrom + (int)lroundf((s.fadeTo - s.fadeFrom) * frac);
    if (dim != s.fadeLastDim) {
      sendDimming(lamp, dim);
      s.fadeLastDim = dim;
    }
    s.fadeLastStep = now;
    if (done) {
      s.fading = false;
      if (s.fadeOffAtEnd) sendOff(lamp);
    }
  }
}

// ---------------------------------------------------------------- routines

// Minute of the day a routine fires on the given date, or -1.
int routineMinute(JsonObjectConst r, const struct tm &lt) {
  JsonObjectConst trig = r["trigger"];
  String type = trig["type"] | "time";
  if (type == "time") {
    const char *t = trig["time"] | "";
    int h = 0, m = 0;
    if (sscanf(t, "%d:%d", &h, &m) != 2 || h < 0 || h > 23 || m < 0 || m > 59) return -1;
    return h * 60 + m;
  }
  int rise = 0, set = 0;
  double lat = cfg["settings"]["lat"] | 17.385, lon = cfg["settings"]["lon"] | 78.4867;
  if (!sun::times(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lat, lon, tzOffsetMin(), rise, set)) return -1;
  int base = type == "sunrise" ? rise : set;
  int offset = constrain(trig["offset"] | 0, -180, 180);
  return constrain(base + offset, 0, 1439);
}

bool routineRunsOn(JsonObjectConst r, int wday) {
  JsonArrayConst days = r["days"];
  if (days.isNull() || days.size() == 0) return true; // no days listed = every day
  for (JsonVariantConst d : days)
    if (d.as<int>() == wday) return true;
  return false;
}

void runRoutine(JsonObjectConst r) {
  String target = r["target"] | "all";
  int n = applyAction(target, r["action"].as<JsonObjectConst>());
  lastRunName = r["name"] | "Routine";
  lastRunAt = time(nullptr);
  Serial.printf("routine '%s' -> %d lamp(s)\n", lastRunName.c_str(), n);
}

void schedulerTick() {
  if (!timeSynced()) return;
  struct tm lt;
  time_t local;
  localNow(lt, local);
  long stamp = (long)(local / 60);
  if (stamp == lastMinuteStamp) return;
  // Catch up a few minutes if the loop was busy, but never replay a long gap.
  long from = (lastMinuteStamp == 0 || stamp - lastMinuteStamp > 3) ? stamp : lastMinuteStamp + 1;
  lastMinuteStamp = stamp;

  for (long m = from; m <= stamp; m++) {
    time_t when = (time_t)m * 60;
    struct tm t;
    gmtime_r(&when, &t);
    int minuteOfDay = t.tm_hour * 60 + t.tm_min;
    // Collect first: running a routine can reload cfg (IP refresh).
    String due[MAX_ROUTINES];
    int count = 0;
    for (JsonObjectConst r : cfg["routines"].as<JsonArrayConst>()) {
      if (!(r["enabled"] | true)) continue;
      if (!routineRunsOn(r, t.tm_wday)) continue;
      if (routineMinute(r, t) != minuteOfDay) continue;
      if (count < MAX_ROUTINES) due[count++] = r["id"] | "";
    }
    for (int i = 0; i < count; i++)
      for (JsonObjectConst r : cfg["routines"].as<JsonArrayConst>())
        if (due[i] == (r["id"] | "")) {
          runRoutine(r);
          break;
        }
  }
}

// ---------------------------------------------------------------- HTTP

void sendJson(int code, JsonDocument &doc) {
  String out;
  serializeJson(doc, out);
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json", out);
}

void sendOk(bool ok = true, const char *error = nullptr) {
  JsonDocument d;
  d["ok"] = ok;
  if (!ok && error) d["error"] = error;
  sendJson(ok ? 200 : 400, d);
}

// Parse the JSON request body. Sends a 400 and returns false on failure.
bool readBody(JsonDocument &doc) {
  String body = server.arg("plain");
  if (body.length() == 0 || body.length() > MAX_BODY || deserializeJson(doc, body)) {
    sendOk(false, "bad json");
    return false;
  }
  return true;
}

void handleState() {
  JsonDocument d;
  JsonObject dev = d["device"].to<JsonObject>();
  dev["version"] = APP_VERSION;
  dev["host"] = String(HOSTNAME) + ".local";
  dev["ip"] = WiFi.localIP().toString();
  dev["ssid"] = WiFi.SSID();
  dev["rssi"] = WiFi.RSSI();
  dev["uptime"] = millis() / 1000;
  dev["heap"] = ESP.getFreeHeap();

  JsonObject tm_ = d["time"].to<JsonObject>();
  tm_["synced"] = timeSynced();
  if (timeSynced()) {
    struct tm lt;
    time_t local;
    localNow(lt, local);
    tm_["local"] = hhmm(lt.tm_hour * 60 + lt.tm_min);
    tm_["dow"] = lt.tm_wday;
    char date[11];
    snprintf(date, sizeof(date), "%04d-%02d-%02d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
    tm_["date"] = date;
    int rise, set;
    double lat = cfg["settings"]["lat"] | 17.385, lon = cfg["settings"]["lon"] | 78.4867;
    if (sun::times(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lat, lon, tzOffsetMin(), rise, set)) {
      tm_["sunrise"] = hhmm(rise);
      tm_["sunset"] = hhmm(set);
    }
  }

  // Refresh lamp status. If one has gone missing, look for its new IP.
  bool missing = false;
  for (JsonObject lamp : cfg["lamps"].as<JsonArray>()) {
    LampRt &s = rtFor(lamp["mac"] | "");
    if (!s.polled || millis() - s.lastPoll > 1500) pollLamp(lamp);
    if (!s.online) missing = true;
  }
  if (missing && millis() - lastDiscover > 60000) discover(false);

  JsonArray lamps = d["lamps"].to<JsonArray>();
  for (JsonObject lamp : cfg["lamps"].as<JsonArray>()) {
    LampRt &s = rtFor(lamp["mac"] | "");
    JsonObject o = lamps.add<JsonObject>();
    o["mac"] = lamp["mac"];
    o["ip"] = lamp["ip"];
    o["name"] = lamp["name"];
    o["online"] = s.online;
    o["state"] = s.state;
    o["dimming"] = s.dimming;
    o["temp"] = s.temp;
    o["sceneId"] = s.sceneId;
    if (s.r >= 0) {
      o["r"] = s.r;
      o["g"] = s.g;
      o["b"] = s.b;
    }
    if (s.fading) {
      JsonObject f = o["fade"].to<JsonObject>();
      uint32_t elapsed = millis() - s.fadeStart;
      f["to"] = s.fadeTo;
      f["off"] = s.fadeOffAtEnd;
      f["remainingSec"] = elapsed >= s.fadeDur ? 0 : (s.fadeDur - elapsed) / 1000;
    }
  }

  d["settings"] = cfg["settings"];
  d["routines"] = cfg["routines"];
  d["presets"] = cfg["presets"];
  if (lastRunAt) {
    d["lastRun"]["name"] = lastRunName;
    d["lastRun"]["at"] = (long)lastRunAt;
  }
  sendJson(200, d);
}

void handleDiscover() {
  int found = discover(true);
  JsonDocument d;
  d["ok"] = true;
  d["found"] = found;
  sendJson(200, d);
}

void handleLampAdd() {
  JsonDocument in;
  if (!readBody(in)) return;
  IPAddress ip;
  if (!ip.fromString(in["ip"] | "")) return sendOk(false, "bad ip");
  JsonDocument resp;
  if (!wizRequest(ip, "{\"method\":\"getPilot\",\"params\":{}}", &resp, 500, 3)) return sendOk(false, "no WiZ lamp answered at that address");
  String mac = resp["result"]["mac"] | "";
  if (mac.length() == 0) return sendOk(false, "unexpected reply");
  JsonObject lamp = findLamp(mac);
  JsonArray lamps = cfg["lamps"].as<JsonArray>();
  if (lamp.isNull()) {
    if ((int)lamps.size() >= MAX_LAMPS) return sendOk(false, "lamp limit reached");
    lamp = lamps.add<JsonObject>();
    lamp["mac"] = mac;
    lamp["name"] = String(in["name"] | "Lamp");
  }
  lamp["ip"] = ip.toString();
  saveCfg();
  sendOk();
}

void handleLampUpdate() {
  JsonDocument in;
  if (!readBody(in)) return;
  JsonObject lamp = findLamp(in["mac"] | "");
  if (lamp.isNull()) return sendOk(false, "unknown lamp");
  String name = in["name"] | "";
  name.trim();
  if (name.length() == 0 || name.length() > 32) return sendOk(false, "bad name");
  lamp["name"] = name;
  saveCfg();
  sendOk();
}

void handleLampRemove() {
  JsonDocument in;
  if (!readBody(in)) return;
  String mac = in["mac"] | "";
  JsonArray lamps = cfg["lamps"].as<JsonArray>();
  for (size_t i = 0; i < lamps.size(); i++) {
    if (mac == (lamps[i]["mac"] | "")) {
      lamps.remove(i);
      for (auto &s : rt)
        if (s.mac == mac) s = LampRt();
      saveCfg();
      return sendOk();
    }
  }
  sendOk(false, "unknown lamp");
}

void handleControl() {
  JsonDocument in;
  if (!readBody(in)) return;
  if (!in["action"].is<JsonObject>()) return sendOk(false, "missing action");
  int n = applyAction(String(in["target"] | "all"), in["action"].as<JsonObjectConst>());
  JsonDocument d;
  d["ok"] = n > 0;
  d["lamps"] = n;
  if (n == 0) d["error"] = "lamp did not respond";
  sendJson(200, d);
}

void handleSleep() {
  JsonDocument in;
  if (!readBody(in)) return;
  String target = in["target"] | "all";
  int minutes = in["minutes"] | 0;
  if (minutes <= 0) { // cancel
    for (JsonObject lamp : cfg["lamps"].as<JsonArray>())
      if (target == "all" || target == (lamp["mac"] | "")) rtFor(lamp["mac"] | "").fading = false;
    return sendOk();
  }
  JsonDocument a;
  a["power"] = false;
  a["fadeMin"] = constrain(minutes, 1, 240);
  applyAction(target, a.as<JsonObjectConst>());
  sendOk();
}

void handleRoutines() {
  JsonDocument in;
  if (!readBody(in)) return;
  if (!in["routines"].is<JsonArray>() || in["routines"].size() > (size_t)MAX_ROUTINES) return sendOk(false, "bad routines");
  cfg["routines"].set(in["routines"]);
  sendOk(saveCfg(), "could not save");
}

void handleRoutineRun() {
  JsonDocument in;
  if (!readBody(in)) return;
  String id = in["id"] | "";
  for (JsonObjectConst r : cfg["routines"].as<JsonArrayConst>())
    if (id == (r["id"] | "")) {
      runRoutine(r);
      return sendOk();
    }
  sendOk(false, "unknown routine");
}

void handlePresets() {
  JsonDocument in;
  if (!readBody(in)) return;
  if (!in["presets"].is<JsonArray>() || in["presets"].size() > 24) return sendOk(false, "bad presets");
  cfg["presets"].set(in["presets"]);
  sendOk(saveCfg(), "could not save");
}

void handleSettings() {
  JsonDocument in;
  if (!readBody(in)) return;
  JsonObject s = cfg["settings"];
  if (in["name"].is<const char *>()) s["name"] = in["name"];
  if (in["city"].is<const char *>()) s["city"] = in["city"];
  if (in["lat"].is<float>()) s["lat"] = constrain(in["lat"].as<float>(), -89.0f, 89.0f);
  if (in["lon"].is<float>()) s["lon"] = constrain(in["lon"].as<float>(), -180.0f, 180.0f);
  if (in["tzOffsetMin"].is<int>()) s["tzOffsetMin"] = constrain(in["tzOffsetMin"].as<int>(), -720, 840);
  lastMinuteStamp = 0; // clock basis may have changed; don't replay minutes
  sendOk(saveCfg(), "could not save");
}

void handleWifiReset() {
  sendOk();
  delay(300);
  WiFiManager wm;
  wm.resetSettings();
  ESP.restart();
}

void handleStatic() {
  String path = server.uri();
  if (path == "/") path = "/index.html";
  for (size_t i = 0; i < WEB_ASSET_COUNT; i++) {
    if (path == WEB_ASSETS[i].path) {
      server.sendHeader("Content-Encoding", "gzip");
      server.sendHeader("Cache-Control", path.endsWith(".png") ? "max-age=86400" : "no-cache");
      server.send_P(200, WEB_ASSETS[i].mime, (PGM_P)WEB_ASSETS[i].data, WEB_ASSETS[i].len);
      return;
    }
  }
  server.send(404, "text/plain", "not found");
}

// ---------------------------------------------------------------- setup / loop

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  if (!LittleFS.begin(true)) Serial.println("LittleFS mount failed");
  loadCfg();

  // First boot: opens a "Its-Automated-Setup" hotspot with a Wi-Fi picker.
  WiFi.setHostname(HOSTNAME);
  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  wm.setConnectTimeout(30);
  if (!wm.autoConnect(SETUP_AP)) {
    Serial.println("no Wi-Fi, restarting");
    delay(1000);
    ESP.restart();
  }
  WiFi.setAutoReconnect(true);
  digitalWrite(LED_PIN, HIGH);
  Serial.printf("connected: http://%s  (http://%s.local)\n", WiFi.localIP().toString().c_str(), HOSTNAME);

  configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com"); // keep UTC, offset applied in code
  if (MDNS.begin(HOSTNAME)) MDNS.addService("http", "tcp", 80);
  udp.begin(LOCAL_UDP_PORT);

  server.on("/api/state", HTTP_GET, handleState);
  server.on("/api/discover", HTTP_POST, handleDiscover);
  server.on("/api/lamps/add", HTTP_POST, handleLampAdd);
  server.on("/api/lamps/update", HTTP_POST, handleLampUpdate);
  server.on("/api/lamps/remove", HTTP_POST, handleLampRemove);
  server.on("/api/control", HTTP_POST, handleControl);
  server.on("/api/sleep", HTTP_POST, handleSleep);
  server.on("/api/routines", HTTP_POST, handleRoutines);
  server.on("/api/routines/run", HTTP_POST, handleRoutineRun);
  server.on("/api/presets", HTTP_POST, handlePresets);
  server.on("/api/settings", HTTP_POST, handleSettings);
  server.on("/api/wifi/reset", HTTP_POST, handleWifiReset);
  server.onNotFound(handleStatic);
  server.begin();

  if (cfg["lamps"].size() == 0) discover(true); // find lamps on first boot
}

void loop() {
  server.handleClient();
  fadeTick();

  static uint32_t lastSched = 0;
  if (millis() - lastSched >= 1000) {
    lastSched = millis();
    schedulerTick();
    digitalWrite(LED_PIN, WiFi.status() == WL_CONNECTED ? HIGH : LOW);
  }
  delay(2);
}
