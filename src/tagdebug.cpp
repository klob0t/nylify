// Barebones RC522 tag troubleshooter.
//
// No WiFi, no OLED, no filesystem, no Spotify -- just the reader, so nothing
// else can stall, reset or confound a reading. Build and run with:
//
//     pio run -e tagdebug -t upload
//     pio device monitor
//
// Then press a single key (no Enter needed):
//
//     s   one scan, reporting every step
//     c   continuous scan until you press a key
//     g   sweep every gain and drive setting  <-- the useful one
//     b   toggle antenna drive boost
//     i   re-init the reader
//     t   chip self-test
//     ?   help
//
// Back to the real firmware with:  pio run -t upload

#include <Arduino.h>
#include <MFRC522.h>
#include <SPI.h>

// Must match include/config.h. Left as literals so this file can be dropped
// into a bare Arduino IDE sketch to rule PlatformIO out entirely.
constexpr uint8_t PIN_SS = 5;
constexpr uint8_t PIN_RST = 27;

// RC522 defaults, from the datasheet's reset values.
constexpr uint8_t DRIVE_GSN_DEFAULT = 0x88;
constexpr uint8_t DRIVE_CWGSP_DEFAULT = 0x20;
constexpr uint8_t DRIVE_GSN_BOOST = 0xFF;
constexpr uint8_t DRIVE_CWGSP_BOOST = 0x3F;

MFRC522 rfid(PIN_SS, PIN_RST);
bool boosted = false;

// ---------------------------------------------------------------------------

void applyDrive(bool boost) {
  rfid.PCD_WriteRegister(MFRC522::GsNReg,
                         boost ? DRIVE_GSN_BOOST : DRIVE_GSN_DEFAULT);
  rfid.PCD_WriteRegister(MFRC522::CWGsPReg,
                         boost ? DRIVE_CWGSP_BOOST : DRIVE_CWGSP_DEFAULT);
  boosted = boost;
}

// WUPA, not REQA -- this matters and is easy to get wrong.
//
// REQA is only answered by cards in IDLE. Once a card has been selected and
// halted it sits in HALT, where it ignores REQA entirely and answers only
// WUPA. A loop that polls with REQA therefore reads a stationary card exactly
// once and then sees silence, which looks identical to a tag that is out of
// range. WUPA wakes cards in both states.
//
// The register writes reset the baud-rate and modulation-width settings that
// a previous session leaves behind; without them every later scan fails.
MFRC522::StatusCode probe(byte* atqa, byte* size) {
  rfid.PCD_WriteRegister(MFRC522::TxModeReg, 0x00);
  rfid.PCD_WriteRegister(MFRC522::RxModeReg, 0x00);
  rfid.PCD_WriteRegister(MFRC522::ModWidthReg, 0x26);
  return rfid.PICC_WakeupA(atqa, size);
}

bool answered(MFRC522::StatusCode s) {
  return s == MFRC522::STATUS_OK || s == MFRC522::STATUS_COLLISION;
}

// One full detect + select cycle. Always halts afterwards -- including when
// select failed, which would otherwise strand the card in READY where the
// next WUPA does not reach it.
bool tryRead(bool* replied) {
  byte atqa[2];
  byte n = sizeof(atqa);

  *replied = answered(probe(atqa, &n));
  if (!*replied) return false;

  const bool ok = rfid.PICC_Select(&rfid.uid) == MFRC522::STATUS_OK;
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
  return ok;
}

String uidHex() {
  String s;
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) s += '0';
    s += String(rfid.uid.uidByte[i], HEX);
  }
  s.toUpperCase();
  return s;
}

void showState() {
  const byte v = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  const byte tx = rfid.PCD_ReadRegister(MFRC522::TxControlReg);
  const byte gain = rfid.PCD_GetAntennaGain();

  Serial.printf("  chip 0x%02X   antenna %s   gain 0x%02X   drive %s\n", v,
                (tx & 0x03) ? "on" : "OFF", gain,
                boosted ? "boosted" : "default");

  if (v == 0x00 || v == 0xFF) {
    Serial.println(F("  !! reader not responding at all -- this is wiring, "
                     "not the tag"));
  }
}

