// RFID Cassette Player
//
// Drop a tag on the reader -> the paired Spotify album/playlist starts.
// Lift it off -> playback pauses. Tap an unpaired tag -> the OLED shows its
// UID so you can pair it from the serial console with: MAP <spotify uri>
//
// Wiring and setup: see README.md

#include <Arduino.h>
#include <MFRC522.h>
#include <SPI.h>
#include <WiFi.h>

#include "card_store.h"
#include "commands.h"
#include "config.h"
#include "console.h"
#include "deck_state.h"
#include "diag.h"
#include "settings.h"
#include "spotify_client.h"
#include "ui.h"
#include "web_api.h"

namespace {

MFRC522 rfid(PIN_RC522_SS, PIN_RC522_RST);
SpotifyClient spotify;

// --- card presence state ---------------------------------------------------
String currentUid;         // UID of the cassette sitting on the deck ("" = none)
uint8_t missCount = 0;     // consecutive polls with no card, for debouncing
uint32_t lastPoll = 0;
uint32_t lastLiftMs = 0;   // when the last card was removed
String lastLiftedUid;

// --- now-playing polling (Spotify task only) -------------------------------
uint32_t lastNowPlaying = 0;

// --- wifi ------------------------------------------------------------------
uint32_t lastWifiAttempt = 0;
bool wifiWasUp = false;
bool warnedNoSsid = false;

String uidToHex(const MFRC522::Uid& uid) {
  String s;
  s.reserve(uid.size * 2);
  for (byte i = 0; i < uid.size; i++) {
    if (uid.uidByte[i] < 0x10) s += '0';
    s += String(uid.uidByte[i], HEX);
  }
  s.toUpperCase();
  return s;
}

// Reads the UID of a card newly placed on the reader. Empty if none.
String readNewCard() {
  if (!rfid.PICC_IsNewCardPresent()) return String();
  if (!rfid.PICC_ReadCardSerial()) return String();

  const String uid = uidToHex(rfid.uid);
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
  return uid;
}

// Asks "is *a* card still in the field?" without a full anticollision cycle.
//
// PICC_IsNewCardPresent() only answers for cards in IDLE state, and a card we
// already selected and halted is not. WUPA wakes halted cards too, which is
// what makes lift-to-pause detection work. The three register writes reset the
// modulation settings that a prior MIFARE session leaves behind.
bool cardStillPresent() {
  byte atqa[2];
  byte size = sizeof(atqa);

  rfid.PCD_WriteRegister(MFRC522::TxModeReg, 0x00);
  rfid.PCD_WriteRegister(MFRC522::RxModeReg, 0x00);
  rfid.PCD_WriteRegister(MFRC522::ModWidthReg, 0x26);

  const MFRC522::StatusCode s = rfid.PICC_WakeupA(atqa, &size);
  const bool present = (s == MFRC522::STATUS_OK || s == MFRC522::STATUS_COLLISION);
  if (present) {
    rfid.PICC_HaltA();
  }
  return present;
}

// Card events only ever *queue* Spotify work. Doing the call here would block
// the reader for a couple of seconds, long enough to miss the tag being lifted
// again -- and it would put a second task inside the TLS client.
void onCardPlaced(const String& uid) {
  currentUid = uid;
  console::setLastUid(uid);
  Serial.printf("[card] on  %s\n", uid.c_str());

  const String uri = cards::lookup(uid);
  deck::setCard(uid, !uri.isEmpty(), uri);

  if (uri.isEmpty()) {
    Serial.printf("[card] unpaired -- pair it with:  MAP %s <spotify uri>\n", uid.c_str());
    return;
  }

  Serial.printf("[card] playing %s\n", uri.c_str());
  commands::push(commands::playUri(uri));
}

void onCardLifted() {
  Serial.printf("[card] off %s\n", currentUid.c_str());
  lastLiftedUid = currentUid;
  lastLiftMs = millis();
  currentUid = "";
  deck::clearCard();

  commands::Command c;
  c.type = commands::Type::Pause;
  commands::push(c);
}

void pollRfid() {
  const uint32_t now = millis();
  if (now - lastPoll < RFID_POLL_INTERVAL_MS) return;
  lastPoll = now;

  if (currentUid.isEmpty()) {
    const String uid = readNewCard();
    if (uid.isEmpty()) return;

    // A cassette that wobbles on the reader can drop out and come straight
    // back. Treat that as a bounce: resume where we were rather than
    // restarting the album from track 1.
    if (uid == lastLiftedUid && now - lastLiftMs < RFID_RETAP_GRACE_MS) {
      currentUid = uid;
      missCount = 0;
      Serial.println(F("[card] bounce -- resuming"));

      if (!cards::lookup(uid).isEmpty()) {
        commands::Command c;
        c.type = commands::Type::Resume;
        commands::push(c);
      }
      return;
    }

    missCount = 0;
    onCardPlaced(uid);
    return;
  }

  // A card is on the deck -- watch for it leaving.
  if (cardStillPresent()) {
    missCount = 0;
  } else if (++missCount >= RFID_REMOVAL_DEBOUNCE) {
    missCount = 0;
    onCardLifted();
  }
}

// --- Spotify task ----------------------------------------------------------
// Everything that talks to Spotify lives here, on its own core. The Arduino
// loop must never block: it owns the RFID reader, the OLED and the web server,
// and a ~2s TLS handshake in the middle of that was the single biggest source
// of latency in the whole system.
//
// This task touches only the SpotifyClient (internally mutexed) and deck_state
// (mutexed). It never touches the display -- I2C from two cores would corrupt
// the bus, so rendering stays on the loop and reads from deck_state.

void runOneCommand() {
  commands::Command c;
  if (!commands::pop(c)) return;

  bool ok = true;
  switch (c.type) {
    case commands::Type::Resume:
      ok = spotify.resume();
      break;
    case commands::Type::Pause:
      ok = spotify.pause();
      break;
    case commands::Type::Next:
      ok = spotify.next();
      break;
    case commands::Type::Previous:
      ok = spotify.previous();
      break;
    case commands::Type::Volume:
      ok = spotify.setVolume(c.value);
      break;
    case commands::Type::PlayUri:
      ok = spotify.play(String(c.uri));
      break;
  }

  if (!ok) {
    // The HTTP request was acknowledged long ago, so this and /api/state's
    // spotify.error are the only places a failure can surface.
    Serial.printf("[cmd] failed: %s\n", spotify.lastError().c_str());
  }
  deck::requestRefresh();
}

void refreshNowPlaying() {
  // Something just changed playback (a button, a placed or lifted cassette).
  // Give Spotify a beat to settle, then re-read rather than waiting out the
  // full interval and leaving the UI stale.
  if (deck::consumeRefreshRequest()) {
    lastNowPlaying = millis() - NOWPLAYING_POLL_MS + 400;
  }

  // Poll while a cassette is driving playback, and also while a browser is
  // watching -- otherwise the web UI would freeze whenever playback was started
  // from the phone app rather than a tag.
  const bool cassetteDriving = !deck::cardUid().isEmpty() && deck::cardPaired();
  if (!cassetteDriving && !deck::watched()) return;

  if (millis() - lastNowPlaying < NOWPLAYING_POLL_MS) return;

  SpotifyClient::NowPlaying np;
  const bool ok = spotify.fetchNowPlaying(np);

  // Stamp AFTER the call, so the interval means "gap between polls" rather than
  // "period including the poll". A round trip can outlast the interval itself,
  // and stamping beforehand would leave no gap at all.
  lastNowPlaying = millis();

  if (ok) deck::setPlayer(np);
}

void spotifyTask(void*) {
  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
      runOneCommand();      // queued work first: it is user-initiated
      refreshNowPlaying();  // then keep the cache warm
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// --- display ---------------------------------------------------------------
// Renders whatever deck_state currently says. Runs on the Arduino loop only.
// Change-detected, because the ui::show* calls redraw the panel immediately and
// doing that every iteration would peg the I2C bus.
void syncDisplay() {
  static String shownTrack;
  static String shownUid;
  static int shownMode = -1;  // 0 idle, 1 unknown card, 2 playing, 3 no wifi

  if (WiFi.status() != WL_CONNECTED) {
    if (shownMode != 3) {
      shownMode = 3;
      ui::showError(F("No WiFi"));
    }
    return;
  }

  const String uid = deck::cardUid();

  if (uid.isEmpty()) {
    if (shownMode != 0) {
      shownMode = 0;
      shownTrack = "";
      ui::showIdle();
    }
    return;
  }

  if (!deck::cardPaired()) {
    if (shownMode != 1 || shownUid != uid) {
      shownMode = 1;
      shownUid = uid;
      ui::showUnknownCard(uid);
    }
    return;
  }

  // Paired cassette on the deck: narrate whatever Spotify says is playing.
  // Until the first poll lands there is nothing to show but a placeholder.
  const SpotifyClient::NowPlaying np = deck::player();
  if (!deck::playerValid() || !np.hasTrack) {
    if (shownMode != 2 || shownTrack != "") {
      shownMode = 2;
      shownTrack = "";
      ui::showPlaying(F("Loading"), F(""));
    }
    return;
  }

  if (shownMode != 2 || shownTrack != np.track) {
    shownMode = 2;
    shownTrack = np.track;
    ui::showPlaying(np.track, np.artist);
  }
}

void connectWifi() {
  warnedNoSsid = false;  // New credentials deserve a fresh complaint if bad.

  const String ssid = settings::wifiSsid();
  if (ssid.isEmpty()) {
    Serial.println(F("[wifi] no network set -- run:  WIFI <ssid> <password>"));
    ui::showError(F("No WiFi set. serial> WIFI <ssid> <pass>"));
    return;
  }

  Serial.printf("[wifi] connecting to %s\n", ssid.c_str());
  ui::showWifiConnecting(ssid);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // Sleep adds seconds of latency to the tap->play path.
  WiFi.begin(ssid.c_str(), settings::wifiPass().c_str());

  const uint32_t deadline = millis() + 15000;
  while (WiFi.status() != WL_CONNECTED && millis() < deadline) {
    ui::tick();
    console::poll();
    delay(10);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[wifi] connected, ip %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println(F("[wifi] failed"));
    ui::showError(F("WiFi failed. Check ssid/pass with STATUS"));
  }
  lastWifiAttempt = millis();
}

void pollWifi() {
  const bool up = WiFi.status() == WL_CONNECTED;
  ui::setWifiConnected(up);

  if (up) {
    if (!wifiWasUp) {
      wifiWasUp = true;
      webapi::onWifiUp();
      // The token is warmed by the Spotify task, not here -- a refresh is a
      // blocking TLS call and this runs on the loop.
    }
    return;
  }

  wifiWasUp = false;

  // With no SSID stored there is nothing to retry: WiFi.begin("") just logs
  // three errors per cycle and buries whatever else is on the console.
  // Say it once and stay quiet until credentials arrive.
  if (settings::wifiSsid().isEmpty()) {
    if (!warnedNoSsid) {
      warnedNoSsid = true;
      Serial.println(F("[wifi] no network set -- run:  WIFI <ssid> <password>"));
    }
    return;
  }

  if (millis() - lastWifiAttempt > WIFI_RETRY_MS) {
    lastWifiAttempt = millis();
    Serial.println(F("[wifi] reconnecting"));
    WiFi.disconnect();
    WiFi.begin(settings::wifiSsid().c_str(), settings::wifiPass().c_str());
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println(F("\n\n=== RFID Cassette Player ==="));

  settings::begin();
  deck::begin();
  ui::begin();
  ui::showBoot(F("starting..."));

  if (!cards::begin()) {
    ui::showError(F("Storage failed"));
  }
  Serial.printf("[cards] %u cassette(s) paired\n", (unsigned)cards::count());

  SPI.begin();
  rfid.PCD_Init();
  delay(50);

  const byte version = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  Serial.printf("[rfid] MFRC522 version 0x%02X\n", version);
  if (version == 0x00 || version == 0xFF) {
    // Almost always miswiring or 5V on a 3.3V-only module.
    Serial.println(F("[rfid] NO READER FOUND -- check SPI wiring and that VCC is 3.3V"));
    ui::showError(F("RC522 not found. Check wiring (3.3V!)"));
    delay(3000);
  } else {
    rfid.PCD_AntennaOn();
    // Deliberately not RxGain_max -- see the note in config.h. The top gain
    // settings saturate on a tag resting against the coil, which is exactly
    // how this device is used.
    rfid.PCD_SetAntennaGain(RC522_RX_GAIN);

#if RC522_BOOST_ANTENNA
    // Receiver gain only helps hear a reply; these raise the transmitted field
    // itself, which is what a small coin tag actually needs to power up.
    rfid.PCD_WriteRegister(MFRC522::GsNReg, 0xFF);    // default 0x88
    rfid.PCD_WriteRegister(MFRC522::CWGsPReg, 0x3F);  // default 0x20
    Serial.println(F("[rfid] antenna drive boosted"));
#endif
  }

  spotify.begin();
  commands::begin();
  console::begin(&spotify, [] { diag::run(rfid); });

  webapi::Hooks hooks;
  hooks.onPaired = [](const String& uid, const String& uri) {
    // Pairing the tag that is sitting on the reader should start it straight
    // away -- otherwise you'd have to lift and re-place it, which is a strange
    // thing to ask right after telling the device what the cassette is.
    //
    // Runs on the web handler; queue rather than call, same as the card path.
    if (uid != deck::cardUid()) return;
    deck::setCard(uid, true, uri);
    commands::push(commands::playUri(uri));
  };
  webapi::begin(&spotify, hooks);

  // Pinned to core 0. The Arduino loop runs on core 1, and keeping Spotify's
  // blocking TLS calls off it is the whole point of the split. The stack is
  // generous because an mbedtls handshake plus ArduinoJson is not cheap.
  xTaskCreatePinnedToCore(spotifyTask, "spotify", 12288, nullptr, 1, nullptr, 0);

  connectWifi();
  if (WiFi.status() == WL_CONNECTED) webapi::onWifiUp();
}

void loop() {
  console::poll();

  if (console::consumeWifiChanged()) {
    WiFi.disconnect(true);
    connectWifi();
  }

  pollWifi();
  webapi::poll();
  pollRfid();
  syncDisplay();
  ui::tick();
}
