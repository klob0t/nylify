#pragma once
#include <Arduino.h>

// Device configuration held in NVS, seeded from include/secrets.h on first boot.
// Anything set over the serial console persists across reflashes.
namespace settings {

void begin();

String wifiSsid();
String wifiPass();
void setWifi(const String& ssid, const String& pass);

String clientId();
String clientSecret();

String refreshToken();
void setRefreshToken(const String& token);

String deviceId();
void setDeviceId(const String& id);

// Wipes the WiFi/token/device keys. Card mappings are a separate store.
void clear();

}  // namespace settings
