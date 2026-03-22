#include <Arduino.h>
#include <Arduino_GFX_Library.h>

// Waveshare Wiki: ESP32-C6-LCD-1.47 (non-touch)
// MOSI=6, SCLK=7, CS=14, DC=15, RST=21, BL=22
#define PIN_LCD_MOSI 6
#define PIN_LCD_SCK 7
#define PIN_LCD_CS 14
#define PIN_LCD_DC 15
#define PIN_LCD_RST 21
#define PIN_LCD_BL 22

Arduino_DataBus *bus = new Arduino_ESP32SPI(PIN_LCD_DC, PIN_LCD_CS, PIN_LCD_SCK, PIN_LCD_MOSI, GFX_NOT_DEFINED, FSPI);
Arduino_GFX *gfx = new Arduino_ST7789(
  bus, PIN_LCD_RST, 0, false,
  172, 320,
  34, 0,
  34, 0);

void drawLabel(const char *label, uint16_t fg, uint16_t bg) {
  gfx->fillScreen(bg);
  gfx->setRotation(1);
  gfx->setTextSize(3);
  gfx->setTextColor(fg, bg);
  gfx->setCursor(20, 50);
  gfx->print(label);
}

void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(PIN_LCD_BL, OUTPUT);
  digitalWrite(PIN_LCD_BL, HIGH);

  if (!gfx->begin()) {
    Serial.println("[BOOT] gfx->begin() failed");
    while (true) {
      delay(1000);
    }
  }
  Serial.println("[BOOT] ST7789 non-touch profile active");
}

void loop() {
  drawLabel("RED", RGB565_WHITE, RGB565_RED);
  delay(5000);
  drawLabel("GREEN", RGB565_BLACK, RGB565_GREEN);
  delay(5000);
  drawLabel("BLUE", RGB565_WHITE, RGB565_BLUE);
  delay(5000);
}