// ---------------------------------------------------------------------------

// One attempt, reporting where in the exchange it stops.
void scanVerbose() {
  byte atqa[2];
  byte n = sizeof(atqa);

  const MFRC522::StatusCode req = probe(atqa, &n);
  Serial.print(F("  REQA   : "));
  Serial.print(MFRC522::GetStatusCodeName(req));

  if (!answered(req)) {
    Serial.println();
    Serial.println(F("  -> nothing in the field. Out of range, or a tag the "
                     "RC522 cannot speak to."));
    return;
  }

  // ATQA bit 6 of the first byte encodes UID size: 0x00=4 byte, 0x40=7 byte.
  const uint16_t atqaWord = ((uint16_t)atqa[1] << 8) | atqa[0];
  Serial.printf("   ATQA 0x%04X", atqaWord);
  switch (atqa[0] & 0xC0) {
    case 0x00: Serial.println(F("  (4-byte UID)")); break;
    case 0x40: Serial.println(F("  (7-byte UID -- NTAG/Ultralight)")); break;
    default:   Serial.println(F("  (10-byte UID)")); break;
  }

  // Select is the long part of the exchange, and where an under-powered tag
  // gives out: a 7-byte UID needs two cascade levels instead of one.
  const MFRC522::StatusCode sel = rfid.PICC_Select(&rfid.uid);
  Serial.print(F("  SELECT : "));
  Serial.println(MFRC522::GetStatusCodeName(sel));

  if (sel != MFRC522::STATUS_OK) {
    Serial.println(F("  -> it answered but browned out mid-select."));
    Serial.println(F("     The tag IS compatible; it is not getting enough "
                     "power."));
    Serial.println(F("     Press it flat on the coil, then try 'b' then 'g'."));
    return;
  }

  Serial.printf("  UID    : %s  (%u bytes)  SAK 0x%02X\n", uidHex().c_str(),
                rfid.uid.size, rfid.uid.sak);
  Serial.print(F("  TYPE   : "));
  Serial.println(MFRC522::PICC_GetTypeName(MFRC522::PICC_GetType(rfid.uid.sak)));
  Serial.println(F("  -> WORKS."));

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
}

void scanContinuous() {
  Serial.println(F("\ncontinuous -- press any key to stop"));
  while (!Serial.available()) {
    bool replied = false;
    if (tryRead(&replied)) {
      Serial.printf("  read  %s  SAK 0x%02X\n", uidHex().c_str(), rfid.uid.sak);
    } else if (replied) {
      Serial.println(F("  reply, but select failed  (under-powered)"));
    } else {
      Serial.print('.');
    }
    delay(200);
  }
  while (Serial.available()) Serial.read();
  Serial.println();
}

// Tries every gain and both drive levels, counting what actually works.
// This is what tells you whether the tag is reachable at all.
void gainSweep() {
  struct Gain {
    byte value;
    const char* label;
  };
  // 0x20/0x30 are datasheet duplicates of 18/23dB, so they are skipped.
  static const Gain kGains[] = {{0x00, "18dB"}, {0x10, "23dB"}, {0x40, "33dB"},
                                {0x50, "38dB"}, {0x60, "43dB"}, {0x70, "48dB"}};
  constexpr int kTries = 10;

  const bool restore = boosted;

  Serial.println(F("\nHold the tag STILL on the coil. ~25 seconds.\n"));
  Serial.println(F("  drive     gain    replies   reads"));
  Serial.println(F("  ------------------------------------"));

  int bestReads = 0;
  const char* bestLabel = nullptr;
  bool bestBoost = false;

  for (int b = 0; b < 2; b++) {
    applyDrive(b == 1);
    for (const Gain& g : kGains) {
      rfid.PCD_SetAntennaGain(g.value);
      delay(30);

      int replies = 0, reads = 0;
      for (int i = 0; i < kTries; i++) {
        bool replied = false;
        if (tryRead(&replied)) reads++;
        if (replied) replies++;
        delay(40);
      }

      Serial.printf("  %-8s  %-6s  %4d/%-3d  %4d/%-3d%s\n",
                    b ? "boosted" : "default", g.label, replies, kTries, reads,
                    kTries, reads == kTries ? "   <-- solid" : "");

      if (reads > bestReads) {
        bestReads = reads;
        bestLabel = g.label;
        bestBoost = (b == 1);
      }
    }
  }

  Serial.println();
  if (bestReads == 0) {
    Serial.println(F("  No setting read the tag."));
    Serial.println(F("  If the bundled white card DOES read, this tag is not "
                     "ISO14443A"));
    Serial.println(F("  despite what the listing says. Otherwise it is out of "
                     "range --"));
    Serial.println(F("  press it directly against the coil and repeat."));
  } else {
    Serial.printf("  Best: %s gain, drive %s (%d/%d reads)\n", bestLabel,
                  bestBoost ? "boosted" : "default", bestReads, kTries);
    if (bestBoost) {
      Serial.println(F("  -> set RC522_BOOST_ANTENNA to 1 in include/config.h"));
    }
    if (bestReads < kTries) {
      Serial.println(F("  -> still marginal; improve position before trusting "
                       "it"));
    }
  }

  // Leave the reader as we found it.
  applyDrive(restore);
  rfid.PCD_SetAntennaGain(MFRC522::RxGain_max);
}

