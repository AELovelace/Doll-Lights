// lights.ino: the console's connection to boxchat (Wi-Fi + MQTT) and every way of changing a light.
//
// Commands use the boxchat zone standard (README of the main project): zone z (1-4), channel c
// (R G B W = 0-3), level v -> command (4*(z-1) + c + 1) * 1000 + v, published to boxchat/<id>/set.
// Relays get 1 / 0; 32002 only listens on the shared "boxchat" topic (activator, then command).
//
// Changes are made to the local copy first (so knobs feel instant), then published at most every
// PUBLISH_GAP_MS per light, only for the channels that actually changed. While our own changes are
// in flight, the lights' state echoes are ignored for HOLD_MS so they can't yank a knob backwards.

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "secrets.h"                           // WIFI_SSID / WIFI_PASSWORD (git-ignored).

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
const unsigned long PUBLISH_GAP_MS = 50;       // Per light: up to 20 updates a second while a knob spins.
const unsigned long HOLD_MS = 800;             // Ignore state echoes this long after our last change.
const unsigned long NET_RETRY_MS = 5000;
unsigned long lastNetTry = 0;
bool netWasUp = false;

// --- Small helpers ------------------------------------------------------------------------------

uint8_t maxLevel(const Rgbw &c) {
  return max(max(c.v[0], c.v[1]), max(c.v[2], c.v[3]));
} // Completes "how bright is this colour" (its biggest channel).

bool isLit(const Rgbw &c) {
  return maxLevel(c) > 0;
} // Completes the on/off check for one zone.

Rgbw scaled(const Rgbw &c, uint8_t brightness) {
  uint8_t top = maxLevel(c);
  Rgbw out = {{0, 0, 0, 0}};
  if (top == 0) return out;
  for (int i = 0; i < 4; i++) out.v[i] = (uint8_t)((c.v[i] * (uint16_t)brightness + top / 2) / top);
  return out;
} // Completes scaling a colour so its biggest channel equals brightness (keeps the hue).

uint8_t zoneFirst(const Light &light) {
  return selZone == 0 ? 0 : min<uint8_t>(selZone, light.zones) - 1;
} // Completes the 0-based first zone the selection covers.

uint8_t zoneLast(const Light &light) {
  return selZone == 0 ? light.zones - 1 : min<uint8_t>(selZone, light.zones) - 1;
} // Completes the 0-based last zone the selection covers.

Light &selected() {
  return lights[selLight];
} // Completes access to the selected light.

void setZone(Light &light, uint8_t z, const Rgbw &colour, bool newHue) {
  light.zone[z] = colour;
  if (newHue && isLit(colour)) light.hue[z] = scaled(colour, 255);  // Remember the shape at full brightness.
  light.dirty[z] = true;
  light.known = true;
  light.holdUntil = millis() + HOLD_MS;
} // Completes one local zone change; publishLights() sends it.

// --- Changing lights (everything below acts on the selection unless it says otherwise) ------------

void adjustSelected(uint8_t channel, int delta) {
  Light &light = selected();
  for (uint8_t z = zoneFirst(light); z <= zoneLast(light); z++) {
    Rgbw c = light.zone[z];
    if (channel == CH_BRI) {
      int bright = constrain((int)maxLevel(c) + delta, 0, 255);
      if (!isLit(light.hue[z])) light.hue[z] = scaled(DEFAULT_ON, 255);  // Never lit: start from white.
      setZone(light, z, scaled(light.hue[z], bright), false);           // Same hue, new brightness.
    } else {
      int idx = channel - CH_R;                // CH_R..CH_W -> 0..3.
      c.v[idx] = constrain((int)c.v[idx] + delta, 0, 255);
      setZone(light, z, c, true);
    }
  }
} // Completes a knob turn (or NAV LEVEL change) on the selected light/zone(s).

uint8_t selectedLevel(uint8_t channel) {
  const Light &light = lights[selLight];
  const Rgbw &c = light.zone[zoneFirst(light)];
  return channel == CH_BRI ? maxLevel(c) : c.v[channel - CH_R];
} // Completes reading the selection's current level of one channel (first covered zone).

