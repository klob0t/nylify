#pragma once
#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Minimal Spotify Web API client for playback control.
//
// Uses the Authorization Code flow: a long-lived refresh token (obtained once
// on a PC via tools/get_refresh_token.py) is exchanged on-device for short-lived
// access tokens. Playback control requires Spotify Premium.
class SpotifyClient {
 public:
  struct NowPlaying {
    String track;
    String artist;
    String album;
    String artUrl;      // Spotify CDN link; the browser loads it directly.
    String deviceName;  // which speaker/phone is actually producing sound
    uint32_t progressMs = 0;
    uint32_t durationMs = 0;
    int volume = -1;  // -1 when the device does not report/accept volume
    bool isPlaying = false;
    bool hasTrack = false;
  };

  void begin();

  // True once we hold a valid, unexpired access token.
  bool ready() const { return _accessToken.length() > 0 && !expired(); }

  // Refreshes the access token if missing or near expiry. Safe to call often.
  bool ensureToken();

  // Starts playback of an album/playlist/artist/track/episode URI.
  bool play(const String& uri);

  // Un-pauses whatever was already loaded, keeping the current track and
  // position -- unlike play(uri), which restarts the context from the top.
  bool resume();

  bool pause();
  bool next();
  bool previous();

  // percent 0-100. Fails harmlessly on devices with fixed volume.
  bool setVolume(int percent);

  bool fetchNowPlaying(NowPlaying& out);

  // Human-readable device list for the DEVICES console command.
  bool fetchDevices(String& out);

  // Moves playback to the configured device, optionally starting it.
  bool transferToDevice(const String& deviceId, bool startPlaying);

  const String& lastError() const { return _lastError; }

 private:
  bool expired() const { return millis() >= _expiresAtMs; }
  int request(const char* method, const String& url, const String& body,
              const char* contentType, String& response, bool withAuth = true);

  WiFiClientSecure _tls;
  String _accessToken;
  uint32_t _expiresAtMs = 0;
  String _lastError;

  // Every public method is serialised. The Spotify task owns nearly all calls,
  // but the serial console's DEVICES command still comes from the Arduino loop,
  // and two tasks inside one WiFiClientSecure would corrupt the TLS session.
  // Recursive because the public methods call ensureToken(), which is itself
  // public and locks.
  SemaphoreHandle_t _mtx = nullptr;
};
