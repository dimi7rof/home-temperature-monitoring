#include <Wire.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Adafruit_AHTX0.h>
#include <Adafruit_BMP280.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>
#include <WebServer.h>

#include "secrets.h"

// ---------- PINS ----------
#define I2C_SDA 21
#define I2C_SCL 22

#define TFT_CS 5
#define TFT_DC 25
#define TFT_RST 26

#define ONE_WIRE_BUS 4

// AHT20 self-heating correction
#define INSIDE_TEMP_OFFSET -5.0f
#define HOT_TEMP_OFFSET 0.4f
#define COLD_TEMP_OFFSET 0.3f


// ---------- OBJECTS ----------
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature ds18b20(&oneWire);
Adafruit_AHTX0 aht;
Adafruit_BMP280 bmp;
WebServer server(80);

// ---------- COLORS ----------
#define RED ST77XX_RED
#define BLUE ST77XX_BLUE
#define WHITE ST77XX_WHITE
#define GREEN ST77XX_GREEN
#define YELLOW ST77XX_YELLOW
#define CYAN ST77XX_CYAN
#define ORANGE 0xFD20
#define GRAY 0x8410
#define LIGHT_GRAY 0xC618

// ---------- DISPLAY ----------
// init(170,320) + setRotation(2) -> portrait 170 w x 320 h
#define DISP_W 170
#define DISP_H 320

// ---------- HOUSE GEOMETRY ----------
// Wall spans HOUSE_Y_EAVE..HOUSE_Y_GROUND = 125..310 = 185px
// 185 / 3 = 61px per floor (integer: 61, 61, 63)
#define HOUSE_X1 15
#define HOUSE_X2 155
#define HOUSE_MID_X 85
#define HOUSE_Y_PEAK 150
#define HOUSE_Y_EAVE 190
#define HOUSE_Y_F2 230  // 125 + 61  – F2 / F1 divider
#define HOUSE_Y_F1 270  // 125 + 122 – F1 / F0 divider
#define HOUSE_Y_GROUND 310

// ---------- TEMPERATURES ----------
#define TEMP_X (HOUSE_X1 + 35)
#define TEMP_X_WATER (HOUSE_X1 + 25)
#define TEMP_Y_OUT (HOUSE_Y_PEAK - 40)
#define TEMP_Y_F2 (HOUSE_Y_EAVE + (HOUSE_Y_F2 - HOUSE_Y_EAVE) / 2 - 10)
#define TEMP_Y_F1 (HOUSE_Y_F2 + (HOUSE_Y_F1 - HOUSE_Y_F2) / 2 - 10)
#define TEMP_Y_IN (HOUSE_Y_F1 + (HOUSE_Y_GROUND - HOUSE_Y_F1) / 2 - 10)
#define TEMP_Y_HOT 15
#define TEMP_Y_COLD 55
#define PRESSURE_Y 145
#define PRESSURE_X 10
#define WATER_SIZE 4
#define AIR_SIZE 3
#define PRESSURE_SIZE 1
#define HUMIDITY_SIZE 1
#define HUMIDITY_X 10
#define HUMIDITY_Y (PRESSURE_Y + 15)

// ---------- GOOGLE SHEETS LOGGING ----------
#define LOG_INTERVAL_MS (5UL * 60UL * 1000UL)

// ---------- REMOTE TEMPERATURES ----------
volatile bool otaInProgress = false;
float g2 = DEVICE_DISCONNECTED_C;
float g1 = DEVICE_DISCONNECTED_C;
float gOut = DEVICE_DISCONNECTED_C;
float gHot = DEVICE_DISCONNECTED_C;
float gCold = DEVICE_DISCONNECTED_C;
float g0 = DEVICE_DISCONNECTED_C;
float gPressure = DEVICE_DISCONNECTED_C;
float gHumidity = DEVICE_DISCONNECTED_C;

String buildJson() {
  auto v = [](float val) -> String {
    if (val == DEVICE_DISCONNECTED_C || val < -50.0f) return "null";

    char buf[16];
    dtostrf(val, 1, 2, buf);

    char* p = buf;
    while (*p == ' ') p++;

    return String(p);
  };

  String json = "{";
  json += "\"floor0\":" + v(g0) + ",";
  json += "\"floor1\":" + v(g1) + ",";
  json += "\"floor2\":" + v(g2) + ",";
  json += "\"out\":" + v(gOut) + ",";
  json += "\"pressure\":" + v(gPressure) + ",";
  json += "\"hot\":" + v(gHot) + ",";
  json += "\"cold\":" + v(gCold) + ",";
  json += "\"humidity\":" + v(gHumidity);
  json += "}";

  return json;
}

