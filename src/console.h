#pragma once
#include <Arduino.h>

#include <functional>

class SpotifyClient;

// Line-based serial control panel: pair cassettes, set WiFi/token/device.
// 115200 baud, newline-terminated. Type HELP for the command list.
namespace console {

// diagFn backs the DIAG command; main supplies it so the console does not
// need to reach into the MFRC522 instance itself.
void begin(SpotifyClient* spotify, std::function<void()> diagFn);

// Non-blocking; call every loop.
void poll();

// Lets MAP with a single argument target the card you just tapped.
void setLastUid(const String& uid);

// True once the WIFI command changed credentials and main should reconnect.
bool consumeWifiChanged();

}  // namespace console
