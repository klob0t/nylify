#include "ui.h"

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Wire.h>

#include "config.h"

namespace {

Adafruit_SSD1306 oled(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
bool ready = false;

ui::Screen screen = ui::Screen::Boot;
String lineA, lineB, lineC;
bool wifiUp = false;

uint32_t lastTick = 0;

// Marquee state for lineA (the track title). Size-2 text is 12px per glyph,
// so ~10 characters fit; anything longer scrolls.
constexpr int kCharW2 = 12;
constexpr int kFitChars2 = OLED_WIDTH / kCharW2;
int scrollOffset = 0;
uint32_t lastScroll = 0;
bool scrollPaused = true;
uint32_t scrollPauseUntil = 0;

void resetScroll() {
  scrollOffset = 0;
  scrollPaused = true;
  scrollPauseUntil = millis() + 1500;  // Hold at the start so it can be read.
}

void drawWifiGlyph() {
  // Three ascending bars, bottom-right. Hollow when disconnected.
  const int x = OLED_WIDTH - 10, y = 0;
  for (int i = 0; i < 3; i++) {
    const int h = 2 + i * 2;
    if (wifiUp) {
      oled.fillRect(x + i * 3, y + 6 - h, 2, h, SSD1306_WHITE);
    } else {
      oled.drawRect(x + i * 3, y + 6 - h, 2, h, SSD1306_WHITE);
    }
  }
}

// Word-wraps into up to `maxLines` rows of `perLine` chars at text size 1.
void drawWrapped(const String& text, int x, int y, int perLine, int maxLines) {
  int line = 0, i = 0;
  const int len = text.length();
  while (i < len && line < maxLines) {
    int take = min(perLine, len - i);
    if (i + take < len) {
      // Break on the last space so words stay intact.
      int sp = take;
      while (sp > 0 && text[i + sp] != ' ') sp--;
      if (sp > perLine / 3) take = sp;
    }
    oled.setCursor(x, y + line * 9);
    oled.print(text.substring(i, i + take));
    i += take;
    while (i < len && text[i] == ' ') i++;
    line++;
  }
}

void render() {
  if (!ready) return;
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);

  switch (screen) {
    case ui::Screen::Boot:
      oled.setTextSize(2);
      oled.setCursor(0, 8);
      oled.print(F("CASSETTE"));
      oled.setTextSize(1);
      oled.setCursor(0, 32);
      oled.print(lineA);
      break;

    case ui::Screen::WifiConnecting:
      oled.setTextSize(1);
      oled.setCursor(0, 8);
      oled.print(F("Connecting to WiFi"));
      oled.setCursor(0, 24);
      oled.print(lineA);
      // Simple activity dots.
      oled.setCursor(0, 44);
      for (int i = 0; i < ((millis() / 400) % 4); i++) oled.print('.');
      break;

    case ui::Screen::Idle:
      oled.setTextSize(1);
      oled.setCursor(0, 20);
      oled.print(F("  Drop a cassette"));
      oled.setCursor(0, 32);
      oled.print(F("   on the deck"));
      // Deck outline for a bit of character.
      oled.drawRect(34, 48, 60, 12, SSD1306_WHITE);
      oled.fillCircle(50, 54, 3, SSD1306_WHITE);
      oled.fillCircle(78, 54, 3, SSD1306_WHITE);
      break;

    case ui::Screen::Playing: {
      // Play triangle + title (marquee) on top, artist wrapped underneath.
      oled.fillTriangle(0, 2, 0, 12, 8, 7, SSD1306_WHITE);

      oled.setTextSize(2);
      const int over = (int)lineA.length() - kFitChars2;
      if (over <= 0) {
        oled.setCursor(0, 20);
        oled.print(lineA);
      } else {
        oled.setCursor(-scrollOffset, 20);
        oled.print(lineA);
        // Mask the glyph bleeding past the right edge.
        oled.fillRect(OLED_WIDTH - 2, 18, 2, 18, SSD1306_BLACK);
      }

      oled.setTextSize(1);
      drawWrapped(lineB, 0, 44, 21, 2);
      break;
    }

    case ui::Screen::UnknownCard:
      oled.setTextSize(1);
      oled.setCursor(0, 2);
      oled.print(F("Unpaired cassette"));
      oled.drawFastHLine(0, 12, OLED_WIDTH, SSD1306_WHITE);
      oled.setTextSize(2);
      oled.setCursor(0, 20);
      oled.print(lineA);
      oled.setTextSize(1);
      oled.setCursor(0, 44);
      oled.print(F("serial> MAP <uri>"));
      break;

    case ui::Screen::Error:
      oled.setTextSize(1);
      oled.setCursor(0, 2);
      oled.print(F("!"));
      oled.setCursor(10, 2);
      oled.print(F("Problem"));
      oled.drawFastHLine(0, 12, OLED_WIDTH, SSD1306_WHITE);
      drawWrapped(lineA, 0, 20, 21, 4);
      break;
  }

  drawWifiGlyph();
  oled.display();
}

}  // namespace

namespace ui {

bool begin() {
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDR)) {
    Serial.println(F("[ui] SSD1306 not found -- check wiring / try addr 0x3D"));
    return false;
  }
  ready = true;
  oled.clearDisplay();
  oled.display();
  return true;
}

void tick() {
  const uint32_t now = millis();
  if (now - lastTick < UI_TICK_MS) return;
  lastTick = now;

  // Advance the title marquee: pause at each end, then step one pixel.
  if (screen == Screen::Playing) {
    const int over = (int)lineA.length() - kFitChars2;
    if (over > 0) {
      const int maxOffset = over * kCharW2;
      if (scrollPaused) {
        if (now >= scrollPauseUntil) scrollPaused = false;
      } else if (now - lastScroll > 40) {
        lastScroll = now;
        scrollOffset++;
        if (scrollOffset >= maxOffset) {
          scrollOffset = 0;
          scrollPaused = true;
          scrollPauseUntil = now + 1500;
        }
      }
    }
  }
  render();
}

void showBoot(const String& line) {
  screen = Screen::Boot;
  lineA = line;
  render();
}

void showWifiConnecting(const String& ssid) {
  screen = Screen::WifiConnecting;
  lineA = ssid;
  render();
}

void showIdle() {
  screen = Screen::Idle;
  render();
}

void showPlaying(const String& track, const String& artist) {
  // Avoid restarting the marquee when the same track is re-reported.
  if (screen != Screen::Playing || lineA != track) resetScroll();
  screen = Screen::Playing;
  lineA = track;
  lineB = artist;
  render();
}

void showUnknownCard(const String& uid) {
  screen = Screen::UnknownCard;
  lineA = uid;
  render();
}

void showError(const String& msg) {
  screen = Screen::Error;
  lineA = msg;
  render();
}

void setWifiConnected(bool connected) {
  if (wifiUp == connected) return;
  wifiUp = connected;
  render();
}

}  // namespace ui
