#include "commands.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

namespace {

QueueHandle_t queue = nullptr;

// Deep enough to absorb someone jabbing "next" a few times, shallow enough
// that a stalled consumer cannot bank up a minute of queued playback changes.
constexpr UBaseType_t kDepth = 8;

}  // namespace

namespace commands {

void begin() {
  if (!queue) queue = xQueueCreate(kDepth, sizeof(Command));
}

Command playUri(const String& uri) {
  Command c;
  c.type = Type::PlayUri;
  strlcpy(c.uri, uri.c_str(), sizeof(c.uri));
  return c;
}

bool push(const Command& c) {
  if (!queue) return false;
  return xQueueSend(queue, &c, 0) == pdTRUE;  // never block the caller
}

bool pop(Command& out) {
  if (!queue) return false;
  return xQueueReceive(queue, &out, 0) == pdTRUE;
}

size_t pending() {
  return queue ? uxQueueMessagesWaiting(queue) : 0;
}

}  // namespace commands
