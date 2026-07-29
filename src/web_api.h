#pragma once
#include <Arduino.h>

#include <functional>

class SpotifyClient;

// Plain-HTTP JSON API on port 80, consumed by the Vite app in web/.
//
// Read paths (/api/state) are served entirely from the deck_state cache, so a
// browser polling once a second never triggers a call out to Spotify. Only the
// control paths block, and only for as long as one Spotify request takes.
namespace webapi {

struct Hooks {
  // Fired when a card is paired from the browser. main decides what to do --
  // typically start playing if that card is the one sitting on the reader.
  std::function<void(const String& uid, const String& uri)> onPaired;
};

// Registers routes only. Safe to call before the network exists.
void begin(SpotifyClient* spotify, const Hooks& hooks);

// Opens the listening socket and advertises cassette.local. Must not be called
// until WiFi is actually connected: the TCP/IP stack is not initialised before
// that, and binding a socket early panics the device. Idempotent.
void onWifiUp();

// Non-blocking; call every loop.
void poll();

}  // namespace webapi
