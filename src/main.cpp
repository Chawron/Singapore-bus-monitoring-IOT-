#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <qrcode.h>
#include <time.h>
#include <ctype.h>

// ===== Waveshare ESP32-C6-Touch-LCD-1.47 official pin map =====
// Source: Waveshare official demo package (Arduino + ESP-IDF)
static const int PIN_LCD_DC = 15;
static const int PIN_LCD_CS = 14;
static const int PIN_LCD_SCK_TOUCH = 1;
static const int PIN_LCD_MOSI_TOUCH = 2;
static const int PIN_LCD_RST_TOUCH = 22;
static const int PIN_LCD_BL_TOUCH = 23;

// Alternate pin profile used by Waveshare ESP32-C6-LCD-1.47 (non-touch).
static const int PIN_LCD_SCK_STD = 7;
static const int PIN_LCD_MOSI_STD = 6;
static const int PIN_LCD_RST_STD = 21;
static const int PIN_LCD_BL_STD = 22;

static const int PIN_TOUCH_SDA = 18;
static const int PIN_TOUCH_SCL = 19;
static const int PIN_TOUCH_RST = 20;
static const int PIN_TOUCH_INT = 21;
static const int PIN_FACTORY_BTN = 9; // BOOT button (active low)

static const int LCD_WIDTH = 172;
static const int LCD_HEIGHT = 320;
static const uint32_t FACTORY_RESET_HOLD_MS = 5000;

// ===== Display / behavior =====
static const uint32_t BUS_REFRESH_MS = 20000;
static const uint32_t WEATHER_REFRESH_MS = 600000;
static const uint32_t MARKET_REFRESH_MS = 600000;
static const uint32_t API_RATE_LIMIT_COOLDOWN_MS = 180000;
static const uint32_t ROTATE_EVERY_MS = 300000;
static const uint32_t SPECIAL_SCREEN_MS = 10000;
static const int MAX_LINES = 8;
static const uint32_t BACKLIGHT_PWM_HZ = 20000;
static const uint8_t BACKLIGHT_PWM_BITS = 8;
static const uint8_t BACKLIGHT_RUN_DUTY = 56; // 0..255 (~22%)
static const uint8_t BACKLIGHT_SETUP_DUTY = 44; // setup readability
static const uint8_t BACKLIGHT_SETUP_IDLE_DUTY = 16; // very dim when idle on setup
static const uint8_t BACKLIGHT_WIFI_LOST_DUTY = 36; // reduce heat during reconnect loops
static const uint32_t SETUP_IDLE_DIM_MS = 60000;
static const uint32_t LOOP_IDLE_DELAY_MS = 60;
static const uint32_t SETUP_LOOP_DELAY_MS = 120;
static const uint32_t WIFI_LOST_LOOP_DELAY_MS = 120;
static const uint8_t THERMAL_CPU_FREQ_MHZ = 80;
static const uint32_t STOP_SWITCH_MS = 30000;
static const uint8_t MAX_STOPS = 4;
static const char *BUS_LINES_ALL = "*";

// ===== Network / setup =====
static const char *AP_SSID = "BusMonitor-Setup";
static const char *AP_PASSWORD = "";
static const IPAddress AP_IP(192, 168, 4, 1);
static const IPAddress AP_GATEWAY(192, 168, 4, 1);
static const IPAddress AP_SUBNET(255, 255, 255, 0);

// ===== Colors =====
static const uint16_t COL_BLACK = RGB565_BLACK;
static const uint16_t COL_WHITE = RGB565_WHITE;
static const uint16_t COL_RED = RGB565_RED;
static const uint16_t COL_GREEN = RGB565_GREEN;
static const uint16_t COL_BLUE = RGB565_BLUE;
static const uint16_t COL_CYAN = RGB565_CYAN;
static const uint16_t COL_YELLOW = RGB565_YELLOW;
static const uint16_t COL_ORANGE = 0xFD20;
static const uint16_t COL_LIGHTGREY = 0xC618;
static const uint16_t COL_DARKGREY = 0x39E7;
static const uint16_t COL_DARKGREEN = 0x0320;
static const uint16_t COL_NAVY = 0x000F;
static const uint16_t COL_MAROON = 0x7800;
static const uint16_t COL_DARKCYAN = 0x03EF;
static const char *FW_BUILD_TAG = "busmon2 " __DATE__ " " __TIME__;

struct AppConfig {
  String wifiSsid;
  String wifiPass;
  String apiKey;
  String busStop;
  String busStopName;
  String busLinesCsv;
  String stopsJson;
  String weatherArea;
  bool configured = false;

  bool isValid() const {
    return configured && !wifiSsid.isEmpty() && !apiKey.isEmpty() && !busStop.isEmpty() && !busLinesCsv.isEmpty();
  }
};

struct BusLineStatus {
  String service;
  int eta1 = -1;
  int eta2 = -1;
  bool hasData = false;
  String load;
};

struct MonitoredStop {
  String code;
  String name;
  String servicesCsv;
};

enum ScreenMode {
  SCREEN_BUS,
  SCREEN_WEATHER,
  SCREEN_MARKET
};

enum SetupPage {
  SETUP_PAGE_WIFI = 0,
  SETUP_PAGE_PORTAL = 1
};

struct DisplayProfile {
  const char *name;
  int sck;
  int mosi;
  int rst;
  int bl;
  bool useVendorInit;
};

struct ApiPollState {
  uint32_t baseIntervalMs;
  uint32_t maxIntervalMs;
  uint32_t nextFetchMs;
  uint8_t failStreak;
};

static const DisplayProfile PROFILE_TOUCH = {
  "Waveshare ESP32-C6-Touch-LCD-1.47", PIN_LCD_SCK_TOUCH, PIN_LCD_MOSI_TOUCH, PIN_LCD_RST_TOUCH, PIN_LCD_BL_TOUCH, true
};

static const DisplayProfile PROFILE_STD = {
  "Waveshare ESP32-C6-LCD-1.47", PIN_LCD_SCK_STD, PIN_LCD_MOSI_STD, PIN_LCD_RST_STD, PIN_LCD_BL_STD, true
};

Preferences prefs;
WebServer webServer(80);

Arduino_DataBus *gBus = nullptr;
Arduino_GFX *gfx = nullptr;
const DisplayProfile *gDisplayProfile = &PROFILE_TOUCH;
int gBacklightPwmPin = -1;
uint8_t gBacklightDuty = 255;

AppConfig gCfg;
String gLines[MAX_LINES];
BusLineStatus gBusLines[MAX_LINES];
int gLineCount = 0;

bool gSetupMode = true;
bool gServerStarted = false;
bool gTimeSynced = false;
uint32_t gFactoryHoldStartMs = 0;
uint32_t gFactoryHoldDrawMs = 0;

ScreenMode gActiveScreen = SCREEN_BUS;
uint32_t gLastDrawMs = 0;
uint32_t gLastRotateCycleMs = 0;
uint32_t gSpecialScreenStartMs = 0;
uint32_t gLastWifiRetryMs = 0;
uint32_t gLastUserActivityMs = 0;
SetupPage gSetupPage = SETUP_PAGE_WIFI;
bool gSetupBtnDown = false;
uint32_t gSetupBtnDownMs = 0;
uint32_t gLastStopSwitchMs = 0;
int gActiveStopIndex = 0;
int gStopCount = 0;
MonitoredStop gStops[MAX_STOPS];

ApiPollState gBusPoll = {BUS_REFRESH_MS, 300000, 0, 0};
ApiPollState gWeatherPoll = {WEATHER_REFRESH_MS, 1800000, 0, 0};
ApiPollState gMarketPoll = {MARKET_REFRESH_MS, 1800000, 0, 0};
ApiPollState gStopInfoPoll = {21600000, 86400000, 0, 0}; // refresh stop name every 6h

bool gBusRateLimited = false;
bool gWeatherRateLimited = false;
bool gMarketRateLimited = false;
bool gStopInfoRateLimited = false;
String gSetupReason = "";
String gDisplayErrorCode = "";
String gDisplayErrorText = "";
bool gUseAllServices = false;

String gBusMessage = "Waiting bus data...";
String gWeatherDesc = "Loading...";
float gWeatherTemp = NAN;
int gWeatherHumidity = -1;
float gGoldPrice = NAN;
float gBtcUsd = NAN;
float gGoldChangePct = NAN;
float gBtcChangePct = NAN;

void drawHeader(const String &title, uint16_t bg);
void parseBusLines();
String ellipsizeToWidth(const String &txt, uint16_t maxWidthPx, uint8_t size);

void setDisplayError(const String &code, const String &text) {
  gDisplayErrorCode = code;
  gDisplayErrorText = text;
  Serial.printf("[ERR][%s] %s\n", code.c_str(), text.c_str());
}

void clearDisplayErrorPrefix(const String &prefix) {
  if (gDisplayErrorCode.startsWith(prefix)) {
    gDisplayErrorCode = "";
    gDisplayErrorText = "";
  }
}

void drawErrorFooterIfAny() {
  if (gDisplayErrorCode.isEmpty()) return;
  String text = gDisplayErrorCode;
  if (!gDisplayErrorText.isEmpty()) {
    text += " ";
    text += gDisplayErrorText;
  }
  text = ellipsizeToWidth(text, gfx->width() - 8, 1);
  const int y = gfx->height() - 12;
  gfx->fillRect(0, y, gfx->width(), 12, COL_MAROON);
  gfx->setTextColor(COL_WHITE, COL_MAROON);
  gfx->setTextSize(1);
  gfx->setCursor(4, y + 3);
  gfx->print(text);
}

uint8_t clampBacklightDuty(uint16_t duty) {
  uint16_t maxDuty = (1U << BACKLIGHT_PWM_BITS) - 1U;
  if (duty > maxDuty) return (uint8_t)maxDuty;
  return (uint8_t)duty;
}

void setBacklightPinDuty(int pin, uint8_t duty) {
  if (pin == GFX_NOT_DEFINED || pin < 0) return;
  analogWriteResolution(pin, BACKLIGHT_PWM_BITS);
  analogWriteFrequency(pin, BACKLIGHT_PWM_HZ);
  analogWrite(pin, clampBacklightDuty(duty));
}

void setBacklightDuty(uint8_t duty) {
  if (gBacklightPwmPin == GFX_NOT_DEFINED || gBacklightPwmPin < 0) return;
  uint8_t clamped = clampBacklightDuty(duty);
  if (clamped == gBacklightDuty) return;
  gBacklightDuty = clamped;
  setBacklightPinDuty(gBacklightPwmPin, clamped);
}

