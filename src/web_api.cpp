#include "web_api.h"

#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <WiFi.h>

#include "card_store.h"
#include "commands.h"
#include "deck_state.h"
#include "settings.h"
#include "spotify_client.h"

namespace {

WebServer server(80);
SpotifyClient* spotify = nullptr;
webapi::Hooks hooks;
bool mdnsUp = false;
bool serverUp = false;

// The page is served from a Vite dev server on another origin, so every
// response needs CORS. In dev the Vite proxy makes it same-origin anyway, but
// this also lets you point a phone straight at the device's IP.
void cors() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET,POST,DELETE,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

void sendJson(int code, const JsonDocument& doc) {
  String out;
  serializeJson(doc, out);
  cors();
  server.send(code, "application/json", out);
}

void sendError(int code, const String& message) {
  JsonDocument doc;
  doc["ok"] = false;
  doc["error"] = message;
  sendJson(code, doc);
}

void sendOk() {
  JsonDocument doc;
  doc["ok"] = true;
  sendJson(200, doc);
}

// Parses a JSON request body. Empty body is treated as an empty object so
// callers can use `doc["x"] | default` uniformly.
bool readBody(JsonDocument& doc) {
  const String body = server.hasArg("plain") ? server.arg("plain") : String();
  if (body.isEmpty()) {
    doc.to<JsonObject>();
    return true;
  }
  return !deserializeJson(doc, body);
}

// Accepts a full share URL as well as a bare URI, matching the serial console.
//   https://open.spotify.com/album/4aawy...?si=xyz -> spotify:album:4aawy...
String normalizeUri(String in) {
  in.trim();
  if (in.startsWith("spotify:")) return in;

  const int host = in.indexOf("open.spotify.com/");
  if (host < 0) return in;

  String path = in.substring(host + 17);
  const int q = path.indexOf('?');
  if (q >= 0) path = path.substring(0, q);

  if (path.startsWith("intl-")) {  // /intl-de/album/<id>
    const int slash = path.indexOf('/');
    if (slash >= 0) path = path.substring(slash + 1);
  }

  const int slash = path.indexOf('/');
  if (slash < 0) return in;
  const String type = path.substring(0, slash);
  const String id = path.substring(slash + 1);
  if (type.isEmpty() || id.isEmpty()) return in;

  return "spotify:" + type + ":" + id;
}

// --- handlers ---------------------------------------------------------------

void handleState() {
  deck::noteWatcher();

  JsonDocument doc;
  doc["ok"] = true;

  JsonObject wifi = doc["wifi"].to<JsonObject>();
  const bool up = WiFi.status() == WL_CONNECTED;
  wifi["connected"] = up;
  if (up) {
    wifi["ip"] = WiFi.localIP().toString();
    wifi["rssi"] = WiFi.RSSI();
  }

  JsonObject sp = doc["spotify"].to<JsonObject>();
  sp["configured"] = !settings::refreshToken().isEmpty();
  sp["tokenOk"] = spotify && spotify->ready();
  if (spotify) sp["error"] = spotify->lastError();

  JsonObject dk = doc["deck"].to<JsonObject>();
  const String uid = deck::cardUid();
  dk["present"] = !uid.isEmpty();
  dk["uid"] = uid;
  dk["paired"] = deck::cardPaired();
  dk["uri"] = deck::cardUri();

  JsonObject pl = doc["player"].to<JsonObject>();
  pl["valid"] = deck::playerValid();
  if (deck::playerValid()) {
    const SpotifyClient::NowPlaying np = deck::player();  // copy, not a reference
    pl["hasTrack"] = np.hasTrack;
    pl["isPlaying"] = np.isPlaying;
    pl["track"] = np.track;
    pl["artist"] = np.artist;
    pl["album"] = np.album;
    pl["art"] = np.artUrl;
    pl["device"] = np.deviceName;
    pl["progressMs"] = np.progressMs;
    pl["durationMs"] = np.durationMs;
    pl["volume"] = np.volume;
    // How old progressMs is. The browser adds this on so the progress bar
    // starts in the right place instead of snapping backwards each poll.
    pl["ageMs"] = deck::playerAgeMs();
  }

  doc["cards"] = cards::count();
  sendJson(200, doc);
}

void handleGetCards() {
  deck::noteWatcher();

  JsonDocument doc;
  JsonArray arr = doc["cards"].to<JsonArray>();
  cards::forEach([&](const String& uid, const String& uri) {
    JsonObject o = arr.add<JsonObject>();
    o["uid"] = uid;
    o["uri"] = uri;
  });
  doc["ok"] = true;
  sendJson(200, doc);
}

void handlePostCard() {
  JsonDocument body;
  if (!readBody(body)) return sendError(400, F("malformed JSON"));

  String uid = body["uid"] | "";
  uid.trim();
  uid.toUpperCase();
  if (uid.isEmpty()) uid = deck::cardUid();  // default to the card on the deck
  if (uid.isEmpty()) return sendError(400, F("no uid given and the deck is empty"));

  const String raw = body["uri"] | "";
  const String uri = normalizeUri(raw);
  if (!uri.startsWith("spotify:")) {
    return sendError(400, "not a Spotify URI or link: " + raw);
  }

  if (!cards::set(uid, uri)) return sendError(500, F("could not save"));

  Serial.printf("[web] paired %s -> %s\n", uid.c_str(), uri.c_str());
  if (hooks.onPaired) hooks.onPaired(uid, uri);

  JsonDocument doc;
  doc["ok"] = true;
  doc["uid"] = uid;
  doc["uri"] = uri;
  sendJson(200, doc);
}

void handleDeleteCard() {
  String uid = server.hasArg("uid") ? server.arg("uid") : String();
  uid.trim();
  uid.toUpperCase();
  if (uid.isEmpty()) return sendError(400, F("uid query parameter required"));

  if (!cards::remove(uid)) return sendError(404, uid + " was not paired");
  Serial.printf("[web] unpaired %s\n", uid.c_str());
  sendOk();
}

// Acknowledges without waiting for Spotify.
//
// A Spotify round trip is ~2s, nearly all of it TLS handshake. Performing it
// inside the handler meant every button held the connection open for two
// seconds and the UI felt broken. Commands are validated here, queued, and
// answered 202; the executor runs them and any failure surfaces on the next
// /api/state as spotify.error.
void sendQueued() {
  JsonDocument doc;
  doc["ok"] = true;
  doc["queued"] = true;
  doc["depth"] = commands::pending();
  sendJson(202, doc);
}

void handlePlayback() {
  JsonDocument body;
  if (!readBody(body)) return sendError(400, F("malformed JSON"));

  String action = body["action"] | "";
  action.toLowerCase();

  // "toggle" is what the single play/pause button sends; resolve it against
  // the cached state so the browser does not have to track it.
  if (action == "toggle") {
    action = (deck::playerValid() && deck::player().isPlaying) ? "pause" : "play";
  }

  commands::Command c;
  if (action == "play") {
    c.type = commands::Type::Resume;
  } else if (action == "pause") {
    c.type = commands::Type::Pause;
  } else if (action == "next") {
    c.type = commands::Type::Next;
  } else if (action == "previous") {
    c.type = commands::Type::Previous;
  } else {
    return sendError(400, "unknown action: " + action);
  }

  if (!commands::push(c)) return sendError(503, F("command queue full"));
  sendQueued();
}

void handleVolume() {
  JsonDocument body;
  if (!readBody(body)) return sendError(400, F("malformed JSON"));
  if (body["value"].isNull()) return sendError(400, F("value required (0-100)"));

  commands::Command c;
  c.type = commands::Type::Volume;
  c.value = constrain(body["value"].as<int>(), 0, 100);

  if (!commands::push(c)) return sendError(503, F("command queue full"));
  sendQueued();
}

void handleDevices() {
  if (!spotify) return sendError(503, F("spotify client not ready"));

  String list;
  if (!spotify->fetchDevices(list)) return sendError(502, spotify->lastError());

  JsonDocument doc;
  doc["ok"] = true;
  doc["devices"] = list;  // already human-readable
  doc["selected"] = settings::deviceId();
  sendJson(200, doc);
}

void handleNotFound() {
  // Browsers fire an OPTIONS preflight before any POST/DELETE carrying JSON.
  if (server.method() == HTTP_OPTIONS) {
    cors();
    server.send(204);
    return;
  }
  sendError(404, "no route for " + server.uri());
}

}  // namespace

