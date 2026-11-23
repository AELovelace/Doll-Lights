// Mega 2560 accessory board for the master light console: a general I/O expander that the
// ESP32 controls over a serial link. Accessories (LEDs, relays, switches, knobs...) plug into the
// Mega's 5 V pins, and the ESP32 decides what they do, so adding one rarely needs a Mega reflash.
//
// Link to the ESP32 (no resistors needed):
//   * ESP32 GPIO15 -> Mega pin 19 (RX1): the Mega receives on its hardware Serial1 (robust,
//     buffered). The ESP32's 3.3 V HIGH is enough for the Mega.
//   * Mega D2 -> ESP32 GPIO5: OPEN-DRAIN. The Mega only ever pulls this line LOW (0 bit) or lets
//     go (1 bit); the ESP32's pull-up makes HIGH = 3.3 V. The Mega never puts 5 V on that wire.
//   * Pin 18 (TX1) is NOT used: Serial1 drives it at 5 V. Leave it unconnected.
//
// Uno screen hub: the Uno + LCD Keypad Shield hangs off this Mega (both 5 V, so no tricks):
//   * Mega 16 (TX2) -> Uno D3, Uno D2 -> Mega 17 (RX2, with the Mega's 5 V pull-up on),
//     9600 baud. The Uno sketch is unchanged.
//   * "U <line>" from the ESP32 is passed to the Uno as "<line>"; every line from the Uno goes
//     back to the ESP32 as "U <line>". So the ESP32 still talks to the Uno, just via the Mega.
//
// !! Never use digitalWrite(2, HIGH) or pinMode(2, INPUT_PULLUP) here: either one puts 5 V on
// !! the ESP32's pin. Flash this sketch BEFORE connecting the D2 wire.
//
// ESP32 link: 19200 baud 8N1, text lines ending in '\n'. Pins are Mega pin numbers (A0-A15 = 54-69).
//   ESP32 -> Mega                    Mega -> ESP32
//   PING                             PO
//   MODE <pin> OUT|IN|PULLUP         HI                 (just started: please configure me)
//   DW <pin> 0|1      (OUT pins)     DV <pin> <0|1>     (answer to DR)
//   PWM <pin> 0-255   (OUT, PWM pin) AV <n> <0-1023>    (answer to AR)
//   DR <pin>                         IN <pin> <0|1>     (watched input changed; also sent on WATCH)
//   AR <n>            (A<n>, 0-15)   ERR <reason>       (command refused)
//   WATCH <pin> / UNWATCH <pin>
//   RESET             (every pin back to a plain input, all watches off, dot matrix blank)
//   MXCLR / MXBRI / MXPIX / MXROW / MXALL / MXTEST   16x8 dot matrix, see matrix.ino
//   U <line>          (forwarded to the Uno)  U <line>           (from the Uno)
//                                           KN <knob> <clicks> / KP <knob>   (rotary knobs, see knobs.ino)
// Pins 0, 1 (USB), 2 (link out), 16, 17 (Uno), 18, 19 (Serial1), 51-53 (dot matrix) and the knob
// pins (A8-A15, 24, 26, 28, 30) are refused.
// Board: Arduino Mega or Mega 2560 (ATmega2560). Libraries: SPI (built in).

#include <SPI.h>                               // Hardware SPI for the MAX7219 dot matrix (matrix.ino).

// --- Link out: open-drain on D2 = port E bit 4 ------------------------------------------
#define LINK_TX_BIT _BV(PE4)
const unsigned int BIT_US = 52;                // One bit at 19200 baud = 52.08 microseconds.
const unsigned long LINK_BAUD = 19200;         // Must match the ESP32's TextLink for the Mega.
const unsigned long UNO_BAUD = 9600;           // Must match the Uno sketch.
const uint8_t UNO_RX_PIN = 17;                 // RX2, from Uno D2 (open-drain, so it needs our pull-up).
const uint8_t MAX_LINE_LENGTH = 48;            // Longest command line accepted from the ESP32 ("MXALL" + 32 hex fits).
const uint8_t MAX_WATCHES = 16;                // Inputs that can be watched at once.
const unsigned long WATCH_DEBOUNCE_MS = 20;    // A watched input must hold steady this long.

