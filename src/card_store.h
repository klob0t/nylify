#pragma once
#include <Arduino.h>

#include <functional>

// Maps an RFID tag's factory UID to a Spotify URI.
//
// Nothing is ever written to the tag itself: we only read the UID that every
// card already has burned in. That means any tag works (MIFARE Classic, NTAG
// stickers, keyfobs) and a cassette can never be corrupted by a bad write.
//
// Backed by /cards.json in LittleFS, so pairing a new cassette needs no reflash.
namespace cards {

bool begin();

// Empty string if the UID is unpaired.
String lookup(const String& uid);

bool set(const String& uid, const String& uri);
bool remove(const String& uid);
void forEach(const std::function<void(const String& uid, const String& uri)>& fn);
size_t count();
bool clear();

}  // namespace cards
