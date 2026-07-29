#include "settings.h"

#include <Preferences.h>

#include "secrets.h"

namespace {

Preferences prefs;

// NVS keys are capped at 15 chars. These name the storage slots -- the values
// themselves come from include/secrets.h or the WIFI serial command.
constexpr const char* kNamespace = "cassette";
constexpr const char* kSsid = "ssid";
constexpr const char* kPass = "pass";
constexpr const char* kRefresh = "refresh";
constexpr const char* kDevice = "device";

// Preferences logs an ESP-IDF error for every missing key, so a fresh device
// boots with a wall of red NOT_FOUND lines that look like a failure but are
// just "nothing stored yet, use the compiled-in default". Ask first.
String stored(const char* key, const char* fallback) {
  return prefs.isKey(key) ? prefs.getString(key) : String(fallback);
}

}  // namespace

namespace settings {

void begin() {
  prefs.begin(kNamespace, false);
}

String wifiSsid() {
  return stored(kSsid, DEFAULT_WIFI_SSID);
}

String wifiPass() {
  return stored(kPass, DEFAULT_WIFI_PASS);
}

void setWifi(const String& ssid, const String& pass) {
  prefs.putString(kSsid, ssid);
  prefs.putString(kPass, pass);
}

// The app credentials are compile-time only. They identify the Spotify app
// rather than the user, so there is no reason to reconfigure them in the field.
String clientId() {
  return String(DEFAULT_SPOTIFY_CLIENT_ID);
}

String clientSecret() {
  return String(DEFAULT_SPOTIFY_CLIENT_SECRET);
}

String refreshToken() {
  String t = stored(kRefresh, DEFAULT_SPOTIFY_REFRESH_TOKEN);
  t.trim();  // A pasted token often arrives with a stray space or newline.

  // The serial command is "TOKEN <value>" and the macro takes bare <value>,
  // so the command word regularly ends up pasted into the macro too. Spotify
  // answers that with a bare invalid_grant, which explains nothing. Forgive it.
  String head = t.substring(0, 6);
  head.toUpperCase();
  if (head == "TOKEN ") {
    t = t.substring(6);
    t.trim();
  }
  return t;
}

void setRefreshToken(const String& token) {
  prefs.putString(kRefresh, token);
}

String deviceId() {
  return stored(kDevice, DEFAULT_SPOTIFY_DEVICE_ID);
}

void setDeviceId(const String& id) {
  prefs.putString(kDevice, id);
}

void clear() {
  prefs.clear();
}

}  // namespace settings
