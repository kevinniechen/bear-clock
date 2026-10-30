// Bear clock firmware: countdown + phrases on a 64x32 HUB75 panel, guest
// messages via ntfy.sh (QR page) or the board's own captive-portal hotspot.
//
// Threads:
//   loop()      core 1: owns BearClock, renders ~40 fps, applies commands.
//   netTask     core 0: Wi-Fi station + ntfy.sh JSON stream + status publish.
//   webTask     core 0: hotspot DNS + HTTP server.
// Commands from every source go through cmdQueue so only loop() touches state.
#include <Arduino.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <DNSServer.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiMulti.h>
#include <Wire.h>
#include <esp_system.h>

#include "bearcore.h"
#include "config.h"
#include "secrets.h"
#include "web_assets.h"

// Default 8 KB is tight with ArduinoJson + command buffers on the loop task.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

using bear::BearClock;
using bear::Frame;
using bear::RGB;

// ---------------------------------------------------------------------------
// Shared state

struct Cmd {
  bool admin;      // admin JSON command vs. guest text
  char body[2048];  // phrases lists can be long; oversized bodies are rejected
};
static QueueHandle_t cmdQueue;

struct Status {
  uint32_t remSec;
  uint32_t durationSec;
  bool running;
  int queue;
  char current[bear::MAX_TEXT + 1];
  char from[bear::MAX_NAME + 1];
  int history;
  uint8_t brightness;
  bool guests;
};
static Status status;
static portMUX_TYPE statusMux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool publishStatus = false;  // net task posts status to ntfy
// ntfy.sh allows 250 published messages/day per IP (shared with phones on the
// same hotspot). Heartbeat 15 min = 96/day, plus one per command/button press.
// web/index.html treats 35 min of silence as offline.
static const uint32_t STATUS_EVERY_MS = 15 * 60 * 1000;
static volatile bool ntfyConnected = false;

static bool enqueue(bool admin, const char* body) {
  Cmd c;
  c.admin = admin;
  if (strlcpy(c.body, body, sizeof(c.body)) >= sizeof(c.body)) {
    Serial.printf("rejected %u-byte command (max %u)\n", (unsigned)strlen(body),
                  (unsigned)sizeof(c.body) - 1);
    return false;
  }
  return xQueueSend(cmdQueue, &c, 0) == pdTRUE;
}

static String statusJson() {
  Status s;
  portENTER_CRITICAL(&statusMux);
  s = status;
  portEXIT_CRITICAL(&statusMux);
  JsonDocument d;
  d["remaining"] = s.remSec;
  d["duration"] = s.durationSec;
  d["running"] = s.running;
  d["queue"] = s.queue;
  d["current"] = s.current;
  d["from"] = s.from;
  d["history"] = s.history;
  d["brightness"] = s.brightness;
  d["guests"] = s.guests;
  d["wifi"] = WiFi.status() == WL_CONNECTED ? WiFi.SSID() : String("");
  d["ip"] = WiFi.localIP().toString();
  d["ntfy"] = (bool)ntfyConnected;
  d["uptime"] = millis() / 1000;
  d["heap"] = ESP.getFreeHeap();
  d["minHeap"] = ESP.getMinFreeHeap();
  d["maxAlloc"] = ESP.getMaxAllocHeap();  // largest block: TLS needs ~40 KB
  d["reset"] = (int)esp_reset_reason();  // 1 power-on, 3 sw, 4 panic, 6/7 watchdog, 9 brownout
  String out;
  serializeJson(d, out);
  return out;
}

// ---------------------------------------------------------------------------
// Panel

static MatrixPanel_I2S_DMA* panel = nullptr;
static bool panelOk = false;
static BearClock clk;
static Frame frame, shown;
static int order[3] = {0, 1, 2};  // framebuffer channel sent as panel R,G,B

