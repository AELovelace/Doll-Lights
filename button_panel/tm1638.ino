// Minimal driver for the "LED&KEY" board: one TM1638 chip with 8 seven-segment digits, 8 LEDs and
// 8 buttons, talking over STB / CLK / DIO. Protocol: STB low starts a frame, bytes go LSB first,
// the chip reads DIO on each rising CLK edge; display RAM is 16 bytes (even = digit segments,
// odd = the LED under that digit).

const uint8_t TM_CMD_WRITE_AUTO = 0x40; // Data command: write, auto-increment address.
const uint8_t TM_CMD_READ_KEYS = 0x42; // Data command: read the key scan.
const uint8_t TM_CMD_ADDRESS = 0xC0; // Address command, + 0-15.
const uint8_t TM_CMD_DISPLAY_ON = 0x88; // Display control: on, + brightness 0-7.
const uint8_t TM_BRIGHTNESS = 3; // 0 (dim) to 7 (bright).
const uint8_t TM_DIGITS = 8;

uint8_t tmSegments[TM_DIGITS] = {}; // What each digit shows (segment bits a-g, dp).
uint8_t tmLeds = 0; // Bit n = LED n lit.

void tmWriteByte(uint8_t value){
    for (uint8_t bit = 0; bit < 8; bit++) {
        digitalWrite(TM_CLK_PIN, LOW);
        digitalWrite(TM_DIO_PIN, (value >> bit) & 1); // Least significant bit first.
        delayMicroseconds(1); // The ESP32 is faster than the TM1638's 1 MHz limit.
        digitalWrite(TM_CLK_PIN, HIGH); // The chip samples DIO on this rising edge.
        delayMicroseconds(1);
    }
} // Completes one byte out.

void tmCommand(uint8_t command){
    digitalWrite(TM_STB_PIN, LOW);
    tmWriteByte(command);
    digitalWrite(TM_STB_PIN, HIGH);
} // Completes a one-byte command frame.

void tmFlush(){
    tmCommand(TM_CMD_WRITE_AUTO);
    digitalWrite(TM_STB_PIN, LOW);
    tmWriteByte(TM_CMD_ADDRESS); // Start at address 0 and stream all 16 bytes.
    for (uint8_t digit = 0; digit < TM_DIGITS; digit++) {
        tmWriteByte(tmSegments[digit]); // Even address: digit segments.
        tmWriteByte((tmLeds >> digit) & 1); // Odd address: the LED under that digit.
    }
    digitalWrite(TM_STB_PIN, HIGH);
} // Completes a full display + LED refresh.

uint8_t tmGlyph(char c){
    switch (c) {
        case '0': case 'O': return 0x3F;
        case '1': return 0x06; case '2': return 0x5B; case '3': return 0x4F; case '4': return 0x66;
        case '5': case 'S': return 0x6D; case '6': return 0x7D; case '7': return 0x07; case '8': return 0x7F;
        case '9': return 0x6F; case 'A': return 0x77; case 'b': case 'B': return 0x7C; case 'C': return 0x39;
        case 'c': return 0x58; case 'd': case 'D': return 0x5E; case 'E': return 0x79; case 'F': return 0x71;
        case 'G': return 0x3D;
        case 'H': return 0x76; case 'I': return 0x30; case 'L': return 0x38; case 'n': return 0x54;
        case 'o': return 0x5C; case 'P': return 0x73; case 'r': return 0x50; case 't': return 0x78;
        case 'U': return 0x3E; case 'u': return 0x1C; case 'y': return 0x6E; case '-': return 0x40;
        default: return 0x00; // Space and anything without a sensible 7-segment shape.
    }
} // Completes the character-to-segments lookup.

void tmShowText(const char *text){
    for (uint8_t digit = 0; digit < TM_DIGITS; digit++) {
        tmSegments[digit] = (*text) ? tmGlyph(*text++) : 0x00; // Pads short text with blanks.
    }
    tmFlush();
} // Completes an 8-character message on the display.

void tmSetLeds(uint8_t mask){
    tmLeds = mask;
    tmFlush();
} // Completes an LED update.

uint8_t tmReadButtons(){
    uint8_t buttons = 0;
    digitalWrite(TM_STB_PIN, LOW);
    tmWriteByte(TM_CMD_READ_KEYS);
    pinMode(TM_DIO_PIN, INPUT_PULLUP); // The chip drives DIO open-drain while we read.
    delayMicroseconds(2); // Required wait between the read command and the first data bit.
    for (uint8_t byteIndex = 0; byteIndex < 4; byteIndex++) {
        uint8_t value = 0;
        for (uint8_t bit = 0; bit < 8; bit++) {
            digitalWrite(TM_CLK_PIN, LOW);
            delayMicroseconds(1);
            digitalWrite(TM_CLK_PIN, HIGH);
            delayMicroseconds(1);
            value |= digitalRead(TM_DIO_PIN) << bit; // Data is valid while CLK is high.
        }
        buttons |= value << byteIndex; // LED&KEY wiring: byte n holds S(n+1) in bit 0 and S(n+5) in bit 4.
    }
    pinMode(TM_DIO_PIN, OUTPUT);
    digitalWrite(TM_STB_PIN, HIGH);
    return buttons; // Bit 0 = S1 (button 0) ... bit 7 = S8.
} // Completes one key scan.

void tmBegin(){
    pinMode(TM_STB_PIN, OUTPUT);
    pinMode(TM_CLK_PIN, OUTPUT);
    pinMode(TM_DIO_PIN, OUTPUT);
    digitalWrite(TM_STB_PIN, HIGH); // Idle: no frame.
    digitalWrite(TM_CLK_PIN, HIGH); // Idle: clock high.
    tmCommand(TM_CMD_DISPLAY_ON | TM_BRIGHTNESS);
    tmShowText("");
    tmSetLeds(0);
} // Completes chip setup with a blank display and all LEDs off.