// Verbose HTTP fetch with full Serial diagnostics
void fetchTemp(const char* url, float& out) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("SKIP (WiFi down)");
    return;
  }

  ArduinoOTA.handle();

  HTTPClient http;
  http.begin(url);
  http.setTimeout(2000);

  int code = http.GET();
  Serial.printf("HTTP %d  ", code);

  if (code == 200) {
    String body = http.getString();

    int idx = body.indexOf("\"temperature\"");
    if (idx == -1) {
      http.end();
      return;
    }
    idx = body.indexOf(':', idx);
    if (idx == -1) {
      http.end();
      return;
    }
    float parsed = body.substring(idx + 1).toFloat();
    out = parsed;
  }

  http.end();
}

// ---------- OTA ----------
void showOTA(const char* label) {
  tft.fillScreen(ST77XX_BLACK);
  tft.setCursor(10, 100);
  tft.setTextColor(WHITE);
  tft.setTextSize(4);
  tft.print(label);
}

// ---------- HELPERS ----------
uint16_t wifiColor(int rssi) {
  if (rssi > -60) return GREEN;
  if (rssi > -75) return YELLOW;
  return RED;
}

// Print temperature at text size 3 (same visual weight as hot/cold)
void eraseAndPrint(int size, int x, int y, float val, uint16_t color, uint16_t bgColor = ST77XX_BLACK) {
  tft.setTextSize(size);
  tft.setCursor(x, y);
  tft.setTextColor(color, bgColor);
  if (val != DEVICE_DISCONNECTED_C && val >= -50.0f) {
    tft.print(val, 1);
  }
}

// ---------- TOP STRIP ----------
void printWifi() {
  int rssi = WiFi.RSSI();
  tft.setTextSize(1);
  tft.setTextColor(wifiColor(rssi), ST77XX_BLACK);
  tft.setCursor(60, 2);
  tft.print("WiFi:");
  tft.print(rssi);
  tft.print("dB");
}

uint16_t getRoomColor(float temp) {
  if (temp == DEVICE_DISCONNECTED_C) return GRAY;
  if (temp > 30.0f) return RED;
  if (temp > 25.0f) return YELLOW;
  if (temp > 20.0f) return GREEN;
  if (temp > 15.0f) return CYAN;
  return BLUE;
}

void printAirTemps() {
  eraseAndPrint(AIR_SIZE, TEMP_X, TEMP_Y_OUT, gOut, getRoomColor(gOut));
  eraseAndPrint(AIR_SIZE, TEMP_X, TEMP_Y_F2, g2, getRoomColor(g2), LIGHT_GRAY);
  eraseAndPrint(AIR_SIZE, TEMP_X, TEMP_Y_F1, g1, getRoomColor(g1), LIGHT_GRAY);
  eraseAndPrint(AIR_SIZE, TEMP_X, TEMP_Y_IN, g0, getRoomColor(g0), LIGHT_GRAY);
}

void printPressure() {
  eraseAndPrint(PRESSURE_SIZE, PRESSURE_X, PRESSURE_Y, gPressure, GRAY);
}

void printHumidity() {
  eraseAndPrint(HUMIDITY_SIZE, HUMIDITY_X, HUMIDITY_Y, gHumidity, GRAY);
}

void printWaterTemps() {
  eraseAndPrint(WATER_SIZE, TEMP_X_WATER, TEMP_Y_HOT, gHot, RED);
  eraseAndPrint(WATER_SIZE, TEMP_X_WATER, TEMP_Y_COLD, gCold, BLUE);
}

void printAllData() {
  printWifi();
  printAirTemps();
  printWaterTemps();
  printPressure();
  printHumidity();
}

