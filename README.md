# RFID Cassette Player

Drop an RFID tag shaped like a cassette (or a vinyl) onto the reader and Spotify
starts playing the album paired to it. Lift it off and playback pauses. The OLED
shows the current track.

The ESP32 talks to the Spotify Web API directly over HTTPS — no PC, no phone app,
no companion script once it's flashed.

```
[cassette] ──► RC522 ──► ESP32 ──► WiFi ──► api.spotify.com
                            │
                            └──► SSD1306 OLED (now playing)
```

**Requires Spotify Premium.** The Web API refuses playback control on free
accounts. It also does not stream audio itself — it tells an *existing* Spotify
device (phone, laptop, speaker, Chromecast) what to play, the same way the phone
app's remote-control works.

---

## 1. Wiring

The RC522 runs on the hardware VSPI bus, the OLED on I²C.

| RC522 | ESP32   | | SSD1306 | ESP32   |
|-------|---------|-|---------|---------|
| SDA / SS | **GPIO 5**  | | SDA | **GPIO 21** |
| SCK   | **GPIO 18** | | SCL | **GPIO 22** |
| MOSI  | **GPIO 23** | | VCC | **3V3**     |
| MISO  | **GPIO 19** | | GND | **GND**     |
| RST   | **GPIO 27** | |     |             |
| GND   | GND     | |         |         |
| VCC   | **3V3** | |         |         |
| IRQ   | *(leave unconnected)* | | | |

Two things that bite people here:

- **RST is on GPIO 27, not the usual 22.** Most RC522 tutorials use 22, but on
  this board that's the OLED's SCL. They cannot share.
- **The RC522 is 3.3 V only.** Its VCC pin is not 5 V tolerant, and wiring it to
  VIN is the single most common way to kill one. The OLED is happy on 3.3 V too.

Also check your OLED's silkscreen before connecting power — these modules ship
with both `GND VCC SCL SDA` and `VCC GND SCL SDA` header orders, and reversing
the first two usually kills the panel.

Once flashed, verify the wiring from the serial monitor rather than guessing:

```
DIAG
```

It scans the I²C bus and reads the RC522's version register, so a fault points
at one module instead of both:

```
I2C bus (OLED)  SDA=21 SCL=22
  device at 0x3C  <- SSD1306 OLED

SPI bus (RC522)  SS=5 SCK=18 MOSI=23 MISO=19 RST=27
  VersionReg = 0x92  (MFRC522 v2.0 -- OK)
```

A version register of `0x00` or `0xFF` means the reader isn't responding at all —
wiring or power, not software. `0x91`, `0x92` and `0x12` are all good.

Pin assignments live in `include/config.h` if your build needs different ones.
There's a fuller wiring reference, including a bus diagram, in `docs/wiring.html`.

## 2. Spotify app credentials

1. Go to <https://developer.spotify.com/dashboard> and create an app (any name).
2. **Settings → Redirect URIs →** add exactly:
   ```
   http://127.0.0.1:8888/callback
   ```
   Spotify requires the literal IP here; `localhost` is rejected.
3. Copy the **Client ID** and **Client Secret**.

Then create your secrets file and paste them in:

```bash
cp include/secrets.example.h include/secrets.h
```

Fill in `DEFAULT_SPOTIFY_CLIENT_ID` and `DEFAULT_SPOTIFY_CLIENT_SECRET`.
`include/secrets.h` is gitignored. Leave the WiFi and refresh-token fields empty
for now — you'll set those over serial in step 4.

## 3. Get a refresh token

The device can renew its own access tokens forever, but the first one has to come
from a browser login. Run this once on your PC (standard library only, nothing to
install):

```bash
python tools/get_refresh_token.py
```

