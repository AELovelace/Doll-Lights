// screens.ino: everything the console shows. The Uno LCD (with the NAV knob's menu), the CYD face,
// the dot-matrix level meter and the Mega's heartbeat LED.

// --- Shared bits ----------------------------------------------------------------------------------

const char *zoneName(const Light &light, uint8_t zone) {
  if (light.zones < 2) return "--";            // Single-zone lights have nothing to choose.
  return zone == 0 ? "BOTH" : zone == 1 ? "LEFT" : "RIGHT";
} // Completes the zone label for screens.

bool blinkPhase() {
  return (millis() / 400) % 2;                 // Shared blink rhythm for every screen.
} // Completes the "should blinking things show right now" check.

int knobStep(int clicks, unsigned long &lastAt) {
  unsigned long now = millis();
  int step = (now - lastAt < 60) ? 16 : 4;     // Spinning fast takes big steps, slow turns are fine.
  lastAt = now;
  return clicks * step;
} // Completes turning knob clicks into a level change, with acceleration.

String hexColour(const Rgbw &c) {
  char text[9];
  snprintf(text, sizeof(text), "%02X%02X%02X%02X", c.v[0], c.v[1], c.v[2], c.v[3]);
  return String(text);
} // Completes RRGGBBWW for the CYD.

// --- Uno LCD + NAV knob menu ------------------------------------------------------------------------

enum NavItem : uint8_t { NAV_LIGHT, NAV_ZONE, NAV_CHAN, NAV_LEVEL, NAV_SCENE, NAV_POWER, NAV_MATRIX, NAV_ALLOFF, NAV_COUNT };
const char *const NAV_NAMES[NAV_COUNT] = {"LIGHT", "ZONE", "CHAN", "LEVEL", "SCENE", "POWER", "MATRIX", "ALL OFF"};
const unsigned long NAV_EDIT_TIMEOUT_MS = 10000;  // An item left alone this long stops editing.

uint8_t navItem = NAV_LIGHT;                   // Menu item shown on the LCD's top row.
bool navEditing = false;                       // false: NAV turns move between items; true: change the value.
uint8_t navScene = 0;                          // Scene picked in the SCENE item (applied on push).
unsigned long navLastUse = 0;
unsigned long navLastClick = 0;                // For LEVEL acceleration.

String lcdRow[2];                              // What each LCD row should show.
bool lcdDirty[2];
unsigned long lcdLastSend = 0, lcdLastRefresh = 0;
const unsigned long LCD_MIN_GAP_MS = 60, LCD_REFRESH_MS = 5000;

int wrapAdd(int value, int delta, int count) {
  return ((value + delta) % count + count) % count;
} // Completes stepping round a list in either direction.

void unoSend(const String &line) {
  mega.sendLine("U " + line);                  // The Mega strips "U " and passes it to the Uno.
} // Completes sending one line to the Uno.

void lcdShow(int row, const String &text) {
  if (lcdRow[row] == text) return;
  lcdRow[row] = text;
  lcdDirty[row] = true;
} // Completes queueing an LCD row change.

void lcdSendAll() {
  for (int row = 0; row < 2; row++) unoSend(String("T") + row + ":" + lcdRow[row]);
  lcdDirty[0] = lcdDirty[1] = false;
  lcdLastSend = lcdLastRefresh = millis();
} // Completes a full LCD redraw (Uno restarted or appeared, and every 5 s to heal garbled lines).

String navValue() {
  const Light &light = lights[selLight];
  switch (navItem) {
    case NAV_LIGHT: return light.shortName;
    case NAV_ZONE: return zoneName(light, selZone);
    case NAV_CHAN: return CHANNEL_NAMES[selChannel];
    case NAV_LEVEL: return String(selectedLevel(selChannel));
    case NAV_SCENE: return SCENES[navScene].name;
    case NAV_POWER:
      if (light.relayId) return light.relayOn == 1 ? "ON" : light.relayOn == 0 ? "OFF" : "?";
      return selectedLit() ? "ON" : "OFF";
    case NAV_MATRIX: return String(matrixBrightness);
    default: return navEditing ? "PUSH!" : "";
  }
} // Completes the value shown next to the current menu item.

