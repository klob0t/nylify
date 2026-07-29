#!/usr/bin/env python3
"""One-time helper: get a Spotify refresh token for the ESP32.

The ESP32 can refresh its own access tokens forever, but the *first* token has
to come from a browser login. This runs that login once on your PC and prints
the refresh token to paste into the device.

Standard library only -- no pip install needed.

    python tools/get_refresh_token.py

Before running, in https://developer.spotify.com/dashboard:
  1. Create an app (any name).
  2. Settings -> Redirect URIs -> add exactly:  http://127.0.0.1:8888/callback
  3. Copy the Client ID and Client Secret.
"""

import base64
import http.server
import json
import secrets
import sys
import threading
import urllib.error
import urllib.parse
import urllib.request
import webbrowser

REDIRECT_URI = "http://127.0.0.1:8888/callback"
PORT = 8888

# Everything the firmware needs: start/stop playback, and read what's playing.
SCOPES = " ".join(
    [
        "user-read-playback-state",
        "user-modify-playback-state",
        "user-read-currently-playing",
    ]
)

_result = {}
_done = threading.Event()


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path != "/callback":
            self.send_error(404)
            return

        params = urllib.parse.parse_qs(parsed.query)
        _result["code"] = params.get("code", [None])[0]
        _result["state"] = params.get("state", [None])[0]
        _result["error"] = params.get("error", [None])[0]

        ok = _result["code"] is not None
        body = (
            "<h2>Authorized.</h2><p>Back to your terminal for the token.</p>"
            if ok
            else f"<h2>Failed</h2><p>{_result['error']}</p>"
        )
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.end_headers()
        self.wfile.write(body.encode())
        _done.set()

    def log_message(self, *args):
        pass  # Keep the terminal clean.


def post_token(client_id: str, client_secret: str, data: dict) -> dict:
    creds = base64.b64encode(f"{client_id}:{client_secret}".encode()).decode()
    req = urllib.request.Request(
        "https://accounts.spotify.com/api/token",
        data=urllib.parse.urlencode(data).encode(),
        headers={
            "Authorization": f"Basic {creds}",
            "Content-Type": "application/x-www-form-urlencoded",
        },
    )
    try:
        with urllib.request.urlopen(req, timeout=20) as resp:
            return json.loads(resp.read())
    except urllib.error.HTTPError as e:
        detail = e.read().decode(errors="replace")
        sys.exit(f"\nToken request failed ({e.code}): {detail}")


def main() -> None:
    print(__doc__.split("Before running")[0].strip())
    print()
    client_id = input("Client ID     : ").strip()
    client_secret = input("Client Secret : ").strip()
    if not client_id or not client_secret:
        sys.exit("Both are required.")

    state = secrets.token_urlsafe(16)
    auth_url = "https://accounts.spotify.com/authorize?" + urllib.parse.urlencode(
        {
            "client_id": client_id,
            "response_type": "code",
            "redirect_uri": REDIRECT_URI,
            "scope": SCOPES,
            "state": state,
            # Force the consent screen so re-runs reliably issue a token.
            "show_dialog": "true",
        }
    )

    server = http.server.HTTPServer(("127.0.0.1", PORT), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()

    print(f"\nOpening your browser. If it doesn't open, visit:\n{auth_url}\n")
    webbrowser.open(auth_url)

    if not _done.wait(timeout=300):
        sys.exit("Timed out waiting for the browser redirect.")
    server.shutdown()

    if _result.get("error"):
        sys.exit(f"Spotify returned: {_result['error']}")
    if _result.get("state") != state:
        sys.exit("State mismatch -- aborting.")

    tokens = post_token(
        client_id,
        client_secret,
        {
            "grant_type": "authorization_code",
            "code": _result["code"],
            "redirect_uri": REDIRECT_URI,
        },
    )

    refresh = tokens.get("refresh_token")
    if not refresh:
        sys.exit(f"No refresh_token in response: {tokens}")

    print("\n" + "=" * 70)
    print("Paste this into the serial monitor (115200 baud, press Enter):\n")
    print(f"TOKEN {refresh}")
    print("\n...or put it in include/secrets.h as DEFAULT_SPOTIFY_REFRESH_TOKEN.")
    print("=" * 70)


if __name__ == "__main__":
    main()
