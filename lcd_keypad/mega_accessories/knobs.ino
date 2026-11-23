// Rotary knobs (KY-040 style) on the Mega. Up to 4, all on port K (A8-A15), which shares one
// pin-change interrupt, so no click is missed even while the Mega is busy.
//
//   Knob | CLK  | DT   | SW (push)   Job on the master console
//   0    | A8   | A9   | 24          NAV: drives the Uno LCD menu
//   1    | A10  | A11  | 26          (spare)
//   2    | A12  | A13  | 28          (spare)
//   3    | A14  | A15  | 30          (spare)
// Power each module from the Mega's 5V and GND. Unused knob pins just sit HIGH on their pull-ups,
// so a knob that isn't fitted never sends anything.
//
// Messages to the ESP32 (sent at most every KNOB_REPORT_MS, so a fast spin is one message):
//   KN <knob> <clicks>   turned; + = clockwise (flip with KNOB_REVERSE)
//   KP <knob>            pushed (debounced; releases aren't sent)

const uint8_t KNOB_COUNT = 4;
const uint8_t KNOB_CLK_PINS[KNOB_COUNT] = {A8, A10, A12, A14};
const uint8_t KNOB_DT_PINS[KNOB_COUNT] = {A9, A11, A13, A15};
const uint8_t KNOB_SW_PINS[KNOB_COUNT] = {24, 26, 28, 30};
const int8_t KNOB_EDGES_PER_CLICK = 4;         // Most KY-040s. If it moves every 2nd click, use 2.
const bool KNOB_REVERSE = false;               // Set true if clockwise counts down.
const unsigned long KNOB_REPORT_MS = 30;       // Bundle clicks for at most this long.
const unsigned long KNOB_SW_DEBOUNCE_MS = 30;

// Indexed by (previous AB << 2) | new AB: +1 / -1 for a valid quarter-step, 0 for bounce.
const int8_t KNOB_TABLE[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};

volatile int16_t knobEdges[KNOB_COUNT];        // Edges counted by the interrupt, not yet reported.
volatile uint8_t knobLastAB[KNOB_COUNT];       // CLK/DT levels at the previous edge.
int16_t knobLeftover[KNOB_COUNT];              // Edges that don't make a whole click yet.
bool knobSwLast[KNOB_COUNT], knobSwStable[KNOB_COUNT];
unsigned long knobSwChangedAt[KNOB_COUNT];
unsigned long lastKnobReport = 0;

uint8_t knobReadAB(uint8_t knob, uint8_t portK) {
  uint8_t clkBit = 2 * knob, dtBit = 2 * knob + 1;  // A8 = PK0, A9 = PK1, ... A15 = PK7.
  return (((portK >> clkBit) & 1) << 1) | ((portK >> dtBit) & 1);
} // Completes reading one knob's two lines from a snapshot of port K.

ISR(PCINT2_vect) {
  uint8_t portK = PINK;                        // All 8 lines at the same instant.
  for (uint8_t knob = 0; knob < KNOB_COUNT; knob++) {
    uint8_t ab = knobReadAB(knob, portK);
    knobEdges[knob] += KNOB_TABLE[(knobLastAB[knob] << 2) | ab];
    knobLastAB[knob] = ab;
  }
} // Completes one pin change: every knob's edge count is updated.

void knobsBegin() {
  for (uint8_t knob = 0; knob < KNOB_COUNT; knob++) {
    pinMode(KNOB_CLK_PINS[knob], INPUT_PULLUP);  // Modules have pull-ups too; these cover bare encoders.
    pinMode(KNOB_DT_PINS[knob], INPUT_PULLUP);
    pinMode(KNOB_SW_PINS[knob], INPUT_PULLUP);
    knobSwLast[knob] = knobSwStable[knob] = HIGH;
  }
  uint8_t portK = PINK;
  for (uint8_t knob = 0; knob < KNOB_COUNT; knob++) knobLastAB[knob] = knobReadAB(knob, portK);
  PCMSK2 = 0xFF;                               // Wake on every A8-A15 change...
  PCICR |= _BV(PCIE2);                         // ...through the PCINT2 interrupt.
} // Completes knob setup: pull-ups, starting positions, interrupt on.

void knobsService() {
  unsigned long now = millis();
  for (uint8_t knob = 0; knob < KNOB_COUNT; knob++) {   // Push switches: debounce, report presses.
    bool level = digitalRead(KNOB_SW_PINS[knob]);
    if (level != knobSwLast[knob]) { knobSwLast[knob] = level; knobSwChangedAt[knob] = now; }
    if (now - knobSwChangedAt[knob] >= KNOB_SW_DEBOUNCE_MS && level != knobSwStable[knob]) {
      knobSwStable[knob] = level;
      if (level == LOW) linkSendLine("KP " + String(knob));
    }
  }
  if (now - lastKnobReport < KNOB_REPORT_MS) return;
  lastKnobReport = now;
  for (uint8_t knob = 0; knob < KNOB_COUNT; knob++) {   // Turns: whole clicks only.
    noInterrupts();
    int16_t edges = knobEdges[knob];
    knobEdges[knob] = 0;
    interrupts();
    knobLeftover[knob] += edges;
    int16_t clicks = knobLeftover[knob] / KNOB_EDGES_PER_CLICK;
    knobLeftover[knob] -= clicks * KNOB_EDGES_PER_CLICK;
    if (clicks) linkSendLine("KN " + String(knob) + " " + String(KNOB_REVERSE ? -clicks : clicks));
  }
} // Completes reporting: pushes straight away, turns bundled every KNOB_REPORT_MS.

bool pinIsKnob(long pin) {
  for (uint8_t knob = 0; knob < KNOB_COUNT; knob++) {
    if (pin == KNOB_CLK_PINS[knob] || pin == KNOB_DT_PINS[knob] || pin == KNOB_SW_PINS[knob]) return true;
  }
  return false;
} // Completes the check that keeps the general pin commands off the knob pins.
