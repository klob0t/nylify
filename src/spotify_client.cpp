#include "spotify_client.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <base64.h>

#include "config.h"
#include "settings.h"

namespace {

constexpr const char* kAccountsUrl = "https://accounts.spotify.com/api/token";
constexpr const char* kApiBase = "https://api.spotify.com/v1";

// A stuck request blocks the whole loop -- the OLED, the RFID poll and the web
// server all wait on it. Long enough for a cold handshake, short enough that a
// hung call is not a ten-second freeze.
constexpr uint16_t kHttpTimeoutMs = 8000;

// Track and episode URIs are queued individually; everything else
// (album/playlist/artist) is a browsable "context".
bool isContextUri(const String& uri) {
  return !(uri.startsWith("spotify:track:") || uri.startsWith("spotify:episode:"));
}

// Scope guard for the client mutex.
struct Guard {
  SemaphoreHandle_t m;
  explicit Guard(SemaphoreHandle_t mm) : m(mm) {
    if (m) xSemaphoreTakeRecursive(m, portMAX_DELAY);
  }
  ~Guard() {
    if (m) xSemaphoreGiveRecursive(m);
  }
};

}  // namespace

void SpotifyClient::begin() {
  _mtx = xSemaphoreCreateRecursiveMutex();

  // Spotify rotates its certificate chain, and a pinned root that expires
  // would brick the device with no way to update it. The alternative is
  // shipping a CA bundle and a way to refresh it; for a device on your own
  // LAN talking to a fixed host, this is the pragmatic trade.
  _tls.setInsecure();
  _tls.setTimeout(10000);

  // The core defaults this to 120 seconds. A handshake that stalls would freeze
  // the OLED, the reader and the web server for two full minutes before giving
  // up -- far longer than any plausible success.
  _tls.setHandshakeTimeout(6);
}

int SpotifyClient::request(const char* method, const String& url, const String& body,
                           const char* contentType, String& response, bool withAuth) {
  // Clear here rather than in each caller: every public method funnels through
  // request(), so a success always wipes the previous failure. Without this a
  // single transient error stays pinned in the web UI's status line forever.
  _lastError = "";

  // One fresh connection per request. Do not "optimise" this into keep-alive.
  //
  // Keep-alive genuinely is faster -- it roughly halves a call by paying the
  // TLS handshake once, and a 75s soak looked perfect: 112 requests, zero
  // failures, zero reconnects. But left alone through an idle period and a
  // power cycle, the Spotify task wedged permanently inside a call on a socket
  // the server had silently dropped. It blocked past both setTimeout(8s) and
  // setHandshakeTimeout(6s), so nothing recovered it: the web server kept
  // answering in 70ms with a cache that was two minutes stale and climbing,
  // and every queued command sat unexecuted.
  //
  // A device that silently stops working is worse than a slower one. The cost
  // of this decision is ~0.7s of extra staleness; the fix for *perceived*
  // latency lives in web_api.cpp, where control requests are queued and
  // acknowledged immediately rather than waiting on Spotify.
  HTTPClient http;
  http.setTimeout(kHttpTimeoutMs);
  http.setReuse(false);

  if (!http.begin(_tls, url)) {
    _lastError = F("connection failed");
    return -1;
  }

  if (contentType) http.addHeader("Content-Type", contentType);

  // HTTPClient only emits Content-Length when the body is non-empty, but
  // Spotify answers 411 to any body-less PUT/POST. That covers pause, resume,
  // next, previous and volume -- everything except play(uri), which carries
  // JSON and so slipped through.
  if (body.isEmpty() && strcmp(method, "GET") != 0) {
    http.addHeader("Content-Length", "0");
  }

  if (withAuth) {
    http.addHeader("Authorization", "Bearer " + _accessToken);
  } else {
    // Token endpoint authenticates the *app*, with HTTP Basic.
    const String creds = settings::clientId() + ":" + settings::clientSecret();
    http.addHeader("Authorization", "Basic " + base64::encode(creds));
  }

  int code;
  if (strcmp(method, "GET") == 0) {
    code = http.GET();
  } else if (strcmp(method, "PUT") == 0) {
    code = http.PUT((uint8_t*)body.c_str(), body.length());
  } else {
    code = http.POST((uint8_t*)body.c_str(), body.length());
  }

  response = (code > 0) ? http.getString() : String();
  http.end();

  if (code <= 0) {
    _lastError = HTTPClient::errorToString(code);
  }
  return code;
}