If `python` isn't on your PATH, use the interpreter PlatformIO already installed —
no separate Python needed:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" tools\get_refresh_token.py
```

It asks for your client id/secret, opens a browser to approve the app, and prints
a `TOKEN <...>` line. Keep that terminal open for step 4.

## 4. Flash and configure

```bash
pio run -t upload
pio device monitor
```

In the serial monitor (115200 baud, type a command and press Enter):

```
WIFI MyNetwork my wifi password
TOKEN AQD3f...paste the long string from step 3...
```

Both are saved to NVS, so they survive reflashing. Check everything took:

```
STATUS
```

Playback needs somewhere to play. Open Spotify on your phone or laptop and play
*anything* for a second so it registers as an active device, then:

```
DEVICES
```

Pin the one you want the cassettes to control:

```
DEVICE 5fbb3ba6aa454b5534c4ba43a8c7e8e10a2f9b1e
```

Pinning is worth doing — otherwise playback follows whatever device you last used,
which is rarely what you want from a physical object sitting on a shelf. `DEVICE -`
unpins it again.

## 5. Pair your cassettes

Drop a tag on the reader. The OLED shows `Unpaired cassette` with its UID, and
the serial log prints the same. Grab a Spotify link (right-click an album →
**Share → Copy link**) and type:

```
MAP https://open.spotify.com/album/4aawyAB9vmqN3uQ7FjRGTy
```

That pairs the album to the tag you just tapped. Paste the share URL as-is —
the tracking `?si=` suffix and `/intl-xx/` locale prefixes are stripped for you.
Bare `spotify:album:...` URIs work too, as do playlists, artists, tracks and
podcast episodes.

Lift the tag and drop it again — it should start playing.

Nothing is ever written to the tag. Only its factory UID is read, so any tag
works (MIFARE Classic, NTAG stickers, keyfobs) and a cassette can't be corrupted.
The UID → URI table lives in the ESP32's flash.

> **Back up your pairings.** They live in ESP32 flash, and `pio run -t erase`
> wipes them. Run `EXPORT` and save the output — it's a list of `MAP` commands
> you can paste straight back in.

## 6. The web UI (optional)

A browser front-end in `web/` shows album art, a live progress bar and playback
controls, and lets you pair cassettes without touching the serial monitor.

The ESP32 serves a JSON API on port 80; the page itself is served by Vite on
your PC. **The deck works fine without it** — this is a companion display, not a
dependency.

```
npm install --prefix web
npm run dev --prefix web
```

Then open the printed URL. Vite proxies `/api` to the device, so no CORS is
involved in dev.

If the dev server logs `ECONNREFUSED` or `EAI_AGAIN`, `cassette.local` isn't
resolving from Node — copy `web/.env.example` to `web/.env` and put the device's
IP in it (`STATUS` on the serial console prints it):

```
VITE_DEVICE_HOST=192.168.1.50
```

The dev server also listens on your LAN, so you can open the same URL on a phone
propped next to the deck.

**What it shows:** album art (with a blurred backdrop), track/artist/album, an
interpolated progress bar, play–pause–skip, a volume slider, which cassette is
currently on the reader, and your whole library with an unpair button. Tapping an
unpaired tag pops up a field to paste a Spotify link — pairing from there starts
playback immediately, without lifting the tag.

### API

| Route | Purpose |
|---|---|
| `GET /api/state` | everything the page renders — served from cache, never blocks on Spotify |
| `GET /api/cards` | all pairings |
| `POST /api/cards` | `{uid?, uri}` — pair (defaults to the card on the deck) |
| `DELETE /api/cards?uid=` | unpair |
| `POST /api/playback` | `{action: play\|pause\|toggle\|next\|previous}` |
| `POST /api/volume` | `{value: 0-100}` |
| `GET /api/devices` | Spotify playback devices |

`/api/state` is answered entirely from a cache that the main loop refreshes, so
polling it once a second costs the ESP32 almost nothing and never hits Spotify's
rate limit. The device only polls Spotify while a cassette is playing *or* a
browser has made a request in the last 15 s — which is what keeps the page live
when you start playback from the phone app instead of a tag.

`progressMs` ships alongside an `ageMs` telling the browser how stale it is, so
the progress bar extrapolates smoothly instead of snapping backwards every poll.

## Serial commands

| Command | What it does |
|---|---|
| `HELP` | command list |
| `STATUS` | wifi / token / device / card count |
| `DIAG` | probe the RC522 and OLED wiring (see step 1) |
| `MAP <uri>` | pair the last-tapped card |
| `MAP <uid> <uri>` | pair a specific card |
| `UNMAP <uid>` | forget a card |
| `LIST` | show all pairings |
| `EXPORT` | dump pairings as `MAP` commands — **back these up** |
| `WIPECARDS` | delete every pairing |
| `WIFI <ssid> <pass>` | set credentials and reconnect |
| `TOKEN <refresh_token>` | store the Spotify refresh token |
| `DEVICES` | list Spotify playback devices |
| `DEVICE <id>` | pin playback to one device (`-` to unpin) |
| `RESET` | clear wifi/token/device and reboot |

## How it behaves

- **Tag placed** → look up UID → `PUT /me/player/play` with the album as
  `context_uri`. If the pinned device has gone idle, playback is transferred to
  it and retried once.
- **Tag lifted** → `PUT /me/player/pause`.
- **Tag wobbles** → a re-tap of the same card within 1.2 s counts as a bounce and
  *resumes* rather than restarting the album from track 1.
- **While playing** → the track name and artist are polled every 0.5 s. Titles too
  wide for the screen scroll.

Removal detection uses `PICC_WakeupA` rather than `PICC_IsNewCardPresent`. The
latter only sees cards in the IDLE state, and a card that's already been selected
and halted isn't — so it can't tell "still sitting there" from "gone". A single
missed read doesn't count as a lift either; eight consecutive misses (~0.5 s) do.

## Tuning

`include/config.h`:

| Setting | Meaning |
|---|---|
| `RFID_REMOVAL_DEBOUNCE` | misses before "lifted" — raise if playback pauses spuriously |
| `RFID_RETAP_GRACE_MS` | bounce window |
| `NOWPLAYING_POLL_MS` | gap between Spotify polls — the freshness bound on the web UI |
| `OLED_I2C_ADDR` | try `0x3D` if the screen stays blank |

**Don't raise `RC522_RX_GAIN` to fix a tag that won't read.** More gain is not
better here. Measured on this build with `src/tagdebug.cpp`, 18–38 dB read both
an NTAG215 coin and a MIFARE card 10/10, while 43 dB was patchy and 48 dB
(`RxGain_max`) read *nothing* — the tag answers and then `SELECT` fails, because
the demodulator saturates on a tag resting against the coil. That failure looks
identical to an incompatible tag, which makes it an easy hour to lose. If reads
fail with the tag touching the reader, lower the gain.

`RC522_BOOST_ANTENNA` is the opposite lever, for tags genuinely out of reach —
it raises the transmitted field at the cost of current and heat.

## Troubleshooting

| Symptom | Cause |
|---|---|
| `MFRC522 version 0x00 / 0xFF` | wiring, or VCC on 5 V instead of 3.3 V |
| OLED blank | try `OLED_I2C_ADDR 0x3D`; check SDA/SCL aren't swapped |
| `needs Premium` | Web API playback control is Premium-only |
| `no active device` | open Spotify somewhere and play something once, then `DEVICES` |
| `token refresh HTTP 400` | bad/expired refresh token — rerun step 3 |
| `no refresh token` | run `TOKEN <...>` |
| Pauses randomly while playing | raise `RFID_REMOVAL_DEBOUNCE` |
| Pairings vanished | flash was erased; paste your `EXPORT` backup back in |

## Layout

```
platformio.ini              build config, partition table, library pins
include/config.h            pin map and timing constants
include/secrets.example.h   template -> copy to secrets.h (gitignored)
src/main.cpp                RFID presence state machine, wifi, glue
src/spotify_client.*        token refresh + play/pause/now-playing
src/card_store.*            UID -> URI map in LittleFS (/cards.json)
src/settings.*              NVS-backed config
src/ui.*                    SSD1306 screens and title marquee
src/console.*               serial command parser
src/deck_state.*            shared blackboard: main writes, web_api reads
src/web_api.*               JSON API on port 80 + mDNS (cassette.local)
src/diag.cpp                DIAG: I2C scan and RC522 probe
src/tagdebug.cpp            gain sweep used to characterise the reader
tools/get_refresh_token.py  one-time OAuth helper
web/                        Vite + vanilla TS front-end (optional)
  src/api.ts                typed wrapper over the device API
  src/main.ts               polling, progress interpolation, controls
  vite.config.ts            /api proxy -> VITE_DEVICE_HOST
```

## Notes on the TLS setup

`SpotifyClient::begin()` calls `setInsecure()`, so certificates aren't verified.
Pinning a root CA would be stronger, but Spotify rotates its chain and an expired
pinned root would brick the device with no way to update it over the air. For a
device on your own LAN talking to one fixed host, that trade favours staying
working. If you'd rather verify, swap in `setCACertBundle()` with the ESP32
core's bundle and plan to reflash when it expires.
