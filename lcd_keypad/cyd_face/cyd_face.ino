// CYD face: the Cheap Yellow Display (ESP32-2432S028, 2.8" 320x240 touch) is the master light
// console's main screen. The master ESP32 owns all the state; the CYD draws it and reports touches.
//
// Wiring (3.3 V to 3.3 V, so a plain serial link, no tricks), on the CYD's header pins:
//   CYD 19 (TX) -> master GPIO32,   master GPIO33 -> CYD 18 (RX),   GND -- GND.
// 18/19 are shared with the microSD slot, so leave the slot empty. Don't use RX0/TX0 (USB serial)
// or 17 (the RGB LED's blue on most CYDs). Power the CYD from its own USB.
//
// Screen (landscape):
//   title bar   DOLL CONSOLE + link chips NET / MEGA / UNO
//   4 tiles     one per light: name, live colour (split per zone), online dot, power badge, key number
//               tap a tile = select that light; tap its badge = switch its master relay (or on/off)
//   zone tabs   LEFT / BOTH / RIGHT (Living Room only)
//   5 bars      BRI R G B W of the selection; tap = give that channel to the knobs, drag = set it
//   pop-ups     scene names, ALL OFF
//   RGB LED     (on the back) glows the selected light's colour
//
// Link: 115200 baud 8N1, text lines ending in '\n'.
//   master -> CYD                                         CYD -> master
//   TN <i> <name>                light i's name           HI                (just started)
//   T <i> <online> <relay> <zones> <RRGGBBWW>...          PO                (answer to PING)
//          relay: 1 on, 0 off, -1 unknown, 9 none         TAP T <i>         tile i
//   S <light> <zone 0-2> <channel 0-4>  selection         TAP P <i>         tile i's power badge
//   K <net> <mega> <uno>         link status (1 = up)     TAP Z <zone>      zone tab (0 BOTH, 1 L, 2 R)
//   MSG <text>                   pop-up message           TAP C <ch>        channel bar (0 BRI ... 4 W)
//   PING                                                  TAP L <ch> <0-255>  bar tapped/dragged to a level
//
// Board: ESP32 Dev Module. Library: LovyanGFX (auto-detects the CYD's ILI9341 or ST7789 and its touch).

#define LGFX_AUTODETECT                        // Let LovyanGFX recognise the CYD, its panel and touch chip.
#include <LovyanGFX.hpp>

static LGFX tft;                               // The physical screen (+ touch).
static LGFX_Sprite canvas(&tft);               // Off-screen frame: draw everything, then push once (no flicker).
LovyanGFX *gfx = &tft;                         // Where drawing goes: the canvas, or the screen if RAM ran out.

HardwareSerial master(2);                      // UART2, routed to two free header pins.
const int LINK_RX_PIN = 18;                    // From master GPIO33.
const int LINK_TX_PIN = 19;                    // To master GPIO32.
const unsigned long LINK_BAUD = 115200;
const unsigned long MASTER_TIMEOUT_MS = 12000; // The master pings every 5 s; this much silence = gone.
const size_t MAX_LINE = 80;

const int LED_RED = 4, LED_GREEN = 16, LED_BLUE = 17;  // CYD's RGB LED on the back (active LOW).
const unsigned long FRAME_MS = 33;             // Redraw at most ~30 times a second.
const unsigned long TOAST_MS = 1300;           // Pop-up messages stay this long.
const unsigned long DRAG_GAP_MS = 60;          // While dragging a bar, send a level at most this often.

// Sakura-CRT palette (same as the Light Console web app). uint32_t = 0xRRGGBB for LovyanGFX.
const uint32_t C_BLACK = 0x000000, C_PINK = 0xFF1493, C_HOT = 0xFF69B4, C_PURPLE = 0x8E1698,
               C_GREEN = 0x74FFB5, C_RED = 0xFF476A, C_WHITE = 0xFFF8FB, C_GREY = 0x5A4A58,
               C_SCAN = 0x12060F, C_BAR = 0x1E0A1C, C_TRACK = 0x2A0F2A;
const uint32_t CHANNEL_COLOURS[5] = {C_HOT, 0xFF5E72, 0x4FE39A, 0x5897FF, 0xFFF1CD};  // BRI R G B W.
const char *const CHANNEL_LABELS[5] = {"BRI", "R", "G", "B", "W"};

