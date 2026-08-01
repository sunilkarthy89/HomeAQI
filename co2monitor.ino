#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ScioSense_ENS160.h> 
#include <Adafruit_AHTX0.h>
#include <Adafruit_NeoPixel.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

#define LED_PIN      2
#define BUZZER_PIN   3
#define BUTTON_PIN   7 
Adafruit_NeoPixel pixel(1, LED_PIN, NEO_GRB + NEO_KHZ800);

ScioSense_ENS160 ens160(ENS160_I2CADDR_0); 
Adafruit_AHTX0 aht;

int currentPage = 0;
bool alarmActive = false;
// Color states: 0=Green, 1=Yellow-Green, 2=Orange, 3=Red
int airState = 0;
bool lastBtnState = HIGH;
unsigned long lastSensorUpdate = 0;
unsigned long lastBuzzerTick = 0;

void centerTextHuge(String text, int y, int size) {
  int16_t x1, y1; uint16_t w, h;
  display.setTextSize(size);
  display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  int x = (SCREEN_WIDTH - w) / 2;
  display.setCursor(x, y);
  display.print(text);
  display.setCursor(x + 1, y);
  display.print(text);
}

void updateHeartbeat() {
  float speed = (airState >= 2) ? 0.012 : 0.004; // Pulse faster if air is bad
  float intensity = (sin(millis() * speed) + 1.0) / 2.0;
  int br = 10 + (intensity * 230); // Brightness scaling

  int r = 0, g = 0, b = 0;

  // Assign base colors based on airState
  switch(airState) {
    case 0: r = 0;   g = br;  b = 0;   break; // Pure Green
    case 1: r = 100; g = br;  b = 0;   break; // Yellow-Green (Lime)
    case 2: r = br;  g = 100; b = 0;   break; // Orange
    case 3: r = br;  g = 0;   b = 0;   break; // Pure Red
  }

  pixel.setPixelColor(0, pixel.Color(r, g, b));
  pixel.show();

  // --- ALARM BUZZER LOGIC ---
  if (alarmActive) {
    if (intensity > 0.95 && (millis() - lastBuzzerTick > 500)) {
      tone(BUZZER_PIN, 1500, 150);
      lastBuzzerTick = millis();
    }
  }
}

void updateDisplay() {
  sensors_event_t h, t; aht.getEvent(&h, &t);
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  
  switch (currentPage) {
    case 0: centerTextHuge("CO2 PPM", 0, 1); centerTextHuge(String(ens160.geteCO2()), 22, 3); break;
    case 1: centerTextHuge("VOC PPB", 0, 1); centerTextHuge(String(ens160.getTVOC()), 22, 3); break;
    case 2: centerTextHuge("AIR QUALITY", 0, 1); centerTextHuge(String(ens160.getAQI()), 18, 4); centerTextHuge("/ 5", 53, 1); break;
    case 3: centerTextHuge(String(t.temperature, 1) + " C", 5, 2); centerTextHuge(String(h.relative_humidity, 0) + "% RH", 38, 2); break;
  }
  display.display();
}

void setup() {
  Wire.begin(8, 9);
  Wire.setClock(400000); 
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
  pixel.begin();
  pixel.setBrightness(255);
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  //display.setRotation(2); 
  ens160.begin();
  aht.begin();
  ens160.setMode(ENS160_OPMODE_STD);
  updateDisplay();
}

void loop() {
  updateHeartbeat();

  bool currentBtn = digitalRead(BUTTON_PIN);
  if (currentBtn == LOW && lastBtnState == HIGH) {
    tone(BUZZER_PIN, 2000, 40); // Button click beep
    currentPage = (currentPage + 1) % 4;
    updateDisplay();
    delay(200); 
  }
  lastBtnState = currentBtn;

 if (millis() - lastSensorUpdate > 4000) {
    lastSensorUpdate = millis();
    sensors_event_t h, t; aht.getEvent(&h, &t);
    ens160.set_envdata(t.temperature, h.relative_humidity);
    
    if (ens160.available()) {
      ens160.measure(false);
      int eco2 = ens160.geteCO2();
      int tvoc = ens160.getTVOC();
      int aqi = ens160.getAQI();

      // Determine the worst state based on all three factors
      if (aqi >= 4 || eco2 > 1500 || tvoc > 1000) {
        airState = 3; // Red / Unhealthy
        alarmActive = true;
      } else if (aqi == 3 || eco2 > 1000 || tvoc > 600) {
        airState = 2; // Orange / Fair
        alarmActive = false;
      } else if (aqi == 2 || eco2 > 800 || tvoc > 250) {
        airState = 1; // Yellowish-Green / Good
        alarmActive = false;
      } else {
        airState = 0; // Green / Excellent
        alarmActive = false;
      }
      updateDisplay(); 
    }
 }
}
