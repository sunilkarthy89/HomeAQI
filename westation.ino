/*
 * ╔══════════════════════════════════════════════════════════════╗
 * ║           ESP8266 Weather Station                            ║
 * ║  ST7735 Display + AHT20 + BMP280 + RGB LED + OpenWeatherMap ║
 * ╚══════════════════════════════════════════════════════════════╝
 *
 * PIN MAPPING:
 *   ST7735:  CS=D1(GPIO5), DC=D2(GPIO4), RST=D0(GPIO16)
 *            MOSI=D7(GPIO13), SCK=D5(GPIO14)  [hardware SPI]
 *   AHT20+BMP280: SDA=D3(GPIO0), SCL=D4(GPIO2)
 *   RGB LED (single WS2812B): DI=D6(GPIO12)
 *
 * REQUIRED LIBRARIES (Library Manager):
 *   - Adafruit ST7735 and ST7789 Library
 *   - Adafruit GFX Library
 *   - Adafruit AHTX0
 *   - Adafruit BMP280
 *   - Adafruit NeoPixel
 *   - WiFiManager (by tzapu)
 *   - ArduinoJson (v6)
 *   - NTPClient (by Fabrice Weinberg)
 */

// ═══════════════════════════════════════════════════════════════
//  INCLUDES
// ═══════════════════════════════════════════════════════════════
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_AHTX0.h>
#include <Adafruit_BMP280.h>
#include <Adafruit_NeoPixel.h>

// ═══════════════════════════════════════════════════════════════
//  PIN DEFINITIONS
// ═══════════════════════════════════════════════════════════════
#define TFT_CS   5   // D1
#define TFT_DC   4   // D2
#define TFT_RST  16  // D0

#define I2C_SDA  0   // D3
#define I2C_SCL  2   // D4

#define LED_PIN   14  // D6
#define LED_COUNT  24  // single RGB LED

// ═══════════════════════════════════════════════════════════════
//  USER CONFIG  ← edit these
// ═══════════════════════════════════════════════════════════════
const char* OWM_API_KEY  = "";
const char* OWM_CITY     = "Ennis,IE";
const char* OWM_UNITS   = "metric";      // "metric" or "imperial"

// UTC offset in seconds (GMT=0, BST/IST=3600, CEST=7200)
#define NTP_OFFSET 0

const unsigned long OWM_INTERVAL    = 10UL * 60UL * 1000UL; // 10 min
const unsigned long SENSOR_INTERVAL = 5000;                   // 5 sec