void markUserActivity() {
  gLastUserActivityMs = millis();
  if (gSetupMode) {
    setBacklightDuty(BACKLIGHT_SETUP_DUTY);
  }
}

bool detectTouchController() {
  pinMode(PIN_TOUCH_RST, OUTPUT);
  digitalWrite(PIN_TOUCH_RST, LOW);
  delay(8);
  digitalWrite(PIN_TOUCH_RST, HIGH);
  delay(40);

  Wire.begin(PIN_TOUCH_SDA, PIN_TOUCH_SCL);
  const uint8_t candidates[] = {0x63, 0x15, 0x2A, 0x38, 0x5A};
  for (uint8_t addr : candidates) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("[BOOT] Touch controller detected at 0x%02X\n", addr);
      return true;
    }
  }

  Serial.println("[BOOT] No touch controller detected on I2C.");
  return false;
}

void enableBacklightForProfile(const DisplayProfile &profile) {
  // Keep reset pin fixed high.
  pinMode(profile.rst, OUTPUT);
  digitalWrite(profile.rst, HIGH);

  // Some Waveshare 1.47 variants swap BL pin mapping.
  // Drive both known BL pins with low-duty PWM; whichever is active will dim.
  const int blPins[] = {PIN_LCD_BL_TOUCH, PIN_LCD_BL_STD};
  for (int pin : blPins) {
    if (pin == GFX_NOT_DEFINED || pin == profile.rst) continue;
    setBacklightPinDuty(pin, BACKLIGHT_RUN_DUTY);
  }
  gBacklightPwmPin = profile.bl;
  setBacklightDuty(BACKLIGHT_RUN_DUTY);
}

void applyThermalPolicy() {
  uint32_t before = getCpuFrequencyMhz();
  if (before > THERMAL_CPU_FREQ_MHZ) {
    bool changed = setCpuFrequencyMhz(THERMAL_CPU_FREQ_MHZ);
    Serial.printf(
      "[THERMAL] CPU %luMHz -> %luMHz (%s)\n",
      (unsigned long)before,
      (unsigned long)getCpuFrequencyMhz(),
      changed ? "ok" : "unchanged");
  } else {
    Serial.printf("[THERMAL] CPU already at %luMHz\n", (unsigned long)before);
  }
}

bool initDisplayForProfile(const DisplayProfile &profile) {
  Serial.printf("[BOOT] Trying display profile: %s\n", profile.name);

  if (gBus) {
    delete gBus;
    gBus = nullptr;
  }
  if (gfx) {
    delete gfx;
    gfx = nullptr;
  }

  gBus = new Arduino_ESP32SPI(PIN_LCD_DC, PIN_LCD_CS, profile.sck, profile.mosi, GFX_NOT_DEFINED, FSPI);
  gfx = new Arduino_ST7789(
    gBus, profile.rst, 0, false,
    LCD_WIDTH, LCD_HEIGHT,
    34, 0,
    34, 0);

  if (!gfx->begin()) {
    Serial.println("[BOOT] gfx->begin() failed.");
    return false;
  }

  gfx->setRotation(1); // landscape mode as requested
  gfx->setTextWrap(false);
  enableBacklightForProfile(profile);
  return true;
}

void runDisplaySelfTest() {
  if (!gfx) {
    return;
  }

  const uint16_t testColors[] = {COL_RED, COL_GREEN, COL_BLUE, COL_WHITE, COL_BLACK};
  for (uint16_t c : testColors) {
    gfx->fillScreen(c);
    gfx->setTextSize(2);
    gfx->setTextColor((c == COL_WHITE) ? COL_BLACK : COL_WHITE, c);
    gfx->setCursor(8, 12);
    gfx->print("LCD TEST");
    gfx->setTextSize(1);
    gfx->setCursor(8, 36);
    gfx->print(gDisplayProfile->name);
    delay(500);
  }
}

String htmlEscape(const String &in) {
  String out = in;
  out.replace("&", "&amp;");
  out.replace("<", "&lt;");
  out.replace(">", "&gt;");
  out.replace("\"", "&quot;");
  out.replace("'", "&#39;");
  return out;
}

String currentTimeText() {
  struct tm t;
  if (getLocalTime(&t, 10)) {
    char buf[16];
    strftime(buf, sizeof(buf), "%H:%M:%S", &t);
    return String(buf);
  }

  uint32_t sec = millis() / 1000;
  uint32_t hh = (sec / 3600) % 24;
  uint32_t mm = (sec / 60) % 60;
  uint32_t ss = sec % 60;
  char fallback[16];
  snprintf(fallback, sizeof(fallback), "%02lu:%02lu:%02lu", (unsigned long)hh, (unsigned long)mm, (unsigned long)ss);
  return String(fallback);
}

String currentTime12Text() {
  struct tm t;
  if (getLocalTime(&t, 10)) {
    char buf[16];
    strftime(buf, sizeof(buf), "%I:%M %p", &t);
    if (buf[0] == '0') {
      return String(buf + 1);
    }
    return String(buf);
  }
  return currentTimeText();
}

String addThousands(const String &in) {
  if (in.isEmpty()) return in;
  bool neg = in[0] == '-';
  int start = neg ? 1 : 0;
  int dot = in.indexOf('.');
  String intPart = (dot >= 0) ? in.substring(start, dot) : in.substring(start);
  String frac = (dot >= 0) ? in.substring(dot) : "";
  String out = "";
  for (int i = 0; i < intPart.length(); i++) {
    out += intPart[i];
    int remain = intPart.length() - i - 1;
    if (remain > 0 && (remain % 3) == 0) out += ",";
  }
  if (neg) out = "-" + out;
  return out + frac;
}

String formatMoney2(float v) {
  if (isnan(v)) return "--";
  char buf[24];
  snprintf(buf, sizeof(buf), "%.2f", v);
  return addThousands(String(buf));
}

String formatMoney0(float v) {
  if (isnan(v)) return "--";
  char buf[24];
  snprintf(buf, sizeof(buf), "%.0f", v);
  return addThousands(String(buf));
}

uint16_t textWidthPx(const String &txt, uint8_t size) {
  if (!gfx) return 0;
  int16_t x1, y1;
  uint16_t w, h;
  gfx->setTextSize(size);
  gfx->getTextBounds(txt, 0, 0, &x1, &y1, &w, &h);
  return w;
}

String ellipsizeToWidth(const String &txt, uint16_t maxWidthPx, uint8_t size) {
  if (txt.isEmpty()) return "";
  if (textWidthPx(txt, size) <= maxWidthPx) return txt;

  const String dots = "...";
  if (textWidthPx(dots, size) >= maxWidthPx) return "";

  for (int keep = txt.length(); keep > 0; keep--) {
    String candidate = txt.substring(0, keep) + dots;
    if (textWidthPx(candidate, size) <= maxWidthPx) {
      return candidate;
    }
  }
  return dots;
}

String trimCopy(const String &input) {
  String v = input;
  v.trim();
  return v;
}

String normalizeBusStopCode(const String &input) {
  String out;
  out.reserve(6);
  for (int i = 0; i < input.length(); i++) {
    const char c = input[i];
    if (c >= '0' && c <= '9') out += c;
  }
  return out;
}

String normalizeBusLinesCsv(const String &input) {
  String csv = input;
  String out;
  int start = 0;
  int added = 0;

  while (start < csv.length() && added < MAX_LINES) {
    int comma = csv.indexOf(',', start);
    if (comma < 0) comma = csv.length();
    String token = csv.substring(start, comma);
    token.trim();
    token.toUpperCase();
    if (!token.isEmpty()) {
      if (!out.isEmpty()) out += ",";
      out += token;
      added++;
    }
    start = comma + 1;
  }

  return out;
}

String servicesCsvFromStopJson(JsonObjectConst stop) {
  String csv;
  JsonVariantConst filter = stop["filterServices"];

  if (filter.is<JsonArrayConst>()) {
    int added = 0;
    for (JsonVariantConst v : filter.as<JsonArrayConst>()) {
      String token = trimCopy(String((const char *)(v | "")));
      token.toUpperCase();
      if (token.isEmpty()) continue;
      if (!csv.isEmpty()) csv += ",";
      csv += token;
      added++;
      if (added >= MAX_LINES) break;
    }
  } else {
    csv = normalizeBusLinesCsv(trimCopy(String((const char *)(stop["filterServices"] | ""))));
  }

  if (csv.isEmpty()) {
    csv = normalizeBusLinesCsv(trimCopy(String((const char *)(stop["bus_lines"] | ""))));
  }
  if (csv.isEmpty()) {
    csv = normalizeBusLinesCsv(trimCopy(String((const char *)(stop["services"] | ""))));
  }

  if (csv.isEmpty()) {
    return String(BUS_LINES_ALL);
  }
  return csv;
}

bool addMonitoredStop(const String &codeIn, const String &nameIn, const String &servicesIn) {
  if (gStopCount >= MAX_STOPS) return false;
  String code = normalizeBusStopCode(codeIn);
  String services = normalizeBusLinesCsv(servicesIn);
  if (code.isEmpty()) return false;
  if (services.isEmpty()) services = BUS_LINES_ALL;

  gStops[gStopCount].code = code;
  gStops[gStopCount].name = trimCopy(nameIn);
  gStops[gStopCount].servicesCsv = services;
  gStopCount++;
  return true;
}

String serializeStopsJson(const MonitoredStop *stops, int count) {
  DynamicJsonDocument doc(4096);
  JsonArray arr = doc.to<JsonArray>();

  for (int i = 0; i < count; i++) {
    JsonObject item = arr.createNestedObject();
    item["code"] = stops[i].code;
    item["name"] = stops[i].name;

    JsonArray filter = item.createNestedArray("filterServices");
    String csv = stops[i].servicesCsv;
    if (csv == BUS_LINES_ALL) {
      continue;
    }
    int start = 0;
    while (start < csv.length()) {
      int comma = csv.indexOf(',', start);
      if (comma < 0) comma = csv.length();
      String token = csv.substring(start, comma);
      token.trim();
      if (!token.isEmpty()) filter.add(token);
      start = comma + 1;
    }
  }

  String out;
  serializeJson(arr, out);
  return out;
}