// --- Layout (pixels) -------------------------------------------------------------------------------
const int TILE_Y = 31, TILE_H = 70, TILE_W = 76, TILE_GAP = 3, TILE_X0 = 4;
const int BADGE_W = 24, BADGE_H = 18;          // Power badge in each tile's bottom-right corner.
const int ZONE_Y = 105, ZONE_H = 20;           // Tabs: LEFT | BOTH | RIGHT across the width.
const int BARS_Y = 129, BAR_ROW_H = 21, BAR_X = 44, BAR_W = 230;  // 5 rows: label, bar, value.

// --- State (everything comes from the master) ---------------------------------------------------------
struct Tile {
  String name = "...";
  bool online = false;
  int relay = 9;                               // 1 on, 0 off, -1 unknown, 9 none.
  int zones = 1;
  uint8_t rgbw[2][4] = {};
};
Tile tiles[4];
int selLight = 0, selZone = 0, selChannel = 0;
bool netUp = false, megaUp = false, unoUp = false;
String toast;
unsigned long toastAt = 0;
String incoming;                               // Line being assembled from the master.
unsigned long lastHeard = 0;                   // When the master last sent anything (0 = never).
unsigned long lastFrame = 0;
bool dirty = true;                             // Something changed: redraw at the next frame.

// --- Helpers --------------------------------------------------------------------------------------------

bool masterAlive() {
  return lastHeard != 0 && millis() - lastHeard < MASTER_TIMEOUT_MS;
} // Completes the "is the master talking to us" check.

const uint8_t *selectedLevels() {
  const Tile &tile = tiles[selLight];
  int zone = (selZone == 0 || selZone > tile.zones) ? 0 : selZone - 1;
  return tile.rgbw[zone];
} // Completes the R/G/B/W of the selection (its first zone when BOTH).

uint8_t channelLevel(int channel) {
  const uint8_t *v = selectedLevels();
  return channel == 0 ? max(max(v[0], v[1]), max(v[2], v[3])) : v[channel - 1];
} // Completes one bar's level (BRI = the biggest channel).

uint32_t screenColour(const uint8_t *v) {
  int r = min(255, v[0] + v[3]), g = min(255, v[1] + v[3]), b = min(255, v[2] + v[3]);
  return ((uint32_t)r << 16) | (g << 8) | b;
} // Completes RGBW -> an on-screen colour (white brightens all three, like the web app).

bool parseHex(const String &text, uint8_t out[4]) {
  if (text.length() < 8) return false;
  for (int i = 0; i < 4; i++) {
    char *end;
    String part = text.substring(i * 2, i * 2 + 2);
    long value = strtol(part.c_str(), &end, 16);
    if (*end) return false;
    out[i] = value;
  }
  return true;
} // Completes RRGGBBWW -> four levels.

void ledShowSelection() {
  const uint8_t *v = selectedLevels();
  int channels[3] = {min(255, v[0] + v[3]), min(255, v[1] + v[3]), min(255, v[2] + v[3])};
  const int pins[3] = {LED_RED, LED_GREEN, LED_BLUE};
  for (int i = 0; i < 3; i++) analogWrite(pins[i], 255 - channels[i]);  // Active LOW: 255 = off.
} // Completes the back LED glowing the selected light's colour.

// --- Messages from the master ---------------------------------------------------------------------------

String field(const String &line, int index) {
  int start = 0;
  for (int i = 0; i < index; i++) {
    start = line.indexOf(' ', start);
    if (start < 0) return "";
    start++;
  }
  int end = line.indexOf(' ', start);
  return end < 0 ? line.substring(start) : line.substring(start, end);
} // Completes picking the index-th space-separated word of a line.

void handleLine(const String &line) {
  lastHeard = millis();
  String kind = field(line, 0);
  if (kind == "PING") {
    master.print("PO\n");
    return;
  } else if (kind == "TN") {
    int i = field(line, 1).toInt();
    int nameStart = line.indexOf(' ', 3);
    if (i >= 0 && i < 4 && nameStart > 0) tiles[i].name = line.substring(nameStart + 1);
  } else if (kind == "T") {
    int i = field(line, 1).toInt();
    if (i < 0 || i > 3) return;
    Tile &tile = tiles[i];
    tile.online = field(line, 2) == "1";
    tile.relay = field(line, 3).toInt();
    tile.zones = constrain(field(line, 4).toInt(), 1, 2);
    for (int z = 0; z < tile.zones; z++) parseHex(field(line, 5 + z), tile.rgbw[z]);
  } else if (kind == "S") {
    selLight = constrain(field(line, 1).toInt(), 0, 3);
    selZone = constrain(field(line, 2).toInt(), 0, 2);
    selChannel = constrain(field(line, 3).toInt(), 0, 4);
  } else if (kind == "K") {
    netUp = field(line, 1) == "1";
    megaUp = field(line, 2) == "1";
    unoUp = field(line, 3) == "1";
  } else if (kind == "MSG") {
    toast = line.substring(4);
    toastAt = millis();
  } else {
    return;                                    // Unknown line: ignore.
  }
  ledShowSelection();
  dirty = true;
} // Completes one update from the master.