static void panelBegin() {
  HUB75_I2S_CFG::i2s_pins pins = {PIN_R1, PIN_G1, PIN_B1, PIN_R2, PIN_G2, PIN_B2, PIN_A,
                                  PIN_B,  PIN_C,  PIN_D,  PIN_E,  PIN_LAT, PIN_OE, PIN_CLK};
  HUB75_I2S_CFG cfg(PANEL_W, PANEL_H, 1, pins);
  cfg.clkphase = PANEL_CLKPHASE;
  if (PANEL_FM6126A) cfg.driver = HUB75_I2S_CFG::FM6126A;
  panel = new MatrixPanel_I2S_DMA(cfg);
  panelOk = panel->begin();
  if (!panelOk) {
    Serial.println("!! panel begin failed (out of DMA memory?)");
    return;
  }
  panel->setBrightness8(clk.settings.brightness);
  panel->clearScreen();

  const char* o = PANEL_RGB_ORDER;
  for (int i = 0; i < 3; i++) order[i] = o[i] == 'R' ? 0 : o[i] == 'G' ? 1 : 2;
  memset(&shown, 0, sizeof(shown));
}

// Solid R, G, B, white with the letter shown: wrong colors -> PANEL_RGB_ORDER,
// garbled or ghosted -> PANEL_FM6126A / PANEL_CLKPHASE (see config.h).
static void panelSelfTest() {
  if (!panelOk) return;
  const struct { RGB c; const char* l; } steps[] = {
      {{255, 0, 0}, "R"}, {{0, 255, 0}, "G"}, {{0, 0, 255}, "B"}, {{255, 255, 255}, "W"}};
  for (auto& st : steps) {
    Frame f;
    f.clear(bear::scale(st.c, 1, 3));
    bear::drawText(f, 26, 9, st.l, {0, 0, 0}, 2);
    for (int y = 0; y < bear::H; y++)
      for (int x = 0; x < bear::W; x++) {
        RGB c = f.px[y][x];
        uint8_t ch[3] = {c.r, c.g, c.b};
        panel->drawPixelRGB888(PANEL_ROTATE_180 ? bear::W - 1 - x : x,
                               PANEL_ROTATE_180 ? bear::H - 1 - y : y, ch[order[0]], ch[order[1]],
                               ch[order[2]]);
      }
    delay(350);
  }
  panel->clearScreen();
}

// Pushes only changed pixels; the DMA engine keeps refreshing on its own.
static void panelShow() {
  if (!panelOk) return;
  for (int y = 0; y < bear::H; y++) {
    for (int x = 0; x < bear::W; x++) {
      RGB c = frame.px[y][x];
      if (c == shown.px[y][x]) continue;
      shown.px[y][x] = c;
      uint8_t ch[3] = {c.r, c.g, c.b};
      int px = PANEL_ROTATE_180 ? bear::W - 1 - x : x;
      int py = PANEL_ROTATE_180 ? bear::H - 1 - y : y;
      panel->drawPixelRGB888(px, py, ch[order[0]], ch[order[1]], ch[order[2]]);
    }
  }
}

// ---------------------------------------------------------------------------
// Persistence (NVS)

static Preferences prefs;
static const uint32_t SETTINGS_VERSION = 2;  // bump when Settings changes
static uint32_t lastSavedRemSec = UINT32_MAX;
static bool lastSavedRunning = false;

static void saveSettings() {
  prefs.putUInt("ver", SETTINGS_VERSION);
  prefs.putBytes("settings", &clk.settings, sizeof(clk.settings));
  String joined;
  for (int i = 0; i < clk.phraseCount(); i++) {
    if (i) joined += '\n';
    joined += clk.phrase(i);
  }
  prefs.putString("phrases", joined);
}

static void saveTimer(uint32_t nowMs, bool force) {
  uint32_t rem = clk.remainingMs(nowMs) / 1000;
  bool run = clk.running();
  if (rem == lastSavedRemSec && run == lastSavedRunning) return;
  // While running, write at most every ~10 s of countdown (NVS wear).
  // (Unsigned math: if time was added, rem > last and the difference is huge.)
  if (!force && run == lastSavedRunning && run && lastSavedRemSec - rem < 10) return;
  prefs.putUInt("rem", rem);
  prefs.putBool("running", run);
  lastSavedRemSec = rem;
  lastSavedRunning = run;
}

