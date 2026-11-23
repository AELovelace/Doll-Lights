// MAX7219 dot-matrix display: two 8x8 blocks in a row = 16 x 8 pixels, raw pixel control.
//
// Wiring (5 V part, so it belongs on the Mega): VCC -> 5V, GND -> GND,
//   DIN -> pin 51 (MOSI), CLK -> pin 52 (SCK), CS -> pin 53. Uses the Mega's hardware SPI.
// Power: every lit LED draws current from the Mega's 5V. Keep brightness low (default 2 of 15)
// when the Mega runs from USB; for full-brightness use, feed the module's VCC from a 5 V supply.
//
// The picture lives in matrixRows[]: row 0 = top, bit 15 = leftmost pixel (x 0), bit 0 = x 15.
// Commands (from the ESP32, see handleLine()):
//   MXCLR                 all pixels off
//   MXBRI <0-15>          brightness
//   MXPIX <x> <y> <0|1>   one pixel (x 0-15 left->right, y 0-7 top->bottom)
//   MXROW <y> <hhhh>      one row as 4 hex digits, leftmost pixel = top bit (e.g. "MXROW 0 F00F")
//   MXALL <32 hex>        whole picture: rows 0-7, 4 hex digits each
//   MXTEST                orientation test pattern (see README "Dot-matrix orientation")
//
// Modules are wired differently between makers. If the test pattern looks wrong, change the four
// MATRIX_ switches below (the README says which one fixes what) and reflash.

#include <SPI.h>

const uint8_t MATRIX_CS_PIN = 53;              // Chip select (LOW while sending).
const uint8_t MATRIX_BLOCKS = 2;               // 8x8 blocks in the chain.
const uint8_t MATRIX_DEFAULT_BRIGHTNESS = 2;   // 0-15. Low keeps USB power happy.
const bool MATRIX_FIRST_IS_LEFT = true;        // Is the block next to the DIN pins the LEFT one? (true on Doll's module, checked 2026-11-22.)
const bool MATRIX_TRANSPOSE = false;           // Turn rows into columns (for blocks mounted 90 degrees round).
const bool MATRIX_FLIP_X = false;              // Mirror each block left <-> right.
const bool MATRIX_FLIP_Y = false;              // Mirror each block top <-> bottom.

// MAX7219 register addresses.
const uint8_t MAX7219_DECODE = 0x09, MAX7219_INTENSITY = 0x0A, MAX7219_SCAN_LIMIT = 0x0B,
              MAX7219_SHUTDOWN = 0x0C, MAX7219_DISPLAY_TEST = 0x0F;

uint16_t matrixRows[8];                        // The picture (see top of file).

void matrixSend(uint8_t reg, const uint8_t perBlock[]) {
  SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  digitalWrite(MATRIX_CS_PIN, LOW);
  for (int8_t chip = MATRIX_BLOCKS - 1; chip >= 0; chip--) {  // Farthest chip first: data shifts through the chain.
    SPI.transfer(reg);
    SPI.transfer(perBlock[chip]);
  }
  digitalWrite(MATRIX_CS_PIN, HIGH);           // Rising CS = every chip takes its 16 bits.
  SPI.endTransaction();
} // Completes one register write to every chip (perBlock[0] = the chip next to DIN).

void matrixSendSame(uint8_t reg, uint8_t value) {
  uint8_t data[MATRIX_BLOCKS];
  for (uint8_t chip = 0; chip < MATRIX_BLOCKS; chip++) data[chip] = value;
  matrixSend(reg, data);
} // Completes writing the same setting to every chip.

bool matrixPixel(uint8_t x, uint8_t y) {
  return (matrixRows[y] >> (15 - x)) & 1;
} // Completes reading one pixel of the picture.

void matrixRefresh() {
  for (uint8_t digit = 0; digit < 8; digit++) {          // MAX7219 "digit" registers 1-8.
    uint8_t data[MATRIX_BLOCKS];
    for (uint8_t chip = 0; chip < MATRIX_BLOCKS; chip++) {
      uint8_t block = MATRIX_FIRST_IS_LEFT ? chip : (MATRIX_BLOCKS - 1 - chip);  // Which 8 columns it shows.
      uint8_t value = 0;
      for (uint8_t bit = 0; bit < 8; bit++) {            // Bit 7 = first LED of the digit.
        uint8_t cx = MATRIX_TRANSPOSE ? digit : 7 - bit;  // Column inside the block...
        uint8_t cy = MATRIX_TRANSPOSE ? 7 - bit : digit;  // ...and row.
        if (MATRIX_FLIP_X) cx = 7 - cx;
        if (MATRIX_FLIP_Y) cy = 7 - cy;
        if (matrixPixel(block * 8 + cx, cy)) value |= (1 << bit);
      }
      data[chip] = value;
    }
    matrixSend(digit + 1, data);
  }
} // Completes copying the picture to the chips, applying the orientation switches.

