// Master Light Console (boxchat controller 32008): the ESP32 brain.
//
// It joins Wi-Fi + the boxchat MQTT broker (lights.ino), follows every light's live state, and gives
// every part of the panel a job:
//
//   Part                  Job
//   MAIN knob (here)      turn = adjust the selected channel (BRI/R/G/B/W), push = next channel
//   4x4 keypad (here)     digits type a level 0-255, # = apply it to the selected channel
//                         (nothing typed: room relay ON), * = cancel typing (nothing typed:
//                         room relay OFF), A-C scenes (A = white), D = ALL OFF (relays too)
//   CYD face              touch tiles (pick a light), zone tabs, channel bars (tap/drag = level),
//                         power badges (relay); its RGB LED glows the selected colour
//   Uno LCD + 5 buttons   NAV menu on top, the selection's R/G/B/W levels underneath;
//                         UP/DOWN = channel, LEFT/RIGHT = light, SELECT = next zone
//   NAV knob (Mega)       turn = move through the LCD menu / change the item, push = enter / confirm
//   Dot matrix (Mega)     level meter: R, G, B, W bars filling left to right; the knob's channel blinks
//   Mega LED (pin 13)     network heartbeat: solid = on boxchat, blinking = offline
//   Mega pin 22           panic ALL OFF (button to GND)
//
// Boards and wiring: ../README.md. Tabs: Console.h (shared state), lights.ino (boxchat), screens.ino
// (LCD/CYD/matrix/LED), keypad4x4.ino, encoder.ino, TextLink.h (serial links). Wi-Fi: secrets.h.
// Board: ESP32 Dev Module. Libraries: PubSubClient, ArduinoJson 7.

#include "Console.h"

unsigned long mainLastClick = 0;               // For MAIN knob acceleration.
unsigned long unoLastPing = 0, unoLastHeard = 0;
const unsigned long UNO_PING_MS = 5000, UNO_TIMEOUT_MS = 12000;

// --- The Uno (reached through the Mega as "U <line>") -----------------------------------------------

void unoHeard() {
  unoLastHeard = millis();
  if (!unoAlive) {
    unoAlive = true;
    Serial.println("Uno link up (via Mega)");
    lcdSendAll();                              // It may have missed everything while it was away.
  }
} // Completes marking the Uno as present.

void unoService() {
  unsigned long now = millis();
  if (now - unoLastPing >= UNO_PING_MS) { unoSend("PING"); unoLastPing = now; }
  if (unoAlive && now - unoLastHeard > UNO_TIMEOUT_MS) {
    unoAlive = false;
    Serial.println("Uno link lost (check the Uno <-> Mega wires and that the Uno is powered)");
  }
} // Completes the Uno keep-alive.

void onShieldButton(char key) {
  switch (key) {
    case 'U': selChannel = (selChannel + CH_COUNT - 1) % CH_COUNT; break;                 // Up the CYD bars (toward BRI).
    case 'D': selChannel = (selChannel + 1) % CH_COUNT; break;                            // Down the bars (toward W).
    case 'L': selLight = (selLight + LIGHT_COUNT - 1) % LIGHT_COUNT; selZone = 0; break;  // Tile to the left.
    case 'R': selLight = (selLight + 1) % LIGHT_COUNT; selZone = 0; break;                // Tile to the right.
    case 'S':                                                                             // Next zone.
      if (selected().zones > 1) selZone = (selZone + 1) % 3;                              // BOTH -> LEFT -> RIGHT -> BOTH.
      cydToast(selected().zones > 1 ? zoneName(selected(), selZone) : "ONE ZONE");        // One-zone lights just say so.
      break;
  }
} // Completes one LCD-shield button: UP/DOWN channel (like the CYD's bar rows), LEFT/RIGHT light (like its tiles), SELECT next zone.

void handleUnoLine(const String &line) {
  unoHeard();
  if (line == "HI") lcdSendAll();              // The Uno restarted with a blank screen.
  else if (line.length() == 2 && line[0] == 'K') onShieldButton(line[1]);  // "KU" "KD" "KL" "KR" "KS".
} // Completes one message from the Uno ("PO" only needs unoHeard()).

// --- The Mega ----------------------------------------------------------------------------------------

void megaSetup() {
  mega.sendLine("RESET");                      // Clean slate: plain inputs, matrix blank.
  mega.sendLine("MODE " + String(MEGA_LED_PIN) + " OUT");
  mega.sendLine("MODE " + String(MEGA_PANIC_PIN) + " PULLUP");  // Panic button to GND reads 0 when pressed.
  mega.sendLine("WATCH " + String(MEGA_PANIC_PIN));
  mega.sendLine("MXBRI " + String(matrixBrightness));
  megaLedSent = -1;                            // Resend the heartbeat...
  matrixDirty = true;                          // ...and the meter.
} // Completes the Mega's configuration; runs whenever it appears or restarts.

