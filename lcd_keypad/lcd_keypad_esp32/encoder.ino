// KY-040 rotary encoder module on the ESP32.
//
// Power the module's + pin from the ESP32's 3.3V (NOT 5V): its built-in 10k pull-ups on CLK
// and DT go to +, so at 3.3 V those lines can never exceed what the ESP32 can take.
// The push switch (SW) usually has no pull-up on the module, so the ESP32's internal one is used.
//
// Turning: CLK and DT are two switches that open and close a quarter-step apart (quadrature).
// An interrupt on every edge of either line looks up the old + new levels in a table to get
// +1 / -1 (or 0 for contact bounce), so no steps are lost even when the loop is busy.

#include "soc/gpio_reg.h"                      // GPIO_IN_REG: read both pins at once inside the interrupt.

const int ENCODER_CLK_PIN = 18;                // Module pin CLK (also called A).
const int ENCODER_DT_PIN = 19;                 // Module pin DT (also called B).
const int ENCODER_SW_PIN = 4;                  // Module pin SW (push switch, LOW when pressed).
const int ENCODER_EDGES_PER_CLICK = 4;         // Most KY-040s: 4 edges per click. If N only moves every 2nd click, use 2.
const bool ENCODER_REVERSE = false;            // Set true if clockwise counts down.
const unsigned long ENCODER_SW_DEBOUNCE_MS = 30;

// Indexed by (previous AB << 2) | new AB. Valid single moves give +1/-1, bounces/glitches give 0.
static const int8_t QUADRATURE_TABLE[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};

volatile int32_t encoderEdges = 0;             // Running total of valid edges (+ one way, - the other).
volatile uint8_t encoderLastAB = 0;            // CLK/DT levels at the previous edge.
portMUX_TYPE encoderMux = portMUX_INITIALIZER_UNLOCKED;  // Guards encoderEdges between the ISR and loop().

void IRAM_ATTR encoderIsr() {
  uint32_t levels = REG_READ(GPIO_IN_REG);     // Both pins sampled at the same instant.
  uint8_t ab = (((levels >> ENCODER_CLK_PIN) & 1) << 1) | ((levels >> ENCODER_DT_PIN) & 1);
  portENTER_CRITICAL_ISR(&encoderMux);
  encoderEdges += QUADRATURE_TABLE[(encoderLastAB << 2) | ab];
  encoderLastAB = ab;
  portEXIT_CRITICAL_ISR(&encoderMux);
} // Completes one edge: adds +1, -1 or 0 to the edge count.

void encoderBegin() {
  pinMode(ENCODER_CLK_PIN, INPUT_PULLUP);      // Module has pull-ups too; the internal ones are a harmless backup.
  pinMode(ENCODER_DT_PIN, INPUT_PULLUP);
  pinMode(ENCODER_SW_PIN, INPUT_PULLUP);       // SW needs this one.
  encoderLastAB = (digitalRead(ENCODER_CLK_PIN) << 1) | digitalRead(ENCODER_DT_PIN);
  attachInterrupt(digitalPinToInterrupt(ENCODER_CLK_PIN), encoderIsr, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENCODER_DT_PIN), encoderIsr, CHANGE);
} // Completes encoder setup: pins, starting position, and interrupts on both lines.

int encoderTakeSteps() {
  static int32_t leftover = 0;                 // Edges that don't add up to a whole click yet.
  portENTER_CRITICAL(&encoderMux);
  int32_t edges = encoderEdges;                // Take everything counted so far...
  encoderEdges = 0;                            // ...and start counting again.
  portEXIT_CRITICAL(&encoderMux);
  leftover += edges;
  int steps = leftover / ENCODER_EDGES_PER_CLICK;  // Whole clicks only.
  leftover -= steps * ENCODER_EDGES_PER_CLICK;
  return ENCODER_REVERSE ? -steps : steps;
} // Completes turning edges into clicks: returns clicks since the last call (+ = clockwise).

bool encoderButtonPressed() {
  static bool lastLevel = HIGH;                // HIGH = not pressed (pull-up).
  static bool stableLevel = HIGH;
  static unsigned long changedAt = 0;
  bool level = digitalRead(ENCODER_SW_PIN);
  if (level != lastLevel) {                    // Still bouncing: restart the timer.
    lastLevel = level;
    changedAt = millis();
  }
  if (millis() - changedAt >= ENCODER_SW_DEBOUNCE_MS && level != stableLevel) {
    stableLevel = level;
    return stableLevel == LOW;                 // Report the press (HIGH -> LOW), not the release.
  }
  return false;
} // Completes a debounced knob-push check: true once per press.