// ═══════════════════════════════════════════════════════════════
//  OBJECTS
// ═══════════════════════════════════════════════════════════════
Adafruit_ST7735   tft   = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);
Adafruit_AHTX0    aht;
Adafruit_BMP280   bmp;
Adafruit_NeoPixel pixel(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

WiFiUDP   ntpUDP;
NTPClient ntpClient(ntpUDP, "pool.ntp.org", NTP_OFFSET, 60000);

// ═══════════════════════════════════════════════════════════════
//  STATE
// ═══════════════════════════════════════════════════════════════
float  localTemp     = 0;
float  localHumidity = 0;
float  localPressure = 0;

float  owmTemp      = 0;
float  owmFeelsLike = 0;
int    owmHumidity  = 0;
float  owmWindSpeed = 0;
int    owmCondCode  = 800;
String owmDesc      = "N/A";
String owmCity      = "";

bool   sensorAHTok  = false;
bool   sensorBMPok  = false;
bool   wifiOK       = false;

String currentTime  = "--:--";

unsigned long lastOWM    = 0;
unsigned long lastSensor = 0;

// ═══════════════════════════════════════════════════════════════
//  COLOUR PALETTE (RGB565)
// ═══════════════════════════════════════════════════════════════
#define C_BG     0x0841
#define C_PANEL  0x10A3
#define C_ACCENT 0x04FF
#define C_WHITE  0xFFFF
#define C_YELLOW 0xFFE0
#define C_RED    0xF800
#define C_GREEN  0x07E0
#define C_GREY   0x8410
#define C_LGREY  0xC618

// ═══════════════════════════════════════════════════════════════
//  TEMPERATURE → PANEL BACKGROUND COLOUR
// ═══════════════════════════════════════════════════════════════
uint16_t tempToColor565(float t) {
  if (t <= -10) return tft.color565( 10,  10, 120);
  if (t <=   0) return tft.color565( 20,  60, 160);
  if (t <=  10) return tft.color565( 10, 100, 180);
  if (t <=  16) return tft.color565(  0, 120, 100);
  if (t <=  22) return tft.color565( 20, 130,  30);
  if (t <=  28) return tft.color565(160, 120,   0);
  if (t <=  35) return tft.color565(180,  60,   0);
  return               tft.color565(160,   0,   0);
}

// ═══════════════════════════════════════════════════════════════
//  RGB LED — sin() BREATHING  (brightness fixed at 255 in setup)
// ═══════════════════════════════════════════════════════════════
void updateLED() {
  static float angle = 0;
  static unsigned long lastLEDUpdate = 0;
  static unsigned long lastSwitch = 0;
  static bool showTemp = true;
  
  // 1. Slow down the update rate for a smooth fade (approx 50 frames per second)
  if (millis() - lastLEDUpdate < 20) return;
  lastLEDUpdate = millis();

  // 2. Toggle between showing Temp (Red/Green) and Hum (Blue) every 10 seconds
  if (millis() - lastSwitch > 10000) {
    showTemp = !showTemp;
    lastSwitch = millis();
  }

  // 3. Calculate breathing intensity using a Sine wave
  // We use (sin + 1.2) / 2.2 to prevent the LEDs from turning completely OFF
  // This avoids the "flicker" at the bottom of the breath.
  float breath = (sin(angle) + 1.2) / 2.2; 
  angle += 0.03; // Adjust this for speed (smaller = slower breath)

  // 4. Determine Color based on the current mode
  int r = 0, g = 0, b = 0;
  if (showTemp) {
    if (localTemp < 22) { r = 0; g = 180; b = 40; }  // Soft Forest Green
    else { r = 200; g = 60; b = 0; }                 // Soft Sunset Orange
  } else {
    r = 0; g = 60; b = 200;                          // Deep Ocean Blue
  }

  // 5. Apply intensity to the whole ring
  uint32_t finalColor = pixel.Color(
    (int)(r * breath), 
    (int)(g * breath), 
    (int)(b * breath)
  );

  for (int i = 0; i < LED_COUNT; i++) {
    pixel.setPixelColor(i, finalColor);
  }
  
  pixel.show();
}

void ledBootTest() {
  pixel.setPixelColor(0, pixel.Color(200,   0,   0)); pixel.show(); delay(400);
  pixel.setPixelColor(0, pixel.Color(  0, 200,   0)); pixel.show(); delay(400);
  pixel.setPixelColor(0, pixel.Color(  0,   0, 200)); pixel.show(); delay(400);
  pixel.setPixelColor(0, pixel.Color(  0,   0,   0)); pixel.show();
}

void ledStatus(uint8_t r, uint8_t g, uint8_t b) {
  pixel.setPixelColor(0, pixel.Color(r, g, b));
  pixel.show();
}

// ═══════════════════════════════════════════════════════════════
//  DISPLAY PRIMITIVES
// ═══════════════════════════════════════════════════════════════
void tftLabel(int x, int y, const char* txt, uint16_t color, uint8_t sz = 1) {
  tft.setTextColor(color);
  tft.setTextSize(sz);
  tft.setCursor(x, y);
  tft.print(txt);
}

void drawRoundPanel(int x, int y, int w, int h, uint16_t color) {
  tft.fillRoundRect(x, y, w, h, 4, color);
}

// Return x cursor to centre a string in [areaX .. areaX+areaW]
// size-3 font: 18px per char (6 * sz)
int centreX(const String& s, int areaX, int areaW, int sz = 3) {
  return areaX + (areaW - (int)s.length() * 6 * sz) / 2;
}

// ═══════════════════════════════════════════════════════════════
//  SMALL CORNER ICONS
// ═══════════════════════════════════════════════════════════════

// Thermometer — 14×22 px, drawn at (x,y)
void drawThermometerSmall(int x, int y, uint16_t bg) {
  uint16_t red = tft.color565(255, 100, 100);
  tft.fillRect(x, y, 14, 22, bg);
  tft.fillCircle(x + 7, y + 18, 5, red);
  tft.fillRoundRect(x + 4, y, 6, 15, 2, C_WHITE);
  tft.fillRoundRect(x + 5, y + 1, 4, 11, 1, bg);
  tft.fillRect(x + 5, y + 7, 4, 5, red);
  tft.drawFastHLine(x + 9, y + 4, 3, C_WHITE);
  tft.drawFastHLine(x + 9, y + 8, 3, C_WHITE);
}

// Water droplet — 12×18 px, drawn at (x,y)
void drawDropletSmall(int x, int y, uint16_t bg) {
  uint16_t blue = tft.color565(80, 160, 255);
  tft.fillRect(x, y, 12, 18, bg);
  tft.fillCircle(x + 6, y + 12, 5, blue);
  tft.fillTriangle(x + 2, y + 13, x + 10, y + 13, x + 6, y + 2, blue);
  tft.fillCircle(x + 4, y + 10, 1, C_WHITE);
}

// ═══════════════════════════════════════════════════════════════
//  WEATHER ICONS  (28×28 px)
// ═══════════════════════════════════════════════════════════════
void _drawCloud(int x, int y, uint16_t col) {
  tft.fillCircle(x + 7,  y + 14, 6, col);
  tft.fillCircle(x + 14, y + 10, 8, col);
  tft.fillCircle(x + 21, y + 14, 6, col);
  tft.fillRect  (x + 7,  y + 14, 15, 8, col);
}

void _drawSun(int x, int y, uint16_t col) {
  tft.fillCircle(x + 14, y + 14, 7, col);
  for (int a = 0; a < 8; a++) {
    float rad = a * PI / 4.0f;
    tft.drawLine(
      x + 14 + (int)(cos(rad) * 9),  y + 14 + (int)(sin(rad) * 9),
      x + 14 + (int)(cos(rad) * 13), y + 14 + (int)(sin(rad) * 13), col);
  }
}

void _drawRaindrops(int x, int y, uint16_t col, int count) {
  int sp = 26 / (count + 1);
  for (int i = 0; i < count; i++) {
    int rx = x + sp * (i + 1);
    tft.drawLine(rx, y + 21, rx - 2, y + 27, col);
  }
}

void _drawSnowflakes(int x, int y, uint16_t col) {
  for (int i = 0; i < 3; i++) {
    int sx = x + 5 + i * 9;
    tft.drawLine(sx, y + 21, sx, y + 27, col);
    tft.drawLine(sx - 2, y + 24, sx + 2, y + 24, col);
  }
}

void _drawLightning(int x, int y, uint16_t col) {
  tft.fillTriangle(x+15, y+13, x+10, y+20, x+16, y+19, col);
  tft.drawLine(x+10, y+20, x+15, y+20, col);
  tft.drawLine(x+15, y+20, x+10, y+27, col);
}

void drawWeatherIcon(int x, int y, int code, uint16_t bg) {
  tft.fillRect(x, y, 28, 28, bg);
  uint16_t CLOUD = 0xC618, RAIN = 0x045F, SNOW = 0xAEFF;
  uint16_t SUN   = 0xFFE0, BOLT = 0xFFE0, FOG  = 0x8C71;

  if      (code >= 200 && code < 300) { _drawCloud(x,y,CLOUD); _drawLightning(x,y,BOLT); }
  else if (code >= 300 && code < 400) { _drawCloud(x,y,CLOUD); _drawRaindrops(x,y,RAIN,2); }
  else if (code >= 500 && code < 600) { _drawCloud(x,y,CLOUD); _drawRaindrops(x,y,RAIN,(code>=502)?4:3); }
  else if (code >= 600 && code < 700) { _drawCloud(x,y,CLOUD); _drawSnowflakes(x,y,SNOW); }
  else if (code >= 700 && code < 800) {
    for (int i = 0; i < 4; i++) {
      tft.drawFastHLine(x+2,  y+8+i*6, 12, FOG);
      tft.drawFastHLine(x+16, y+8+i*6,  8, FOG);
    }
  }
  else if (code == 800) { _drawSun(x,y,SUN); }
  else if (code == 801) { _drawSun(x+2,y+2,SUN); _drawCloud(x-2,y+6,CLOUD); }
  else                  { _drawCloud(x-2,y+2,tft.color565(100,100,100)); _drawCloud(x+2,y+8,CLOUD); }
}

// ═══════════════════════════════════════════════════════════════
//  STATUS BAR  (time "HH:MM" + wifi dot)
// ═══════════════════════════════════════════════════════════════
void drawStatusBar(uint16_t bg) {
  tft.fillRect(55, 0, 73, 10, bg);
  tft.setTextColor(C_WHITE); tft.setTextSize(1);
  tft.setCursor(80, 2);
  tft.print(currentTime);
  tft.fillCircle(123, 5, 4, wifiOK ? C_GREEN : C_RED);
}

// ═══════════════════════════════════════════════════════════════
//  VALUE PANEL  — centred big number + small corner icon
//  areaX, areaW : quadrant bounds  (0,64) or (64,64)
//  valueY       : top of the number text
//  isTemp       : true=thermometer icon, false=droplet icon
// ═══════════════════════════════════════════════════════════════
void drawValuePanel(int areaX, int areaW, int valueY,
                    const String& valStr, const char* unit,
                    uint16_t valCol, bool isTemp, uint16_t bg) {
  // Big centred number
  tft.setTextColor(valCol); tft.setTextSize(3);
  tft.setCursor(centreX(valStr, areaX + 2, areaW - 4, 3), valueY + 3);
  tft.print(valStr);
  tft.setTextSize(1); tft.print(unit);

  // Corner icon — bottom-right of quadrant
  // Size-3 number = 24px tall; icon top at valueY+26
  if (isTemp) drawThermometerSmall(areaX + areaW - 17, valueY + 15,     bg);
  else        drawDropletSmall    (areaX + areaW - 14, valueY + 17,     bg);
}

// ═══════════════════════════════════════════════════════════════
//  SPLASH + WIFI SETUP SCREENS
// ═══════════════════════════════════════════════════════════════
void drawSplash() {
  tft.fillScreen(C_BG);
  tft.drawRoundRect(4, 4, 120, 152, 6, C_ACCENT);
  tftLabel(20,  20, "WEATHER",          C_ACCENT, 2);
  tftLabel(18,  40, "STATION",          C_WHITE,  2);
  tft.drawFastHLine(10, 58, 108, C_ACCENT);
  tftLabel(10,  68, "ESP8266 + ST7735", C_LGREY,  1);
  tftLabel(10,  80, "AHT20 + BMP280",  C_LGREY,  1);
  tftLabel(10,  92, "RGB LED",          C_LGREY,  1);
  tftLabel(10, 108, "OpenWeatherMap",   C_GREY,   1);
}

void drawWifiSetup() {
  tft.fillScreen(C_BG);
  drawRoundPanel(2, 2, 124, 24, C_PANEL);
  tftLabel(8,   8, "WiFi Setup",     C_ACCENT, 1);
  tftLabel(4,  34, "Connect to:",    C_WHITE,  1);
  tftLabel(4,  46, "WeatherStation", C_YELLOW, 1);
  tftLabel(4,  58, "hotspot, then",  C_WHITE,  1);
  tftLabel(4,  70, "open browser:",  C_WHITE,  1);
  tftLabel(4,  82, "192.168.4.1",    C_ACCENT, 1);
  tftLabel(4, 100, "Enter WiFi",     C_LGREY,  1);
  tftLabel(4, 112, "credentials",    C_LGREY,  1);
}

// ═══════════════════════════════════════════════════════════════
//  INDOOR HALF DRAW  (y 0..74)
// ═══════════════════════════════════════════════════════════════
void drawIndoor() {
  static float lastT = -99;
  static float lastH = -99;

  // Only refresh if values change by a visible amount
  if (abs(localTemp - lastT) > 0.2 || abs(localHumidity - lastH) > 0.5) {
    lastT = localTemp;
    lastH = localHumidity;

    uint16_t bg = tempToColor565(localTemp);
    tft.fillRect(0, 0, 128, 75, bg); 

    tftLabel(4, 2, "INDOOR", C_WHITE, 1);
    drawStatusBar(bg);
    tft.drawFastHLine(0, 11, 128, C_WHITE);
    tft.drawFastVLine(64, 12, 50, C_WHITE);

    drawValuePanel(0, 64, 20, String((int)round(localTemp)), "C", C_WHITE, true, bg);
    drawValuePanel(64, 64, 20, String((int)round(localHumidity)), "%", C_WHITE, false, bg);

    tft.fillRect(0, 62, 128, 13, 0x0000); 
    tftLabel(4, 65, "P:", C_ACCENT, 1);
    tft.setCursor(16, 65);
    tft.print(sensorBMPok ? String((int)round(localPressure)) : "---");
    tft.print(" hPa");
  }
}

void drawOutdoor() {
  static float lastOT = -99;
  
  if (abs(owmTemp - lastOT) > 0.2) {
    lastOT = owmTemp;

    uint16_t bg = tempToColor565(owmTemp);
    tft.fillRect(0, 75, 128, 85, bg); 

    tftLabel(4, 77, "OUTDOOR", C_WHITE, 1);
    drawWeatherIcon(96, 74, owmCondCode, bg);
    tft.drawFastHLine(0, 86, 96, C_WHITE);
    tft.drawFastVLine(64, 87, 50, C_WHITE);

    drawValuePanel(0, 64, 95, String((int)round(owmTemp)), "C", C_YELLOW, true, bg);
    drawValuePanel(64, 64, 95, String(owmHumidity), "%", C_WHITE, false, bg);

    tftLabel(4, 127, "FEELS ", C_WHITE, 1);
    tft.setTextColor(tft.color565(220, 220, 150));
    tft.print((int)round(owmFeelsLike)); tft.print("C");

    tft.fillRect(0, 150, 128, 10, C_PANEL);
    String desc = owmCity + ": " + owmDesc;
    tftLabel(4, 152, desc.substring(0, 21).c_str(), C_LGREY, 1);
  }
}

void drawMainScreen() {
  drawIndoor();
  drawOutdoor();
}

// ═══════════════════════════════════════════════════════════════
//  OPENWEATHERMAP FETCH
// ═══════════════════════════════════════════════════════════════
void fetchOWM() {
  if (strcmp(OWM_API_KEY, "YOUR_API_KEY_HERE") == 0) {
    Serial.println("[OWM] No API key set — skipping");
    return;
  }
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  String url = String("https://api.openweathermap.org/data/2.5/weather?q=")
             + OWM_CITY + "&units=" + OWM_UNITS + "&appid=" + OWM_API_KEY;

  Serial.println("[OWM] " + url);
  if (!https.begin(client, url)) { Serial.println("[OWM] begin() failed"); return; }

  int httpCode = https.GET();
  Serial.printf("[OWM] HTTP %d\n", httpCode);

  if (httpCode == HTTP_CODE_OK) {
    String payload = https.getString();
    StaticJsonDocument<1536> doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (!err) {
      owmTemp      = doc["main"]["temp"].as<float>();
      owmFeelsLike = doc["main"]["feels_like"].as<float>();
      owmHumidity  = doc["main"]["humidity"].as<int>();
      owmWindSpeed = doc["wind"]["speed"].as<float>();
      owmCondCode  = doc["weather"][0]["id"].as<int>();
      owmDesc      = doc["weather"][0]["description"].as<String>();
      owmCity      = doc["name"].as<String>();
      if (owmDesc.length()) owmDesc[0] = toupper(owmDesc[0]);
      Serial.printf("[OWM] %.1f°C  hum=%d  wind=%.1f  code=%d\n",
                    owmTemp, owmHumidity, owmWindSpeed, owmCondCode);
      drawOutdoor();
    } else {
      Serial.println("[OWM] JSON error: " + String(err.c_str()));
    }
  }
  https.end();
}

// ═══════════════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  Serial.println("\n[BOOT] Weather Station");

  // LED — brightness set ONCE here, never changed again
  pinMode(LED_PIN, OUTPUT);
  pixel.begin();
  pixel.setBrightness(255);
  pixel.clear(); pixel.show();
  ledBootTest();

  // Display
  SPI.begin();
  tft.initR(INITR_BLACKTAB);  // 1.8" 128×160
  tft.setRotation(0);          // portrait
  tft.fillScreen(C_BG);
  drawSplash();
  ledStatus(0, 100, 255);      // blue = booting
  delay(1200);

  // Sensors
  Wire.begin(I2C_SDA, I2C_SCL);

  sensorAHTok = aht.begin(&Wire);
  Serial.println(sensorAHTok ? "[AHT20] OK" : "[AHT20] NOT FOUND");

  sensorBMPok = bmp.begin(0x76) || bmp.begin(0x77);
  if (sensorBMPok) {
    bmp.setSampling(Adafruit_BMP280::MODE_NORMAL,
                    Adafruit_BMP280::SAMPLING_X2,
                    Adafruit_BMP280::SAMPLING_X16,
                    Adafruit_BMP280::FILTER_X16,
                    Adafruit_BMP280::STANDBY_MS_500);
    Serial.println("[BMP280] OK");
  } else {
    Serial.println("[BMP280] NOT FOUND");
  }

  // WiFi via WiFiManager
  drawWifiSetup();
  ledStatus(255, 100, 0);  // orange = waiting

  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  WiFiManagerParameter p_key ("owm_key",  "OWM API Key",              OWM_API_KEY, 40);
  WiFiManagerParameter p_city("owm_city", "City,CC (e.g. Dublin,IE)", OWM_CITY,    32);
  wm.addParameter(&p_key);
  wm.addParameter(&p_city);

  bool connected = wm.autoConnect("WeatherStation", "weather123");

  if (connected) {
    wifiOK = true;
    Serial.println("[WiFi] Connected: " + WiFi.localIP().toString());
    ntpClient.begin();
    ntpClient.update();
    currentTime = ntpClient.getFormattedTime().substring(0, 5); // "HH:MM"
    ledStatus(0, 220, 80);  // green
    delay(800);
  } else {
    Serial.println("[WiFi] Offline / timed out");
    ledStatus(220, 0, 0);   // red
    delay(1000);
  }

  drawMainScreen();

  lastSensor = millis() - SENSOR_INTERVAL; // trigger immediately
  lastOWM    = millis() - OWM_INTERVAL;
}