enum PinMode : uint8_t { MODE_UNTOUCHED = 0, MODE_OUT, MODE_IN, MODE_PULLUP };
uint8_t pinModes[NUM_DIGITAL_PINS];            // What the ESP32 set each pin to (all start untouched).

struct Watch {
  uint8_t pin;
  bool used;
  uint8_t stable;                              // Last value reported to the ESP32.
  uint8_t last;                                // Value on the previous check.
  unsigned long changedAt;                     // When `last` last changed (for debouncing).
};
Watch watches[MAX_WATCHES];

char lineBuffer[MAX_LINE_LENGTH + 1];          // Command line being assembled from the ESP32.
uint8_t lineLength = 0;
bool lineOverflow = false;                     // True when the current line got too long (it's dropped).
char unoBuffer[MAX_LINE_LENGTH + 1];           // Line being assembled from the Uno.
uint8_t unoLength = 0;

void linkLineRelease() {
  DDRE &= ~LINK_TX_BIT;                        // Input with PORTE bit 0 = floating: the ESP32 pull-up makes it 3.3 V.
} // Completes a "1" (or idle) level without the Mega ever driving HIGH.

void linkLinePullLow() {
  DDRE |= LINK_TX_BIT;                         // Output, and PORTE bit 4 is always 0, so this drives 0 V.
} // Completes a "0" level.

void linkWriteByte(uint8_t value) {
  uint8_t savedInterrupts = SREG;
  cli();                                       // Interrupts would stretch bits and garble the byte.
  linkLinePullLow();                           // Start bit.
  delayMicroseconds(BIT_US);
  for (uint8_t bit = 0; bit < 8; bit++) {      // Data bits, least significant first.
    if (value & 0x01) linkLineRelease(); else linkLinePullLow();
    delayMicroseconds(BIT_US);
    value >>= 1;
  }
  linkLineRelease();                           // Stop bit and back to idle.
  delayMicroseconds(BIT_US);
  SREG = savedInterrupts;                      // Serial1 keeps receiving in hardware meanwhile (2-byte buffer).
} // Completes one 8N1 byte on the open-drain line (about 1 ms with interrupts off).

void linkSendLine(const String &text) {
  for (unsigned int i = 0; i < text.length(); i++) linkWriteByte((uint8_t)text[i]);
  linkWriteByte('\n');
  Serial.print(F("-> ESP32: "));
  Serial.println(text);                        // USB Serial Monitor log for debugging.
} // Completes one message line to the ESP32.

// --- Pin helpers ---------------------------------------------------------------------------

bool pinIsReserved(long pin) {
  return pin == 0 || pin == 1 || pin == 2 || pin == 18 || pin == 19  // USB serial, link out, Serial1.
         || pin == 16 || pin == 17                                  // Uno link (Serial2).
         || pinIsKnob(pin)                                          // Rotary knobs (knobs.ino).
         || (pin >= 51 && pin <= 53);                                // Dot matrix DIN, CLK, CS.
} // Completes the check for pins the link, USB and dot matrix need.

bool parsePin(const char *text, long &pin) {
  if (!text) return false;
  char *end;
  pin = strtol(text, &end, 10);
  return *end == '\0' && end != text && pin >= 0 && pin < NUM_DIGITAL_PINS && !pinIsReserved(pin);
} // Completes turning text into a usable Mega pin number (false = missing, bad or reserved).

bool parseNumber(const char *text, long low, long high, long &value) {
  if (!text) return false;
  char *end;
  value = strtol(text, &end, 10);
  return *end == '\0' && end != text && value >= low && value <= high;
} // Completes turning text into a number within [low, high].

void sendPinValue(const char *kind, long pin, int value) {
  linkSendLine(String(kind) + " " + pin + " " + value);
} // Completes a "DV/IN <pin> <value>" style reply.

// --- Watched inputs ------------------------------------------------------------------------

Watch *findWatch(long pin) {
  for (uint8_t i = 0; i < MAX_WATCHES; i++) {
    if (watches[i].used && watches[i].pin == pin) return &watches[i];
  }
  return nullptr;
} // Completes the lookup of an existing watch (nullptr = not watched).