void drawLcd() {
  if (keyEntry.length()) {                     // Keypad typing takes over the top row.
    String name = String("SET ") + CHANNEL_NAMES[selChannel];
    while (name.length() < 8) name += ' ';
    lcdShow(0, "#" + name + keyEntry + "_");   // e.g. "#SET R   128_": press # to apply.
  } else {
    String name = NAV_NAMES[navItem];
    while (name.length() < 8) name += ' ';     // Values line up in column 9.
    lcdShow(0, String(navEditing ? '*' : '>') + name + navValue());  // '>' browsing, '*' editing.
  }

  if (!netUp() && blinkPhase()) {
    lcdShow(1, "NO BOXCHAT LINK");             // Alternates with the levels while offline.
    return;
  }
  const Light &light = lights[selLight];
  const Rgbw &c = light.zone[zoneFirst(light)];
  const char letters[4] = {'R', 'G', 'B', 'W'};
  String row;
  for (int i = 0; i < 4; i++) {
    bool hide = (selChannel == CH_R + i) && blinkPhase();  // The knob's channel blinks.
    char level[4];
    snprintf(level, sizeof(level), "%03u", c.v[i]);
    row += hide ? ' ' : letters[i];
    row += level;
  }
  lcdShow(1, row);                             // e.g. "R255G060B000W000" (exactly 16).
} // Completes the LCD: menu on top, the selection's levels underneath.

void lcdService() {
  if (navEditing && millis() - navLastUse > NAV_EDIT_TIMEOUT_MS) navEditing = false;
  drawLcd();
  if (millis() - lcdLastRefresh >= LCD_REFRESH_MS) { lcdSendAll(); return; }
  if (millis() - lcdLastSend < LCD_MIN_GAP_MS) return;
  for (int row = 0; row < 2; row++) {
    if (!lcdDirty[row]) continue;
    unoSend(String("T") + row + ":" + lcdRow[row]);
    lcdDirty[row] = false;
    lcdLastSend = millis();
  }
} // Completes keeping the Uno LCD up to date without flooding the links.

void navTurn(int clicks) {
  navLastUse = millis();
  if (!navEditing) {                           // Browsing: move between menu items.
    navItem = wrapAdd(navItem, clicks, NAV_COUNT);
    return;
  }
  Light &light = selected();
  switch (navItem) {
    case NAV_LIGHT: selLight = wrapAdd(selLight, clicks, LIGHT_COUNT); selZone = 0; break;
    case NAV_ZONE: if (light.zones > 1) selZone = wrapAdd(selZone, clicks, 3); break;
    case NAV_CHAN: selChannel = wrapAdd(selChannel, clicks, CH_COUNT); break;
    case NAV_LEVEL: adjustSelected(selChannel, knobStep(clicks, navLastClick)); break;
    case NAV_SCENE: navScene = wrapAdd(navScene, clicks, SCENE_COUNT); break;
    case NAV_POWER:
      if (light.relayId) setRelay(light, clicks > 0);   // Clockwise = on, anticlockwise = off.
      else if (clicks > 0) selectedOn(); else selectedOff();
      break;
    case NAV_MATRIX:
      matrixBrightness = constrain(matrixBrightness + clicks, 0, 15);
      mega.sendLine("MXBRI " + String(matrixBrightness));
      break;
    default: break;                            // ALL OFF: turning does nothing (push confirms).
  }
} // Completes one NAV knob turn: move through the menu, or change the item being edited.

void navPush() {
  navLastUse = millis();
  if (!navEditing) {
    navEditing = true;                         // Start editing this item.
    return;
  }
  if (navItem == NAV_SCENE) { colourSelected(SCENES[navScene].colour); cydToast(SCENES[navScene].name); }
  if (navItem == NAV_ALLOFF) { allOff(); cydToast("ALL OFF"); }
  navEditing = false;                          // Second push: done (and apply, for SCENE / ALL OFF).
} // Completes a NAV knob push: enter an item, or confirm and leave it.

// --- CYD face -----------------------------------------------------------------------------------------

String cydSent[LIGHT_COUNT + 2];               // Last tile lines, then selection and links ("" = resend).

void cydSendIfChanged(int slot, const String &line) {
  if (cydSent[slot] == line) return;
  cyd.sendLine(line);
  cydSent[slot] = line;
} // Completes sending one CYD line when it changed.

void cydToast(const String &text) {
  cyd.sendLine("MSG " + text);                 // A big message that pops up for a moment.
} // Completes a CYD pop-up (scenes, ALL OFF, ...).