void rebuildMonitoredStopsFromConfig() {
  gStopCount = 0;

  if (!gCfg.stopsJson.isEmpty()) {
    DynamicJsonDocument doc(4096);
    if (!deserializeJson(doc, gCfg.stopsJson)) {
      JsonArrayConst arr = doc.as<JsonArrayConst>();
      for (JsonObjectConst stop : arr) {
        if (gStopCount >= MAX_STOPS) break;
        String code = trimCopy(String((const char *)(stop["code"] | "")));
        String name = trimCopy(String((const char *)(stop["name"] | "")));
        String services = servicesCsvFromStopJson(stop);
        addMonitoredStop(code, name, services);
      }
    }
  }

  if (gStopCount == 0) {
    addMonitoredStop(gCfg.busStop, gCfg.busStopName, gCfg.busLinesCsv);
  }

  if (gStopCount == 0) {
    gActiveStopIndex = 0;
  } else if (gActiveStopIndex < 0 || gActiveStopIndex >= gStopCount) {
    gActiveStopIndex = 0;
  }
}

void applyActiveStop(bool force = false) {
  if (gStopCount <= 0) {
    parseBusLines();
    return;
  }

  if (gActiveStopIndex < 0 || gActiveStopIndex >= gStopCount) {
    gActiveStopIndex = 0;
  }

  const MonitoredStop &stop = gStops[gActiveStopIndex];
  String code = normalizeBusStopCode(stop.code);
  String name = trimCopy(stop.name);
  String services = normalizeBusLinesCsv(stop.servicesCsv);
  if (code.isEmpty() || services.isEmpty()) return;

  bool changed = force ||
                 gCfg.busStop != code ||
                 gCfg.busStopName != name ||
                 gCfg.busLinesCsv != services;

  gCfg.busStop = code;
  gCfg.busStopName = name;
  gCfg.busLinesCsv = services;
  parseBusLines();

  if (changed) {
    Serial.printf("[STOP] Active %d/%d -> %s (%s)\n",
                  gActiveStopIndex + 1,
                  gStopCount,
                  gCfg.busStop.c_str(),
                  gCfg.busLinesCsv.c_str());
  }
}

int parseEtaMinutes(const char *iso8601) {
  if (!iso8601 || strlen(iso8601) < 19) {
    return -1;
  }

  int y, mon, d, h, m, s;
  if (sscanf(iso8601, "%4d-%2d-%2dT%2d:%2d:%2d", &y, &mon, &d, &h, &m, &s) != 6) {
    return -1;
  }

  struct tm targetTm;
  memset(&targetTm, 0, sizeof(targetTm));
  targetTm.tm_year = y - 1900;
  targetTm.tm_mon = mon - 1;
  targetTm.tm_mday = d;
  targetTm.tm_hour = h;
  targetTm.tm_min = m;
  targetTm.tm_sec = s;

  time_t target = mktime(&targetTm);
  time_t now = time(nullptr);
  if (now < 100000) {
    return -1;
  }

  long diffMin = (long)((target - now + 30) / 60);
  if (diffMin < 0) diffMin = 0;
  if (diffMin > 999) diffMin = 999;
  return (int)diffMin;
}

String etaToText(int eta) {
  if (eta < 0) return "--";
  if (eta == 0) return "Arr";
  return String(eta) + "m";
}

int findLineIndex(const String &serviceNo) {
  for (int i = 0; i < gLineCount; i++) {
    if (gLines[i].equalsIgnoreCase(serviceNo)) return i;
  }
  return -1;
}

void parseBusLines() {
  gUseAllServices = false;
  gLineCount = 0;
  String csv = gCfg.busLinesCsv;
  csv.trim();
  if (csv == BUS_LINES_ALL) {
    gUseAllServices = true;
    return;
  }
  int start = 0;

  while (start < csv.length() && gLineCount < MAX_LINES) {
    int comma = csv.indexOf(',', start);
    if (comma < 0) comma = csv.length();

    String token = csv.substring(start, comma);
    token.trim();
    token.toUpperCase();

    if (!token.isEmpty()) {
      gLines[gLineCount++] = token;
    }

    start = comma + 1;
  }

  for (int i = 0; i < gLineCount; i++) {
    gBusLines[i].service = gLines[i];
    gBusLines[i].eta1 = -1;
    gBusLines[i].eta2 = -1;
    gBusLines[i].hasData = false;
    gBusLines[i].load = "";
  }
}

void loadConfig() {
  if (!prefs.begin("busmon2", false)) {
    gCfg = AppConfig();
    rebuildMonitoredStopsFromConfig();
    applyActiveStop(true);
    return;
  }
  gCfg.wifiSsid = prefs.getString("wifi_ssid", "");
  gCfg.wifiPass = prefs.getString("wifi_pass", "");
  gCfg.apiKey = prefs.getString("api_key", "");
  gCfg.busStop = prefs.getString("bus_stop", "");
  gCfg.busStopName = prefs.getString("bus_stop_name", "");
  gCfg.busLinesCsv = prefs.getString("bus_lines", "");
  gCfg.stopsJson = prefs.getString("stops_json", "");
  gCfg.weatherArea = prefs.getString("weather_area", "City");
  gCfg.configured = prefs.getBool("configured", false);
  prefs.end();

  gCfg.busStopName.trim();
  gCfg.weatherArea.trim();
  if (gCfg.weatherArea.isEmpty()) gCfg.weatherArea = "City";
  gCfg.busLinesCsv = normalizeBusLinesCsv(gCfg.busLinesCsv);

  rebuildMonitoredStopsFromConfig();
  applyActiveStop(true);
}

void saveConfig(const AppConfig &cfg) {
  prefs.begin("busmon2", false);
  prefs.putString("wifi_ssid", cfg.wifiSsid);
  prefs.putString("wifi_pass", cfg.wifiPass);
  prefs.putString("api_key", cfg.apiKey);
  prefs.putString("bus_stop", cfg.busStop);
  prefs.putString("bus_stop_name", cfg.busStopName);
  prefs.putString("bus_lines", cfg.busLinesCsv);
  prefs.putString("stops_json", cfg.stopsJson);
  prefs.putString("weather_area", cfg.weatherArea);
  prefs.putBool("configured", cfg.configured);
  prefs.end();
}

bool checkAndHandleFactoryReset() {
  pinMode(PIN_FACTORY_BTN, INPUT_PULLUP);
  if (digitalRead(PIN_FACTORY_BTN) != LOW) {
    return false;
  }

  uint32_t start = millis();
  uint32_t lastDraw = 0;

  while (digitalRead(PIN_FACTORY_BTN) == LOW) {
    uint32_t heldMs = millis() - start;

    if (gfx && (millis() - lastDraw > 200)) {
      lastDraw = millis();
      uint32_t remainSec = (heldMs >= FACTORY_RESET_HOLD_MS) ? 0 : ((FACTORY_RESET_HOLD_MS - heldMs + 999) / 1000);

      gfx->fillScreen(COL_BLACK);
      drawHeader("FACTORY RESET", COL_RED);
      gfx->setTextColor(COL_WHITE, COL_BLACK);
      gfx->setTextSize(2);
      gfx->setCursor(8, 38);
      gfx->print("Hold BOOT key");
      gfx->setCursor(8, 64);
      gfx->print("to clear config");
      gfx->setTextColor(COL_YELLOW, COL_BLACK);
      gfx->setCursor(8, 96);
      gfx->print("Keep holding: ");
      gfx->print(remainSec);
      gfx->print("s");

      int barX = 10;
      int barY = 124;
      int barW = gfx->width() - 20;
      int fillW = (int)((((heldMs > FACTORY_RESET_HOLD_MS) ? FACTORY_RESET_HOLD_MS : heldMs) * barW) / FACTORY_RESET_HOLD_MS);
      gfx->drawRect(barX, barY, barW, 12, COL_DARKGREY);
      if (fillW > 2) {
        gfx->fillRect(barX + 1, barY + 1, fillW - 2, 10, COL_ORANGE);
      }
    }

    if (heldMs >= FACTORY_RESET_HOLD_MS) {
      Serial.println("[BOOT] BOOT held 5s, clearing saved config.");
      bool cleared = false;
      if (prefs.begin("busmon2", false)) {
        cleared = prefs.clear();
        prefs.end();
      }

      if (gfx) {
        gfx->fillScreen(COL_BLACK);
        drawHeader("FACTORY RESET", COL_RED);
        gfx->setTextSize(2);
        gfx->setCursor(8, 56);
        gfx->setTextColor(cleared ? COL_GREEN : COL_ORANGE, COL_BLACK);
        gfx->print(cleared ? "Config cleared" : "Clear failed");
        gfx->setTextSize(1);
        gfx->setCursor(8, 90);
        gfx->setTextColor(COL_WHITE, COL_BLACK);
        gfx->print("Restarting...");
      }

      delay(900);
      ESP.restart();
      return true;
    }

    delay(20);
  }

  return false;
}

bool pollRuntimeFactoryReset() {
  bool pressed = (digitalRead(PIN_FACTORY_BTN) == LOW);
  if (!pressed) {
    gFactoryHoldStartMs = 0;
    return false;
  }

  if (gFactoryHoldStartMs == 0) {
    gFactoryHoldStartMs = millis();
    gFactoryHoldDrawMs = 0;
  }

  uint32_t heldMs = millis() - gFactoryHoldStartMs;

  if (gfx && (millis() - gFactoryHoldDrawMs > 220)) {
    gFactoryHoldDrawMs = millis();
    uint32_t remainSec = (heldMs >= FACTORY_RESET_HOLD_MS) ? 0 : ((FACTORY_RESET_HOLD_MS - heldMs + 999) / 1000);
    gfx->fillScreen(COL_BLACK);
    drawHeader("FACTORY RESET", COL_RED);
    gfx->setTextColor(COL_WHITE, COL_BLACK);
    gfx->setTextSize(2);
    gfx->setCursor(8, 40);
    gfx->print("Hold BOOT key");
    gfx->setCursor(8, 66);
    gfx->print("to clear config");
    gfx->setTextColor(COL_YELLOW, COL_BLACK);
    gfx->setCursor(8, 96);
    gfx->print("Keep holding: ");
    gfx->print(remainSec);
    gfx->print("s");

    int barX = 10;
    int barY = 124;
    int barW = gfx->width() - 20;
    int fillW = (int)((((heldMs > FACTORY_RESET_HOLD_MS) ? FACTORY_RESET_HOLD_MS : heldMs) * barW) / FACTORY_RESET_HOLD_MS);
    gfx->drawRect(barX, barY, barW, 12, COL_DARKGREY);
    if (fillW > 2) {
      gfx->fillRect(barX + 1, barY + 1, fillW - 2, 10, COL_ORANGE);
    }
  }

  if (heldMs < FACTORY_RESET_HOLD_MS) {
    return false;
  }

  Serial.println("[RUN] BOOT held 5s, clearing saved config.");
  bool cleared = false;
  if (prefs.begin("busmon2", false)) {
    cleared = prefs.clear();
    prefs.end();
  }

  if (gfx) {
    gfx->fillScreen(COL_BLACK);
    drawHeader("FACTORY RESET", COL_RED);
    gfx->setTextSize(2);
    gfx->setCursor(8, 56);
    gfx->setTextColor(cleared ? COL_GREEN : COL_ORANGE, COL_BLACK);
    gfx->print(cleared ? "Config cleared" : "Clear failed");
    gfx->setTextSize(1);
    gfx->setCursor(8, 90);
    gfx->setTextColor(COL_WHITE, COL_BLACK);
    gfx->print("Restarting...");
  }

  delay(900);
  ESP.restart();
  return true;
}

