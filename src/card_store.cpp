#include "card_store.h"

#include <ArduinoJson.h>
#include <LittleFS.h>

namespace {

constexpr const char* kPath = "/cards.json";

// The whole map lives in RAM; a few dozen cassettes is a few KB at most.
JsonDocument doc;

bool save() {
  File f = LittleFS.open(kPath, "w");
  if (!f) {
    Serial.println(F("[cards] cannot open /cards.json for writing"));
    return false;
  }
  const bool ok = serializeJson(doc, f) > 0;
  f.close();
  return ok;
}

}  // namespace

namespace cards {

bool begin() {
  if (!LittleFS.begin(true)) {  // true = format if the partition is blank
    Serial.println(F("[cards] LittleFS mount failed"));
    return false;
  }

  File f = LittleFS.open(kPath, "r");
  if (!f) {
    doc.to<JsonObject>();
    return save();
  }

  const DeserializationError err = deserializeJson(doc, f);
  f.close();

  if (err) {
    Serial.printf("[cards] %s is corrupt (%s), starting empty\n", kPath, err.c_str());
    doc.to<JsonObject>();
    return save();
  }
  return true;
}

String lookup(const String& uid) {
  JsonVariant v = doc[uid];
  return v.isNull() ? String() : v.as<String>();
}

bool set(const String& uid, const String& uri) {
  doc[uid] = uri;
  return save();
}

bool remove(const String& uid) {
  if (doc[uid].isNull()) return false;
  doc.remove(uid);
  return save();
}

void forEach(const std::function<void(const String&, const String&)>& fn) {
  for (JsonPair kv : doc.as<JsonObject>()) {
    fn(String(kv.key().c_str()), kv.value().as<String>());
  }
}

size_t count() {
  return doc.as<JsonObject>().size();
}

bool clear() {
  doc.to<JsonObject>();
  return save();
}

}  // namespace cards