static void setPhrasesFromJoined(const String& joined) {
  static char buf[bear::MAX_PHRASES][bear::MAX_TEXT + 1];
  const char* ptrs[bear::MAX_PHRASES];
  int n = 0, start = 0;
  while (n < bear::MAX_PHRASES && start <= (int)joined.length()) {
    int nl = joined.indexOf('\n', start);
    if (nl < 0) nl = joined.length();
    String line = joined.substring(start, nl);
    line.trim();
    if (line.length()) {
      strlcpy(buf[n], line.c_str(), sizeof(buf[n]));
      ptrs[n] = buf[n];
      n++;
    }
    start = nl + 1;
  }
  if (n) clk.setPhrases(ptrs, n);
}

static void loadState() {
  prefs.begin("bear", false);
  if (prefs.getUInt("ver", 0) == SETTINGS_VERSION &&
      prefs.getBytesLength("settings") == sizeof(clk.settings)) {
    prefs.getBytes("settings", &clk.settings, sizeof(clk.settings));
    clk.settings.zeroText[sizeof(clk.settings.zeroText) - 1] = 0;
    String p = prefs.getString("phrases", "");
    if (p.length()) setPhrasesFromJoined(p);
  }
  clk.setDuration(clk.settings.durationSec);
  if (prefs.isKey("rem")) {
    clk.setRemaining(prefs.getUInt("rem"), millis());
    // resume(), not start(): a saved 0 means TIME'S UP, not "restart".
    if (prefs.getBool("running", false)) clk.resume(millis());
  }
  lastSavedRemSec = clk.remainingMs(millis()) / 1000;
  lastSavedRunning = clk.running();

  size_t hn = prefs.getBytesLength("hist");
  if (hn && hn % sizeof(bear::Message) == 0 && hn <= sizeof(bear::Message) * bear::MAX_HISTORY) {
    static bear::Message buf[bear::MAX_HISTORY];
    prefs.getBytes("hist", buf, hn);
    for (auto& m : buf) { m.text[bear::MAX_TEXT] = 0; m.from[bear::MAX_NAME] = 0; }
    clk.restoreHistory(buf, hn / sizeof(bear::Message));
  }
}

// Replay history survives reboots; written at most every 5 s when it changes.
static void saveHistory(uint32_t nowMs) {
  static uint32_t savedVer = 0, lastMs = 0;
  if (clk.historyVersion() == savedVer || nowMs - lastMs < 5000) return;
  static bear::Message buf[bear::MAX_HISTORY];
  int n = clk.historyCount();
  for (int i = 0; i < n; i++) buf[i] = clk.history(i);
  if (n) prefs.putBytes("hist", buf, n * sizeof(bear::Message));
  else prefs.remove("hist");
  savedVer = clk.historyVersion();
  lastMs = nowMs;
}

// ---------------------------------------------------------------------------
// Commands