bool isTimeDue(uint32_t nowMs, uint32_t dueMs) {
  return (int32_t)(nowMs - dueMs) >= 0;
}

void schedulePollSuccess(ApiPollState &state, uint32_t nowMs) {
  state.failStreak = 0;
  state.nextFetchMs = nowMs + state.baseIntervalMs;
}

void schedulePollFailure(ApiPollState &state, uint32_t nowMs, bool rateLimited) {
  if (state.failStreak < 6) {
    state.failStreak++;
  }

  uint8_t shift = (state.failStreak > 5) ? 5 : state.failStreak;
  uint64_t backoff = (uint64_t)state.baseIntervalMs << shift;
  if (backoff > state.maxIntervalMs) {
    backoff = state.maxIntervalMs;
  }

  uint32_t delayMs = (uint32_t)backoff;
  if (rateLimited && delayMs < API_RATE_LIMIT_COOLDOWN_MS) {
    delayMs = API_RATE_LIMIT_COOLDOWN_MS;
  }

  state.nextFetchMs = nowMs + delayMs;
}

extern const uint8_t setup_html_start[] asm("_binary_setup_html_start");
extern const uint8_t setup_html_end[] asm("_binary_setup_html_end");

void handleRoot() {
  markUserActivity();
  webServer.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  webServer.sendHeader("Pragma", "no-cache");
  webServer.sendHeader("Expires", "0");
  webServer.sendHeader("X-Firmware", FW_BUILD_TAG);
  webServer.send_P(200, "text/html", (const char *)setup_html_start, setup_html_end - setup_html_start);
}

void handleGetConfig() {
  markUserActivity();
  DynamicJsonDocument doc(4096);
  doc["wifi_ssid"] = gCfg.wifiSsid;
  doc["api_key"] = gCfg.apiKey;
  doc["bus_stop"] = gCfg.busStop;
  doc["bus_stop_name"] = gCfg.busStopName;
  doc["bus_lines"] = gCfg.busLinesCsv;
  doc["weather_area"] = gCfg.weatherArea;
  doc["configured"] = gCfg.configured;

  JsonArray stops = doc.createNestedArray("stops");
  for (int i = 0; i < gStopCount; i++) {
    JsonObject item = stops.createNestedObject();
    item["code"] = gStops[i].code;
    item["name"] = gStops[i].name;
    JsonArray filter = item.createNestedArray("filterServices");
    String csv = gStops[i].servicesCsv;
    int start = 0;
    while (start < csv.length()) {
      int comma = csv.indexOf(',', start);
      if (comma < 0) comma = csv.length();
      String token = csv.substring(start, comma);
      token.trim();
      if (!token.isEmpty()) filter.add(token);
      start = comma + 1;
    }
  }

  String json;
  serializeJson(doc, json);
  webServer.send(200, "application/json", json);
}

void handleFwInfo() {
  markUserActivity();
  DynamicJsonDocument doc(256);
  doc["fw"] = FW_BUILD_TAG;
  doc["ap_ip"] = WiFi.softAPIP().toString();
  doc["sta_ip"] = WiFi.localIP().toString();
  String out;
  serializeJson(doc, out);
  webServer.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  webServer.send(200, "application/json", out);
}

void handleConfigPost() {
  markUserActivity();
  if (!webServer.hasArg("plain")) {
    webServer.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Missing JSON body\"}");
    return;
  }

  DynamicJsonDocument doc(4096);
  DeserializationError err = deserializeJson(doc, webServer.arg("plain"));
  if (err) {
    webServer.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Invalid JSON\"}");
    return;
  }

  AppConfig cfg;
  cfg.wifiSsid = trimCopy(String((const char *)(doc["wifi_ssid"] | "")));
  cfg.wifiPass = trimCopy(String((const char *)(doc["wifi_pass"] | "")));
  cfg.apiKey = trimCopy(String((const char *)(doc["api_key"] | "")));
  cfg.busStop = normalizeBusStopCode(trimCopy(String((const char *)(doc["bus_stop"] | ""))));
  cfg.busStopName = trimCopy(String((const char *)(doc["bus_stop_name"] | "")));
  cfg.busLinesCsv = normalizeBusLinesCsv(trimCopy(String((const char *)(doc["bus_lines"] | ""))));
  cfg.weatherArea = trimCopy(String((const char *)(doc["weather_area"] | "City")));
  if (cfg.weatherArea.isEmpty()) cfg.weatherArea = "City";

  MonitoredStop tmpStops[MAX_STOPS];
  int tmpStopCount = 0;

  JsonArray stops = doc["stops"].as<JsonArray>();
  if (!stops.isNull()) {
    for (JsonObjectConst stop : stops) {
      if (tmpStopCount >= MAX_STOPS) break;
      String code = normalizeBusStopCode(trimCopy(String((const char *)(stop["code"] | ""))));
      String name = trimCopy(String((const char *)(stop["name"] | "")));
      String services = servicesCsvFromStopJson(stop);
      if (code.isEmpty() || services.isEmpty()) continue;
      tmpStops[tmpStopCount].code = code;
      tmpStops[tmpStopCount].name = name;
      tmpStops[tmpStopCount].servicesCsv = services;
      tmpStopCount++;
    }
  }

  if (tmpStopCount > 0) {
    cfg.busStop = tmpStops[0].code;
    cfg.busStopName = tmpStops[0].name;
    cfg.busLinesCsv = tmpStops[0].servicesCsv;
    cfg.stopsJson = serializeStopsJson(tmpStops, tmpStopCount);
  } else {
    cfg.stopsJson = "";
  }

  if (cfg.wifiSsid.isEmpty() || cfg.wifiPass.isEmpty() || cfg.apiKey.isEmpty() || cfg.busStop.isEmpty() || cfg.busLinesCsv.isEmpty()) {
    webServer.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Missing required fields\"}");
    return;
  }

  cfg.configured = true;
  saveConfig(cfg);
  gSetupReason = "";
  clearDisplayErrorPrefix("E_CFG_");
  clearDisplayErrorPrefix("E_WIFI_");
  webServer.send(200, "application/json", "{\"status\":\"ok\",\"message\":\"Saved. Restarting...\"}");
  delay(1200);
  ESP.restart();
}

void handleScan() {
  markUserActivity();
  int n = WiFi.scanNetworks(false, true);
  if (n < 0) {
    webServer.send(500, "application/json", "[]");
    return;
  }

  DynamicJsonDocument doc(2048);
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < n; i++) {
    JsonObject o = arr.createNestedObject();
    o["ssid"] = WiFi.SSID(i);
    o["rssi"] = WiFi.RSSI(i);
    o["auth"] = (int)WiFi.encryptionType(i);
  }
  WiFi.scanDelete();

  String json;
  serializeJson(doc, json);
  webServer.send(200, "application/json", json);
}

void handleSave() {
  markUserActivity();
  AppConfig cfg;
  cfg.wifiSsid = trimCopy(webServer.arg("wifi_ssid"));
  cfg.wifiPass = webServer.arg("wifi_pass");
  cfg.apiKey = webServer.arg("api_key");
  cfg.busStop = normalizeBusStopCode(webServer.arg("bus_stop"));
  cfg.busStopName = webServer.arg("bus_stop_name");
  cfg.busLinesCsv = normalizeBusLinesCsv(webServer.arg("bus_lines"));
  cfg.stopsJson = "";
  cfg.weatherArea = webServer.arg("weather_area");

  cfg.wifiSsid.trim();
  cfg.wifiPass.trim();
  cfg.apiKey.trim();
  cfg.busStop = normalizeBusStopCode(cfg.busStop);
  cfg.busStopName.trim();
  cfg.busLinesCsv = normalizeBusLinesCsv(cfg.busLinesCsv);
  cfg.weatherArea.trim();

  if (cfg.weatherArea.isEmpty()) cfg.weatherArea = "City";

  if (cfg.wifiSsid.isEmpty() || cfg.wifiPass.isEmpty() || cfg.apiKey.isEmpty() || cfg.busStop.isEmpty() || cfg.busLinesCsv.isEmpty()) {
    webServer.send(400, "text/plain", "Missing required fields.");
    return;
  }

  cfg.configured = true;
  saveConfig(cfg);
  gSetupReason = "";
  clearDisplayErrorPrefix("E_CFG_");
  clearDisplayErrorPrefix("E_WIFI_");

  webServer.send(200, "text/html", "<html><body style='font-family:Arial;background:#000;color:#fff;padding:20px'><h2>Saved</h2><p>Device will restart now.</p></body></html>");
  delay(1200);
  ESP.restart();
}

void startWebServer() {
  if (gServerStarted) return;

  webServer.on("/", HTTP_GET, handleRoot);
  webServer.on("/setup", HTTP_GET, handleRoot);
  webServer.on("/setup.html", HTTP_GET, handleRoot);
  webServer.on("/index.html", HTTP_GET, handleRoot);
  webServer.on("/api/config", HTTP_GET, handleGetConfig);
  webServer.on("/api/config", HTTP_POST, handleConfigPost);
  webServer.on("/api/fw", HTTP_GET, handleFwInfo);
  webServer.on("/api/scan", HTTP_GET, handleScan);
  webServer.on("/save", HTTP_POST, handleSave);
  webServer.onNotFound([]() {
    if (webServer.uri().startsWith("/api/")) {
      webServer.send(404, "application/json", "{\"status\":\"error\",\"message\":\"Not found\"}");
      return;
    }
    webServer.sendHeader("Location", "/", true);
    webServer.send(302, "text/plain", "");
  });
  webServer.begin();
  gServerStarted = true;
}