void setSelectedLevel(uint8_t channel, int level) {
  adjustSelected(channel, constrain(level, 0, 255) - (int)selectedLevel(channel));
} // Completes jumping a channel straight to a level (CYD bar taps).

void colourSelected(const Rgbw &colour) {
  Light &light = selected();
  for (uint8_t z = zoneFirst(light); z <= zoneLast(light); z++) setZone(light, z, colour, true);
} // Completes painting the selection one colour (scenes, on).

bool selectedLit() {
  const Light &light = lights[selLight];
  for (uint8_t z = zoneFirst(light); z <= zoneLast(light); z++) if (isLit(light.zone[z])) return true;
  return false;
} // Completes "is any part of the selection on".

void selectedOn() {
  Light &light = selected();
  for (uint8_t z = zoneFirst(light); z <= zoneLast(light); z++) {
    if (isLit(light.zone[z])) continue;        // Already on: leave it.
    setZone(light, z, isLit(light.hue[z]) ? light.hue[z] : DEFAULT_ON, true);  // Last colour, or white.
  }
} // Completes switching the selection on (its last colour at full brightness).

void selectedOff() {
  Light &light = selected();
  for (uint8_t z = zoneFirst(light); z <= zoneLast(light); z++) setZone(light, z, Rgbw{{0, 0, 0, 0}}, false);
} // Completes switching the selection off (the hue is kept for the next "on").

void toggleSelected() {
  if (selectedLit()) selectedOff(); else selectedOn();
} // Completes on/off for the selection.

void setRelay(Light &light, bool on) {
  if (!light.relayId || !mqtt.connected()) return;
  String id = String(light.relayId);
  if (light.relayShared) {                     // Shared protocol: arm with the activator, then one command.
    mqtt.publish(BASE_TOPIC, id.c_str());
    mqtt.publish(BASE_TOPIC, on ? "1" : "0");
  } else {
    mqtt.publish((String(BASE_TOPIC) + "/" + id + "/set").c_str(), on ? "1" : "0");
  }
  light.relayOn = on ? 1 : 0;                  // Relays report nothing back, so remember what we sent.
} // Completes switching a light's master-power relay.

void toggleRelay(Light &light) {
  setRelay(light, light.relayOn != 1);         // Unknown counts as off, so the first press turns it on.
} // Completes the power-badge / POWER-menu toggle.

void allOff() {
  for (uint8_t i = 0; i < LIGHT_COUNT; i++) {
    Light &light = lights[i];
    for (uint8_t z = 0; z < light.zones; z++) {
      light.zone[z] = Rgbw{{0, 0, 0, 0}};
      light.sent[z] = light.zone[z];           // "0" below already turns every zone off.
      light.dirty[z] = false;
    }
    light.holdUntil = millis() + HOLD_MS;
    if (mqtt.connected()) mqtt.publish((String(BASE_TOPIC) + "/" + light.id + "/set").c_str(), "0");
    setRelay(light, false);
  }
} // Completes ALL OFF: every light to 0 (firmware all-off command) and every relay off.

// --- Publishing ------------------------------------------------------------------------------------

void publishLights() {
  if (!mqtt.connected()) return;               // Changes wait (dirty) until the broker is back.
  unsigned long now = millis();
  for (uint8_t i = 0; i < LIGHT_COUNT; i++) {
    Light &light = lights[i];
    if (now - light.lastPublish < PUBLISH_GAP_MS) continue;
    String topic = String(BASE_TOPIC) + "/" + light.id + "/set";
    for (uint8_t z = 0; z < light.zones; z++) {
      if (!light.dirty[z]) continue;
      for (uint8_t c = 0; c < 4; c++) {
        if (light.zone[z].v[c] == light.sent[z].v[c]) continue;  // Only what changed.
        long command = (4L * z + c + 1) * 1000 + light.zone[z].v[c];
        mqtt.publish(topic.c_str(), String(command).c_str());
        light.sent[z].v[c] = light.zone[z].v[c];
      }
      light.dirty[z] = false;
      light.lastPublish = now;
    }
  }
} // Completes sending pending changes as zone-standard commands, rate-limited per light.