static bool parseHexColor(const char* s, RGB& out) {
  if (!s) return false;
  if (*s == '#') s++;
  if (strlen(s) != 6) return false;
  char* end;
  uint32_t v = strtoul(s, &end, 16);
  if (*end) return false;
  out = {uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
  return true;
}

// Returns false for unknown/invalid commands.
static bool applyAdmin(JsonDocument& d, uint32_t now) {
  const char* cmd = d["cmd"] | "";
  bool settingsChanged = false;
  auto& st = clk.settings;

  if (!strcmp(cmd, "start")) clk.start(now);
  else if (!strcmp(cmd, "pause")) clk.pause(now);
  else if (!strcmp(cmd, "toggle")) clk.toggle(now);
  else if (!strcmp(cmd, "reset")) clk.reset(now);
  else if (!strcmp(cmd, "set")) {
    // {"cmd":"set","h":3,"m":0,"s":0,"start":true} sets the full duration.
    // Floats allowed ("h":1.5); ArduinoJson's `| 0L` would turn them into 0.
    long sec = lround((d["h"] | 0.0) * 3600 + (d["m"] | 0.0) * 60 + (d["s"] | 0.0));
    if (sec <= 0 || sec > 9999L * 3600) return false;
    clk.setDuration((uint32_t)sec);
    if (d["start"] | false) clk.start(now);
    settingsChanged = true;
  } else if (!strcmp(cmd, "remaining")) {
    // Jump the current countdown without changing the full duration.
    long sec = d["sec"] | 0L;
    if (sec < 0 || sec > 9999L * 3600) return false;
    clk.setRemaining((uint32_t)sec, now);
  } else if (!strcmp(cmd, "add")) clk.addSeconds(d["sec"] | 0, now);
  else if (!strcmp(cmd, "flash")) clk.flashNow(d["text"] | "", now);
  else if (!strcmp(cmd, "msg")) {
    // Admin-posted message: bypasses the guests-off switch.
    bool g = st.guestMessages;
    st.guestMessages = true;
    clk.pushGuest(d["text"] | "", d["name"] | "", now);
    st.guestMessages = g;
  } else if (!strcmp(cmd, "skip")) clk.skip(now);
  else if (!strcmp(cmd, "clear")) clk.clearQueue(now);
  else if (!strcmp(cmd, "replay")) { st.replay = d["on"] | true; settingsChanged = true; }
  else if (!strcmp(cmd, "clearhistory")) clk.clearHistory();
  else if (!strcmp(cmd, "guests")) { st.guestMessages = d["on"] | true; settingsChanged = true; }
  else if (!strcmp(cmd, "filter")) { st.filter = d["on"] | true; settingsChanged = true; }
  else if (!strcmp(cmd, "brightness")) {
    st.brightness = constrain((int)(d["v"] | 90), 1, 255);
    if (panelOk) panel->setBrightness8(st.brightness);
    settingsChanged = true;
  } else if (!strcmp(cmd, "color")) {
    if (!parseHexColor(d["hex"], st.color)) return false;
    settingsChanged = true;
  } else if (!strcmp(cmd, "textcolor")) {
    if (!parseHexColor(d["hex"], st.textColor)) return false;
    settingsChanged = true;
  } else if (!strcmp(cmd, "flashevery")) {
    st.flashEverySec = constrain((int)(d["sec"] | 40), 0, 3600);
    settingsChanged = true;
  } else if (!strcmp(cmd, "repeats")) {
    st.guestRepeats = constrain((int)(d["n"] | 2), 1, 5);
    settingsChanged = true;
  } else if (!strcmp(cmd, "ghost")) { st.ghostSegments = d["on"] | true; settingsChanged = true; }
  else if (!strcmp(cmd, "zero")) {
    char t[bear::MAX_TEXT + 1];
    bear::sanitize(d["text"] | "", t);
    if (!*t) return false;
    strlcpy(st.zeroText, t, sizeof(st.zeroText));
    settingsChanged = true;
  } else if (!strcmp(cmd, "phrases")) {
    JsonArray a = d["list"];
    if (a.isNull() || a.size() == 0) return false;
    String joined;
    for (JsonVariant v : a) {
      if (joined.length()) joined += '\n';
      joined += v.as<const char*>();
    }
    setPhrasesFromJoined(joined);
    settingsChanged = true;
  } else if (!strcmp(cmd, "status") || !strcmp(cmd, "ping")) {
    // nothing to do; status gets published below
  } else if (!strcmp(cmd, "restart")) {
    saveTimer(now, true);
    delay(100);
    ESP.restart();
  } else {
    return false;
  }
  if (settingsChanged) saveSettings();
  saveTimer(now, true);
  return true;
}

static void handleCmd(const Cmd& c, uint32_t now) {
  if (!c.admin) {
    // Guest page sends {"n": name, "t": text}; anything else is plain text.
    JsonDocument d;
    bool json = c.body[0] == '{' && !deserializeJson(d, c.body) && d["t"].is<const char*>();
    const char* text = json ? d["t"].as<const char*>() : c.body;
    const char* name = json ? (d["n"] | "") : "";
    bool ok = clk.pushGuest(text, name, now);
    Serial.printf("guest msg %s: %s\n", ok ? "queued" : "dropped", c.body);
    return;
  }
  JsonDocument d;
  if (deserializeJson(d, c.body)) {
    Serial.printf("bad admin json: %s\n", c.body);
    return;
  }
  bool ok = applyAdmin(d, now);
  Serial.printf("admin %s: %s\n", ok ? "ok" : "REJECTED", c.body);
  publishStatus = true;
}

// ---------------------------------------------------------------------------
// Buttons: BOOT (GPIO0) + the 3-way thumb wheel behind the PCA9557 expander.

static uint8_t pcaAddr = 0, pcaIdle = 0;

static void buttonsBegin() {
  pinMode(PIN_BOOT, INPUT_PULLUP);
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 100000);
  for (uint8_t a = 0x18; a <= 0x1F; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) { pcaAddr = a; break; }
  }
  if (pcaAddr) {
    Wire.beginTransmission(pcaAddr);
    Wire.write(0x00);  // input port register (read-only access; never write config)
    Wire.endTransmission(false);
    if (Wire.requestFrom(pcaAddr, (uint8_t)1) == 1) pcaIdle = Wire.read();
    Serial.printf("PCA9557 at 0x%02X idle=0x%02X\n", pcaAddr, pcaIdle);
  } else {
    Serial.println("PCA9557 not found; thumb wheel disabled (BOOT button still works)");
  }
}

