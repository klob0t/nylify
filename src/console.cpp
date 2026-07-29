#include "console.h"

#include <WiFi.h>

#include "card_store.h"
#include "settings.h"
#include "spotify_client.h"

namespace {

SpotifyClient* spotify = nullptr;
std::function<void()> runDiag;
String buffer;
String lastUid;
bool wifiChanged = false;

void printHelp() {
  Serial.println(F(
      "\n--- cassette console -------------------------------------------\n"
      "  HELP                  this list\n"
      "  STATUS                wifi / token / card count\n"
      "  DIAG                  check the RC522 and OLED wiring\n"
      "\n"
      "  MAP <uri>             pair the last-tapped card with a Spotify URI\n"
      "  MAP <uid> <uri>       pair a specific card\n"
      "  UNMAP <uid>           forget a card\n"
      "  LIST                  show all paired cassettes\n"
      "  EXPORT                dump mappings as MAP commands (back these up!)\n"
      "  WIPECARDS             delete every mapping\n"
      "\n"
      "  WIFI <ssid> <pass>    set and reconnect (pass may contain spaces)\n"
      "  TOKEN <refresh_token> store the Spotify refresh token\n"
      "  DEVICES               list your Spotify playback devices\n"
      "  DEVICE <id>           pin playback to one device ('-' to unpin)\n"
      "  RESET                 clear wifi/token/device, then reboot\n"
      "\n"
      "  URIs look like: spotify:album:4aawyAB9vmqN3uQ7FjRGTy\n"
      "  (Spotify app -> Share -> Copy link, then see README for converting)\n"
      "----------------------------------------------------------------"));
}

// Accepts a full share URL as well as a bare URI, so you can paste either.
//   https://open.spotify.com/album/4aawy...?si=xyz  ->  spotify:album:4aawy...
String normalizeUri(String in) {
  in.trim();
  if (in.startsWith("spotify:")) return in;

  const int host = in.indexOf("open.spotify.com/");
  if (host < 0) return in;  // Not something we recognise; pass through.

  String path = in.substring(host + 17);
  const int q = path.indexOf('?');
  if (q >= 0) path = path.substring(0, q);

  // Locale-prefixed links look like /intl-de/album/<id>.
  if (path.startsWith("intl-")) {
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

void cmdStatus() {
  Serial.println();
  Serial.printf("  wifi ssid : %s\n", settings::wifiSsid().c_str());
  Serial.printf("  wifi state: %s\n", WiFi.status() == WL_CONNECTED
                                          ? WiFi.localIP().toString().c_str()
                                          : "disconnected");
  Serial.printf("  client id : %s\n",
                settings::clientId().isEmpty() ? "MISSING (set in secrets.h)" : "set");
  // Length matters: a token truncated by a bad paste looks "set" but fails
  // with invalid_grant. Spotify's are typically ~130-200 chars.
  const String rt = settings::refreshToken();
  if (rt.isEmpty()) {
    Serial.println(F("  refresh   : MISSING (run TOKEN <refresh_token>)"));
  } else {
    Serial.printf("  refresh   : set, %u chars, ends '%s'\n", rt.length(),
                  rt.substring(rt.length() - 6).c_str());
  }
  Serial.printf("  access tok: %s\n", spotify && spotify->ready() ? "valid" : "none/expired");
  const String dev = settings::deviceId();
  Serial.printf("  device    : %s\n", dev.isEmpty() ? "(whatever is active)" : dev.c_str());
  Serial.printf("  cassettes : %u\n", (unsigned)cards::count());
  if (!lastUid.isEmpty()) Serial.printf("  last card : %s\n", lastUid.c_str());
  Serial.println();
}

void cmdList() {
  if (cards::count() == 0) {
    Serial.println(F("  no cassettes paired yet -- tap one, then: MAP <uri>"));
    return;
  }
  cards::forEach([](const String& uid, const String& uri) {
    Serial.printf("  %-16s %s\n", uid.c_str(), uri.c_str());
  });
}

void cmdExport() {
  Serial.println(F("# paste these back after a flash erase"));
  cards::forEach([](const String& uid, const String& uri) {
    Serial.printf("MAP %s %s\n", uid.c_str(), uri.c_str());
  });
}

void cmdMap(const String& args) {
  if (args.isEmpty()) {
    Serial.println(F("  usage: MAP <uri>   or   MAP <uid> <uri>"));
    return;
  }

  String uid, rest;
  const int sp = args.indexOf(' ');
  // Two-argument form only if the first token really looks like a UID.
  if (sp > 0 && !args.startsWith("spotify:") && args.indexOf("open.spotify.com") != 0) {
    uid = args.substring(0, sp);
    rest = args.substring(sp + 1);
    uid.toUpperCase();
  } else {
    uid = lastUid;
    rest = args;
  }

  if (uid.isEmpty()) {
    Serial.println(F("  no card seen yet -- tap one on the reader first"));
    return;
  }

  const String uri = normalizeUri(rest);
  if (!uri.startsWith("spotify:")) {
    Serial.printf("  '%s' is not a Spotify URI or link\n", rest.c_str());
    return;
  }

  if (cards::set(uid, uri)) {
    Serial.printf("  paired %s -> %s\n", uid.c_str(), uri.c_str());
  } else {
    Serial.println(F("  save failed"));
  }
}

void cmdUnmap(const String& args) {
  String uid = args;
  uid.trim();
  uid.toUpperCase();
  if (uid.isEmpty()) uid = lastUid;
  if (uid.isEmpty()) {
    Serial.println(F("  usage: UNMAP <uid>"));
    return;
  }
  Serial.println(cards::remove(uid) ? "  removed " + uid : "  " + uid + " was not paired");
}

void cmdDevices() {
  if (!spotify) return;
  String out;
  if (spotify->fetchDevices(out)) {
    Serial.println();
    Serial.print(out);
    Serial.println(F("  pin one with: DEVICE <id>"));
  } else {
    Serial.printf("  failed: %s\n", spotify->lastError().c_str());
  }
}

void handle(String line) {
  line.trim();
  if (line.isEmpty()) return;

  String cmd = line, args;
  const int sp = line.indexOf(' ');
  if (sp > 0) {
    cmd = line.substring(0, sp);
    args = line.substring(sp + 1);
    args.trim();
  }
  cmd.toUpperCase();

  if (cmd == "HELP" || cmd == "?") {
    printHelp();
  } else if (cmd == "STATUS") {
    cmdStatus();
  } else if (cmd == "DIAG") {
    if (runDiag) runDiag();
  } else if (cmd == "MAP") {
    cmdMap(args);
  } else if (cmd == "UNMAP") {
    cmdUnmap(args);
  } else if (cmd == "LIST") {
    cmdList();
  } else if (cmd == "EXPORT") {
    cmdExport();
  } else if (cmd == "WIPECARDS") {
    cards::clear();
    Serial.println(F("  all mappings deleted"));
  } else if (cmd == "WIFI") {
    // Password may contain spaces, so split on the first one only.
    const int s2 = args.indexOf(' ');
    if (s2 < 0) {
      Serial.println(F("  usage: WIFI <ssid> <password>"));
      return;
    }
    settings::setWifi(args.substring(0, s2), args.substring(s2 + 1));
    wifiChanged = true;
    Serial.println(F("  saved, reconnecting..."));
  } else if (cmd == "TOKEN") {
    if (args.isEmpty()) {
      Serial.println(F("  usage: TOKEN <refresh_token>"));
      return;
    }
    settings::setRefreshToken(args);
    Serial.println(F("  refresh token saved"));
  } else if (cmd == "DEVICES") {
    cmdDevices();
  } else if (cmd == "DEVICE") {
    if (args == "-" || args.isEmpty()) {
      settings::setDeviceId("");
      Serial.println(F("  unpinned -- will use whichever device is active"));
    } else {
      settings::setDeviceId(args);
      Serial.printf("  pinned to %s\n", args.c_str());
    }
  } else if (cmd == "RESET") {
    settings::clear();
    Serial.println(F("  settings cleared, rebooting"));
    delay(200);
    ESP.restart();
  } else {
    Serial.printf("  unknown command '%s' -- try HELP\n", cmd.c_str());
  }
}

}  // namespace

namespace console {

void begin(SpotifyClient* sp, std::function<void()> diagFn) {
  spotify = sp;
  runDiag = std::move(diagFn);
  buffer.reserve(160);
  printHelp();
}

void poll() {
  while (Serial.available()) {
    const char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (buffer.length() > 0) {
        handle(buffer);
        buffer = "";
      }
    } else if (buffer.length() < 320) {
      buffer += c;
    }
  }
}

void setLastUid(const String& uid) {
  lastUid = uid;
}

bool consumeWifiChanged() {
  const bool v = wifiChanged;
  wifiChanged = false;
  return v;
}

}  // namespace console