bool SpotifyClient::ensureToken() {
  Guard g(_mtx);
  if (_accessToken.length() > 0 && !expired()) return true;

  const String rt = settings::refreshToken();
  if (rt.isEmpty()) {
    _lastError = F("no refresh token");
    return false;
  }
  if (settings::clientId().isEmpty()) {
    _lastError = F("no client id");
    return false;
  }

  String body = "grant_type=refresh_token&refresh_token=" + rt;
  String resp;
  const int code = request("POST", kAccountsUrl, body,
                           "application/x-www-form-urlencoded", resp,
                           /*withAuth=*/false);

  if (code != 200) {
    _lastError = "token refresh HTTP " + String(code);
    Serial.printf("[spotify] %s: %s\n", _lastError.c_str(), resp.c_str());
    return false;
  }

  JsonDocument doc;
  if (deserializeJson(doc, resp)) {
    _lastError = F("bad token JSON");
    return false;
  }

  _accessToken = doc["access_token"].as<String>();
  const uint32_t ttl = doc["expires_in"] | 3600;
  _expiresAtMs = millis() + (ttl - TOKEN_EARLY_REFRESH_S) * 1000UL;

  // Rarely, Spotify hands back a new refresh token. Persist it or we lose
  // access the next time the old one is rejected.
  if (!doc["refresh_token"].isNull()) {
    const String fresh = doc["refresh_token"].as<String>();
    if (fresh != rt) {
      settings::setRefreshToken(fresh);
      Serial.println(F("[spotify] stored rotated refresh token"));
    }
  }

  Serial.printf("[spotify] access token ok (%us)\n", ttl);
  return true;
}

bool SpotifyClient::play(const String& uri) {
  Guard g(_mtx);
  if (!ensureToken()) return false;

  const String deviceId = settings::deviceId();
  String url = String(kApiBase) + "/me/player/play";
  if (!deviceId.isEmpty()) url += "?device_id=" + deviceId;

  String body;
  if (isContextUri(uri)) {
    body = "{\"context_uri\":\"" + uri + "\"}";
  } else {
    body = "{\"uris\":[\"" + uri + "\"]}";
  }

  String resp;
  int code = request("PUT", url, body, "application/json", resp);

  // 404 here means "no active device". If we have one configured, wake it up
  // and retry once -- this is the normal path when the speaker went idle.
  if (code == 404 && !deviceId.isEmpty()) {
    Serial.println(F("[spotify] no active device, transferring"));
    if (transferToDevice(deviceId, /*startPlaying=*/false)) {
      delay(500);  // Spotify needs a moment to register the transfer.
      code = request("PUT", url, body, "application/json", resp);
    }
  }

  if (code == 200 || code == 202 || code == 204) return true;

  if (code == 404) {
    _lastError = F("no active device");
  } else if (code == 403) {
    _lastError = F("needs Premium");
  } else if (code == 401) {
    _accessToken = "";  // Force a refresh on the next attempt.
    _lastError = F("token rejected");
  } else {
    _lastError = "play HTTP " + String(code);
  }
  Serial.printf("[spotify] play failed: %s %s\n", _lastError.c_str(), resp.c_str());
  return false;
}

bool SpotifyClient::resume() {
  Guard g(_mtx);
  if (!ensureToken()) return false;

  const String deviceId = settings::deviceId();
  String url = String(kApiBase) + "/me/player/play";
  if (!deviceId.isEmpty()) url += "?device_id=" + deviceId;

  // No body at all: Spotify continues from the saved position.
  String resp;
  const int code = request("PUT", url, "", "application/json", resp);
  if (code == 200 || code == 202 || code == 204) return true;

  _lastError = "resume HTTP " + String(code);
  return false;
}

bool SpotifyClient::pause() {
  Guard g(_mtx);
  if (!ensureToken()) return false;

  const String deviceId = settings::deviceId();
  String url = String(kApiBase) + "/me/player/pause";
  if (!deviceId.isEmpty()) url += "?device_id=" + deviceId;

  String resp;
  const int code = request("PUT", url, "", "application/json", resp);

  // 403 on pause usually means it was already paused -- not worth surfacing.
  if (code == 200 || code == 202 || code == 204 || code == 403 || code == 404) return true;

  _lastError = "pause HTTP " + String(code);
  return false;
}

bool SpotifyClient::next() {
  Guard g(_mtx);
  if (!ensureToken()) return false;

  const String deviceId = settings::deviceId();
  String url = String(kApiBase) + "/me/player/next";
  if (!deviceId.isEmpty()) url += "?device_id=" + deviceId;

  String resp;
  const int code = request("POST", url, "", "application/json", resp);
  if (code == 200 || code == 202 || code == 204) return true;

  _lastError = "next HTTP " + String(code);
  return false;
}

bool SpotifyClient::previous() {
  Guard g(_mtx);
  if (!ensureToken()) return false;

  const String deviceId = settings::deviceId();
  String url = String(kApiBase) + "/me/player/previous";
  if (!deviceId.isEmpty()) url += "?device_id=" + deviceId;

  String resp;
  const int code = request("POST", url, "", "application/json", resp);
  if (code == 200 || code == 202 || code == 204) return true;

  _lastError = "previous HTTP " + String(code);
  return false;
}