// key ids: 0 = BOOT, 1..3 = K1..K3
static void onButton(int key, bool longPress, uint32_t now) {
  Serial.printf("button %d %s\n", key, longPress ? "long" : "short");
  if (longPress) {
    if (key == 0 || key == 1) clk.reset(now);
  } else if (key == 0 || key == 1) {
    clk.toggle(now);
  } else if (key == 2) {
    clk.skip(now);
  } else if (key == 3) {
    if (clk.phraseCount()) clk.flashNow(clk.phrase(random(clk.phraseCount())), now);
  }
  saveTimer(now, true);
  publishStatus = true;
}

static void pollButtons(uint32_t now) {
  static uint32_t lastPoll = 0, downAt[4] = {0};
  static bool down[4] = {false}, longFired[4] = {false};
  if (now - lastPoll < 20) return;
  lastPoll = now;

  bool pressed[4];
  pressed[0] = digitalRead(PIN_BOOT) == LOW;
  uint8_t in = pcaIdle;
  if (pcaAddr) {
    Wire.beginTransmission(pcaAddr);
    Wire.write(0x00);
    if (Wire.endTransmission(false) == 0 && Wire.requestFrom(pcaAddr, (uint8_t)1) == 1)
      in = Wire.read();
  }
  const uint8_t bits[3] = {1 << 1, 1 << 3, 1 << 2};  // K1=IO1, K2=IO3, K3=IO2
  for (int k = 0; k < 3; k++) pressed[k + 1] = ((in ^ pcaIdle) & bits[k]) != 0;

  for (int k = 0; k < 4; k++) {
    if (pressed[k] && !down[k]) { down[k] = true; downAt[k] = now; longFired[k] = false; }
    if (pressed[k] && down[k] && !longFired[k] && now - downAt[k] > 2000) {
      longFired[k] = true;
      onButton(k, true, now);
    }
    if (!pressed[k] && down[k]) {
      down[k] = false;
      if (!longFired[k] && now - downAt[k] > 30) onButton(k, false, now);
    }
  }
}

// ---------------------------------------------------------------------------
// Network: Wi-Fi + ntfy stream

struct Net { const char* ssid; const char* pass; };
static const Net NETWORKS[] = {WIFI_NETWORKS};

static void publishStatusNow() {
  WiFiClientSecure tls;
  tls.setInsecure();
  HTTPClient http;
  // Public status topic: the guest page shows the live countdown from it.
  String url = String("https://") + NTFY_HOST + "/" + NTFY_GUEST_TOPIC + "-status";
  if (!http.begin(tls, url)) return;
  http.setTimeout(5000);
  int code = http.POST(statusJson());
  http.end();
  if (code != 200) Serial.printf("status publish failed: %d\n", code);
}

// Reads one '\n'-terminated line; returns false on timeout/disconnect.
static bool readLine(WiFiClientSecure& c, String& line, uint32_t timeoutMs) {
  line = "";
  static uint32_t lastPublish = 0;
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    if (publishStatus || millis() - lastPublish > STATUS_EVERY_MS) {
      publishStatus = false;
      lastPublish = millis();
      publishStatusNow();
    }
    while (c.available()) {
      char ch = c.read();
      if (ch == '\n') return true;
      if (line.length() < 4096) line += ch;
    }
    if (!c.connected()) return false;
    delay(10);
  }
  return false;
}