// Live hit-rate meter for hunting the sweet spot.
//
// A gain sweep is meaningless while the tag is being hand-held: tremor moves
// it further than any register does. Tape the tag down, then slide the READER
// around underneath it and watch the bar.
void positionFinder() {
  constexpr int kBatch = 10;

  Serial.println(F("\nTAPE THE TAG DOWN. Then move the reader under it slowly."));
  Serial.println(F("Try the board edges, over the coil trace, not just the middle."));
  Serial.printf("Current gain 0x%02X, drive %s. Keys 1-6 change gain after "
                "stopping.\n",
                rfid.PCD_GetAntennaGain(), boosted ? "boosted" : "default");
  Serial.println(F("Press any key to stop.\n"));

  int bestRate = -1;

  while (!Serial.available()) {
    int hits = 0;
    for (int i = 0; i < kBatch && !Serial.available(); i++) {
      bool replied = false;
      if (tryRead(&replied)) hits++;
      delay(55);
    }

    const int rate = hits * 100 / kBatch;
    if (rate > bestRate) bestRate = rate;

    Serial.print(F("  ["));
    for (int i = 0; i < kBatch; i++) Serial.print(i < hits ? '#' : '.');
    Serial.printf("] %3d%%%s\n", rate, rate == 100 ? "   <-- keep it here" : "");
  }

  while (Serial.available()) Serial.read();
  Serial.printf("\n  best batch: %d%%\n", bestRate);
  if (bestRate < 100) {
    Serial.println(F("  Anything under 100% is not good enough to drop a "
                     "cassette on."));
  }
}

// A small coil returns a much weaker subcarrier than a credit-card-sized one.
// RxThresholdReg's MinLevel field decides how weak a reply the bit decoder
// will still accept -- the default (8) can silently discard an NTAG coin that
// a MIFARE card clears with room to spare. Nothing about the tag is wrong in
// that case; the reader is just refusing to listen hard enough.
void thresholdSweep() {
  // MinLevel in bits 7:4, CollLevel 4 in bits 2:0. Default is 0x84.
  static const byte kThresholds[] = {0x84, 0x74, 0x64, 0x54, 0x44, 0x34, 0x24};
  constexpr int kTries = 10;

  const bool restore = boosted;
  applyDrive(true);
  rfid.PCD_SetAntennaGain(MFRC522::RxGain_max);

  Serial.println(F("\nMax gain + boosted drive, lowering the receive threshold."));
  Serial.println(F("Hold the tag STILL on the coil. ~15 seconds.\n"));
  Serial.println(F("  RxThreshold  MinLevel  replies   reads"));
  Serial.println(F("  ----------------------------------------"));

  byte best = 0;
  int bestReads = 0;

  for (byte t : kThresholds) {
    rfid.PCD_WriteRegister(MFRC522::RxThresholdReg, t);
    delay(30);

    int replies = 0, reads = 0;
    for (int i = 0; i < kTries; i++) {
      bool replied = false;
      if (tryRead(&replied)) reads++;
      if (replied) replies++;
      delay(40);
    }

    Serial.printf("  0x%02X%s        %2u       %4d/%-3d  %4d/%-3d%s\n", t,
                  t == 0x84 ? " (default)" : "         ", t >> 4, replies,
                  kTries, reads, kTries, reads == kTries ? "  <-- solid" : "");

    if (reads > bestReads) {
      bestReads = reads;
      best = t;
    }
  }

  Serial.println();
  if (bestReads == 0) {
    Serial.println(F("  Still nothing. The receiver is not the bottleneck --"));
    Serial.println(F("  either no RF is reaching the tag, or it does not speak"));
    Serial.println(F("  ISO14443A. Try a different coin from the batch first."));
  } else {
    Serial.printf("  Best: RxThresholdReg = 0x%02X (%d/%d reads)\n", best,
                  bestReads, kTries);
    Serial.println(F("  -> tell me this value and I will wire it into the "
                     "firmware"));
  }

  rfid.PCD_WriteRegister(MFRC522::RxThresholdReg, 0x84);
  applyDrive(restore);
}