void readMaster() {
  while (master.available()) {
    char c = (char)master.read();
    if (c == '\r') continue;
    if (c == '\n') {
      if (incoming.length()) handleLine(incoming);
      incoming = "";
    } else if (incoming.length() < MAX_LINE) {
      incoming += c;
    }
  }
} // Completes gathering the master's characters into lines.

// --- Touch ------------------------------------------------------------------------------------------------

bool touchWasDown = false;
int dragChannel = -1;                          // Bar being dragged, or -1.
int dragLastLevel = -1;
unsigned long dragLastSend = 0;

void sendTap(const String &what) {
  master.print("TAP " + what + "\n");
} // Completes reporting one touch to the master.

void touchService() {
  lgfx::touch_point_t point;
  bool down = tft.getTouch(&point) > 0;
  if (!down) {                                 // Finger lifted: end any drag.
    touchWasDown = false;
    dragChannel = -1;
    return;
  }
  int x = point.x, y = point.y;
  bool pressStart = !touchWasDown;
  touchWasDown = true;

  if (dragChannel >= 0) {                      // Dragging along a bar: level follows the finger.
    int level = constrain((x - BAR_X) * 255 / BAR_W, 0, 255);
    if (level != dragLastLevel && millis() - dragLastSend >= DRAG_GAP_MS) {
      sendTap("L " + String(dragChannel) + " " + level);
      dragLastLevel = level;
      dragLastSend = millis();
    }
    return;
  }
  if (!pressStart) return;                     // Everything else reacts once per touch.

  if (y >= TILE_Y && y < TILE_Y + TILE_H) {    // A light tile (or its power badge).
    int i = (x - TILE_X0) / (TILE_W + TILE_GAP);
    if (i < 0 || i > 3) return;
    int tileX = TILE_X0 + i * (TILE_W + TILE_GAP);
    bool onBadge = x >= tileX + TILE_W - BADGE_W - 2 && y >= TILE_Y + TILE_H - BADGE_H - 2;
    sendTap(String(onBadge ? "P " : "T ") + i);
  } else if (y >= ZONE_Y && y < ZONE_Y + ZONE_H) {  // Zone tabs: LEFT | BOTH | RIGHT.
    if (tiles[selLight].zones < 2) return;
    int column = x * 3 / 320;
    sendTap("Z " + String(column == 0 ? 1 : column == 1 ? 0 : 2));
  } else if (y >= BARS_Y && y < BARS_Y + 5 * BAR_ROW_H) {  // Channel bars.
    int channel = (y - BARS_Y) / BAR_ROW_H;
    if (x >= BAR_X - 4 && x <= BAR_X + BAR_W + 4) {
      dragChannel = channel;                   // On the bar itself: set the level, keep following.
      dragLastLevel = -1;
      dragLastSend = 0;
      touchService();                          // Send the first level straight away.
    } else {
      sendTap("C " + String(channel));         // On the label/value: just give it to the knobs.
    }
  }
} // Completes touch handling: taps select things, dragging a bar sets its level live.

// --- Drawing ----------------------------------------------------------------------------------------------

const int CHIP_WIDTH = 52;
const int CHIP_X[3] = {152, 152 + 55, 152 + 110};  // NET, MEGA, UNO (last one ends at x 314).

void drawChip(int x, const char *name, bool up) {
  uint32_t colour = up ? C_GREEN : C_RED;
  gfx->drawRoundRect(x, 7, CHIP_WIDTH, 16, 3, colour);
  gfx->fillCircle(x + 8, 15, 3, colour);       // Status dot.
  gfx->setFont(&fonts::Font0);
  gfx->setTextColor(colour);
  gfx->setTextDatum(middle_left);
  gfx->drawString(name, x + 15, 15);
} // Completes one link-status chip (green = up, red = down).