// ---------- HOUSE STRUCTURE ----------
void drawHouse() {
  uint16_t wallColor = 0xC618;  // light gray
  uint16_t roofColor = ORANGE;
  uint16_t chimneyCol = 0xC300;  // brick red

  // Chimney – drawn before roof so the eave line covers its base
  tft.fillRect(118, HOUSE_Y_PEAK - 10, 14, 38, chimneyCol);
  tft.drawRect(118, HOUSE_Y_PEAK - 10, 14, 38, WHITE);

  // Filled roof triangle
  for (int y = HOUSE_Y_PEAK; y <= HOUSE_Y_EAVE; y++) {
    float frac = float(y - HOUSE_Y_PEAK) / float(HOUSE_Y_EAVE - HOUSE_Y_PEAK);
    int x0 = HOUSE_MID_X - int(frac * (HOUSE_MID_X - HOUSE_X1 + 5));
    int x1 = HOUSE_MID_X + int(frac * (HOUSE_X2 + 5 - HOUSE_MID_X));
    tft.drawFastHLine(x0, y, x1 - x0 + 1, roofColor);
  }
  // Roof outline
  tft.drawLine(HOUSE_MID_X, HOUSE_Y_PEAK, HOUSE_X1-5, HOUSE_Y_EAVE, WHITE);
  tft.drawLine(HOUSE_MID_X, HOUSE_Y_PEAK, HOUSE_X2+5, HOUSE_Y_EAVE, WHITE);

  // Walls
  tft.fillRect(HOUSE_X1, HOUSE_Y_EAVE,
               HOUSE_X2 - HOUSE_X1, HOUSE_Y_GROUND - HOUSE_Y_EAVE,
               wallColor);
  tft.drawRect(HOUSE_X1, HOUSE_Y_EAVE,
               HOUSE_X2 - HOUSE_X1, HOUSE_Y_GROUND - HOUSE_Y_EAVE,
               WHITE);

  // Floor dividers
  tft.drawFastHLine(HOUSE_X1, HOUSE_Y_F2, HOUSE_X2 - HOUSE_X1, GRAY);
  tft.drawFastHLine(HOUSE_X1, HOUSE_Y_F1, HOUSE_X2 - HOUSE_X1, GRAY);

  // Ground line
  tft.drawFastHLine(0, HOUSE_Y_GROUND, DISP_W, GRAY);
}

void postToSheets() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[SHEETS] SKIP (WiFi down)");
    return;
  }

  ArduinoOTA.handle();

  String body = buildJson();

  HTTPClient http;
  http.begin(SHEETS_URL);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(20000);

  int code = http.POST((uint8_t*)body.c_str(), body.length());
  http.end();
}

unsigned long row = 1;
unsigned long rowH = 20;

// ---------- BOOT STATUS ----------
void showStatus(const char* line, uint16_t color = WHITE) {
  tft.setTextColor(color);
  tft.setTextSize(2);
  int y = row * rowH;
  row++;
  tft.setCursor(5, y);
  tft.print(line);
}

void setupOta() {
  ArduinoOTA.setHostname("w-temp");
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.setTimeout(30000);

  ArduinoOTA
    .onStart([]() {
      otaInProgress = true;
      showOTA("UPDATE");
    })
    .onProgress([](unsigned int p, unsigned int t) {
      static uint8_t lastPercent = 255;

      uint8_t percent = (p * 100) / t;

      if (percent != lastPercent) {
        lastPercent = percent;

        // Update display only every 5%
        if (percent % 10 == 0) {
          tft.fillRect(0, 150, 170, 40, ST77XX_BLACK);

          tft.setCursor(20, 150);
          tft.setTextSize(4);
          tft.setTextColor(GREEN);

          tft.printf("%u%%", percent);
        }
      }
    })
    .onEnd([]() {
      showOTA("DONE!");
      delay(1000);
      ESP.restart();
    })
    .onError([](ota_error_t error) {
      otaInProgress = false;

      showOTA("ERROR!");
      delay(500);
    });

  ArduinoOTA.begin();
}

void drawStaticUI() {
  tft.fillScreen(ST77XX_BLACK);
  tft.drawFastHLine(0, 88, DISP_W, GRAY);
  drawHouse();
}

void readLocalSensors() {
  ds18b20.requestTemperatures();
  ds18b20.setWaitForConversion(true);
  gHot = ds18b20.getTempCByIndex(0) + HOT_TEMP_OFFSET;
  gCold = ds18b20.getTempCByIndex(1) + COLD_TEMP_OFFSET;

  sensors_event_t humidity, temp;
  aht.getEvent(&humidity, &temp);
  g0 = temp.temperature + INSIDE_TEMP_OFFSET;
  gHumidity = humidity.relative_humidity;

  gPressure = bmp.readPressure() / 100.0f;
}