void selfTest() {
  Serial.println(F("\nrunning chip self-test..."));
  const bool ok = rfid.PCD_PerformSelfTest();
  Serial.println(ok ? F("  PASS -- the chip's digital core is healthy")
                    : F("  FAIL -- chip damaged, or a clone that omits the "
                        "self-test data"));
  Serial.println(F("  (a FAIL on a clone is common and not conclusive)"));

  // The self-test leaves the PCD unusable until re-initialised.
  rfid.PCD_Init();
  rfid.PCD_AntennaOn();
  rfid.PCD_SetAntennaGain(MFRC522::RxGain_max);
  applyDrive(boosted);
}

void initReader() {
  SPI.begin();
  rfid.PCD_Init();
  delay(50);
  rfid.PCD_AntennaOn();
  rfid.PCD_SetAntennaGain(MFRC522::RxGain_max);
  applyDrive(boosted);
}

void help() {
  Serial.println(F(
      "\n  s  single scan, step by step\n"
      "  c  continuous scan\n"
      "  g  sweep all gain + drive settings\n"
      "  r  sweep receive threshold (for weak/small tags)\n"
      "  p  live hit-rate meter -- tape the tag down and hunt the sweet spot\n"
      "  b  toggle antenna drive boost\n"
      "  1-6  set gain: 18 23 33 38 43 48 dB\n"
      "  i  re-init reader\n"
      "  t  chip self-test\n"
      "  ?  this help\n"));
}

// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println(F("\n\n=== RC522 tag debug ==========================="));
  Serial.printf("SS=%u RST=%u  SCK=18 MOSI=23 MISO=19\n", PIN_SS, PIN_RST);

  initReader();
  showState();
  help();
  Serial.println(F("Start with 'g' while holding the tag on the coil."));
}

void loop() {
  if (!Serial.available()) return;

  const char c = Serial.read();
  if (c == '\r' || c == '\n' || c == ' ') return;

  switch (c) {
    case 's': Serial.println(F("\n-- scan")); scanVerbose(); break;
    case 'c': scanContinuous(); break;
    case 'g': gainSweep(); break;
    case 'r': thresholdSweep(); break;
    case 'p': positionFinder(); break;

    case '1': case '2': case '3': case '4': case '5': case '6': {
      static const byte kGain[] = {0x00, 0x10, 0x40, 0x50, 0x60, 0x70};
      static const char* kLabel[] = {"18dB", "23dB", "33dB", "38dB", "43dB", "48dB"};
      const int i = c - '1';
      rfid.PCD_SetAntennaGain(kGain[i]);
      Serial.printf("\n  gain now %s\n", kLabel[i]);
      break;
    }
    case 'b':
      applyDrive(!boosted);
      Serial.printf("\n  drive now %s\n", boosted ? "BOOSTED" : "default");
      break;
    case 'i': Serial.println(F("\n-- re-init")); initReader(); showState(); break;
    case 't': selfTest(); showState(); break;
    case '?': case 'h': help(); break;
    default: Serial.printf("\n  '%c'? press ? for help\n", c); break;
  }
}
