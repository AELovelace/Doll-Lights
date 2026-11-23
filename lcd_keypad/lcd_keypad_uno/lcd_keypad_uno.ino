// LCD Keypad Shield driver for an Arduino Uno, controlled by the ESP32 brain over a serial link.
//
// The Uno owns the 16x2 LCD and the five shield buttons (all 5 V parts), and the ESP32
// tells it what to show. In the master console the Uno is wired to the MEGA, which relays
// every line to/from the ESP32 ("U <line>"), so this sketch doesn't know or care:
//
//   * Mega 16 (TX2) -> Uno D3: an ordinary serial line (SoftwareSerial on D3).
//   * Uno D2 -> Mega 17 (RX2): D2 is OPEN-DRAIN. The Uno only ever pulls the line to ground
//     (a 0 bit) or lets go (a 1 bit), and the far side's pull-up holds it HIGH. This was
//     built for a direct 3.3 V ESP32 link (ESP32 GPIO33 -> D3, D2 -> GPIO32) and still works
//     that way; on the Mega it's simply 5 V to 5 V. Full pin map: ../PINMAP.md.
//
// !! Never use digitalWrite(2, HIGH) or pinMode(2, INPUT_PULLUP) in this sketch: on a direct
// !! ESP32 link either one would put 5 V on its pin. Flash this sketch BEFORE connecting D2.
//
// Link: 9600 baud, 8N1, text lines ending in '\n'. See ../README.md for the protocol.
// Board: Arduino Uno. Libraries: LiquidCrystal and SoftwareSerial (both built in).

#include <LiquidCrystal.h>
#include <SoftwareSerial.h>

// --- Pins (LCD Keypad Shield layout, DFRobot style) -----------------------------------
LiquidCrystal lcd(8, 9, 4, 5, 6, 7);          // RS, EN, D4, D5, D6, D7 as wired on the shield.
const uint8_t BUTTON_PIN = A0;                // All five buttons share A0 through a resistor ladder.
// D10 is the shield's backlight transistor. It's left alone (never set as an output)
// because driving it HIGH overloads the pin on many of these shields; the backlight stays on.

const uint8_t LINK_RX_PIN = 3;                // From Mega 16 / TX2 (the brain talks through the Mega, Uno listens).
const uint8_t LINK_UNUSED_TX_PIN = 12;        // SoftwareSerial insists on a TX pin; leave D12 unconnected.
// Uno -> ESP32 uses D2 = port D bit 2, driven by register writes for exact open-drain timing.
#define LINK_TX_BIT _BV(PD2)

const unsigned long LINK_BAUD = 9600;
const unsigned int BIT_US = 104;              // One bit at 9600 baud = 104.17 microseconds.
const uint8_t MAX_LINE_LENGTH = 40;                  // Longest command line accepted from the ESP32.
const uint8_t LCD_COLS = 16;
const uint8_t LCD_ROWS = 2;

SoftwareSerial link(LINK_RX_PIN, LINK_UNUSED_TX_PIN);  // Receive-only in practice.

char lineBuffer[MAX_LINE_LENGTH + 1];                // Command line being assembled from the ESP32.
uint8_t lineLength = 0;
bool lineOverflow = false;                    // True when the current line got too long (it's dropped).

// --- Open-drain transmit on D2 ---------------------------------------------------------

void linkLineRelease() {
  DDRD &= ~LINK_TX_BIT;                       // Input with PORTD bit 0 = floating: the ESP32 pull-up makes it 3.3 V.
} // Completes a "1" (or idle) level without the Uno ever driving HIGH.

void linkLinePullLow() {
  DDRD |= LINK_TX_BIT;                        // Output, and PORTD bit 2 is always 0, so this drives 0 V.
} // Completes a "0" level.

void linkWriteByte(uint8_t value) {
  uint8_t savedInterrupts = SREG;             // Remember whether interrupts were on.
  cli();                                      // Interrupts would stretch bits and garble the byte.
  linkLinePullLow();                          // Start bit (always 0).
  delayMicroseconds(BIT_US);
  for (uint8_t bit = 0; bit < 8; bit++) {     // Data bits, least significant first (standard UART order).
    if (value & 0x01) linkLineRelease(); else linkLinePullLow();
    delayMicroseconds(BIT_US);
    value >>= 1;
  }
  linkLineRelease();                          // Stop bit (always 1) and back to idle.
  delayMicroseconds(BIT_US);
  SREG = savedInterrupts;                     // Interrupts back on (SoftwareSerial receive resumes).
} // Completes one 8N1 byte at 9600 baud on the open-drain line (about 1 ms with interrupts off).

void linkSendLine(const char *text) {
  for (const char *c = text; *c; c++) linkWriteByte((uint8_t)*c);  // Each character in turn...
  linkWriteByte('\n');                        // ...then the end-of-line marker.
  Serial.print(F("-> ESP32: "));
  Serial.println(text);                       // USB Serial Monitor log for debugging.
} // Completes one message line to the ESP32.

// --- LCD -----------------------------------------------------------------------------

void lcdShowRow(uint8_t row, const char *text) {
  lcd.setCursor(0, row);
  uint8_t column = 0;
  for (; column < LCD_COLS && text[column]; column++) lcd.write(text[column]);  // Text, cut at 16 chars.
  for (; column < LCD_COLS; column++) lcd.write(' ');                           // Blank the rest of the row.
} // Completes a whole-row rewrite so old characters never linger.