void bootPrintLocalSensors() {
  char buf[32];
  snprintf(buf, sizeof(buf), "Hot = %.1f", gHot);
  showStatus(buf, GREEN);
  delay(500);
  snprintf(buf, sizeof(buf), "Cold = %.1f", gCold);
  showStatus(buf, GREEN);
  delay(500);
  snprintf(buf, sizeof(buf), "Air = %.1f", g0);
  showStatus(buf, GREEN);
  delay(500);
  snprintf(buf, sizeof(buf), "%.1f %%", gHumidity);
  showStatus(buf, GREEN);
  delay(500);
  snprintf(buf, sizeof(buf), "%.1f hPa", gPressure);
  showStatus(buf, GREEN);
}

void fetchOut() {
  fetchTemp(OUT_SENSOR_URL, gOut);
}

void fetchFloor1() {
  fetchTemp(FLOOR1_SENSOR_URL, g1);
}

void fetchFloor2() {
  fetchTemp(FLOOR2_SENSOR_URL, g2);
}

void readRemoteSensors() {
  fetchFloor2();
  fetchFloor1();
  fetchOut();
}

void bootPrintRemoteSensors() {
  readLocalSensors();
  bootPrintLocalSensors();

  char buf[32];
  fetchFloor2();
  if (g2 != DEVICE_DISCONNECTED_C) {
    snprintf(buf, sizeof(buf), "F2 = %.1f", g2);
    showStatus(buf, GREEN);
  }
  else {
    showStatus("F2 FAIL", RED);
  }

  fetchFloor1();
  if (g1 != DEVICE_DISCONNECTED_C) {
    snprintf(buf, sizeof(buf), "F1 = %.1f", g1);
    showStatus(buf, GREEN);
  }
  else {
    showStatus("F1 FAIL", RED);
  }

  fetchOut();
  if (gOut != DEVICE_DISCONNECTED_C) {
    snprintf(buf, sizeof(buf), "OUT = %.1f", gOut);
    showStatus(buf, GREEN);
  }
  else {
    showStatus("OUT FAIL", RED);
  }
}

TaskHandle_t httpTask = NULL;

void httpWorker(void* param) {
  TickType_t lastLog = 0;   // 0 forces immediate post on first wake

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(30000));

    readRemoteSensors();

    if (xTaskGetTickCount() - lastLog >= pdMS_TO_TICKS(LOG_INTERVAL_MS)) {
      postToSheets();
      lastLog = xTaskGetTickCount();
    }
  }
}

void setup() {
  Serial.begin(115200);

  // Display first
  tft.init(170, 320);
  tft.setRotation(2);
  tft.fillScreen(ST77XX_BLACK);

  // I2C
  Wire.begin(I2C_SDA, I2C_SCL);

  // DS18B20
  ds18b20.begin();
  ds18b20.requestTemperatures();
  ds18b20.setWaitForConversion(true);
  float Hot = ds18b20.getTempCByIndex(0);
  float Cold = ds18b20.getTempCByIndex(1);
  if (Hot == DEVICE_DISCONNECTED_C || Cold == DEVICE_DISCONNECTED_C) {
    showStatus("DS18B20 FAIL", RED);
  }
  else {
    showStatus("DS18B20 OK", GREEN);
  }

  // AHT
  if (aht.begin()) {
    showStatus("AHT20 OK", GREEN);
  }
  else {
    showStatus("AHT20 FAIL", RED);
  }

  // BMP
  if (bmp.begin()) {
    showStatus("BMP280 OK", GREEN);
  }
  else {
    showStatus("BMP280 FAIL", RED);
  }

  // WiFi
  showStatus("Connecting...");

  WiFi.setSleep(false);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    ArduinoOTA.handle();
    delay(500);
  }

  String ip = WiFi.localIP().toString();
  showStatus(ip.c_str(), GREEN);

  xTaskCreatePinnedToCore(
    httpWorker, "httpWorker",
    8192,        // stack
    NULL, 1,     // priority (lower than main loop = 1)
    &httpTask,
    1            // core 1, leave core 0 for OTA
  );

  // mDNS
  MDNS.begin("w-temp");

  // OTA
  setupOta();

  // HTTP API
  server.on("/", HTTP_GET, []() {
    server.send(200, "application/json", buildJson());
  });

  server.begin();

  // Initial read to populate display immediately
  bootPrintRemoteSensors();

  // Final
  showStatus("READY", GREEN);

  delay(2000);

  drawStaticUI();
}

// ---------- LOOP ----------
unsigned long lastRead = 0;

void loop() {
  ArduinoOTA.handle();

  if (otaInProgress) {
    delay(10);
    return;
  }

  server.handleClient();

  if (millis() - lastRead >= 2000) {
    lastRead = millis();
    readLocalSensors();
    printAllData();
  }
}
