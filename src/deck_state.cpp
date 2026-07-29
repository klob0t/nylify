#include "deck_state.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace {

String g_uid;
bool g_paired = false;
String g_uri;

SpotifyClient::NowPlaying g_np;
bool g_npValid = false;
uint32_t g_npAt = 0;

uint32_t g_lastWatch = 0;
bool g_refreshWanted = false;

SemaphoreHandle_t g_mtx = nullptr;

// How long after the last browser request we keep polling Spotify.
constexpr uint32_t kWatchWindowMs = 15000;

struct Guard {
  explicit Guard() {
    if (g_mtx) xSemaphoreTake(g_mtx, portMAX_DELAY);
  }
  ~Guard() {
    if (g_mtx) xSemaphoreGive(g_mtx);
  }
};

}  // namespace

namespace deck {

void begin() {
  if (!g_mtx) g_mtx = xSemaphoreCreateMutex();
}

void setCard(const String& uid, bool paired, const String& uri) {
  Guard g;
  g_uid = uid;
  g_paired = paired;
  g_uri = uri;
}

void clearCard() {
  Guard g;
  g_uid = "";
  g_paired = false;
  g_uri = "";
}

String cardUid() {
  Guard g;
  return g_uid;
}

bool cardPaired() {
  Guard g;
  return g_paired;
}

String cardUri() {
  Guard g;
  return g_uri;
}

void setPlayer(const SpotifyClient::NowPlaying& np) {
  Guard g;
  g_np = np;
  g_npValid = true;
  g_npAt = millis();
}

SpotifyClient::NowPlaying player() {
  Guard g;
  return g_np;  // by value: the caller must not hold a reference into shared state
}

bool playerValid() {
  Guard g;
  return g_npValid;
}

uint32_t playerAgeMs() {
  Guard g;
  return g_npValid ? millis() - g_npAt : 0;
}

void requestRefresh() {
  Guard g;
  g_refreshWanted = true;
}

bool consumeRefreshRequest() {
  Guard g;
  const bool v = g_refreshWanted;
  g_refreshWanted = false;
  return v;
}

void noteWatcher() {
  Guard g;
  g_lastWatch = millis();
}

bool watched() {
  Guard g;
  return g_lastWatch != 0 && (millis() - g_lastWatch) < kWatchWindowMs;
}

}  // namespace deck