// --- Commands from the ESP32 -------------------------------------------------------------

void handleLine(char *line) {
  Serial.print(F("<- ESP32: "));
  Serial.println(line);                       // USB Serial Monitor log for debugging.
  if ((line[0] == 'T') && (line[1] == '0' || line[1] == '1') && line[2] == ':') {
    uint8_t row = line[1] - '0';              // "T0:..." = top row, "T1:..." = bottom row.
    if (row < LCD_ROWS) lcdShowRow(row, line + 3);
  } else if (strcmp(line, "CLR") == 0) {
    lcd.clear();                              // Blank the whole screen.
  } else if (strcmp(line, "PING") == 0) {
    linkSendLine("PO");                       // PONG: tells the ESP32 the Uno is alive.
  }                                           // Anything else (e.g. a garbled line) is ignored.
} // Completes one command from the ESP32.

void readLink() {
  while (link.available()) {
    char c = (char)link.read();
    if (c == '\r') continue;                  // Tolerate Windows-style line endings.
    if (c == '\n') {
      lineBuffer[lineLength] = '\0';
      if (!lineOverflow && lineLength > 0) handleLine(lineBuffer);
      lineLength = 0;                         // Start the next line fresh.
      lineOverflow = false;
    } else if (lineLength < MAX_LINE_LENGTH) {
      lineBuffer[lineLength++] = c;
    } else {
      lineOverflow = true;                    // Too long: drop this whole line at its '\n'.
    }
  }
} // Completes gathering characters into lines and running each finished line.

// --- Buttons ---------------------------------------------------------------------------

char decodeButton(int raw) {
  // Thresholds sit halfway between the values measured on Doll's shield on 2026-11-22
  // (RIGHT 0, UP 206, DOWN 407, LEFT 625). This shield is not the DFRobot v1.1 ladder.
  if (raw < 103) return 'R';                  // RIGHT  (measured 0)
  if (raw < 306) return 'U';                  // UP     (measured 206)
  if (raw < 516) return 'D';                  // DOWN   (measured 407)
  if (raw < 724) return 'L';                  // LEFT   (measured 625)
  if (raw < 950) return 'S';                  // SELECT (expected ~824 from the ladder's ~210 steps; never read yet)
  return 0;                                   // Nothing pressed (~1023).
} // Completes the A0 reading -> button letter; logRawPress() prints raw values for re-calibrating.

void logRawPress(int raw) {
  static bool logged = false;                 // True once this press has been printed.
  static uint8_t lowSamples = 0;              // How many samples in a row were below "nothing pressed".
  if (raw >= 1000) {                          // Released (A0 sits near 1023 with no button down).
    lowSamples = 0;
    logged = false;
    return;
  }
  if (lowSamples < 3) lowSamples++;
  if (lowSamples == 3 && !logged) {           // Settled for ~45 ms: print it once.
    Serial.print(F("A0="));
    Serial.print(raw);
    Serial.print(F(" -> "));
    char key = decodeButton(raw);
    Serial.println(key ? key : '-');          // '-' = this value isn't mapped to any button.
    logged = true;
  }
} // Completes the calibration log; read it in Serial Monitor (115200) to set decodeButton()'s thresholds.

void checkButtons() {
  static char lastSeen = 0;                   // Button seen on the previous sample.
  static char reported = 0;                   // Button already sent to the ESP32.
  static unsigned long lastSample = 0;
  if (millis() - lastSample < 15) return;     // Sample about 66 times a second.
  lastSample = millis();

  int raw = analogRead(BUTTON_PIN);
  logRawPress(raw);                           // Calibration aid: prints A0 for every press, even unrecognised ones.
  char now = decodeButton(raw);
  if (now == lastSeen && now != reported) {   // Same on two samples in a row = settled (debounced).
    reported = now;
    if (now) {                                // Only presses are sent, not releases.
      char message[3] = {'K', now, '\0'};     // "KU", "KD", "KL", "KR" or "KS".
      linkSendLine(message);
    }
  }
  lastSeen = now;
} // Completes debounced button presses, each sent once as a 2-letter message.

// --- Arduino entry points ----------------------------------------------------------------

void setup() {
  PORTD &= ~LINK_TX_BIT;                      // D2 output level is 0 forever (open-drain rule).
  linkLineRelease();                          // Idle = let go.
  Serial.begin(115200);                       // USB Serial Monitor, for debugging only.
  link.begin(LINK_BAUD);                      // Listen to the ESP32 on D3.
  pinMode(LINK_RX_PIN, INPUT);                // SoftwareSerial switched on D3's 5 V pull-up; turn it off so no
                                              // 5 V leaks into the ESP32's TX pin (the ESP32 drives this line itself).
  lcd.begin(LCD_COLS, LCD_ROWS);
  lcdShowRow(0, "Keypad ready :3");
  lcdShowRow(1, "Waiting ESP32...");
  delay(50);                                  // Let the ESP32 side see a steady idle line first.
  linkSendLine("HI");                         // Tells the ESP32 to (re)send the whole screen.
  Serial.println(F("LCD keypad (Uno side) started"));
} // Completes start-up: safe D2, LCD splash, and a hello to the ESP32.

void loop() {
  readLink();                                 // Apply any screen commands from the ESP32.
  checkButtons();                             // Report any new button press.
} // Completes one pass; neither step blocks for long.
