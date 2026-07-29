#pragma once
#include <Arduino.h>

#include "spotify_client.h"

// Shared blackboard between the Arduino loop, the Spotify task and the web API.
//
//   Arduino loop  : writes card state (it owns the RFID reader), reads for the OLED
//   Spotify task  : writes the player cache
//   web handlers  : read everything
//
// All three run concurrently, so every accessor is mutex-guarded and getters
// return by value -- handing out a reference into shared state would let a
// reader observe a String mid-reassignment.
namespace deck {

void begin();

// --- what is physically on the reader --------------------------------------
void setCard(const String& uid, bool paired, const String& uri);
void clearCard();
String cardUid();  // "" when the deck is empty
bool cardPaired();
String cardUri();

// --- cached Spotify player state -------------------------------------------
// Refreshed by the Spotify task so an HTTP request never has to make a blocking
// call out to Spotify just to render the page.
void setPlayer(const SpotifyClient::NowPlaying& np);
SpotifyClient::NowPlaying player();
bool playerValid();
uint32_t playerAgeMs();  // how stale the cache is, for progress interpolation

// Asks the Spotify task to re-poll at its next opportunity -- call after
// anything that changes playback (play, skip, volume).
void requestRefresh();
bool consumeRefreshRequest();

// --- watcher tracking -------------------------------------------------------
// An open browser counts as "someone is watching", which keeps the player cache
// warm even with no cassette on the deck. Without this the page would freeze
// whenever playback was started from the phone app instead of a tag.
void noteWatcher();
bool watched();

}  // namespace deck