void matrixBegin() {
  pinMode(MATRIX_CS_PIN, OUTPUT);
  digitalWrite(MATRIX_CS_PIN, HIGH);           // Idle = not selected.
  SPI.begin();
  matrixSendSame(MAX7219_DISPLAY_TEST, 0);     // Normal operation (not all-LEDs-on test mode).
  matrixSendSame(MAX7219_DECODE, 0);           // Raw pixels, no 7-segment font.
  matrixSendSame(MAX7219_SCAN_LIMIT, 7);       // Use all 8 rows.
  matrixSendSame(MAX7219_INTENSITY, MATRIX_DEFAULT_BRIGHTNESS);
  matrixSendSame(MAX7219_SHUTDOWN, 1);         // Wake up.
  memset(matrixRows, 0, sizeof(matrixRows));
  matrixRefresh();                             // Start blank.
} // Completes chip setup: raw mode, all rows, low brightness, blank picture.

void matrixClear() {
  memset(matrixRows, 0, sizeof(matrixRows));
  matrixRefresh();
} // Completes blanking the whole display.

void matrixTestPattern() {
  memset(matrixRows, 0, sizeof(matrixRows));
  matrixRows[0] = 0xFC00;                      // Top row: x 0-5 (a long arm along the top-left)...
  for (uint8_t y = 0; y < 4; y++) matrixRows[y] |= 0x8000;  // ...left column: y 0-3 (a short arm down).
  matrixRows[7] |= 0x0001;                     // Single dot in the bottom-right corner (x 15, y 7).
  matrixRefresh();
} // Completes the orientation test: an upside-down "L" in the top-left corner plus a dot bottom-right.

bool parseHex16(const char *text, uint16_t &value) {
  if (!text || strlen(text) < 4) return false;
  value = 0;
  for (uint8_t i = 0; i < 4; i++) {
    char c = text[i];
    uint8_t nibble;
    if (c >= '0' && c <= '9') nibble = c - '0';
    else if (c >= 'A' && c <= 'F') nibble = c - 'A' + 10;
    else if (c >= 'a' && c <= 'f') nibble = c - 'a' + 10;
    else return false;
    value = (value << 4) | nibble;
  }
  return true;
} // Completes reading 4 hex digits ("F00F" -> 0xF00F) from the start of text.

bool matrixCommand(const char *command, char *arg1, char *arg2, char *arg3) {
  long x, y, value;
  uint16_t row;
  if (!strcmp(command, "MXCLR")) {
    matrixClear();
    return true;
  } else if (!strcmp(command, "MXBRI")) {
    if (!parseNumber(arg1, 0, 15, value)) { linkSendLine(F("ERR MXBRI needs 0-15")); return true; }
    matrixSendSame(MAX7219_INTENSITY, value);
    return true;                               // Brightness doesn't change the picture.
  } else if (!strcmp(command, "MXPIX")) {
    if (!parseNumber(arg1, 0, 15, x) || !parseNumber(arg2, 0, 7, y) || !parseNumber(arg3, 0, 1, value)) {
      linkSendLine(F("ERR MXPIX needs <x 0-15> <y 0-7> <0|1>"));
      return true;
    }
    uint16_t mask = (uint16_t)1 << (15 - x);
    if (value) matrixRows[y] |= mask; else matrixRows[y] &= ~mask;
  } else if (!strcmp(command, "MXROW")) {
    if (!parseNumber(arg1, 0, 7, y) || !arg2 || strlen(arg2) != 4 || !parseHex16(arg2, row)) {
      linkSendLine(F("ERR MXROW needs <y 0-7> <4 hex digits>"));
      return true;
    }
    matrixRows[y] = row;
  } else if (!strcmp(command, "MXALL")) {
    if (!arg1 || strlen(arg1) != 32) { linkSendLine(F("ERR MXALL needs 32 hex digits")); return true; }
    uint16_t rows[8];
    for (uint8_t r = 0; r < 8; r++) {
      if (!parseHex16(arg1 + r * 4, rows[r])) { linkSendLine(F("ERR MXALL needs 32 hex digits")); return true; }
    }
    memcpy(matrixRows, rows, sizeof(rows));    // Only replace the picture once all 32 digits were valid.
  } else if (!strcmp(command, "MXTEST")) {
    matrixTestPattern();
    return true;
  } else {
    return false;                              // Not a matrix command.
  }
  matrixRefresh();
  return true;
} // Completes one MX... command (true = it was a matrix command, handled or refused with ERR).