void handleMegaLine(const String &line) {
  if (line.startsWith("U ")) { handleUnoLine(line.substring(2)); return; }  // Relayed from the Uno.
  int s1 = line.indexOf(' '), s2 = line.indexOf(' ', s1 + 1);
  String kind = s1 < 0 ? line : line.substring(0, s1);
  long a = s1 < 0 ? 0 : line.substring(s1 + 1, s2 < 0 ? line.length() : s2).toInt();
  long b = s2 < 0 ? 0 : line.substring(s2 + 1).toInt();
  if (kind == "HI") megaSetup();               // The Mega restarted.
  else if (kind == "KN" && a == NAV_KNOB) navTurn(b);           // NAV knob turned b clicks.
  else if (kind == "KP" && a == NAV_KNOB) navPush();            // NAV knob pushed.
  else if (kind == "IN" && a == MEGA_PANIC_PIN && b == 0) { allOff(); cydToast("ALL OFF"); }  // Panic!
  else if (kind == "ERR") Serial.println("Mega error: " + line.substring(4));
} // Completes one message from the Mega ("PO" only needs the keep-alive).

// --- Inputs on the master itself ------------------------------------------------------------------------

void keyEntryToast() {
  cydToast(String(CHANNEL_NAMES[selChannel]) + " " + keyEntry + "_");  // e.g. "R 12_" while typing.
} // Completes showing the number being typed on the CYD.

void onKeypadKey(char key) {
  if (key >= '0' && key <= '9') {                                             // Type a level.
    if (keyEntry.length() < KEY_ENTRY_DIGITS) keyEntry += key;                // A 4th digit is ignored.
    keyEntryAt = millis();
    keyEntryToast();
  }
  else if (key == '#') {
    if (keyEntry.length()) {                                                  // Enter: apply the typed level.
      int level = constrain(keyEntry.toInt(), 0, 255);                       // "300" becomes 255.
      setSelectedLevel(selChannel, level);                                    // BRI scales; R/G/B/W set one level.
      cydToast(String(CHANNEL_NAMES[selChannel]) + " " + level);
      keyEntry = "";
    }
    else keypadPower(true);                    // Nothing typed: the room's relay on.
  }
  else if (key == '*') {
    if (keyEntry.length()) { keyEntry = ""; cydToast("CANCEL"); }             // Typing: throw it away.
    else keypadPower(false);                                                  // Not typing: the room's relay off.
  }
  else if (key >= 'A' && key <= 'C') {                                         // Scenes: A WHITE, B SUNSET, C VIOLET.
    const Scene &scene = SCENES[key - 'A'];
    colourSelected(scene.colour);
    cydToast(scene.name);
  }
  else if (key == 'D') { allOff(); cydToast("ALL OFF"); }                     // ALL STOP: every light and relay off.
  Serial.printf("keypad %c -> light %u zone %u channel %s entry '%s'\n", key, selLight + 1, selZone,
                CHANNEL_NAMES[selChannel], keyEntry.c_str());
} // Completes one keypad key: digits type a level, # applies it (or relay on), * cancels (or relay off),
  // A-C scenes, D ALL OFF.

void keypadPower(bool on) {
  Light &light = selected();
  if (light.relayId) setRelay(light, on);      // The room's master power relay (32002 / 32007).
  else if (on) selectedOn();                   // No relay (Mnemerynne, Cync): switch the light itself.
  else selectedOff();
  cydToast(String(light.shortName) + (on ? " ON" : " OFF"));
} // Completes # / * with nothing typed: the selected room's relay on or off (or the light, if it has no relay).

void keyEntryService() {
  if (keyEntry.length() && millis() - keyEntryAt > KEY_ENTRY_TIMEOUT_MS) keyEntry = "";  // Forgotten entry.
} // Completes dropping a half-typed number after a pause.

// --- Arduino entry points ----------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);                        // USB Serial Monitor.
  cyd.begin();
  mega.begin();
  keypadBegin();                               // keypad4x4.ino
  encoderBegin();                              // encoder.ino (MAIN knob)
  networkBegin();                              // lights.ino: Wi-Fi + MQTT
  delay(100);
  megaSetup();                                 // In case the Mega is already running (it won't say HI)...
  drawLcd();
  lcdSendAll();                                // ...same for the Uno behind it...
  cydResendAll();                              // ...and the CYD.
  Serial.println("Master Light Console (32008) started");
} // Completes start-up: links, inputs, network, and every screen's first picture.

void loop() {
  String line;
  while (mega.readLine(line)) handleMegaLine(line);  // NAV knob, panic button, the Uno (relayed).
  while (cyd.readLine(line)) handleCydLine(line);    // Touches.
  if (mega.takeCameUp()) megaSetup();
  if (cyd.takeCameUp()) cydResendAll();
  mega.service();                              // Keep-alive pings for every link.
  cyd.service();
  unoService();
  networkService();                            // boxchat: receive states, publish changes.

  char key = keypadRead();                     // 4x4 keypad (0 = no new press).
  if (key) onKeypadKey(key);
  keyEntryService();                           // Drop a half-typed number after a pause.
  int clicks = encoderTakeSteps();             // MAIN knob clicks since last time (+ clockwise).
  if (clicks) adjustSelected(selChannel, knobStep(clicks, mainLastClick));
  if (encoderButtonPressed()) selChannel = (selChannel + 1) % CH_COUNT;  // MAIN push: next channel.

  lcdService();                                // Bring every display up to date.
  cydService();
  matrixService();
  heartbeatService();
} // Completes one pass: read every input, talk to boxchat, refresh every display.