bool addWatch(long pin) {
  Watch *watch = findWatch(pin);
  if (!watch) {
    for (uint8_t i = 0; i < MAX_WATCHES && !watch; i++) {
      if (!watches[i].used) watch = &watches[i];  // First free slot.
    }
  }
  if (!watch) return false;                    // All slots in use.
  uint8_t value = digitalRead(pin);
  *watch = {(uint8_t)pin, true, value, value, millis()};
  sendPinValue("IN", pin, value);              // Tell the ESP32 the starting value straight away.
  return true;
} // Completes watching a pin: its current value now, and every debounced change later.

void checkWatches() {
  for (uint8_t i = 0; i < MAX_WATCHES; i++) {
    Watch &watch = watches[i];
    if (!watch.used) continue;
    uint8_t value = digitalRead(watch.pin);
    if (value != watch.last) {                 // Still bouncing: restart the timer.
      watch.last = value;
      watch.changedAt = millis();
    } else if (value != watch.stable && millis() - watch.changedAt >= WATCH_DEBOUNCE_MS) {
      watch.stable = value;                    // Steady long enough: report it.
      sendPinValue("IN", watch.pin, value);
    }
  }
} // Completes one debounced pass over every watched input.

void resetPins() {
  for (uint8_t pin = 0; pin < NUM_DIGITAL_PINS; pin++) {
    if (pinModes[pin] != MODE_UNTOUCHED) {
      pinMode(pin, INPUT);                     // Plain input: no output, no pull-up.
      pinModes[pin] = MODE_UNTOUCHED;
    }
  }
  for (uint8_t i = 0; i < MAX_WATCHES; i++) watches[i].used = false;
  matrixClear();                               // Blank dot matrix too (matrix.ino).
} // Completes RESET: everything the ESP32 set up is undone.

// --- Commands from the ESP32 ----------------------------------------------------------------

void handleLine(char *line) {
  Serial.print(F("<- ESP32: "));
  Serial.println(line);
  if (line[0] == 'U' && line[1] == ' ') {      // "U <line>": for the Uno screen, pass it straight on.
    Serial2.print(line + 2);
    Serial2.print('\n');
    return;
  }
  char *command = strtok(line, " ");
  char *arg1 = strtok(nullptr, " ");
  char *arg2 = strtok(nullptr, " ");
  char *arg3 = strtok(nullptr, " ");
  long pin, value;
  if (!command) return;

  if (matrixCommand(command, arg1, arg2, arg3)) {
    // MX... commands are handled in matrix.ino.
  } else if (!strcmp(command, "PING")) {
    linkSendLine("PO");
  } else if (!strcmp(command, "MODE")) {
    if (!parsePin(arg1, pin) || !arg2) return linkSendLine(F("ERR MODE needs <pin> OUT|IN|PULLUP"));
    if (!strcmp(arg2, "OUT")) { pinMode(pin, OUTPUT); pinModes[pin] = MODE_OUT; }
    else if (!strcmp(arg2, "IN")) { pinMode(pin, INPUT); pinModes[pin] = MODE_IN; }
    else if (!strcmp(arg2, "PULLUP")) { pinMode(pin, INPUT_PULLUP); pinModes[pin] = MODE_PULLUP; }
    else linkSendLine(F("ERR MODE must be OUT, IN or PULLUP"));
  } else if (!strcmp(command, "DW")) {
    if (!parsePin(arg1, pin) || !parseNumber(arg2, 0, 1, value)) return linkSendLine(F("ERR DW needs <pin> 0|1"));
    if (pinModes[pin] != MODE_OUT) return linkSendLine("ERR pin " + String(pin) + " is not OUT");
    digitalWrite(pin, value);
  } else if (!strcmp(command, "PWM")) {
    if (!parsePin(arg1, pin) || !parseNumber(arg2, 0, 255, value)) return linkSendLine(F("ERR PWM needs <pin> 0-255"));
    if (pinModes[pin] != MODE_OUT || !digitalPinHasPWM(pin)) {
      return linkSendLine("ERR pin " + String(pin) + " is not an OUT PWM pin (3-13, 44-46)");
    }
    analogWrite(pin, value);
  } else if (!strcmp(command, "DR")) {
    if (!parsePin(arg1, pin)) return linkSendLine(F("ERR DR needs <pin>"));
    sendPinValue("DV", pin, digitalRead(pin));
  } else if (!strcmp(command, "AR")) {
    if (!parseNumber(arg1, 0, 15, value)) return linkSendLine(F("ERR AR needs 0-15 (A0-A15)"));
    sendPinValue("AV", value, analogRead(A0 + value));
  } else if (!strcmp(command, "WATCH")) {
    if (!parsePin(arg1, pin)) return linkSendLine(F("ERR WATCH needs <pin>"));
    if (!addWatch(pin)) linkSendLine(F("ERR too many watched pins (16 max)"));
  } else if (!strcmp(command, "UNWATCH")) {
    if (!parsePin(arg1, pin)) return linkSendLine(F("ERR UNWATCH needs <pin>"));
    Watch *watch = findWatch(pin);
    if (watch) watch->used = false;
  } else if (!strcmp(command, "RESET")) {
    resetPins();
  } else {
    linkSendLine("ERR unknown command " + String(command));
  }
} // Completes one command; anything refused is answered with "ERR ...".

