#pragma once
#include <Arduino.h>

// SSD1306 front panel. Call tick() often -- it drives the marquee for titles
// too long to fit on 128px.
namespace ui {

enum class Screen {
  Boot,
  WifiConnecting,
  Idle,        // no cassette on the reader
  Playing,     // showing track + artist
  UnknownCard, // tag scanned but not paired yet
  Error,
};

bool begin();
void tick();

void showBoot(const String& line);
void showWifiConnecting(const String& ssid);
void showIdle();
void showPlaying(const String& track, const String& artist);
void showUnknownCard(const String& uid);
void showError(const String& msg);

// Small WiFi glyph in the corner, updated independently of the screen body.
void setWifiConnected(bool connected);

}  // namespace ui