void drawPowerIcon(int cx, int cy, uint32_t colour) {
  gfx->drawArc(cx, cy, 6, 5, 300, 240, colour);  // Broken ring...
  gfx->drawFastVLine(cx, cy - 7, 7, colour);   // ...with the line through the gap: the "power" symbol.
} // Completes the ⏻ symbol.

void drawTile(int i) {
  const Tile &tile = tiles[i];
  int x = TILE_X0 + i * (TILE_W + TILE_GAP), y = TILE_Y;
  bool picked = i == selLight;
  gfx->fillRoundRect(x, y, TILE_W, TILE_H, 5, C_BAR);
  gfx->drawRoundRect(x, y, TILE_W, TILE_H, 5, picked ? C_PINK : C_PURPLE);
  if (picked) gfx->drawRoundRect(x + 1, y + 1, TILE_W - 2, TILE_H - 2, 4, C_HOT);  // Thick glow when selected.

  gfx->setFont(&fonts::Font0);
  gfx->setTextDatum(top_left);
  gfx->setTextColor(picked ? C_WHITE : C_HOT);
  gfx->drawString(tile.name.substring(0, 12), x + 4, y + 4);

  int swatchY = y + 15, swatchH = 30, swatchW = (TILE_W - 8) / tile.zones;  // Colour per zone.
  for (int z = 0; z < tile.zones; z++) {
    int sx = x + 4 + z * swatchW;
    gfx->fillRect(sx, swatchY, swatchW - (z + 1 < tile.zones ? 1 : 0), swatchH, screenColour(tile.rgbw[z]));
  }
  gfx->drawRect(x + 4, swatchY, TILE_W - 8, swatchH, C_PURPLE);

  gfx->fillCircle(x + 9, y + TILE_H - 10, 4, tile.online ? C_GREEN : C_RED);   // Online dot.
  gfx->setTextColor(C_GREY);
  gfx->drawString(String(i + 1), x + 17, y + TILE_H - 14);                     // Keypad number.

  bool lit = false;                            // Badge colour: relay state, or the light's on/off.
  for (int z = 0; z < tile.zones; z++) for (int c = 0; c < 4; c++) lit |= tile.rgbw[z][c] > 0;
  uint32_t badge = tile.relay == 1 ? C_GREEN : tile.relay == 0 ? C_RED : tile.relay == -1 ? C_GREY : (lit ? C_GREEN : C_GREY);
  drawPowerIcon(x + TILE_W - 14, y + TILE_H - 11, badge);
} // Completes one light tile.

void drawZoneTabs() {
  const Tile &tile = tiles[selLight];
  const char *names[3] = {"LEFT", "BOTH", "RIGHT"};
  const int zoneFor[3] = {1, 0, 2};            // Tabs are laid out like the room.
  gfx->setFont(&fonts::FreeMonoBold9pt7b);
  gfx->setTextDatum(middle_center);
  for (int column = 0; column < 3; column++) {
    int x = 4 + column * 104, w = 104 - 4;
    bool active = tile.zones > 1 && selZone == zoneFor[column];
    uint32_t edge = tile.zones > 1 ? C_PINK : C_GREY;
    if (active) gfx->fillRoundRect(x, ZONE_Y, w, ZONE_H, 4, C_PINK);
    else gfx->drawRoundRect(x, ZONE_Y, w, ZONE_H, 4, edge);
    gfx->setTextColor(active ? C_BLACK : edge);
    gfx->drawString(tile.zones > 1 ? names[column] : (column == 1 ? "1 ZONE" : "-"), x + w / 2, ZONE_Y + ZONE_H / 2 + 1);
  }
} // Completes the LEFT | BOTH | RIGHT tabs (greyed for single-zone lights).

void drawBars() {
  gfx->setFont(&fonts::FreeMonoBold9pt7b);
  for (int channel = 0; channel < 5; channel++) {
    int y = BARS_Y + channel * BAR_ROW_H, mid = y + BAR_ROW_H / 2;
    bool knob = channel == selChannel;          // The channel the knobs adjust.
    if (knob) gfx->fillRoundRect(4, y + 1, 36, BAR_ROW_H - 2, 3, CHANNEL_COLOURS[channel]);
    gfx->setTextDatum(middle_center);
    gfx->setTextColor(knob ? C_BLACK : CHANNEL_COLOURS[channel]);
    gfx->drawString(CHANNEL_LABELS[channel], 22, mid + 1);

    uint8_t level = channelLevel(channel);
    gfx->fillRoundRect(BAR_X, y + 5, BAR_W, BAR_ROW_H - 10, 3, C_TRACK);
    int fill = level * BAR_W / 255;
    if (fill > 0) gfx->fillRoundRect(BAR_X, y + 5, max(fill, 6), BAR_ROW_H - 10, 3, CHANNEL_COLOURS[channel]);
    if (knob) gfx->drawRoundRect(BAR_X - 2, y + 3, BAR_W + 4, BAR_ROW_H - 6, 4, C_WHITE);

    gfx->setTextDatum(middle_right);
    gfx->setTextColor(knob ? C_WHITE : C_GREY);
    gfx->drawString(String(level), 314, mid + 1);
  }
} // Completes the BRI/R/G/B/W bars; the knobs' channel is filled in and outlined.