// --- Receiving -------------------------------------------------------------------------------------

int lightIndex(uint16_t id) {
  for (uint8_t i = 0; i < LIGHT_COUNT; i++) {
    if (lights[i].id == id || lights[i].relayId == id) return i;
  }
  return -1;
} // Completes finding the light a board (or its relay) belongs to.

void onMqttMessage(char *topic, byte *payload, unsigned int length) {
  String t(topic);
  int slash1 = t.indexOf('/'), slash2 = t.indexOf('/', slash1 + 1);
  if (slash1 < 0 || slash2 < 0) return;        // Not boxchat/<id>/<kind>.
  uint16_t id = t.substring(slash1 + 1, slash2).toInt();
  String kind = t.substring(slash2 + 1);
  int index = lightIndex(id);
  if (index < 0) return;
  Light &light = lights[index];

  if (kind == "status") {
    String status;
    for (unsigned int i = 0; i < length; i++) status += (char)payload[i];
    if (id == light.id) {                      // Only the light board's own status counts as "online".
      if (status == "online" || status == "ready") light.online = true;
      else if (status == "offline") light.online = false;   // Other texts are command errors: ignore.
    }
    return;
  }
  if (kind != "state" || id != light.id) return;
  if ((long)(millis() - light.holdUntil) < 0) return;  // Our own change is in flight: ignore the echo.

  JsonDocument doc;
  if (deserializeJson(doc, payload, length)) return;  // Not valid JSON: ignore.
  JsonArray zones = doc["zones"].as<JsonArray>();
  for (uint8_t z = 0; z < light.zones; z++) {
    JsonVariant src;                           // This zone's {"r","g","b","w"}.
    if (!zones.isNull()) src = zones[z];       // Multi-zone firmware lists every zone...
    else if (z == 0) src = doc.as<JsonVariant>();  // ...single-zone firmware reports only the flat object.
    if (src.isNull()) continue;
    Rgbw c;
    const char *keys[4] = {"r", "g", "b", "w"};
    for (int i = 0; i < 4; i++) c.v[i] = (uint8_t)constrain((int)(src[keys[i]] | 0), 0, 255);
    light.zone[z] = c;
    light.sent[z] = c;                         // The light already has it: nothing to publish.
    light.dirty[z] = false;
    if (isLit(c)) light.hue[z] = scaled(c, 255);
  }
  light.known = true;                          // (Retained state can be old, so it doesn't prove "online".)
} // Completes applying a light's retained state (or status) from boxchat.

// --- Connection ------------------------------------------------------------------------------------

bool netUp() {
  return mqtt.connected();
} // Completes the "are we on boxchat" check (Wi-Fi + broker).

void networkBegin() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(512);                     // Room for a 4-zone state JSON.
  mqtt.setCallback(onMqttMessage);
} // Completes starting Wi-Fi and preparing the MQTT client (connecting happens in networkService()).

void networkService() {
  if (mqtt.connected()) {
    mqtt.loop();                               // Receive state, keep the connection alive.
    publishLights();
    return;
  }
  if (netWasUp) {
    netWasUp = false;
    Serial.println("boxchat disconnected; retrying every 5 s");
  }
  if (WiFi.status() != WL_CONNECTED || millis() - lastNetTry < NET_RETRY_MS) return;
  lastNetTry = millis();
  String statusTopic = String(BASE_TOPIC) + "/" + CONSOLE_ID + "/status";
  String clientId = "MasterConsole-" + WiFi.macAddress();  // Unique, so it never kicks another board off.
  if (mqtt.connect(clientId.c_str(), statusTopic.c_str(), 1, true, "offline")) {
    mqtt.publish(statusTopic.c_str(), "online", true);
    mqtt.subscribe("boxchat/+/state");         // Every light's retained colour...
    mqtt.subscribe("boxchat/+/status");        // ...and whether it's online.
    netWasUp = true;
    Serial.printf("boxchat connected (Wi-Fi IP %s)\n", WiFi.localIP().toString().c_str());
  }
} // Completes keeping Wi-Fi + MQTT up without ever blocking the panel for long.