bool connectToWifi() {
  if (gCfg.wifiSsid.isEmpty()) {
    gSetupReason = "E_CFG_01 No WiFi SSID";
    setDisplayError("E_CFG_01", "No WiFi SSID");
    return false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_17dBm);
  WiFi.begin(gCfg.wifiSsid.c_str(), gCfg.wifiPass.c_str());

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 45000) {
    delay(250);
  }

  bool ok = (WiFi.status() == WL_CONNECTED);
  if (ok) {
    WiFi.setTxPower(WIFI_POWER_11dBm);
    WiFi.setSleep(true);
    gSetupReason = "";
    clearDisplayErrorPrefix("E_WIFI_");
  } else {
    gSetupReason = "E_WIFI_01 Connect failed (" + String((int)WiFi.status()) + ")";
    setDisplayError("E_WIFI_01", "Connect failed (" + String((int)WiFi.status()) + ")");
  }

  return ok;
}

void syncClockSg() {
  configTzTime("SGT-8", "pool.ntp.org", "time.google.com", "time.windows.com");
  time_t now = time(nullptr);
  uint32_t start = millis();
  while (now < 1700000000 && millis() - start < 12000) {
    delay(200);
    now = time(nullptr);
  }
  gTimeSynced = now >= 1700000000;
}

void drawHeader(const String &title, uint16_t bg) {
  gfx->fillRect(0, 0, gfx->width(), 20, bg);
  gfx->setTextColor(COL_WHITE, bg);
  gfx->setTextSize(1);
  gfx->setCursor(4, 6);
  gfx->print(title);
  gfx->setCursor(gfx->width() - 58, 6);
  gfx->print(currentTimeText());
}

void drawFooter(const String &text, uint16_t bg) {
  gfx->fillRect(0, gfx->height() - 16, gfx->width(), 16, bg);
  gfx->setTextColor(COL_WHITE, bg);
  gfx->setTextSize(1);
  gfx->setCursor(4, gfx->height() - 12);
  gfx->print(text);
}

void drawBootScreen() {
  gfx->fillScreen(COL_BLACK);
  drawHeader("BUS MONITOR", COL_BLACK);
  drawFooter("Booting...", COL_BLACK);

  const String brand = "Softinex.com";
  gfx->setTextSize(2);
  gfx->setTextColor(COL_WHITE, COL_BLACK);
  int textW = (int)textWidthPx(brand, 2);
  int x = (gfx->width() - textW) / 2;
  if (x < 0) x = 0;
  int y = (gfx->height() / 2) - 8;
  gfx->setCursor(x, y);
  gfx->print(brand);
}

void drawQrCode(int x, int y, const char *payload, const char *label, int targetPx = 68) {
  if (!payload || !*payload) return;

  const uint8_t version = 3;
  const uint8_t ecc = 0; // lowest ECC keeps QR modules smaller on this display
  uint8_t qrData[qrcode_getBufferSize(version)];
  QRCode qr;
  qrcode_initText(&qr, qrData, version, ecc, payload);

  const int border = 2;
  int scale = targetPx / (qr.size + border * 2);
  if (scale < 1) scale = 1;

  const int codeW = (qr.size + border * 2) * scale;
  gfx->fillRect(x, y, codeW, codeW, COL_WHITE);

  for (int yy = 0; yy < qr.size; yy++) {
    for (int xx = 0; xx < qr.size; xx++) {
      if (qrcode_getModule(&qr, xx, yy)) {
        gfx->fillRect(
          x + (xx + border) * scale,
          y + (yy + border) * scale,
          scale,
          scale,
          COL_BLACK);
      }
    }
  }

  gfx->setTextColor(COL_LIGHTGREY, COL_BLACK);
  gfx->setTextSize(1);
  gfx->setCursor(x, y + codeW + 4);
  gfx->print(label);
}

void drawSetupScreen() {
  gfx->fillScreen(COL_BLACK);
  drawHeader("SETUP MODE", COL_BLACK);

  const char *wifiQr = "WIFI:T:nopass;S:BusMonitor-Setup;;";
  const char *urlQr = "http://192.168.4.1/";
  const int qrTop = 38;
  const int qrX = 190;
  const int qrSize = 112;

  if (gSetupPage == SETUP_PAGE_WIFI) {
    gfx->setTextColor(COL_CYAN, COL_BLACK);
    gfx->setTextSize(2);
    gfx->setCursor(10, 38);
    gfx->print("1) Connect WiFi");

    gfx->setTextColor(COL_YELLOW, COL_BLACK);
    gfx->setTextSize(2);
    gfx->setCursor(10, 66);
    gfx->print(AP_SSID);

    gfx->setTextColor(COL_LIGHTGREY, COL_BLACK);
    gfx->setTextSize(1);
    gfx->setCursor(10, 96);
    gfx->print("Open phone camera and scan");
    if (!gSetupReason.isEmpty()) {
      gfx->setTextColor(COL_ORANGE, COL_BLACK);
      gfx->setCursor(10, 112);
      gfx->print(gSetupReason);
    }

    drawQrCode(qrX, qrTop, wifiQr, "Join WiFi", qrSize);
    drawFooter("Page 1/2  BOOT: Next", COL_BLACK);
  } else {
    gfx->setTextColor(COL_GREEN, COL_BLACK);
    gfx->setTextSize(2);
    gfx->setCursor(10, 38);
    gfx->print("2) Open Setup");

    gfx->setTextColor(COL_WHITE, COL_BLACK);
    gfx->setTextSize(2);
    gfx->setCursor(10, 66);
    gfx->print("192.168.4.1");

    gfx->setTextColor(COL_LIGHTGREY, COL_BLACK);
    gfx->setTextSize(1);
    gfx->setCursor(10, 96);
    gfx->print("Scan to open web setup");
    if (!gSetupReason.isEmpty()) {
      gfx->setTextColor(COL_ORANGE, COL_BLACK);
      gfx->setCursor(10, 112);
      gfx->print(gSetupReason);
    }

    drawQrCode(qrX, qrTop, urlQr, "Open Setup", qrSize);
    drawFooter("Page 2/2  BOOT: Back", COL_BLACK);
  }
}

void handleSetupPageButton() {
  bool pressed = (digitalRead(PIN_FACTORY_BTN) == LOW);
  uint32_t now = millis();

  if (pressed && !gSetupBtnDown) {
    gSetupBtnDown = true;
    gSetupBtnDownMs = now;
    return;
  }

  if (!pressed && gSetupBtnDown) {
    gSetupBtnDown = false;
    uint32_t heldMs = now - gSetupBtnDownMs;
    // Short press cycles setup pages; keep long-press behavior reserved.
    if (heldMs >= 50 && heldMs < 1200) {
      gSetupPage = (gSetupPage == SETUP_PAGE_WIFI) ? SETUP_PAGE_PORTAL : SETUP_PAGE_WIFI;
      gLastUserActivityMs = now;
      setBacklightDuty(BACKLIGHT_SETUP_DUTY);
      drawSetupScreen();
    }
  }
}

void drawWifiLostScreen() {
  setDisplayError("E_WIFI_02", "WiFi disconnected");
  setBacklightDuty(BACKLIGHT_WIFI_LOST_DUTY);
  gfx->fillScreen(COL_BLACK);
  drawHeader("WIFI DISCONNECTED", COL_BLACK);

  gfx->setTextColor(COL_WHITE, COL_BLACK);
  gfx->setTextSize(2);
  gfx->setCursor(10, 56);
  gfx->print("Reconnecting...");

  drawFooter("E_WIFI_02 Check router/password", COL_BLACK);
}

void drawWeatherIconCompact(int x, int y) {
  // Simple sun-cloud icon to mimic the reference layout.
  gfx->fillCircle(x + 10, y + 10, 8, COL_ORANGE);
  gfx->drawCircle(x + 10, y + 10, 8, COL_YELLOW);
  gfx->fillCircle(x + 20, y + 18, 7, COL_LIGHTGREY);
  gfx->fillCircle(x + 12, y + 20, 6, COL_WHITE);
  gfx->fillRect(x + 10, y + 20, 18, 6, COL_WHITE);
}