void cydService() {
  for (uint8_t i = 0; i < LIGHT_COUNT; i++) {  // One tile per light: online, relay, zone colours.
    const Light &light = lights[i];
    String line = "T " + String(i) + " " + (light.online ? 1 : 0) + " " + (light.relayId ? light.relayOn : 9)
                + " " + light.zones;
    for (uint8_t z = 0; z < light.zones; z++) line += " " + hexColour(light.zone[z]);
    cydSendIfChanged(i, line);                 // relay 9 = this light has no relay.
  }
  cydSendIfChanged(LIGHT_COUNT, "S " + String(selLight) + " " + selZone + " " + selChannel);
  cydSendIfChanged(LIGHT_COUNT + 1, String("K ") + (netUp() ? 1 : 0) + " " + (mega.alive() ? 1 : 0) + " " + (unoAlive ? 1 : 0));
} // Completes the CYD update: only what changed goes out.

void cydResendAll() {
  for (uint8_t i = 0; i < LIGHT_COUNT; i++) cyd.sendLine("TN " + String(i) + " " + lights[i].name);
  for (int slot = 0; slot < LIGHT_COUNT + 2; slot++) cydSent[slot] = "";
  cydService();
} // Completes a full CYD refresh: names, tiles, selection, links.

void handleCydLine(const String &line) {
  if (line == "HI") { cydResendAll(); return; }  // The CYD just (re)started.
  if (!line.startsWith("TAP ")) return;        // "PO" only needs the keep-alive.
  char what = line[4];
  int a = line.substring(6).toInt();
  int secondSpace = line.indexOf(' ', 6);
  int b = secondSpace > 0 ? line.substring(secondSpace + 1).toInt() : 0;
  switch (what) {
    case 'T': if (a >= 0 && a < LIGHT_COUNT) { selLight = a; selZone = 0; } break;  // Tile: select light.
    case 'Z': if (selected().zones > 1 && a >= 0 && a <= 2) selZone = a; break;    // Zone tab.
    case 'C': if (a >= 0 && a < CH_COUNT) selChannel = a; break;                   // Channel bar: knob target.
    case 'L': if (a >= 0 && a < CH_COUNT) { selChannel = a; setSelectedLevel(a, b); } break;  // Bar drag: level.
    case 'P':                                  // Power badge.
      if (a >= 0 && a < LIGHT_COUNT) {
        if (lights[a].relayId) toggleRelay(lights[a]);
        else { selLight = a; selZone = 0; toggleSelected(); }
      }
      break;
  }
} // Completes one touch from the CYD.

// --- Dot matrix: level meter for the selection ----------------------------------------------------------

uint16_t matrixFrame[8];                       // 16x8 picture: row 0 = top, bit 15 = leftmost pixel.
unsigned long matrixLastSend = 0;
const unsigned long MATRIX_MIN_GAP_MS = 100;

void matrixService() {
  const Light &light = lights[selLight];
  const Rgbw &c = light.zone[zoneFirst(light)];
  uint16_t frame[8] = {0};
  for (int bar = 0; bar < 4; bar++) {                       // R, G, B, W top to bottom (same order as the CYD).
    uint8_t channel = CH_R + bar;
    bool blink = (selChannel == channel || selChannel == CH_BRI) && blinkPhase();  // BRI scales them all: all blink.
    if (blink) continue;
    int length = (c.v[bar] * 16 + 254) / 255;               // 0-16 pixels, any light at all shows 1.
    uint16_t pixels = length ? (uint16_t)(0xFFFF << (16 - length)) : 0;  // Filled from the left (bit 15).
    frame[bar * 2] = frame[bar * 2 + 1] = pixels;           // Each bar is 2 rows tall.
  }
  if (memcmp(frame, matrixFrame, sizeof(frame))) { memcpy(matrixFrame, frame, sizeof(frame)); matrixDirty = true; }
  if (!matrixDirty || millis() - matrixLastSend < MATRIX_MIN_GAP_MS) return;
  String hex;
  char digits[5];
  for (int y = 0; y < 8; y++) { snprintf(digits, sizeof(digits), "%04X", matrixFrame[y]); hex += digits; }
  mega.sendLine("MXALL " + hex);
  matrixDirty = false;
  matrixLastSend = millis();
} // Completes the meter: R, G, B, W bars filling left to right, the knob's channel blinking (BRI = all blink).

// --- Mega LED: network heartbeat ----------------------------------------------------------------------

void heartbeatService() {
  int want = netUp() ? 1 : (blinkPhase() ? 1 : 0);  // Solid on boxchat, blinking when offline.
  if (want == megaLedSent) return;
  mega.sendLine("DW " + String(MEGA_LED_PIN) + " " + want);
  megaLedSent = want;
} // Completes the heartbeat on the Mega's onboard LED.
