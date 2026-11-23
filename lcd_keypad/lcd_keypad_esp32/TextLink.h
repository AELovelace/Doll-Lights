// TextLink: one serial link from the master ESP32 to another board (the Mega hub, the CYD face).
//
// Every link works the same way: 8N1, plain-text lines ending in '\n', speed set per link.
//   * The ESP32 transmits normally (3.3 V is HIGH enough for a 5 V Arduino).
//   * A 5 V Arduino transmits OPEN-DRAIN: it only ever pulls the wire LOW, and the pull-up that
//     begin() switches on here makes HIGH = 3.3 V. So the ESP32 never sees 5 V.
//     (A 3.3 V board like the CYD just transmits normally; the pull-up does no harm.)
//   * Both sides answer "PING" with "PO". service() pings every PING_MS and notices silence.
//   * An Arduino says "HI" when it starts, so the ESP32 can resend whatever it needs.

#pragma once

#include <Arduino.h>
#include "driver/gpio.h"                        // gpio_pullup_en(): pull-up without detaching the UART.

class TextLink {
 public:
  static const unsigned long PING_MS = 5000;     // Ask for a pong this often.
  static const unsigned long TIMEOUT_MS = 12000; // Silence this long = the other board is gone.
  static const size_t MAX_LINE = 64;             // Longer lines are cut (their tail is dropped).

  TextLink(const char *name, uint8_t uartNumber, int rxPin, int txPin, unsigned long baud)
      : name_(name), port_(uartNumber), rxPin_(rxPin), txPin_(txPin), baud_(baud) {}

  void begin() {
    port_.setTxBufferSize(1024);                  // Bursts (screen + matrix) queue up instead of stalling loop().
    port_.begin(baud_, SERIAL_8N1, rxPin_, txPin_);
    gpio_pullup_en((gpio_num_t)rxPin_);           // The open-drain line's HIGH comes from this 3.3 V pull-up.
    port_.print('\n');                            // End any half-line the other board got while we booted.
  } // Completes UART setup on the chosen pins, with the receive pull-up on.

  void sendLine(const String &line) {
    port_.print(line);
    port_.print('\n');
  } // Completes sending one command line.

  bool readLine(String &line) {
    while (port_.available()) {
      char c = (char)port_.read();
      if (c == '\r') continue;                    // Tolerate Windows-style line endings.
      if (c == '\n') {
        if (buffer_.length() == 0) continue;      // Skip empty lines.
        line = buffer_;
        buffer_ = "";
        heard();
        return true;
      }
      if (buffer_.length() < MAX_LINE) buffer_ += c;
    }
    return false;
  } // Completes reading: true (with `line` filled) each time a whole line has arrived.

  void service() {
    unsigned long now = millis();
    if (now - lastPing_ >= PING_MS) {
      sendLine("PING");                           // The other board answers "PO".
      lastPing_ = now;
    }
    if (alive_ && now - lastHeard_ > TIMEOUT_MS) {
      alive_ = false;
      Serial.printf("%s link lost (check wiring, GND and that it's powered)\n", name_);
    }
  } // Completes the keep-alive: call it every loop.

  bool alive() const { return alive_; }

  bool takeCameUp() {
    bool result = cameUp_;
    cameUp_ = false;
    return result;
  } // Completes the "link just came up" check: true once each time the other board appears.

 private:
  void heard() {
    lastHeard_ = millis();
    if (!alive_) {
      alive_ = true;
      cameUp_ = true;
      Serial.printf("%s link up\n", name_);
    }
  } // Completes marking the other board as present.

  const char *name_;
  HardwareSerial port_;
  int rxPin_;
  int txPin_;
  unsigned long baud_;                            // Link speed (both boards must match).
  String buffer_;                                 // Line being assembled.
  unsigned long lastPing_ = 0;
  unsigned long lastHeard_ = 0;
  bool alive_ = false;
  bool cameUp_ = false;
};
