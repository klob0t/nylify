#include "diag.h"

#include <Arduino.h>
#include <MFRC522.h>
#include <Wire.h>

#include "config.h"

namespace {

void checkI2C() {
  Serial.println(F("\nI2C bus (OLED)  SDA=21 SCL=22"));

  int found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  device at 0x%02X", addr);
      if (addr == 0x3C || addr == 0x3D) {
        Serial.print(F("  <- SSD1306 OLED"));
        if (addr != OLED_I2C_ADDR) {
          Serial.printf("  (config.h says 0x%02X -- change it to 0x%02X)",
                        OLED_I2C_ADDR, addr);
        }
      }
      Serial.println();
      found++;
    }
  }

  if (found == 0) {
    Serial.println(F("  NOTHING FOUND"));
    Serial.println(F("  -> check SDA/SCL are not swapped, and OLED VCC is on 3V3"));
  }
}

// Returns false if the chip isn't talking, in which case scanning is pointless.
bool checkRfid(MFRC522& rfid) {
  Serial.println(F("\nSPI bus (RC522)  SS=5 SCK=18 MOSI=23 MISO=19 RST=27"));

  const byte v = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  Serial.printf("  VersionReg = 0x%02X  ", v);

  switch (v) {
    case 0x91: Serial.println(F("(MFRC522 v1.0 -- OK)")); break;
    case 0x92: Serial.println(F("(MFRC522 v2.0 -- OK)")); break;
    case 0x12: Serial.println(F("(clone -- usually works)")); break;
    case 0x00:
    case 0xFF:
      // Both rails read as a stuck bus: 0x00 is MISO held low, 0xFF held high.
      Serial.println(F("NO RESPONSE"));
      Serial.println(F("  -> VCC must be 3V3, NOT VIN/5V (this kills RC522s)"));
      Serial.println(F("  -> check MISO=19 and MOSI=23 are not swapped"));
      Serial.println(F("  -> RST must be GPIO 27 here; 22 is taken by the OLED"));
      Serial.println(F("  -> resolder the header pins if they are only pushed in"));
      return false;
    default:
      Serial.println(F("(unknown chip, but the bus is alive)"));
      break;
  }

  const byte gain = rfid.PCD_GetAntennaGain();
  Serial.printf("  antenna gain = 0x%02X %s\n", gain,
                gain == RC522_RX_GAIN ? "(as configured)" : "(UNEXPECTED)");
  if (gain >= 0x60) {
    Serial.println(F("  -> this high a gain saturates on a tag touching the "
                     "coil; expect select failures"));
  }

  // The antenna driver can be off even when the chip answers over SPI.
  const byte txc = rfid.PCD_ReadRegister(MFRC522::TxControlReg);
  const bool antennaOn = (txc & 0x03) != 0;
  Serial.printf("  antenna     = %s\n", antennaOn ? "on" : "OFF (!)");
  if (!antennaOn) {
    Serial.println(F("  -> no RF field is being generated; no tag can ever reply"));
  }
  return true;
}

// Actively hunts for a tag and reports exactly what came back. This is the
// part that tells "wrong kind of tag" apart from "wrong wiring".
void scanForTags(MFRC522& rfid) {
  constexpr uint32_t kWindowMs = 8000;

  Serial.println(F("\nTag scan -- hold your tag flat on the reader now (8s)"));

  uint32_t replies = 0, fullReads = 0;
  String lastUid;
  const uint32_t deadline = millis() + kWindowMs;

  while (millis() < deadline) {
    byte atqa[2];
    byte size = sizeof(atqa);

    // Same register reset the library does before every REQA.
    rfid.PCD_WriteRegister(MFRC522::TxModeReg, 0x00);
    rfid.PCD_WriteRegister(MFRC522::RxModeReg, 0x00);
    rfid.PCD_WriteRegister(MFRC522::ModWidthReg, 0x26);

    const MFRC522::StatusCode s = rfid.PICC_RequestA(atqa, &size);

    if (s == MFRC522::STATUS_OK || s == MFRC522::STATUS_COLLISION) {
      replies++;
      if (rfid.PICC_ReadCardSerial()) {
        String uid;
        for (byte i = 0; i < rfid.uid.size; i++) {
          if (rfid.uid.uidByte[i] < 0x10) uid += '0';
          uid += String(rfid.uid.uidByte[i], HEX);
        }
        uid.toUpperCase();

        if (uid != lastUid) {
          lastUid = uid;
          fullReads++;
          const MFRC522::PICC_Type t = rfid.PICC_GetType(rfid.uid.sak);
          Serial.printf("  UID %s  (%u bytes)  SAK 0x%02X\n", uid.c_str(),
                        rfid.uid.size, rfid.uid.sak);
          Serial.printf("  type: %s\n", rfid.PICC_GetTypeName(t));
        }
        rfid.PICC_HaltA();
        rfid.PCD_StopCrypto1();
      }
    }
    delay(120);
  }

  Serial.printf("\n  replies=%u  reads=%u\n", replies, fullReads);

  if (fullReads > 0) {
    Serial.println(F("  RESULT: the reader and this tag work together."));
    Serial.println(F("  Pair it with:  MAP <spotify uri>"));
    return;
  }

  if (replies > 0) {
    // Something is in the field but anticollision never completed.
    Serial.println(F("  RESULT: a tag answered but could not be read fully."));
    Serial.println(F("  -> move it slightly; it may be half out of the field"));
    Serial.println(F("  -> remove any other tags/cards nearby (collision)"));
    return;
  }

  Serial.println(F("  RESULT: total silence -- nothing answered the RF field."));
  Serial.println(F("  The RC522 reads 13.56MHz ISO14443A ONLY. It physically"));
  Serial.println(F("  cannot read:"));
  Serial.println(F("    - 125kHz tags (EM4100/TK4100/T5577) -- most cheap fobs"));
  Serial.println(F("    - ISO15693 / NFC Type 5 (ICODE SLIX)"));
  Serial.println(F("  Test with the white card or blue fob that came with the"));
  Serial.println(F("  RC522, or an e-Money/Flazz/hotel key card. If those read"));
  Serial.println(F("  and yours does not, your tag is the wrong type."));
}

}  // namespace

namespace diag {

void run(MFRC522& rfid) {
  Serial.println(F("\n=== wiring check ==============================="));
  checkI2C();
  if (checkRfid(rfid)) {
    scanForTags(rfid);
  }
  Serial.println(F("================================================\n"));
}

}  // namespace diag
