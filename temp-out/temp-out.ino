#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>

#include <ESPmDNS.h>
#include <ArduinoOTA.h>

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_AHTX0.h>

#include "secrets.h"

#define SDA_PIN 8
#define SCL_PIN 9

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 32
#define OLED_RESET -1

// AHT20 self-heating correction
#define TEMP_OFFSET -5.7f

const unsigned long postInterval = 30UL * 60UL * 1000UL;
unsigned long lastPost = 0;

Adafruit_SSD1306 display(
  SCREEN_WIDTH,
  SCREEN_HEIGHT,
  &Wire,
  OLED_RESET);

Adafruit_AHTX0 aht;
WebServer server(80);

float temperature = 0;
float humidity = 0;
bool displayConnected = false;

void handleTemperature() {
  String json =
    "{"
    "\"temperature\":"
    + String(temperature, 1) + ","
                               "\"humidity\":"
    + String(humidity, 1) + "}";

  server.send(200, "application/json", json);
}

void handleWifi() {
  int tries = 0;

  while (WiFi.status() != WL_CONNECTED && tries < 30) {
    delay(500);
    Serial.print(".");

    Serial.print(" status=");
    Serial.println(WiFi.status());

    tries++;
  }
}

bool isWiFiConnected() {
  return WiFi.status() == WL_CONNECTED;
}

void postTemperature(float temp) {
  if (!isWiFiConnected()) return;

  HTTPClient http;
  String url = String(SHEETS_URL) + "?temp=" + String(temp, 1);
  http.begin(url);
  int httpCode = http.GET();
  Serial.printf("Temperature upload HTTP status: %d\n", httpCode);
  http.end();
}

void setup() {
  Serial.begin(115200);
  Serial.println("Alive!");
  Wire.begin(SDA_PIN, SCL_PIN);
  // OLED
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED failed");
    displayConnected = false;
  } else {
    displayConnected = true;

    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);  // IMPORTANT FIX
    display.setCursor(0, 0);
    display.println("Booting...");
    display.display();

    delay(100);
    Serial.println("Display OK");
  }

  // AHT20
  if (!aht.begin()) {
    Serial.println("AHT20 failed");
  }
  Serial.println("AHT20 OK");

  if (displayConnected) {
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("Connecting...");
    display.display();
    delay(100);
  }

  // WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.println("Connecting WiFi...");
  Serial.printf("SSID: %s\n", WIFI_SSID);

  handleWifi();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi connected!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
    if (displayConnected) {
      display.println(WiFi.localIP());
    }
    delay(500);
  } else {
    Serial.println("WiFi FAILED");
    if (displayConnected) {
      display.println("WiFi FAILED");
    }
    delay(500);
  }
  // Start mDNS AFTER WiFi
  if (MDNS.begin("temp-out")) {
    Serial.println("mDNS responder started");
  } else {
    Serial.println("mDNS failed");
  }

  server.on("/temp", handleTemperature);
  server.begin();

  Serial.println(WiFi.localIP());

  // OTA update
  ArduinoOTA.setHostname("temp-out");
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.begin();
}

void loop() {
  ArduinoOTA.handle();

  server.handleClient();

  sensors_event_t humidityEvent;
  sensors_event_t tempEvent;

  aht.getEvent(&humidityEvent, &tempEvent);

  temperature = tempEvent.temperature + TEMP_OFFSET;
  humidity = humidityEvent.relative_humidity;

  // OLED display (only if connected)
  if (displayConnected) {
    display.clearDisplay();

    // Temperature in large font
    display.setCursor(10, 2);
    display.setTextSize(4);
    display.print(temperature, 1);

    // WiFi status at top (small font)
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(110, 0);
    display.println(isWiFiConnected() ? "OK" : "NOK");

    display.display();
  }

  if (WiFi.status() != WL_CONNECTED) {
    handleWifi();
  }

  if (isWiFiConnected() && (lastPost == 0 || millis() - lastPost >= postInterval)) {
    postTemperature(temperature);
    lastPost = millis();
  }

  delay(250);
}