void drawDashboard() {
  unsigned long now = millis();
  gfx->fillScreen(C_BLACK);
  for (int y = 0; y < 240; y += 3) gfx->drawFastHLine(0, y, 320, C_SCAN);  // CRT scanlines behind everything.
  gfx->drawRect(0, 0, 320, 240, C_PINK);

  gfx->fillRect(1, 1, 318, 27, C_BAR);         // Title bar.
  gfx->setFont(&fonts::FreeMonoBold9pt7b);
  gfx->setTextColor(C_PINK);
  gfx->setTextDatum(middle_left);
  gfx->drawString("DOLL CONSOLE", 9, 16);
  if ((now / 500) % 2) gfx->fillRect(139, 9, 8, 14, C_HOT);  // Blinking cursor.
  drawChip(CHIP_X[0], "NET", masterAlive() && netUp);
  drawChip(CHIP_X[1], "MEGA", masterAlive() && megaUp);
  drawChip(CHIP_X[2], "UNO", masterAlive() && unoUp);

  for (int i = 0; i < 4; i++) drawTile(i);
  drawZoneTabs();
  drawBars();

  String message = !masterAlive() ? String("WAITING FOR MASTER") : (now - toastAt < TOAST_MS ? toast : String(""));
  if (message.length()) {                      // Pop-up over the bars.
    uint32_t colour = masterAlive() ? C_PINK : C_RED;
    gfx->fillRoundRect(40, 150, 240, 50, 8, C_BLACK);
    gfx->drawRoundRect(40, 150, 240, 50, 8, colour);
    gfx->drawRoundRect(42, 152, 236, 46, 6, C_PURPLE);
    gfx->setFont(&fonts::FreeMonoBold12pt7b);
    gfx->setTextDatum(middle_center);
    gfx->setTextColor(colour);
    gfx->drawString(message, 160, 176);
  }
} // Completes one full frame: title, tiles, zone tabs, bars, and any pop-up.

// --- Arduino entry points -----------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);                        // USB Serial Monitor (debug only).
  for (int pin : {LED_RED, LED_GREEN, LED_BLUE}) analogWrite(pin, 255);  // LED off (active LOW).
  tft.init();
  tft.setRotation(1);                          // Landscape, 320 x 240.
  tft.setBrightness(200);
  canvas.setColorDepth(16);
  if (canvas.createSprite(320, 240)) {         // 150 KB: try full colour first...
    gfx = &canvas;
  } else {
    canvas.setColorDepth(8);                   // ...then 75 KB with fewer colours...
    if (canvas.createSprite(320, 240)) gfx = &canvas;  // ...else draw straight to the screen (may flicker).
  }
  master.begin(LINK_BAUD, SERIAL_8N1, LINK_RX_PIN, LINK_TX_PIN);
  delay(50);
  master.print("HI\n");                        // Ask the master for everything.
  Serial.printf("CYD face started (%s frame buffer)\n", gfx == &canvas ? "with" : "without");
} // Completes start-up: LED off, screen on, frame buffer, link, hello.

void loop() {
  readMaster();
  touchService();
  unsigned long now = millis();
  static unsigned long lastBlink = 0;
  if (now / 500 != lastBlink) { lastBlink = now / 500; dirty = true; }  // Cursor blink + link timeouts.
  bool toastShowing = toastAt && now - toastAt < TOAST_MS + FRAME_MS;
  if ((dirty || toastShowing) && now - lastFrame >= FRAME_MS) {
    drawDashboard();
    if (gfx == &canvas) canvas.pushSprite(0, 0);  // Show the finished frame in one go.
    lastFrame = now;
    dirty = false;
  }
} // Completes one pass: read the master, handle touch, redraw when something changed.