static void ntfyStream() {
  static String lastId;
  WiFiClientSecure c;
  c.setInsecure();
  c.setTimeout(10);
  if (!c.connect(NTFY_HOST, 443)) {
    Serial.println("ntfy connect failed");
    return;
  }
  String path = String("/") + NTFY_GUEST_TOPIC + "," + NTFY_ADMIN_TOPIC + "/json";
  if (lastId.length()) path += "?since=" + lastId;  // catch up after reconnects
  // HTTP/1.0 so the response isn't chunked: just newline-delimited JSON.
  c.printf("GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: bear-clock\r\n\r\n", path.c_str(),
           NTFY_HOST);

  String line;
  if (!readLine(c, line, 10000) || line.indexOf(" 200") < 0) {
    Serial.printf("ntfy bad response: %s\n", line.c_str());
    return;
  }
  while (readLine(c, line, 10000) && line.length() > 1) {}  // skip headers

  ntfyConnected = true;
  Serial.println("ntfy connected");
  static bool announced = false;
  if (!announced) {  // one visible sign on the panel that the internet path works
    announced = enqueue(true, "{\"cmd\":\"flash\",\"text\":\"ONLINE\"}");
  }
  publishStatus = true;
  // ntfy sends a keepalive event every ~45 s; reconnect if we hear nothing.
  while (WiFi.status() == WL_CONNECTED && readLine(c, line, 70000)) {
    JsonDocument d;
    if (deserializeJson(d, line)) continue;
    const char* ev = d["event"] | "";
    if (strcmp(ev, "message")) continue;
    lastId = d["id"] | "";
    const char* topic = d["topic"] | "";
    const char* msg = d["message"] | "";
    bool admin = !strcmp(topic, NTFY_ADMIN_TOPIC);
    if (!enqueue(admin, msg)) Serial.println("command dropped (queue full or too long)");
  }
  ntfyConnected = false;
  Serial.println("ntfy disconnected");
}

static volatile bool otaStarted = false;

static void netTask(void*) {
  WiFiMulti multi;
  int nNets = 0;
  for (const Net& n : NETWORKS) {
    if (n.ssid && *n.ssid) { multi.addAP(n.ssid, n.pass); nNets++; }
  }
  uint32_t backoff = 1000, wifiBackoff = 3000;
  for (;;) {
    if (!nNets) { vTaskDelay(pdMS_TO_TICKS(5000)); continue; }
    if (WiFi.status() != WL_CONNECTED) {
      // Each scan pulls the radio off the hotspot channel for a few seconds,
      // so back off while failing, and wait longer if phones are on the hotspot.
      if (multi.run(15000) != WL_CONNECTED) {
        uint32_t wait = WiFi.softAPgetStationNum() > 0 ? std::max<uint32_t>(wifiBackoff, 60000)
                                                       : wifiBackoff;
        wifiBackoff = std::min<uint32_t>(wifiBackoff * 2, 300000);
        vTaskDelay(pdMS_TO_TICKS(wait));
        continue;
      }
      wifiBackoff = 3000;
      Serial.printf("wifi: %s ip=%s rssi=%d\n", WiFi.SSID().c_str(),
                    WiFi.localIP().toString().c_str(), WiFi.RSSI());
      if (!otaStarted) {
        // Wi-Fi updates: ./bear ota <ip>. Handled in webTask.
        ArduinoOTA.setHostname("bear-clock");
        ArduinoOTA.setPassword(ADMIN_PIN);
        ArduinoOTA.onStart([] { Serial.println("OTA start"); });
        ArduinoOTA.onError([](ota_error_t e) { Serial.printf("OTA error %d\n", e); });
        ArduinoOTA.begin();
        otaStarted = true;
      }
    }
    uint32_t t0 = millis();
    ntfyStream();
    // Reset backoff if the stream stayed up a while; otherwise grow it.
    backoff = millis() - t0 > 60000 ? 1000 : std::min<uint32_t>(backoff * 2, 30000);
    vTaskDelay(pdMS_TO_TICKS(backoff));
  }
}

// ---------------------------------------------------------------------------
// Hotspot + captive portal + local API

static WebServer server(80);
static DNSServer dns;

static void sendNoCache(int code, const char* type, const String& body) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, type, body);
}