void drawBusScreen() {
  const int w = gfx->width();
  const int h = gfx->height();
  const int headerH = 24;
  const int rowH = 49;
  const int maxRows = 3;
  const int leftX = 8;
  const int leftW = 188;
  const int eta1BoxX = 206;
  const int eta1BoxW = 60;
  const int eta2BoxX = 270;
  const int eta2BoxW = 42;

  gfx->fillScreen(COL_BLACK);
  gfx->drawRect(0, 0, w, h, COL_BLACK);

  gfx->fillRect(1, 1, w - 2, headerH - 1, COL_BLACK);
  gfx->setTextSize(1);
  gfx->setTextColor(COL_WHITE, COL_BLACK);
  gfx->setCursor(8, 7);
  gfx->print("My Favorites");
  if (gStopCount > 1) {
    gfx->setCursor(80, 7);
    gfx->print("#");
    gfx->print(gActiveStopIndex + 1);
    gfx->print("/");
    gfx->print(gStopCount);
  }
  gfx->setTextColor(COL_LIGHTGREY, COL_BLACK);
  gfx->setCursor(w - 110, 7);
  gfx->print("Updated ");
  gfx->print(currentTime12Text());

  if (!gDisplayErrorCode.isEmpty()) {
    String e = gDisplayErrorCode;
    if (!gDisplayErrorText.isEmpty()) {
      e += " ";
      e += gDisplayErrorText;
    }
    e = ellipsizeToWidth(e, w - 12, 1);
    gfx->setTextColor(COL_ORANGE, COL_BLACK);
    gfx->setCursor(8, 15);
    gfx->print(e);
  }

  String stopLabel = trimCopy(gCfg.busStopName);
  if (stopLabel.isEmpty()) stopLabel = "Stop " + gCfg.busStop;
  stopLabel = ellipsizeToWidth(stopLabel, leftW, 1);

  for (int i = 0; i <= maxRows; i++) {
    int y = headerH + i * rowH;
    if (y < h) gfx->drawFastHLine(1, y, w - 2, 0x0000);
  }

  for (int i = 0; i < gLineCount && i < maxRows; i++) {
    int ry = headerH + i * rowH;
    const BusLineStatus &b = gBusLines[i];
    const uint16_t rowBg = COL_BLACK;
    gfx->fillRect(1, ry + 1, w - 2, rowH - 1, rowBg);
    gfx->drawFastVLine(eta1BoxX - 4, ry + 6, rowH - 12, 0x0000);
    gfx->drawFastVLine(eta2BoxX - 4, ry + 6, rowH - 12, 0x0000);

    gfx->setTextColor(COL_WHITE, rowBg);
    gfx->setTextSize(3);
    gfx->setCursor(leftX, ry + 8);
    gfx->print(b.service);

    gfx->setTextSize(1);
    gfx->setTextColor(0x7BEF, rowBg);
    gfx->setCursor(leftX, ry + 34);
    gfx->print(stopLabel);

    uint16_t etaColor = COL_WHITE;
    String eta1 = etaToText(b.eta1);
    if (b.eta1 > 0) {
      if (b.eta1 > 99) eta1 = "99+";
      else {
        char buf[4];
        snprintf(buf, sizeof(buf), "%02d", b.eta1);
        eta1 = String(buf);
      }
    }
    String eta2 = etaToText(b.eta2);
    if (b.eta2 > 0) eta2 = (b.eta2 > 99) ? "99+" : String(b.eta2);

    if (eta1 == "Arr") {
      etaColor = 0xF98B;
    } else if (b.load == "SEA") {
      etaColor = 0x07E0;
    }

    uint8_t eta1Size = (eta1.length() > 3) ? 2 : 3;
    gfx->setTextSize(eta1Size);
    gfx->setTextColor(etaColor, rowBg);
    int eta1w = (int)textWidthPx(eta1, eta1Size);
    int eta1x = eta1BoxX + ((eta1BoxW - eta1w) / 2);
    if (eta1x < eta1BoxX) eta1x = eta1BoxX;
    gfx->setCursor(eta1x, ry + ((eta1Size == 3) ? 8 : 13));
    gfx->print(eta1);

    gfx->setTextSize(2);
    gfx->setTextColor((b.load == "SDA") ? COL_ORANGE : COL_WHITE, rowBg);
    int eta2w = (int)textWidthPx(eta2, 2);
    int eta2x = eta2BoxX + ((eta2BoxW - eta2w) / 2);
    if (eta2x < eta2BoxX) eta2x = eta2BoxX;
    gfx->setCursor(eta2x, ry + 12);
    gfx->print(eta2);

    gfx->setTextSize(1);
    gfx->setTextColor(0x632C, rowBg);
    gfx->setCursor(eta2BoxX + 7, ry + 34);
    gfx->print("Next");

    int dotY = ry + 41;
    if (b.load == "SEA") {
      gfx->fillCircle(eta1BoxX + 22, dotY, 2, 0x07E0);
      gfx->fillCircle(eta1BoxX + 30, dotY, 2, 0x07E0);
    } else if (b.load == "SDA") {
      gfx->fillCircle(eta1BoxX + 26, dotY, 2, 0x07E0);
    }
  }
}

void drawWeatherScreen() {
  const int w = gfx->width();
  const int h = gfx->height();

  gfx->fillScreen(COL_BLACK);
  gfx->drawRect(0, 0, w, h, COL_BLACK);
  gfx->drawFastHLine(1, 86, w - 2, 0x0000);

  drawWeatherIconCompact(12, 24);

  gfx->setTextColor(COL_WHITE, COL_BLACK);
  gfx->setTextSize(5);
  gfx->setCursor(56, 26);
  if (isnan(gWeatherTemp)) {
    gfx->print("--");
  } else {
    gfx->print((int)(gWeatherTemp + 0.5f));
  }
  gfx->setTextSize(2);
  gfx->setTextColor(COL_LIGHTGREY, COL_BLACK);
  gfx->setCursor(118, 22);
  gfx->print("oC");

  gfx->setTextSize(2);
  gfx->setTextColor(COL_WHITE, COL_BLACK);
  gfx->setCursor(220, 32);
  gfx->print(gWeatherDesc);
  gfx->setTextSize(1);
  gfx->setTextColor(0x7BEF, COL_BLACK);
  gfx->setCursor(228, 52);
  gfx->print("Singapore, SG");

  int row1Y = 100;
  int row2Y = 132;

  gfx->fillCircle(16, row1Y + 8, 10, 0xFEA0);
  gfx->setTextSize(1);
  gfx->setTextColor(COL_BLACK, 0xFEA0);
  gfx->setCursor(10, row1Y + 5);
  gfx->print("Au");
  gfx->setTextSize(2);
  gfx->setTextColor(COL_WHITE, COL_BLACK);
  gfx->setCursor(34, row1Y + 1);
  gfx->print("Gold");
  gfx->setCursor(188, row1Y + 1);
  gfx->print(formatMoney2(gGoldPrice));
  if (!isnan(gGoldChangePct)) {
    uint16_t boxCol = (gGoldChangePct >= 0.0f) ? 0x03E0 : 0x7800;
    uint16_t txtCol = (gGoldChangePct >= 0.0f) ? COL_BLACK : COL_WHITE;
    gfx->fillRect(270, row1Y, 45, 16, boxCol);
    gfx->setTextColor(txtCol, boxCol);
    gfx->setTextSize(1);
    char pct[12];
    snprintf(pct, sizeof(pct), "%+.1f%%", gGoldChangePct);
    gfx->setCursor(275, row1Y + 5);
    gfx->print(pct);
  }

  gfx->fillCircle(16, row2Y + 8, 10, COL_ORANGE);
  gfx->setTextSize(1);
  gfx->setTextColor(COL_WHITE, COL_ORANGE);
  gfx->setCursor(13, row2Y + 5);
  gfx->print("B");
  gfx->setTextSize(2);
  gfx->setTextColor(COL_WHITE, COL_BLACK);
  gfx->setCursor(34, row2Y + 1);
  gfx->print("BTC");
  gfx->setCursor(210, row2Y + 1);
  gfx->print(formatMoney0(gBtcUsd));
  if (!isnan(gBtcChangePct)) {
    uint16_t boxCol = (gBtcChangePct >= 0.0f) ? 0x03E0 : 0x7800;
    uint16_t txtCol = (gBtcChangePct >= 0.0f) ? COL_BLACK : COL_WHITE;
    gfx->fillRect(270, row2Y, 45, 16, boxCol);
    gfx->setTextColor(txtCol, boxCol);
    gfx->setTextSize(1);
    char pct[12];
    snprintf(pct, sizeof(pct), "%+.1f%%", gBtcChangePct);
    gfx->setCursor(275, row2Y + 5);
    gfx->print(pct);
  }

  drawErrorFooterIfAny();
}

void drawMarketScreen() {
  drawWeatherScreen();
}

void drawCurrentScreen(bool force = false) {
  if (!force && millis() - gLastDrawMs < 1000) return;
  gLastDrawMs = millis();

  switch (gActiveScreen) {
    case SCREEN_BUS:
      drawBusScreen();
      break;
    case SCREEN_WEATHER:
      drawWeatherScreen();
      break;
    case SCREEN_MARKET:
      drawWeatherScreen();
      break;
  }
}

void enterSetupMode() {
  gSetupMode = true;
  gSetupPage = SETUP_PAGE_WIFI;
  gSetupBtnDown = false;
  gSetupBtnDownMs = 0;
  WiFi.mode(WIFI_AP_STA);
  WiFi.setTxPower(WIFI_POWER_11dBm);
  WiFi.setSleep(true);
  WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET);
  bool apOk = false;
  if (strlen(AP_PASSWORD) == 0) {
    apOk = WiFi.softAP(AP_SSID);
  } else {
    apOk = WiFi.softAP(AP_SSID, AP_PASSWORD);
  }
  startWebServer();
  gLastUserActivityMs = millis();
  setBacklightDuty(BACKLIGHT_SETUP_DUTY);
  drawSetupScreen();

  Serial.printf("[SETUP] AP start=%d SSID: %s IP: %s\n", apOk ? 1 : 0, AP_SSID, WiFi.softAPIP().toString().c_str());
}

void enterRunMode() {
  gSetupMode = false;
  setBacklightDuty(BACKLIGHT_RUN_DUTY);
  startWebServer();
  syncClockSg();

  rebuildMonitoredStopsFromConfig();
  gActiveStopIndex = 0;
  applyActiveStop(true);
  gLastStopSwitchMs = millis();

  gLastRotateCycleMs = millis();
  gActiveScreen = SCREEN_BUS;
  gBusPoll.nextFetchMs = 0;
  gWeatherPoll.nextFetchMs = 0;
  gMarketPoll.nextFetchMs = 0;
  gStopInfoPoll.nextFetchMs = 0;
  gBusPoll.failStreak = 0;
  gWeatherPoll.failStreak = 0;
  gMarketPoll.failStreak = 0;
  gStopInfoPoll.failStreak = 0;

  Serial.printf("[RUN] Connected. IP=%s\n", WiFi.localIP().toString().c_str());
}

bool parseStopNamePayload(const String &payload, const String &stopCode, String &nameOut, bool &parseError) {
  parseError = false;

  // Parse only fields we need to avoid memory blowups if API returns many rows.
  DynamicJsonDocument filter(256);
  filter["value"][0]["BusStopCode"] = true;
  filter["value"][0]["Description"] = true;
  filter["value"][0]["RoadName"] = true;
  filter["BusStops"][0]["BusStopCode"] = true;
  filter["BusStops"][0]["Description"] = true;
  filter["BusStops"][0]["RoadName"] = true;

  DynamicJsonDocument doc(8 * 1024);
  DeserializationError err = deserializeJson(doc, payload, DeserializationOption::Filter(filter));
  if (err) {
    // Fallback: parse full payload with larger buffer for stricter responses.
    DynamicJsonDocument fullDoc(48 * 1024);
    DeserializationError err2 = deserializeJson(fullDoc, payload);
    if (err2) {
      parseError = true;
      return false;
    }

    JsonArray fullArr = fullDoc["value"].as<JsonArray>();
    if (fullArr.isNull()) fullArr = fullDoc["BusStops"].as<JsonArray>();
    if (fullArr.isNull()) {
      parseError = true;
      return false;
    }

    for (JsonObject row : fullArr) {
      String code = trimCopy(String((const char *)(row["BusStopCode"] | "")));
      if (!stopCode.isEmpty() && code != stopCode) continue;

      String desc = trimCopy(String((const char *)(row["Description"] | "")));
      if (desc.isEmpty()) {
        desc = trimCopy(String((const char *)(row["RoadName"] | "")));
      }
      if (!desc.isEmpty()) {
        nameOut = desc;
        return true;
      }
    }
    return false;
  }

  JsonArray arr = doc["value"].as<JsonArray>();
  if (arr.isNull()) arr = doc["BusStops"].as<JsonArray>();
  if (arr.isNull()) {
    parseError = false;
    return false;
  }

  for (JsonObject row : arr) {
    String code = trimCopy(String((const char *)(row["BusStopCode"] | "")));
    if (!stopCode.isEmpty() && code != stopCode) continue;

    String desc = trimCopy(String((const char *)(row["Description"] | "")));
    if (desc.isEmpty()) {
      desc = trimCopy(String((const char *)(row["RoadName"] | "")));
    }
    if (!desc.isEmpty()) {
      nameOut = desc;
      return true;
    }
  }

  return false;
}

