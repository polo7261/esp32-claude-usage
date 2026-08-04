/*
 * Claude Usage Monitor — ESP32-S3 + Waveshare 1.47" LCD (ST7789V2, 172x320)
 * Fetches usage JSON from Mac server, draws progress bars on LCD.
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include "esp_wifi.h"

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

// ── 本機設定 ──
// WiFi 憑證與 server 位址都在 secrets.h（未進版控）。
// 第一次編譯前： cp secrets.h.example secrets.h  再填入自己的值。
#include "secrets.h"

const IPAddress SERVER_FALLBACK_IP(SERVER_IP_OCTETS);
IPAddress serverIp;  // resolved via mDNS, falls back to the static address
String serverUrl;    // rebuilt whenever serverIp changes — single source
const unsigned long FETCH_INTERVAL = 60000;
// Reconnect backstop bounds. The floor is well clear of a WPA2 handshake plus
// DHCP so a retry never lands on top of one still in flight.
const unsigned long RECONNECT_BACKOFF_MIN = 15000;
const unsigned long RECONNECT_BACKOFF_MAX = 120000;

// ── LCD Pin Config (Waveshare ESP32-S3-LCD-1.47) ──
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel;
  lgfx::Bus_SPI _bus;
  lgfx::Light_PWM _light;

public:
  LGFX() {
    {
      auto cfg = _bus.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 80000000;
      cfg.freq_read  = 16000000;
      cfg.pin_sclk = 40;
      cfg.pin_mosi = 45;
      cfg.pin_miso = -1;
      cfg.pin_dc   = 41;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs   = 42;
      cfg.pin_rst  = 39;
      cfg.pin_busy = -1;
      cfg.memory_width  = 172;
      cfg.memory_height = 320;
      cfg.panel_width   = 172;
      cfg.panel_height  = 320;
      cfg.offset_x = 34;
      cfg.offset_y = 0;
      cfg.offset_rotation = 0;
      cfg.invert = true;
      cfg.rgb_order = false;
      _panel.config(cfg);
    }
    {
      auto cfg = _light.config();
      cfg.pin_bl = 46;
      cfg.invert = false;
      cfg.freq = 44100;
      cfg.pwm_channel = 7;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    setPanel(&_panel);
  }
};

static LGFX lcd;
static LGFX_Sprite band(&lcd);

static const uint16_t COL_BG     = 0x1082;
static const uint16_t COL_BAR_BG = 0x2945;
static const uint16_t COL_TEXT   = 0xC618;
static const uint16_t COL_BRIGHT = 0xFFFF;
static const uint16_t COL_BLUE   = 0x3B7F;
static const uint16_t COL_GREEN  = 0x2E8B;
static const uint16_t COL_YELLOW = 0xFE60;
static const uint16_t COL_RED    = 0xF800;
static const uint16_t COL_CYAN   = 0x07FA;
static const uint16_t COL_PURPLE = 0xA95F;
static const uint16_t COL_ORANGE = 0xFCC0;
static const uint16_t COL_MINT   = 0x2FEB;

// Each model bar takes the next hue so two models never look alike.
static const uint16_t MODEL_ACCENTS[] = { COL_PURPLE, COL_MINT, COL_CYAN, COL_GREEN };
static const int MODEL_ACCENT_COUNT = sizeof(MODEL_ACCENTS) / sizeof(MODEL_ACCENTS[0]);

static const int MAX_MODELS = 4;
static const int BARS_PER_PAGE = 3;

struct ModelUsage {
  String name;
  int pct;
  String reset;
};

// Local transcript accounting (ccusage) — a different source from the plan
// limits above. pct is this model's share of the week's tokens, and cost is a
// list-price valuation, NOT what the subscription is billed.
struct TokenUsage {
  String name;
  int pct;
  String tok;
  float cost;
};

unsigned long lastFetch = 0;
unsigned long lastAnim = 0;
unsigned long lastReconnect = 0;
unsigned long reconnectBackoff = RECONNECT_BACKOFF_MIN;
int sessionPct = -1;
int weeklyPct = -1;
int sessionResetMin = 0;
ModelUsage models[MAX_MODELS];
int modelCount = 0;
TokenUsage tokModels[MAX_MODELS];
int tokModelCount = 0;
bool ccOk = false;
String tokActive;
String tokWeek;
float burnHr = 0;
bool demoModels = false;
bool spendEnabled = false;
int spendPct = -1;
float spendUsed = 0;
float spendLimit = 0;
String spendCur;
String weeklyResetDay;
String apiStatus;
bool hasData = false;
bool fetchError = false;
int curPage = 0;

// Per-metric accent in the normal range, shared warning colours above it —
// so the hue tells you *which* limit and the shift tells you *how bad*.
uint16_t barColor(int pct, uint16_t accent) {
  if (pct >= 90) return COL_RED;
  if (pct >= 75) return COL_YELLOW;
  return accent;
}

// warn=false for bars whose percentage is a share, not a limit — a model at
// 98% of this week's tokens is not a warning, so it keeps its own colour.
void drawBar(int y, const char* label, int pct, uint16_t accent, const char* footer,
             bool warn = true) {
  int barX = 16;
  int barW = 140;
  int barH = 14;

  lcd.setTextColor(COL_TEXT, COL_BG);
  lcd.setFont(&fonts::Font2);
  lcd.setCursor(barX, y);
  lcd.print(label);

  lcd.setTextColor(COL_BRIGHT, COL_BG);
  lcd.setFont(&fonts::Font4);
  lcd.setCursor(barX + 84, y - 2);
  lcd.printf("%d%%", pct);

  int barY = y + 22;
  lcd.fillRoundRect(barX, barY, barW, barH, 4, COL_BAR_BG);

  int fillW = (barW * pct) / 100;
  if (fillW > barW) fillW = barW;
  if (fillW > 0) {
    lcd.fillRoundRect(barX, barY, fillW, barH, 4, warn ? barColor(pct, accent) : accent);
  }

  lcd.setTextColor(COL_TEXT, COL_BG);
  lcd.setFont(&fonts::Font0);
  lcd.setCursor(barX, barY + 20);
  lcd.print(footer);
}

// Page 0 is the plan limits, page 1 the token split when ccusage answered,
// then one page per 3 API model buckets. Both middle sections can be absent.
bool hasTokenPage() { return ccOk && tokModelCount > 0; }

int pageCount() {
  int n = 1;
  if (hasTokenPage()) n += 1;
  if (modelCount > 0) n += (modelCount + BARS_PER_PAGE - 1) / BARS_PER_PAGE;
  return n;
}

void drawScreen() {
  lcd.fillScreen(COL_BG);

  lcd.setTextColor(COL_BLUE, COL_BG);
  lcd.setFont(&fonts::Font4);
  lcd.setCursor(16, 12);
  lcd.print("Claude");

  bool tokPage = hasTokenPage() && curPage == 1;
  bool apiPage = curPage > 0 && !tokPage;

  // Synthetic bars must never be mistakable for real usage — say so in the header
  lcd.setTextColor(apiPage && demoModels ? COL_ORANGE : COL_TEXT, COL_BG);
  lcd.setFont(&fonts::Font2);
  lcd.setCursor(112, 18);
  lcd.print(curPage == 0 ? "Usage" : tokPage ? "Token" : (demoModels ? "DEMO" : "Model"));

  lcd.drawFastHLine(16, 42, 140, 0x3186);

  if (!hasData) {
    lcd.setTextColor(COL_TEXT, COL_BG);
    lcd.setFont(&fonts::Font2);
    lcd.setCursor(30, 150);
    lcd.print(fetchError ? "Fetch Error" : "Loading...");
    return;
  }

  const int slotY[BARS_PER_PAGE] = { 54, 132, 210 };
  char label[28];
  char footer[32];

  if (curPage == 0) {
    int slot = 0;

    if (sessionResetMin >= 60) {
      snprintf(footer, sizeof(footer), "Resets: %dh %dm",
               sessionResetMin / 60, sessionResetMin % 60);
    } else {
      snprintf(footer, sizeof(footer), "Resets: %dm", sessionResetMin);
    }
    drawBar(slotY[slot++], "Session (5h)", sessionPct, COL_BLUE, footer);

    snprintf(footer, sizeof(footer), "Resets: %s", weeklyResetDay.c_str());
    drawBar(slotY[slot++], "Weekly (7d)", weeklyPct, COL_CYAN, footer);

    // No token counts exist in the usage API — credits are the only
    // consumption figure it reports, so that is what this bar shows.
    if (spendEnabled && slot < BARS_PER_PAGE) {
      snprintf(footer, sizeof(footer), "%.2f / %.2f %s",
               spendUsed, spendLimit, spendCur.c_str());
      drawBar(slotY[slot++], "Credits", spendPct, COL_ORANGE, footer);
    }
  } else if (tokPage) {
    // Headline numbers for the live 5h block, above the per-model split
    lcd.setTextColor(COL_TEXT, COL_BG);
    lcd.setFont(&fonts::Font0);
    lcd.setCursor(16, 44);
    lcd.printf("5h %s $%.0f/hr  wk %s", tokActive.c_str(), burnHr, tokWeek.c_str());

    for (int i = 0; i < BARS_PER_PAGE && i < tokModelCount; i++) {
      const TokenUsage &t = tokModels[i];
      snprintf(footer, sizeof(footer), "%s tok   $%.2f", t.tok.c_str(), t.cost);
      drawBar(slotY[i], t.name.c_str(), t.pct,
              MODEL_ACCENTS[i % MODEL_ACCENT_COUNT], footer, false);
    }
  } else {
    int first = (curPage - 1 - (hasTokenPage() ? 1 : 0)) * BARS_PER_PAGE;
    for (int i = 0; i < BARS_PER_PAGE && first + i < modelCount; i++) {
      const ModelUsage &m = models[first + i];
      snprintf(label, sizeof(label), "%s (7d)", m.name.c_str());
      snprintf(footer, sizeof(footer), "Resets: %s", m.reset.c_str());
      drawBar(slotY[i], label, m.pct,
              MODEL_ACCENTS[(first + i) % MODEL_ACCENT_COUNT], footer);
    }
  }

  int dotY = 292;
  uint16_t dotColor = (apiStatus == "allowed") ? COL_GREEN : COL_RED;
  lcd.fillCircle(24, dotY, 4, dotColor);
  lcd.setTextColor(COL_TEXT, COL_BG);
  lcd.setFont(&fonts::Font0);
  lcd.setCursor(34, dotY - 4);
  lcd.print(apiStatus == "allowed" ? "OK" : apiStatus.c_str());

  lcd.setCursor(60, dotY - 4);
  unsigned long ago = (millis() - lastFetch) / 1000;
  if (ago < 60) {
    lcd.printf("(%lus ago)", ago);
  } else {
    lcd.printf("(%lum ago)", ago / 60);
  }

  // Page indicator, right-aligned so it never collides with the status text
  int pages = pageCount();
  if (pages > 1) {
    for (int p = 0; p < pages; p++) {
      int px = 156 - (pages - 1 - p) * 10;
      if (p == curPage) lcd.fillCircle(px, dotY, 3, COL_BRIGHT);
      else              lcd.drawCircle(px, dotY, 3, COL_BAR_BG);
    }
  }
}

// Pixel ghost styled after VK's reference: blocky body, square eyes, 4 stub legs
void drawGhost(LGFX_Sprite &s, int gx, int gy, uint16_t body) {
  const int C = 4;  // cell size
  s.fillRect(gx + 1 * C, gy, 9 * C, 7 * C, body);
  s.fillRect(gx, gy + 2 * C, C, 2 * C, body);
  s.fillRect(gx + 10 * C, gy + 2 * C, C, 2 * C, body);
  for (int i = 0; i < 4; i++)
    s.fillRect(gx + (2 + 2 * i) * C, gy + 7 * C, C, 2 * C, body);
  s.fillRect(gx + 3 * C, gy + 1 * C, C, C, TFT_BLACK);
  s.fillRect(gx + 7 * C, gy + 1 * C, C, C, TFT_BLACK);
}

// Pac-Man chomps left-to-right eating dots, Claude ghost in pursuit (~3s).
// VK views the board landscape (USB to the right), but we never touch
// lcd rotation — switching it corrupts this offset panel's address window.
// Instead the landscape band is composed in a sprite and pushed rotated +90°.
void pacmanTransition() {
  const int W = 320, H = 48;
  if (!band.createSprite(W, H)) return;
  lcd.fillScreen(COL_BG);
  const uint16_t GHOST_COL = lcd.color565(226, 105, 78);
  const int r = 15;
  // ~230 frames × ~26ms ≈ 6s (VK-specified duration)
  for (int x = -24; x <= W + 116; x += 2) {
    band.fillSprite(COL_BG);
    for (int dx = 8; dx < W; dx += 16)
      if (dx > x + r) band.fillCircle(dx, H / 2, 2, COL_TEXT);
    band.fillCircle(x, H / 2, r, TFT_YELLOW);
    if ((x / 12) % 2 == 0)
      band.fillTriangle(x, H / 2, x + r + 2, H / 2 - 11, x + r + 2, H / 2 + 11, COL_BG);
    drawGhost(band, x - 68, H / 2 - 20, GHOST_COL);
    band.pushRotateZoom(86.0f, 160.0f, 90.0f, 1.0f, 1.0f);
    delay(16);
  }
  band.deleteSprite();
}

// Ask mDNS where the server is now. The static IP is only a fallback for APs
// that filter multicast; when mDNS does answer, a renumbered LAN costs nothing
// — no reflash, the next call just returns the new address.
bool resolveServer() {
  IPAddress found = MDNS.queryHost(SERVER_HOST, 3000);
  bool ok = (uint32_t)found != 0;
  serverIp = ok ? found : SERVER_FALLBACK_IP;
  serverUrl = "http://" + serverIp.toString() + ":" + String(SERVER_PORT) + "/usage";
  Serial.printf("[mDNS] %s.local -> %s%s\n", SERVER_HOST, serverIp.toString().c_str(),
                ok ? "" : "  (no answer, using fallback)");
  return ok;
}

void fetchUsage() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  http.begin(serverUrl);
  http.setTimeout(10000);
  int code = http.GET();
  Serial.printf("[Fetch] GET %s -> %d\n", serverUrl.c_str(), code);

  // Weak-signal SYN loss shows up as sporadic -1; one quick retry clears most
  if (code < 0) {
    http.end();
    delay(600);
    http.begin(serverUrl);
    http.setTimeout(10000);
    code = http.GET();
    Serial.printf("[Fetch] retry -> %d\n", code);
  }

  if (code < 0) {
    Serial.printf("[Diag] RSSI=%d IP=%s GW=%s\n", WiFi.RSSI(),
      WiFi.localIP().toString().c_str(), WiFi.gatewayIP().toString().c_str());
    WiFiClient probe;
    unsigned long t = millis();
    bool gwOk = probe.connect(WiFi.gatewayIP(), 80);
    Serial.printf("[Diag] TCP gateway:80 %s (%lums)\n", gwOk ? "OK" : "FAIL", millis() - t);
    probe.stop();
    t = millis();
    String server = serverIp.toString();
    bool svOk = probe.connect(serverIp, SERVER_PORT);
    Serial.printf("[Diag] TCP %s:%u %s (%lums)\n", server.c_str(), SERVER_PORT, svOk ? "OK" : "FAIL", millis() - t);
    probe.stop();
    t = millis();
    bool p80 = probe.connect(serverIp, 80);
    Serial.printf("[Diag] TCP %s:80 %s (%lums) <fast fail=RST=path ok>\n", server.c_str(), p80 ? "OK" : "FAIL", millis() - t);
    probe.stop();
    t = millis();
    bool inet = probe.connect(IPAddress(8, 8, 8, 8), 53);
    Serial.printf("[Diag] TCP 8.8.8.8:53 %s (%lums) <internet>\n", inet ? "OK" : "FAIL", millis() - t);
    probe.stop();

    // The server most likely just moved. Re-resolve so the next poll finds it
    // at its new address instead of retrying a stale one forever.
    resolveServer();
  }

  if (code == 200) {
    String payload = http.getString();
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    // A server error blob parses as valid JSON but lacks the data fields —
    // treat anything without session_pct as a failed fetch, keep old data
    if (!err && doc["session_pct"].is<int>()) {
      sessionPct = doc["session_pct"] | -1;
      weeklyPct = doc["weekly_pct"] | -1;
      sessionResetMin = doc["session_reset_min"] | 0;
      weeklyResetDay = doc["weekly_reset_day"].as<String>();
      apiStatus = doc["status"].as<String>();

      demoModels = doc["demo"] | false;
      modelCount = 0;
      for (JsonObject m : doc["models"].as<JsonArray>()) {
        if (modelCount >= MAX_MODELS) break;
        models[modelCount].name  = m["name"].as<String>();
        models[modelCount].pct   = m["pct"] | 0;
        models[modelCount].reset = m["reset_day"].as<String>();
        modelCount++;
      }

      ccOk = doc["cc_ok"] | false;
      tokActive = doc["tok_active"].as<String>();
      tokWeek = doc["tok_week"].as<String>();
      burnHr = doc["burn_hr"] | 0.0f;
      tokModelCount = 0;
      for (JsonObject t : doc["tok_models"].as<JsonArray>()) {
        if (tokModelCount >= MAX_MODELS) break;
        tokModels[tokModelCount].name = t["name"].as<String>();
        tokModels[tokModelCount].pct  = t["pct"] | 0;
        tokModels[tokModelCount].tok  = t["tok"].as<String>();
        tokModels[tokModelCount].cost = t["cost"] | 0.0f;
        tokModelCount++;
      }

      spendEnabled = doc["spend_enabled"] | false;
      spendPct = doc["spend_pct"] | 0;
      spendUsed = doc["spend_used"] | 0.0f;
      spendLimit = doc["spend_limit"] | 0.0f;
      spendCur = doc["spend_cur"].as<String>();

      // A model bucket can vanish between polls — never leave the view
      // parked on a page that no longer exists.
      if (curPage >= pageCount()) curPage = 0;

      hasData = true;
      fetchError = false;
    } else {
      fetchError = true;
    }
  } else {
    fetchError = true;
  }
  http.end();
  lastFetch = millis();
}

void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    Serial.printf("[WiFi] disconnected, reason=%d\n", info.wifi_sta_disconnected.reason);
  } else if (event == ARDUINO_EVENT_WIFI_STA_CONNECTED) {
    Serial.println("[WiFi] associated");
  } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    Serial.printf("[WiFi] got IP: %s\n", WiFi.localIP().toString().c_str());
  }
}

void setup() {
  Serial.begin(115200);
  // Wait up to 6s for the USB-CDC host to attach so boot logs aren't dropped
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 6000) delay(50);
  delay(500);
  Serial.println("Claude Usage Monitor starting...");

  // serverUrl is built in resolveServer(), once WiFi is up — mDNS needs a link

  lcd.init();
  lcd.setRotation(0);
  lcd.setBrightness(128);
  lcd.fillScreen(COL_BG);

  WiFi.persistent(false);
  WiFi.onEvent(onWiFiEvent);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  delay(100);

  // Scan first: confirm AP visible, log auth/channel/RSSI
  int n = WiFi.scanNetworks();
  Serial.printf("Scan found %d networks:\n", n);
  int targetAuth = -1;
  for (int i = 0; i < n; i++) {
    Serial.printf("  [%d] %s  RSSI:%d  Ch:%d  Auth:%d\n",
      i, WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.channel(i), WiFi.encryptionType(i));
    if (WiFi.SSID(i) == WIFI_SSID) targetAuth = WiFi.encryptionType(i);
  }
  WiFi.scanDelete();
  Serial.printf("Target '%s' %s (auth=%d)\n", WIFI_SSID,
    targetAuth >= 0 ? "FOUND" : "NOT FOUND", targetAuth);

  // Accept any auth down to WEP (default floor is WPA2, blocks old WPA APs)
  WiFi.setMinSecurity(WIFI_AUTH_WEP);
  WiFi.setAutoReconnect(true);
  // S3 boards commonly fail auth (reason=2 loop) at full TX power — cap it
  WiFi.setTxPower(WIFI_POWER_13dBm);

  Serial.printf("Connecting to: %s (pass len=%d)\n", WIFI_SSID, (int)strlen(WIFI_PASS));

  lcd.setTextColor(COL_TEXT, COL_BG);
  lcd.setFont(&fonts::Font2);
  lcd.setCursor(16, 140);
  lcd.printf("WiFi: %s", WIFI_SSID);

  WiFi.begin(WIFI_SSID, WIFI_PASS);

  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 40) {
    delay(500);
    lcd.print(".");
    Serial.printf(".");
    tries++;
    if (tries % 10 == 0) {
      Serial.printf(" status=%d\n", WiFi.status());
    }
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("Connected! IP: %s\n", WiFi.localIP().toString().c_str());
    lcd.fillScreen(COL_BG);
    lcd.setCursor(16, 140);
    lcd.setTextColor(COL_BLUE, COL_BG);
    lcd.printf("IP: %s", WiFi.localIP().toString().c_str());
    delay(1000);
  } else {
    Serial.printf("WiFi FAILED. Status: %d\n", WiFi.status());
    lcd.setCursor(16, 160);
    lcd.setTextColor(COL_RED, COL_BG);
    lcd.printf("FAIL status:%d", WiFi.status());
    delay(5000);
  }

  MDNS.begin("claude-usage");  // also makes this board reachable as claude-usage.local
  resolveServer();

  fetchUsage();
  drawScreen();
}

void loop() {
  // Data loop (background): fetch every 60s, retry every 10s after a failure.
  // Quiet redraw only — the show is on its own clock below.
  unsigned long interval = (hasData && !fetchError) ? FETCH_INTERVAL : 10000;
  if (millis() - lastFetch > interval) {
    fetchUsage();
    drawScreen();
  }

  // Show loop: 30s of info, then the transition, on a fixed rhythm (VK spec).
  // The transition now also turns the page — with one page it just replays.
  if (hasData && millis() - lastAnim > 30000) {
    pacmanTransition();
    curPage = (curPage + 1) % pageCount();
    drawScreen();
    lastAnim = millis();
  }

  // Reconnect backstop. setAutoReconnect(true) already retries in the
  // background; calling reconnect() on top of that tears down the handshake
  // in flight and restarts it, so a dropped link never recovers — the board
  // sits in a reason=2/36 loop indefinitely. Step in only after the system
  // has had a fair shot, then back off. No delay() here: blocking froze the
  // display for 5s on every pass.
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastReconnect > reconnectBackoff) {
      WiFi.reconnect();
      lastReconnect = millis();
      reconnectBackoff = min(reconnectBackoff * 2, RECONNECT_BACKOFF_MAX);
    }
  } else {
    reconnectBackoff = RECONNECT_BACKOFF_MIN;
  }

  delay(1000);
}
