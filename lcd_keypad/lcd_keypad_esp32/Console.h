// Console.h: everything the master console's tabs share (lights, selection, menu state).
// Arduino joins all the .ino tabs into one file after the main tab, so shared types and
// variables live here, included once at the top of lcd_keypad_esp32.ino.

#pragma once

#include <Arduino.h>
#include <WiFi.h>                              // Used in lights.ino; included here so every tab sees the types.
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "TextLink.h"

// --- boxchat network --------------------------------------------------------------------------
const char *const MQTT_HOST = "192.168.1.253";
const uint16_t MQTT_PORT = 1883;
const uint16_t CONSOLE_ID = 32008;             // This console's boxchat number (NAS registry, 2026-11-22).
const char *const BASE_TOPIC = "boxchat";

// --- Channels ---------------------------------------------------------------------------------
// The knobs work on one channel at a time. BRI scales the whole colour; R/G/B/W set one level.
enum Channel : uint8_t { CH_BRI = 0, CH_R, CH_G, CH_B, CH_W, CH_COUNT };
const char *const CHANNEL_NAMES[CH_COUNT] = {"BRI", "R", "G", "B", "W"};

struct Rgbw {
  uint8_t v[4];                                // Red, green, blue, white (0-255 each).
};

// --- Scenes (keypad A-D and the NAV menu): the same colours as Light Console's presets -------
struct Scene {
  const char *name;
  Rgbw colour;
};
const uint8_t SCENE_COUNT = 4;
const Scene SCENES[SCENE_COUNT] = {
  {"WHITE", {{0, 0, 0, 220}}},
  {"SUNSET", {{255, 72, 12, 0}}},
  {"VIOLET", {{166, 0, 255, 0}}},
  {"AQUA", {{0, 180, 255, 0}}},
};
const Rgbw DEFAULT_ON = {{0, 0, 0, 220}};      // "On" for a light we never saw lit: white.

// --- Lights -----------------------------------------------------------------------------------
// The same boards as Light Console's config.json. A light's relay is its master power supply.
struct Light {
  uint16_t id;                                 // boxchat activator number.
  const char *name;                            // Full name (CYD tiles).
  const char *shortName;                       // Up to 5 letters (Uno LCD).
  uint8_t zones;                               // 1 or 2 (Living Room: 1 = Left, 2 = Right).
  uint16_t relayId;                            // Master power relay, or 0 for none.
  bool relayShared;                            // 32002 only understands the shared "boxchat" topic.
  // Live state (filled from boxchat/<id>/state and our own commands); the defaults are the start-up state:
  Rgbw zone[2] = {};                           // What each zone shows (or will, once our commands land).
  Rgbw hue[2] = {};                            // Each zone's colour at full brightness (BRI scales this).
  bool known = false;                          // A state message has arrived (or we set it ourselves).
  bool online = false;                         // From boxchat/<id>/status.
  int8_t relayOn = -1;                         // Relay: 1 on, 0 off, -1 unknown (relays report nothing).
  unsigned long holdUntil = 0;                 // Ignore reported state until then: our change is in flight.
  bool dirty[2] = {};                          // Zone changed locally but not published yet.
  Rgbw sent[2] = {};                           // What we last published per zone.
  unsigned long lastPublish = 0;
};

const uint8_t LIGHT_COUNT = 4;
Light lights[LIGHT_COUNT] = {                  // Keypad 1-4 / CYD tiles, in this order.
  {32001, "Doll-Office", "OFFIC", 1, 32002, true},
  {32003, "Mnemerynne", "MNEMO", 1, 0, false},
  {32004, "Living Room", "LIVRM", 2, 32007, false},
  {32005, "Cync Strip", "CYNC", 1, 0, false},
};

// --- Selection: what the knobs, keypad and buttons act on --------------------------------------
uint8_t selLight = 0;                          // Index into lights[].
uint8_t selZone = 0;                           // 0 = every zone together, 1 = Left/zone 1, 2 = Right/zone 2.
uint8_t selChannel = CH_BRI;                   // What MAIN (and NAV's LEVEL item) adjusts.

// --- Keypad number entry: digits typed so far, applied to selChannel with # --------------------------
String keyEntry = "";                          // "" = not typing; otherwise up to 3 digits, e.g. "128".
unsigned long keyEntryAt = 0;                  // Last digit's time (the entry is dropped after a pause).
const unsigned long KEY_ENTRY_TIMEOUT_MS = 6000;
const uint8_t KEY_ENTRY_DIGITS = 3;            // Levels are 0-255.

// --- Links (TextLink.h) -------------------------------------------------------------------------
TextLink cyd("CYD", 2, 32, 33, 115200);        // UART2: RX GPIO32 (CYD 19), TX GPIO33 (CYD 18).
TextLink mega("Mega", 1, 5, 15, 19200);        // UART1: RX GPIO5 (Mega D2), TX GPIO15 (Mega pin 19).
bool unoAlive = false;                         // The Uno is reached through the Mega ("U ..." lines).

// --- Display state the Mega setup needs (used in screens.ino) -----------------------------------
int matrixBrightness = 2;                      // Dot matrix 0-15 (NAV menu MATRIX item).
bool matrixDirty = true;                       // Meter picture must be (re)sent to the Mega.
int megaLedSent = -1;                          // Heartbeat level last sent to the Mega (-1 = resend).

// --- Pins the master itself uses (Mega pins are in mega_accessories) ---------------------------
const int MEGA_LED_PIN = 13;                   // Mega's onboard LED: network heartbeat.
const int MEGA_PANIC_PIN = 22;                 // Button from Mega pin 22 to GND: ALL OFF.
const uint8_t NAV_KNOB = 0;                    // Mega knob 0 (A8/A9, push on 24): Uno menu.
