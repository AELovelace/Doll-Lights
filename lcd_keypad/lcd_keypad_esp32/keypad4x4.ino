// 4x4 matrix keypad on the ESP32 (membrane type: 8 wires, keys 0-9, A-D, * and #).
//
// The keypad is just 16 switches, each joining one row wire to one column wire. Columns are
// inputs held HIGH by the ESP32's internal 3.3 V pull-ups. To scan, one row at a time is pulled
// LOW; a column that reads LOW then means the key at that row/column is down.
// Idle rows are left floating (INPUT), never driven HIGH, so pressing two keys in the same
// column can't short a HIGH pin against a LOW one.
//
// Keypad connector, pins 1-8 from left to right with the keys facing you:
//   1-4 = rows R1-R4 (top to bottom), 5-8 = columns C1-C4 (left to right).
// If keys come out swapped, fix the order in ROW_PINS / COL_PINS rather than rewiring.

const int ROW_PINS[4] = {13, 14, 27, 26};      // Keypad pins 1-4 (R1-R4).
const int COL_PINS[4] = {25, 23, 22, 21};      // Keypad pins 5-8 (C1-C4).
const char KEYMAP[4][4] = {                    // Printed labels, row by row.
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'},
};
const unsigned long KEYPAD_SCAN_MS = 10;       // Scan 100 times a second.

void keypadBegin() {
  for (int r = 0; r < 4; r++) pinMode(ROW_PINS[r], INPUT);         // Rows float until scanned.
  for (int c = 0; c < 4; c++) pinMode(COL_PINS[c], INPUT_PULLUP);  // Columns idle HIGH (3.3 V).
} // Completes keypad pin setup.

char keypadScan() {
  char found = 0;
  for (int r = 0; r < 4 && !found; r++) {
    digitalWrite(ROW_PINS[r], LOW);            // Output level first...
    pinMode(ROW_PINS[r], OUTPUT);              // ...then drive this row LOW.
    delayMicroseconds(10);                     // Let the line settle through the membrane.
    for (int c = 0; c < 4; c++) {
      if (digitalRead(COL_PINS[c]) == LOW) {   // Column pulled LOW = key at (r, c) is down.
        found = KEYMAP[r][c];
        break;
      }
    }
    pinMode(ROW_PINS[r], INPUT);               // Release the row (floating again).
  }
  return found;                                // First key found, or 0 for none.
} // Completes one full scan; with several keys held, the first one found wins.

char keypadRead() {
  static unsigned long lastScan = 0;
  static char lastSeen = 0;                    // Key seen on the previous scan.
  static char reported = 0;                    // Key already handed to the sketch.
  if (millis() - lastScan < KEYPAD_SCAN_MS) return 0;
  lastScan = millis();
  char now = keypadScan();
  char result = 0;
  if (now == lastSeen && now != reported) {    // Same on two scans in a row = settled (debounced).
    reported = now;
    result = now;                              // Report presses only (0 = released is not reported).
  }
  lastSeen = now;
  return result;
} // Completes a debounced read: returns a key once when pressed, otherwise 0.
