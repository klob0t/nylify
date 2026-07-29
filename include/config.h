#pragma once

// ---------------------------------------------------------------------------
// Pin map -- ESP32 DevKit v1
// ---------------------------------------------------------------------------
// RC522 rides the hardware VSPI bus. Note RST is on 27, NOT the usual 22:
// GPIO 22 is the OLED's SCL on this board.
//
//   RC522        ESP32          OLED (SSD1306)   ESP32
//   -----        -----          --------------   -----
//   SDA/SS  ->   GPIO 5         SDA          ->  GPIO 21
//   SCK     ->   GPIO 18        SCL          ->  GPIO 22
//   MOSI    ->   GPIO 23        VCC          ->  3V3
//   MISO    ->   GPIO 19        GND          ->  GND
//   RST     ->   GPIO 27
//   3.3V    ->   3V3   <-- 3.3V ONLY. 5V kills the RC522.
//   GND     ->   GND
//
// IRQ on the RC522 stays unconnected; we poll instead.

#define PIN_RC522_SS 5
#define PIN_RC522_RST 27

#define PIN_I2C_SDA 21
#define PIN_I2C_SCL 22

#define OLED_WIDTH 128
#define OLED_HEIGHT 64
#define OLED_I2C_ADDR 0x3C  // Try 0x3D if the screen stays blank.

// ---------------------------------------------------------------------------
// Behaviour
// ---------------------------------------------------------------------------

// Receiver gain. More is NOT better: with a tag sitting on the coil, the top
// settings saturate the demodulator -- the tag answers REQA/WUPA and then
// SELECT fails, which looks exactly like an incompatible tag.
//
// Measured on this build (src/tagdebug.cpp, 'g'), NTAG215 coin and MIFARE card:
//   18/23/33/38dB  10/10 reads      43dB  patchy      48dB  0/10 reads
// 33dB sits in the middle of the working range with margin on both sides.
//
//   0x00=18dB  0x10=23dB  0x40=33dB  0x50=38dB  0x60=43dB  0x70=48dB
#define RC522_RX_GAIN 0x40

// Raises the TX driver conductance for a stronger field. Only helps tags that
// are genuinely out of reach, and costs extra current and heat -- if reads are
// failing with a tag ON the coil, lower RC522_RX_GAIN instead.
#define RC522_BOOST_ANTENNA 0

// How often we ask the RC522 whether a card is sitting on it.
#define RFID_POLL_INTERVAL_MS 60

// The reader misses a still-present card now and then, so a single failed poll
// must not count as "lifted". Require this many consecutive misses first.
// 8 * 60ms ~= half a second of grace.
#define RFID_REMOVAL_DEBOUNCE 8

// Ignore a re-tap of the same card within this window, so a wobbly cassette
// does not restart the album.
#define RFID_RETAP_GRACE_MS 1200

// The *gap* between Spotify polls, measured from the end of the previous one
// (see refreshNowPlaying). This is the real freshness bound on the web UI --
// polling the device faster than this just re-reads the same cached answer.
//
// It used to need headroom above a round trip, because polling ran on the
// Arduino loop and back-to-back calls starved the web server. Since the split
// this runs on its own core and blocks nothing, so the only real limit is
// Spotify's rate limit. Cycle time is this plus ~1.1s of call, so 500 gives
// roughly a 1.6s cycle -- about 37 requests/minute, comfortably inside it.
#define NOWPLAYING_POLL_MS 500

// Refresh the access token this many seconds before it actually expires.
#define TOKEN_EARLY_REFRESH_S 60

// OLED redraw tick (drives the scrolling marquee).
#define UI_TICK_MS 50

#define WIFI_RETRY_MS 10000