bool SpotifyClient::setVolume(int percent) {
  Guard g(_mtx);
  if (!ensureToken()) return false;

  percent = constrain(percent, 0, 100);
  const String deviceId = settings::deviceId();
  String url = String(kApiBase) + "/me/player/volume?volume_percent=" + String(percent);
  if (!deviceId.isEmpty()) url += "&device_id=" + deviceId;

  String resp;
  const int code = request("PUT", url, "", "application/json", resp);
  if (code == 200 || code == 202 || code == 204) return true;

  // Plenty of targets (Chromecast, some speakers) simply have no remote volume.
  _lastError = (code == 403) ? String(F("device volume is fixed"))
                             : "volume HTTP " + String(code);
  return false;
}

bool SpotifyClient::fetchNowPlaying(NowPlaying& out) {
  Guard g(_mtx);
  if (!ensureToken()) return false;

  // /me/player rather than /me/player/currently-playing: it returns the active
  // device alongside the track, so volume and speaker name come free instead of
  // costing a second round trip.
  String resp;
  const int code = request("GET", String(kApiBase) + "/me/player", "", nullptr, resp);

  if (code == 204) {  // No active device at all.
    out = NowPlaying{};
    return true;
  }
  if (code != 200) {
    _lastError = "nowplaying HTTP " + String(code);
    return false;
  }

  // Only pull the fields we render; the full payload is far too big for RAM.
  JsonDocument filter;
  filter["is_playing"] = true;
  filter["progress_ms"] = true;
  filter["device"]["name"] = true;
  filter["device"]["volume_percent"] = true;
  filter["item"]["name"] = true;
  filter["item"]["duration_ms"] = true;
  filter["item"]["artists"][0]["name"] = true;
  filter["item"]["album"]["name"] = true;
  filter["item"]["album"]["images"] = true;

  JsonDocument doc;
  if (deserializeJson(doc, resp, DeserializationOption::Filter(filter))) {
    _lastError = F("bad nowplaying JSON");
    return false;
  }

  out = NowPlaying{};
  out.isPlaying = doc["is_playing"] | false;
  out.progressMs = doc["progress_ms"] | 0;

  if (!doc["device"].isNull()) {
    out.deviceName = doc["device"]["name"].as<String>();
    // Absent (rather than 0) on devices without remote volume control.
    out.volume = doc["device"]["volume_percent"].isNull()
                     ? -1
                     : doc["device"]["volume_percent"].as<int>();
  }

  out.hasTrack = !doc["item"].isNull();
  if (out.hasTrack) {
    out.track = doc["item"]["name"].as<String>();
    out.artist = doc["item"]["artists"][0]["name"].as<String>();
    out.album = doc["item"]["album"]["name"].as<String>();
    out.durationMs = doc["item"]["duration_ms"] | 0;

    // images[] runs widest first (640/300/64). Take the middle one: big enough
    // for the browser, and we only ever pass the URL along, never the bytes.
    JsonArray images = doc["item"]["album"]["images"].as<JsonArray>();
    if (images.size() > 1) {
      out.artUrl = images[1]["url"].as<String>();
    } else if (images.size() == 1) {
      out.artUrl = images[0]["url"].as<String>();
    }
  }
  return true;
}

bool SpotifyClient::fetchDevices(String& out) {
  Guard g(_mtx);
  if (!ensureToken()) return false;

  String resp;
  const int code = request("GET", String(kApiBase) + "/me/player/devices", "", nullptr, resp);
  if (code != 200) {
    _lastError = "devices HTTP " + String(code);
    return false;
  }

  JsonDocument doc;
  if (deserializeJson(doc, resp)) {
    _lastError = F("bad devices JSON");
    return false;
  }

  out = "";
  for (JsonObject d : doc["devices"].as<JsonArray>()) {
    out += String(d["is_active"] ? "* " : "  ");
    out += d["name"].as<String>();
    out += " [" + d["type"].as<String>() + "]\n    id: ";
    out += d["id"].as<String>();
    out += "\n";
  }
  if (out.isEmpty()) {
    out = F("(none -- open Spotify on a phone/PC/speaker first)\n");
  }
  return true;
}

bool SpotifyClient::transferToDevice(const String& deviceId, bool startPlaying) {
  Guard g(_mtx);
  if (!ensureToken()) return false;

  const String body = "{\"device_ids\":[\"" + deviceId + "\"],\"play\":" +
                      (startPlaying ? "true" : "false") + "}";
  String resp;
  const int code = request("PUT", String(kApiBase) + "/me/player", body,
                           "application/json", resp);
  return code == 200 || code == 202 || code == 204;
}