// ═══════════════════════════════════════════════════════════════
//  LOOP
// ═══════════════════════════════════════════════════════════════
void loop() {
  unsigned long now = millis();

  // ── NTP time — update every 60s, redraw status bar only
  static unsigned long lastTimeUpdate = 0;
  if (wifiOK && (lastTimeUpdate == 0 || now - lastTimeUpdate >= 60000)) {
    lastTimeUpdate = now;
    ntpClient.update();
    currentTime = ntpClient.getFormattedTime().substring(0, 5);
    drawStatusBar(tempToColor565(localTemp));
  }

  // ── Local sensors — every 5s
  if (now - lastSensor >= SENSOR_INTERVAL) {
    lastSensor = now;
    if (sensorAHTok) {
      sensors_event_t h, t;
      aht.getEvent(&h, &t);
      localTemp     = t.temperature;
      localHumidity = h.relative_humidity;
    }
    if (sensorBMPok) localPressure = bmp.readPressure() / 100.0f;
    Serial.printf("[LOCAL] %.1f°C  %.1f%%  %.1fhPa\n",
                  localTemp, localHumidity, localPressure);
    drawIndoor();
  }

  // ── OWM — every 10 min
  if (wifiOK && (now - lastOWM >= OWM_INTERVAL)) {
    lastOWM = now;
    fetchOWM();
  }

  // ── LED breathing — every loop iteration, non-blocking
  updateLED();

  yield();
}