static void webBegin() {
  server.on("/", HTTP_GET, [] { server.send_P(200, "text/html", WEB_INDEX); });
  server.on("/admin", HTTP_GET, [] { server.send_P(200, "text/html", WEB_ADMIN); });
  server.on("/api/state", HTTP_GET, [] { sendNoCache(200, "application/json", statusJson()); });
  server.on("/api/msg", HTTP_POST, [] {
    String body = server.arg("plain");
    if (!body.length()) return sendNoCache(400, "text/plain", "empty");
    bool ok = enqueue(false, body.c_str());
    sendNoCache(ok ? 200 : 503, "text/plain", ok ? "ok" : "busy");
  });
  server.on("/api/cmd", HTTP_POST, [] {
    if (server.arg("pin") != ADMIN_PIN) return sendNoCache(403, "text/plain", "bad pin");
    String body = server.arg("plain");
    if (body.length() >= sizeof(Cmd::body)) return sendNoCache(413, "text/plain", "too long");
    bool ok = enqueue(true, body.c_str());
    sendNoCache(ok ? 200 : 503, "text/plain", ok ? "ok" : "busy");
  });
  // Captive portal: anything else (OS connectivity checks included) -> guest page.
  server.onNotFound([] {
    server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/");
    sendNoCache(302, "text/plain", "");
  });
  server.begin();
  dns.start(53, "*", WiFi.softAPIP());
}

static void webTask(void*) {
  for (;;) {
    dns.processNextRequest();
    server.handleClient();
    if (otaStarted) ArduinoOTA.handle();
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

// ---------------------------------------------------------------------------
// Serial console: type a guest message, or an admin JSON like {"cmd":"start"}.

static void pollSerial() {
  static char buf[512];
  static size_t n = 0;
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == '\r') continue;
    if (ch != '\n') {
      if (n < sizeof(buf) - 1) buf[n++] = ch;
      continue;
    }
    buf[n] = 0;
    n = 0;
    if (!buf[0]) continue;
    if (!strcmp(buf, "status")) { Serial.println(statusJson()); continue; }
    enqueue(buf[0] == '{', buf);
  }
}

// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  pinMode(PIN_SPK_EN, OUTPUT);
  digitalWrite(PIN_SPK_EN, LOW);
  delay(300);
  Serial.printf("\n== Bear clock == reset reason %d\n", (int)esp_reset_reason());

  cmdQueue = xQueueCreate(8, sizeof(Cmd));
  loadState();
  panelBegin();
  panelSelfTest();
  buttonsBegin();

  WiFi.persistent(false);
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  WiFi.softAP(AP_SSID, strlen(AP_PASS) ? AP_PASS : nullptr);
  Serial.printf("hotspot \"%s\" at http://%s/  (admin: /admin, pin in secrets.h)\n", AP_SSID,
                WiFi.softAPIP().toString().c_str());
  webBegin();

  xTaskCreatePinnedToCore(netTask, "net", 12288, nullptr, 1, nullptr, 0);
  xTaskCreatePinnedToCore(webTask, "web", 8192, nullptr, 1, nullptr, 0);
  Serial.println("type a message, or JSON like {\"cmd\":\"start\"}, or 'status'");
  // Reboot if loop() ever stalls > 5 s; the countdown resumes from NVS.
  enableLoopWDT();
}

void loop() {
  uint32_t now = millis();

  static Cmd c;  // 2 KB: keep it off the loop task's stack
  while (xQueueReceive(cmdQueue, &c, 0) == pdTRUE) handleCmd(c, now);
  pollSerial();
  pollButtons(now);

  clk.render(now, frame);
  panelShow();
  saveTimer(now, false);
  saveHistory(now);

  portENTER_CRITICAL(&statusMux);
  status.remSec = (clk.remainingMs(now) + 999) / 1000;
  status.durationSec = clk.settings.durationSec;
  status.running = clk.running();
  status.queue = clk.queueLength();
  strlcpy(status.current, clk.currentText(), sizeof(status.current));
  strlcpy(status.from, clk.currentFrom(), sizeof(status.from));
  status.history = clk.historyCount();
  status.brightness = clk.settings.brightness;
  status.guests = clk.settings.guestMessages;
  portEXIT_CRITICAL(&statusMux);

  // ~40 fps
  uint32_t spent = millis() - now;
  if (spent < 25) delay(25 - spent);
}
