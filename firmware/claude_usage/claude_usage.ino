/*
 * Claude Usage Monitor — ESP32-S3 + Waveshare 1.47" LCD (ST7789V2, 172x320)
 * Fetches usage JSON from Mac server, draws progress bars on LCD.
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "esp_wifi.h"

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

// ── WiFi ──
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";

// ── Server ──
// 改成跑 server.py 那台電腦的區網 IP（下方 Diag 段的 IP 也要一起改）
const char* SERVER_URL = "http://192.168.1.100:8266/usage";
const unsigned long FETCH_INTERVAL = 60000;

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

unsigned long lastFetch = 0;
unsigned long lastAnim = 0;
int sessionPct = -1;
int weeklyPct = -1;
int sessionResetMin = 0;
int fablePct = -1;
String fableLabel;
String weeklyResetDay;
String apiStatus;
bool hasData = false;
bool fetchError = false;

uint16_t barColor(int pct) {
  if (pct < 50) return COL_BLUE;
  if (pct < 75) return COL_YELLOW;
  return COL_RED;
}

void drawBar(int y, const char* label, int pct, const char* resetInfo) {
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
  if (fillW > 0) {
    lcd.fillRoundRect(barX, barY, fillW, barH, 4, barColor(pct));
  }

  lcd.setTextColor(COL_TEXT, COL_BG);
  lcd.setFont(&fonts::Font0);
  lcd.setCursor(barX, barY + 20);
  lcd.printf("Resets: %s", resetInfo);
}

void drawScreen() {
  lcd.fillScreen(COL_BG);

  lcd.setTextColor(COL_BLUE, COL_BG);
  lcd.setFont(&fonts::Font4);
  lcd.setCursor(16, 12);
  lcd.print("Claude");

  lcd.setTextColor(COL_TEXT, COL_BG);
  lcd.setFont(&fonts::Font2);
  lcd.setCursor(112, 18);
  lcd.print("Usage");

  lcd.drawFastHLine(16, 42, 140, 0x3186);

  if (!hasData) {
    lcd.setTextColor(COL_TEXT, COL_BG);
    lcd.setFont(&fonts::Font2);
    lcd.setCursor(30, 150);
    lcd.print(fetchError ? "Fetch Error" : "Loading...");
    return;
  }

  char resetBuf[32];
  if (sessionResetMin >= 60) {
    snprintf(resetBuf, sizeof(resetBuf), "%dh %dm", sessionResetMin / 60, sessionResetMin % 60);
  } else {
    snprintf(resetBuf, sizeof(resetBuf), "%dm", sessionResetMin);
  }
  drawBar(54, "Session (5h)", sessionPct, resetBuf);

  drawBar(132, "Weekly", weeklyPct, weeklyResetDay.c_str());

  if (fablePct >= 0) {
    char fableTitle[24];
    snprintf(fableTitle, sizeof(fableTitle), "%s (7d)",
             fableLabel.length() ? fableLabel.c_str() : "Fable");
    drawBar(210, fableTitle, fablePct, weeklyResetDay.c_str());
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

void fetchUsage() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  http.begin(SERVER_URL);
  http.setTimeout(10000);
  int code = http.GET();
  Serial.printf("[Fetch] GET %s -> %d\n", SERVER_URL, code);

  // Weak-signal SYN loss shows up as sporadic -1; one quick retry clears most
  if (code < 0) {
    http.end();
    delay(600);
    http.begin(SERVER_URL);
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
    bool svOk = probe.connect(IPAddress(192, 168, 1, 100), 8266);
    Serial.printf("[Diag] TCP 192.168.1.100:8266 %s (%lums)\n", svOk ? "OK" : "FAIL", millis() - t);
    probe.stop();
    t = millis();
    bool p80 = probe.connect(IPAddress(192, 168, 1, 100), 80);
    Serial.printf("[Diag] TCP 192.168.1.100:80 %s (%lums) <fast fail=RST=path ok>\n", p80 ? "OK" : "FAIL", millis() - t);
    probe.stop();
    t = millis();
    bool inet = probe.connect(IPAddress(8, 8, 8, 8), 53);
    Serial.printf("[Diag] TCP 8.8.8.8:53 %s (%lums) <internet>\n", inet ? "OK" : "FAIL", millis() - t);
    probe.stop();
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
      fablePct = doc["fable_pct"] | -1;
      fableLabel = doc["fable_label"].as<String>();
      weeklyResetDay = doc["weekly_reset_day"].as<String>();
      apiStatus = doc["status"].as<String>();
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

  // Show loop: 30s of info, then the transition, on a fixed rhythm (VK spec)
  if (hasData && millis() - lastAnim > 30000) {
    pacmanTransition();
    drawScreen();
    lastAnim = millis();
  }

  if (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
    delay(5000);
  }

  delay(1000);
}