bool fetchBusStopName() {
  if (WiFi.status() != WL_CONNECTED) return false;
  gStopInfoRateLimited = false;

  const String stopCode = normalizeBusStopCode(gCfg.busStop);
  if (stopCode.isEmpty() || gCfg.apiKey.isEmpty()) {
    setDisplayError("E_STOP_00", "Missing stop/API key");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  const String url = "https://datamall2.mytransport.sg/ltaodataservice/BusStops?$filter=BusStopCode%20eq%20%27" + stopCode + "%27";

  if (!http.begin(client, url)) {
    setDisplayError("E_STOP_01", "HTTP begin failed");
    return false;
  }

  http.setTimeout(10000);
  http.addHeader("AccountKey", gCfg.apiKey);
  http.addHeader("accept", "application/json");

  int statusCode = http.GET();
  if (statusCode == 429) {
    gStopInfoRateLimited = true;
    setDisplayError("E_STOP_429", "Rate limited");
  }
  if (statusCode != 200) {
    http.end();
    setDisplayError("E_STOP_" + String(statusCode), "Bus stop HTTP failed");
    return false;
  }

  String payload = http.getString();
  http.end();

  String apiName;
  bool parseError = false;
  if (!parseStopNamePayload(payload, stopCode, apiName, parseError)) {
    // Stop-name lookup is best-effort. Don't block run mode if parsing fails.
    if (gCfg.busStopName.isEmpty()) {
      gCfg.busStopName = "Stop " + stopCode;
      if (gStopCount > 0 && gActiveStopIndex >= 0 && gActiveStopIndex < gStopCount) {
        gStops[gActiveStopIndex].name = gCfg.busStopName;
        gCfg.stopsJson = serializeStopsJson(gStops, gStopCount);
      }
      saveConfig(gCfg);
    }
    if (parseError) {
      Serial.printf("[WARN][E_STOP_02] parse failed for stop %s\n", stopCode.c_str());
    } else {
      Serial.printf("[WARN][E_STOP_03] name not found for stop %s\n", stopCode.c_str());
    }
    clearDisplayErrorPrefix("E_STOP_");
    return true;
  }

  apiName.trim();
  if (apiName.isEmpty()) {
    if (gCfg.busStopName.isEmpty()) {
      gCfg.busStopName = "Stop " + stopCode;
      if (gStopCount > 0 && gActiveStopIndex >= 0 && gActiveStopIndex < gStopCount) {
        gStops[gActiveStopIndex].name = gCfg.busStopName;
        gCfg.stopsJson = serializeStopsJson(gStops, gStopCount);
      }
      saveConfig(gCfg);
    }
    clearDisplayErrorPrefix("E_STOP_");
    return true;
  }

  if (gCfg.busStopName != apiName) {
    gCfg.busStopName = apiName;
    if (gStopCount > 0 && gActiveStopIndex >= 0 && gActiveStopIndex < gStopCount) {
      gStops[gActiveStopIndex].name = apiName;
      gCfg.stopsJson = serializeStopsJson(gStops, gStopCount);
    }
    saveConfig(gCfg);
    Serial.printf("[BUSSTOP] %s -> %s\n", stopCode.c_str(), gCfg.busStopName.c_str());
  }

  clearDisplayErrorPrefix("E_STOP_");

  return true;
}

bool fetchBusData() {
  if (WiFi.status() != WL_CONNECTED) return false;
  gBusRateLimited = false;

  int clearCount = gUseAllServices ? MAX_LINES : gLineCount;
  for (int i = 0; i < clearCount; i++) {
    gBusLines[i].service = gLines[i];
    gBusLines[i].eta1 = -1;
    gBusLines[i].eta2 = -1;
    gBusLines[i].hasData = false;
    gBusLines[i].load = "";
  }

  const String url = "https://datamall2.mytransport.sg/ltaodataservice/v3/BusArrival?BusStopCode=" + gCfg.busStop;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, url)) {
    gBusMessage = "HTTP begin fail";
    setDisplayError("E_BUS_00", "HTTP begin failed");
    return false;
  }

  http.setTimeout(10000);
  http.addHeader("AccountKey", gCfg.apiKey);
  http.addHeader("accept", "application/json");

  int code = http.GET();
  if (code != 200) {
    if (code == 429) {
      gBusRateLimited = true;
      setDisplayError("E_BUS_429", "Rate limited");
    } else {
      setDisplayError("E_BUS_" + String(code), "Bus API failed");
    }
    gBusMessage = "Bus API " + String(code);
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();

  DynamicJsonDocument doc(24 * 1024);
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    gBusMessage = "Bus JSON error";
    setDisplayError("E_BUS_01", "Bus JSON parse failed");
    return false;
  }

  JsonArray services = doc["Services"].as<JsonArray>();
  bool anyLineMatched = false;
  if (gUseAllServices) {
    gLineCount = 0;
    for (JsonObject svc : services) {
      if (gLineCount >= MAX_LINES) break;
      String svcNo = String((const char *)svc["ServiceNo"]);
      if (svcNo.isEmpty()) continue;
      anyLineMatched = true;
      gLines[gLineCount] = svcNo;
      gBusLines[gLineCount].service = svcNo;

      const char *eta1 = svc["NextBus"]["EstimatedArrival"] | "";
      const char *eta2 = svc["NextBus2"]["EstimatedArrival"] | "";
      const char *load = svc["NextBus"]["Load"] | "";

      gBusLines[gLineCount].eta1 = parseEtaMinutes(eta1);
      gBusLines[gLineCount].eta2 = parseEtaMinutes(eta2);
      gBusLines[gLineCount].load = String(load);
      gBusLines[gLineCount].hasData = true;
      gLineCount++;
    }
  } else {
    for (JsonObject svc : services) {
      String svcNo = String((const char *)svc["ServiceNo"]);
      int idx = findLineIndex(svcNo);
      if (idx < 0) continue;
      anyLineMatched = true;

      const char *eta1 = svc["NextBus"]["EstimatedArrival"] | "";
      const char *eta2 = svc["NextBus2"]["EstimatedArrival"] | "";
      const char *load = svc["NextBus"]["Load"] | "";

      gBusLines[idx].eta1 = parseEtaMinutes(eta1);
      gBusLines[idx].eta2 = parseEtaMinutes(eta2);
      gBusLines[idx].load = String(load);
      gBusLines[idx].hasData = true;
    }
  }

  if (!anyLineMatched) {
    setDisplayError("E_BUS_02", gUseAllServices ? "No services at stop" : "Bus lines not found");
  } else {
    clearDisplayErrorPrefix("E_BUS_");
  }

  gBusMessage = "Updated " + currentTimeText();
  return true;
}

bool fetchWeatherData() {
  if (WiFi.status() != WL_CONNECTED) return false;
  gWeatherRateLimited = false;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  String forecastPayload;
  if (!http.begin(client, "https://api.data.gov.sg/v1/environment/2-hour-weather-forecast")) {
    setDisplayError("E_WTH_00", "Forecast HTTP begin failed");
    return false;
  }
  http.setTimeout(10000);
  int code = http.GET();
  if (code == 200) {
    forecastPayload = http.getString();
  } else if (code == 429) {
    gWeatherRateLimited = true;
    setDisplayError("E_WTH_429", "Weather rate limited");
  }
  http.end();

  if (code != 200) {
    setDisplayError("E_WTH_" + String(code), "Forecast API failed");
    return false;
  }

  DynamicJsonDocument forecastDoc(28 * 1024);
  if (deserializeJson(forecastDoc, forecastPayload)) {
    setDisplayError("E_WTH_01", "Forecast JSON parse failed");
    return false;
  }

  String areaWanted = gCfg.weatherArea;
  areaWanted.trim();
  if (areaWanted.isEmpty()) areaWanted = "City";

  String forecastText = "Unknown";
  JsonArray forecasts = forecastDoc["items"][0]["forecasts"].as<JsonArray>();

  for (JsonObject f : forecasts) {
    String area = String((const char *)f["area"]);
    if (area.equalsIgnoreCase(areaWanted)) {
      forecastText = String((const char *)f["forecast"]);
      break;
    }
  }

  if (forecastText == "Unknown" && !forecasts.isNull() && forecasts.size() > 0) {
    forecastText = String((const char *)forecasts[0]["forecast"]);
  }

  gWeatherDesc = forecastText;

  if (!http.begin(client, "https://api.data.gov.sg/v1/environment/air-temperature")) {
    clearDisplayErrorPrefix("E_WTH_");
    return true;
  }
  code = http.GET();
  String tempPayload = (code == 200) ? http.getString() : "";
  if (code == 429) {
    gWeatherRateLimited = true;
    setDisplayError("E_WTH_429", "Weather rate limited");
  }
  http.end();

  if (code == 200) {
    DynamicJsonDocument tempDoc(8 * 1024);
    if (!deserializeJson(tempDoc, tempPayload)) {
      gWeatherTemp = tempDoc["items"][0]["readings"][0]["value"] | gWeatherTemp;
    }
  }

  if (!http.begin(client, "https://api.data.gov.sg/v1/environment/relative-humidity")) {
    clearDisplayErrorPrefix("E_WTH_");
    return true;
  }
  code = http.GET();
  String humPayload = (code == 200) ? http.getString() : "";
  if (code == 429) {
    gWeatherRateLimited = true;
    setDisplayError("E_WTH_429", "Weather rate limited");
  }
  http.end();

  if (code == 200) {
    DynamicJsonDocument humDoc(8 * 1024);
    if (!deserializeJson(humDoc, humPayload)) {
      gWeatherHumidity = humDoc["items"][0]["readings"][0]["value"] | gWeatherHumidity;
    }
  }

  clearDisplayErrorPrefix("E_WTH_");
  return true;
}