namespace webapi {

void begin(SpotifyClient* sp, const Hooks& h) {
  spotify = sp;
  hooks = h;

  server.on("/api/state", HTTP_GET, handleState);
  server.on("/api/cards", HTTP_GET, handleGetCards);
  server.on("/api/cards", HTTP_POST, handlePostCard);
  server.on("/api/cards", HTTP_DELETE, handleDeleteCard);
  server.on("/api/playback", HTTP_POST, handlePlayback);
  server.on("/api/volume", HTTP_POST, handleVolume);
  server.on("/api/devices", HTTP_GET, handleDevices);
  server.onNotFound(handleNotFound);

  // Deliberately no server.begin() here. Opening the listening socket needs the
  // lwIP TCP/IP stack, which does not exist until WiFi.mode() brings the network
  // interface up -- calling it earlier asserts on an invalid mbox and panics.
  // The socket is opened in onWifiUp() instead.
}

void onWifiUp() {
  if (!serverUp) {
    server.begin();
    serverUp = true;
    Serial.println(F("[web] API listening on port 80"));
  }

  if (!mdnsUp) {
    if (MDNS.begin("cassette")) {
      MDNS.addService("http", "tcp", 80);
      mdnsUp = true;
      Serial.println(F("[web] http://cassette.local/api/state"));
    } else {
      Serial.println(F("[web] mDNS failed -- use the IP address instead"));
    }
  }
}

void poll() {
  if (!serverUp) return;  // No network yet; nothing is listening.
  server.handleClient();
}

}  // namespace webapi
