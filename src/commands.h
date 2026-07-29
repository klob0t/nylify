#pragma once
#include <Arduino.h>

// Playback commands handed from the web API to whoever owns the Spotify client.
//
// A Spotify round trip costs ~2s, almost all of it TLS handshake. Running that
// inside an HTTP handler made every button in the UI feel broken, so requests
// are validated, queued and acknowledged immediately; execution happens later.
//
// Backed by a FreeRTOS queue, so it is already safe once the Spotify work moves
// off the Arduino loop onto its own task.
namespace commands {

enum class Type : uint8_t {
  Resume,
  Pause,
  Next,
  Previous,
  Volume,
  PlayUri,  // start a context; carries uri
};

// Kept POD so it can be memcpy'd into the FreeRTOS queue -- hence the fixed
// char array rather than a String. Spotify URIs run to about 40 characters
// ("spotify:playlist:" plus a 22-char base62 id), so 64 leaves room.
struct Command {
  Type type = Type::Resume;
  int16_t value = 0;  // volume percent; unused by the others
  char uri[64] = {0};
};

// Convenience for the PlayUri case; truncates rather than overflowing.
Command playUri(const String& uri);

void begin();

// False when the queue is full -- callers should surface that rather than
// silently dropping the command.
bool push(const Command& c);

// False when empty. Non-blocking.
bool pop(Command& out);

size_t pending();

}  // namespace commands
