#pragma once

// Copy this file to include/secrets.h and fill it in.
//   cp include/secrets.example.h include/secrets.h
//
// Everything here is only a DEFAULT. Anything you set later over the serial
// console (WIFI / TOKEN / DEVICE) is saved to NVS and wins over these values,
// so you can leave them blank and configure the device entirely at runtime.
//
// See README.md for how to get the client id/secret and the refresh token.

#define DEFAULT_WIFI_SSID ""
#define DEFAULT_WIFI_PASS ""

// From https://developer.spotify.com/dashboard -> your app -> Settings
#define DEFAULT_SPOTIFY_CLIENT_ID ""
#define DEFAULT_SPOTIFY_CLIENT_SECRET ""

// Printed by: python tools/get_refresh_token.py
// Paste the VALUE only. The script prints it as `TOKEN AQ...` because that is
// the serial command; the "TOKEN " word is not part of the token itself.
#define DEFAULT_SPOTIFY_REFRESH_TOKEN ""

// Optional: pin playback to one speaker/phone. Leave "" to use whatever
// device is currently active. Find ids with the DEVICES serial command.
#define DEFAULT_SPOTIFY_DEVICE_ID ""