void readLink() {
  while (Serial1.available()) {
    char c = (char)Serial1.read();
    if (c == '\r') continue;
    if (c == '\n') {
      lineBuffer[lineLength] = '\0';
      if (!lineOverflow && lineLength > 0) handleLine(lineBuffer);
      lineLength = 0;
      lineOverflow = false;
    } else if (lineLength < MAX_LINE_LENGTH) {
      lineBuffer[lineLength++] = c;
    } else {
      lineOverflow = true;                     // Too long: drop this whole line at its '\n'.
    }
  }
} // Completes gathering characters into lines and running each finished line.

void readUno() {
  while (Serial2.available()) {
    char c = (char)Serial2.read();
    if (c == '\r') continue;
    if (c == '\n') {
      unoBuffer[unoLength] = '\0';
      if (unoLength > 0) linkSendLine(String("U ") + unoBuffer);  // Tag it so the ESP32 knows it's the Uno.
      unoLength = 0;
    } else if (unoLength < MAX_LINE_LENGTH) {
      unoBuffer[unoLength++] = c;
    }                                          // Overlong lines are cut.
  }
} // Completes relaying the Uno's messages (buttons, HI, PO) to the ESP32.

// --- Arduino entry points ----------------------------------------------------------------

void setup() {
  PORTE &= ~LINK_TX_BIT;                       // D2 output level is 0 forever (open-drain rule).
  linkLineRelease();                           // Idle = let go.
  Serial.begin(115200);                        // USB Serial Monitor, for debugging only.
  Serial1.begin(LINK_BAUD);                    // Receive from the ESP32 on pin 19. Never write to Serial1:
                                               // its TX (pin 18) is push-pull 5 V and stays unconnected.
  Serial2.begin(UNO_BAUD);                     // Uno screen link on pins 16 (TX2) / 17 (RX2).
  digitalWrite(UNO_RX_PIN, HIGH);              // Pull-up on RX2: the Uno's D2 only pulls LOW (5 V to 5 V, safe).
  matrixBegin();                               // Dot matrix: set up and blank (matrix.ino).
  knobsBegin();                                // Rotary knobs on A8-A15 (knobs.ino).
  delay(50);                                   // Let the ESP32 see a steady idle line first.
  linkSendLine("HI");                          // Asks the ESP32 to send this board's configuration.
  Serial.println(F("Mega accessories started"));
} // Completes start-up: safe D2, Serial1 listening, and a hello to the ESP32.

void loop() {
  readLink();                                  // Run any commands from the ESP32.
  readUno();                                   // Pass the Uno's messages up to the ESP32.
  knobsService();                              // Report knob turns and pushes.
  checkWatches();                              // Report changes on watched inputs.
} // Completes one pass; neither step blocks for long.