float parseStooqClose(const String &csvLine) {
  int parts = 0;
  int from = 0;
  String closeVal;

  while (from <= csvLine.length()) {
    int comma = csvLine.indexOf(',', from);
    if (comma < 0) comma = csvLine.length();
    String token = csvLine.substring(from, comma);
    if (parts == 6) {
      closeVal = token;
      break;
    }
    parts++;
    from = comma + 1;
  }

  closeVal.trim();
  if (closeVal.isEmpty()) return NAN;
  return closeVal.toFloat();
}

bool fetchMarketData() {
  if (WiFi.status() != WL_CONNECTED) return false;
  gMarketRateLimited = false;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  bool anyOk = false;

  if (http.begin(client, "https://api.coingecko.com/api/v3/simple/price?ids=bitcoin&vs_currencies=usd")) {
    http.setTimeout(10000);
    int code = http.GET();
    if (code == 200) {
      DynamicJsonDocument btcDoc(1024);
      if (!deserializeJson(btcDoc, http.getString())) {
        JsonVariant v = btcDoc["bitcoin"]["usd"];
        if (!v.isNull()) {
          float newBtc = v.as<float>();
          if (!isnan(gBtcUsd) && gBtcUsd > 0.0f && newBtc > 0.0f) {
            gBtcChangePct = ((newBtc - gBtcUsd) / gBtcUsd) * 100.0f;
          }
          gBtcUsd = newBtc;
          anyOk = true;
        }
      }
    } else if (code == 429) {
      gMarketRateLimited = true;
      setDisplayError("E_MKT_429", "Market rate limited");
    } else {
      setDisplayError("E_MKT_" + String(code), "BTC API failed");
    }
    http.end();
  } else {
    setDisplayError("E_MKT_00", "BTC HTTP begin failed");
  }

  bool goldOk = false;
  if (http.begin(client, "https://api.gold-api.com/price/XAU")) {
    http.setTimeout(10000);
    int code = http.GET();
    if (code == 200) {
      DynamicJsonDocument goldDoc(2048);
      if (!deserializeJson(goldDoc, http.getString())) {
        JsonVariant v = goldDoc["price"];
        if (!v.isNull()) {
          float newGold = v.as<float>();
          if (!isnan(gGoldPrice) && gGoldPrice > 0.0f && newGold > 0.0f) {
            gGoldChangePct = ((newGold - gGoldPrice) / gGoldPrice) * 100.0f;
          }
          gGoldPrice = newGold;
          goldOk = true;
          anyOk = true;
        }
      }
    } else if (code == 429) {
      gMarketRateLimited = true;
      setDisplayError("E_MKT_429", "Market rate limited");
    } else {
      setDisplayError("E_MKT_" + String(code), "Gold API failed");
    }
    http.end();
  }

  if (!goldOk && http.begin(client, "https://stooq.com/q/l/?s=xauusd&i=d")) {
    int code = http.GET();
    if (code == 200) {
      float fallback = parseStooqClose(http.getString());
      if (!isnan(fallback)) {
        if (!isnan(gGoldPrice) && gGoldPrice > 0.0f && fallback > 0.0f) {
          gGoldChangePct = ((fallback - gGoldPrice) / gGoldPrice) * 100.0f;
        }
        gGoldPrice = fallback;
        anyOk = true;
      }
    }
    http.end();
  }

  if (anyOk) {
    clearDisplayErrorPrefix("E_MKT_");
  } else if (gDisplayErrorCode.isEmpty() || !gDisplayErrorCode.startsWith("E_MKT_")) {
    setDisplayError("E_MKT_01", "Market data unavailable");
  }

  return anyOk;
}

void updateScreenRotation() {
  const uint32_t now = millis();

  if (gActiveScreen == SCREEN_BUS) {
    if (now - gLastRotateCycleMs >= ROTATE_EVERY_MS) {
      gActiveScreen = SCREEN_WEATHER;
      gSpecialScreenStartMs = now;
      drawCurrentScreen(true);
    }
    return;
  }

  if (gActiveScreen == SCREEN_WEATHER) {
    if (now - gSpecialScreenStartMs >= SPECIAL_SCREEN_MS) {
      gActiveScreen = SCREEN_BUS;
      gLastRotateCycleMs = now;
      drawCurrentScreen(true);
    }
  }
}

bool initDisplay() {
  // Auto-select profile: touch board exposes an I2C touch controller.
  bool hasTouch = detectTouchController();
  const DisplayProfile *preferred = hasTouch ? &PROFILE_TOUCH : &PROFILE_STD;
  const DisplayProfile *fallback = hasTouch ? &PROFILE_STD : &PROFILE_TOUCH;

  gDisplayProfile = preferred;
  if (initDisplayForProfile(*preferred)) {
    return true;
  }

  if (initDisplayForProfile(*fallback)) {
    gDisplayProfile = fallback;
    return true;
  }

  return false;
}

void setup() {
  Serial.begin(115200);
  delay(200);
  applyThermalPolicy();
  Serial.printf("[BOOT] Firmware: %s\n", FW_BUILD_TAG);

  if (!initDisplay()) {
    Serial.println("[BOOT] Display init failed");
    while (true) {
      delay(1000);
    }
  }

  runDisplaySelfTest();
  drawBootScreen();

  if (checkAndHandleFactoryReset()) {
    return;
  }

  loadConfig();

  if (!gCfg.isValid()) {
    gSetupReason = "E_CFG_00 No saved config";
    setDisplayError("E_CFG_00", "No saved config");
    Serial.println("[BOOT] No valid config, entering setup mode.");
    enterSetupMode();
    return;
  }

  if (!connectToWifi()) {
    Serial.println("[BOOT] Wi-Fi connect failed, entering setup mode.");
    enterSetupMode();
    return;
  }

  enterRunMode();
  bool stopInfoOk = fetchBusStopName();
  if (stopInfoOk) {
    schedulePollSuccess(gStopInfoPoll, millis());
  } else {
    schedulePollFailure(gStopInfoPoll, millis(), gStopInfoRateLimited);
  }

  bool busOk = fetchBusData();
  if (busOk) {
    schedulePollSuccess(gBusPoll, millis());
  } else {
    schedulePollFailure(gBusPoll, millis(), gBusRateLimited);
  }

  bool weatherOk = fetchWeatherData();
  if (weatherOk) {
    schedulePollSuccess(gWeatherPoll, millis());
  } else {
    schedulePollFailure(gWeatherPoll, millis(), gWeatherRateLimited);
  }

  bool marketOk = fetchMarketData();
  if (marketOk) {
    schedulePollSuccess(gMarketPoll, millis());
  } else {
    schedulePollFailure(gMarketPoll, millis(), gMarketRateLimited);
  }

  drawCurrentScreen(true);
}

void loop() {
  if (gServerStarted) {
    webServer.handleClient();
  }

  if (gSetupMode) {
    handleSetupPageButton();
    if (millis() - gLastUserActivityMs > SETUP_IDLE_DIM_MS) {
      setBacklightDuty(BACKLIGHT_SETUP_IDLE_DUTY);
    }
    delay(SETUP_LOOP_DELAY_MS);
    return;
  }

  if (pollRuntimeFactoryReset()) {
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    drawWifiLostScreen();
    if (millis() - gLastWifiRetryMs > 15000) {
      gLastWifiRetryMs = millis();
      WiFi.reconnect();
    }
    delay(WIFI_LOST_LOOP_DELAY_MS);
    return;
  }

  clearDisplayErrorPrefix("E_WIFI_02");

  if (!gTimeSynced && millis() > 20000) {
    syncClockSg();
  }

  uint32_t now = millis();

  if (gStopCount > 1 && (now - gLastStopSwitchMs) >= STOP_SWITCH_MS) {
    gLastStopSwitchMs = now;
    gActiveStopIndex = (gActiveStopIndex + 1) % gStopCount;
    applyActiveStop(true);

    bool stopOk = fetchBusStopName();
    uint32_t ts1 = millis();
    if (stopOk) {
      schedulePollSuccess(gStopInfoPoll, ts1);
    } else {
      schedulePollFailure(gStopInfoPoll, ts1, gStopInfoRateLimited);
    }

    bool busOk = fetchBusData();
    uint32_t ts2 = millis();
    if (busOk) {
      schedulePollSuccess(gBusPoll, ts2);
    } else {
      schedulePollFailure(gBusPoll, ts2, gBusRateLimited);
    }

    if (gActiveScreen == SCREEN_BUS) drawCurrentScreen(true);
  }

  if (isTimeDue(now, gBusPoll.nextFetchMs)) {
    bool ok = fetchBusData();
    uint32_t t = millis();
    if (ok) {
      schedulePollSuccess(gBusPoll, t);
    } else {
      schedulePollFailure(gBusPoll, t, gBusRateLimited);
    }
    if (gActiveScreen == SCREEN_BUS) drawCurrentScreen(true);
  }

  if (isTimeDue(now, gWeatherPoll.nextFetchMs)) {
    bool ok = fetchWeatherData();
    uint32_t t = millis();
    if (ok) {
      schedulePollSuccess(gWeatherPoll, t);
    } else {
      schedulePollFailure(gWeatherPoll, t, gWeatherRateLimited);
    }
    if (gActiveScreen == SCREEN_WEATHER) drawCurrentScreen(true);
  }

  if (isTimeDue(now, gMarketPoll.nextFetchMs)) {
    bool ok = fetchMarketData();
    uint32_t t = millis();
    if (ok) {
      schedulePollSuccess(gMarketPoll, t);
    } else {
      schedulePollFailure(gMarketPoll, t, gMarketRateLimited);
    }
    if (gActiveScreen == SCREEN_WEATHER) drawCurrentScreen(true);
  }

  if (isTimeDue(now, gStopInfoPoll.nextFetchMs)) {
    bool ok = fetchBusStopName();
    uint32_t t = millis();
    if (ok) {
      schedulePollSuccess(gStopInfoPoll, t);
    } else {
      schedulePollFailure(gStopInfoPoll, t, gStopInfoRateLimited);
    }
    if (gActiveScreen == SCREEN_BUS) drawCurrentScreen(true);
  }

  updateScreenRotation();
  drawCurrentScreen(false);

  delay(LOOP_IDLE_DELAY_MS);
}
