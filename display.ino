// Optional status displays on their own I2C bus (pins and on/off switch are in the unit block):
//   0.96" 128x64 SSD1306 OLED at 0x3C: unit, Wi-Fi, MQTT, R/G/B/W bars per zone, last command.
//   16x2 character LCD on a PCF8574 backpack (0x20-0x27 or 0x38-0x3F): rotating text pages.
// Either display may be missing; nothing here can stop the lights or MQTT from working.

const uint8_t OLED_ADDRESS = 0x3C; // Most 0.96" modules; some use 0x3D (change it here).
const uint32_t I2C_BUS_HZ = 100000; // The LCD backpack's PCF8574 is a 100 kHz part.
const uint32_t OLED_MIN_REDRAW_MS = 250; // At most four OLED redraws a second.
const uint32_t OLED_IDLE_REDRAW_MS = 1000; // Redraw at least once a second for network changes.
const uint32_t LCD_REFRESH_MS = 200; // How often the LCD rows are checked for changes.
const uint32_t LCD_PAGE_MS = 3000; // How long each LCD page stays up.

Adafruit_SSD1306 oled(128, 64, &Wire, -1); // Switches the bus to 400 kHz only during its own transfers.
LiquidCrystal_I2C *lcd = nullptr; // Created once its address is found.
bool oledFound = false;
char lastCommandText[17] = "none yet"; // Fits one LCD row.
char lcdShown[2][17] = {"", ""}; // What each LCD row shows now, to skip unchanged rewrites.
bool displayDirty = true; // Set when levels or the last command change.
uint32_t lastOledDraw = 0;
uint32_t lastLcdCheck = 0;
uint32_t lcdPageStarted = 0;
uint8_t lcdPage = 0;

bool i2cDevicePresent(uint8_t address){
    Wire.beginTransmission(address);
    return Wire.endTransmission() == 0; // 0 = the device acknowledged its address.
} // Completes one address probe.

void startDisplays(){
    if (!DISPLAYS_ENABLED) {
        return;
    }
    Wire.begin(DISPLAY_SDA_PIN, DISPLAY_SCL_PIN, I2C_BUS_HZ); // Later Wire.begin() calls keep these pins.
    Wire.setTimeOut(50); // A disconnected display can't stall the loop for long.

    if (i2cDevicePresent(OLED_ADDRESS)) {
        oledFound = oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS, true, false); // false: bus already started.
    }
    const uint8_t lcdAddresses[] = {0x27, 0x3F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E};
    for (uint8_t address : lcdAddresses) {
        if (address != OLED_ADDRESS && i2cDevicePresent(address)) {
            lcd = new LiquidCrystal_I2C(address, 16, 2);
            lcd->init(); // Its internal Wire.begin() keeps our pins because the bus is already running.
            lcd->backlight();
            Serial.printf("Status LCD found at 0x%02X\n", address);
            break;
        }
    }
    Serial.printf("Status OLED %s (SDA GPIO %u, SCL GPIO %u)\n", oledFound ? "found at 0x3C" : "not found",
                  DISPLAY_SDA_PIN, DISPLAY_SCL_PIN);
    if (!lcd) {
        Serial.println("Status LCD not found");
    }
    updateDisplays(); // Shows the boot screen straight away.
} // Completes display detection and setup.

void markDisplayDirty(){
    displayDirty = true;
} // Completes a request for an early redraw.

void setLastCommand(const char *text){
    snprintf(lastCommandText, sizeof(lastCommandText), "%s", text); // Cut to 16 characters.
    displayDirty = true;
} // Completes the "last command" update.

const char *mqttStatusText(){
    static char text[22];
    if (client.connected()) {
        return "online";
    }
    if (WiFi.status() != WL_CONNECTED) {
        return "waiting for WiFi";
    }
    snprintf(text, sizeof(text), "retry (state %d)", client.state()); // PubSubClient's reason code.
    return text;
} // Completes the MQTT status line.

void drawOled(){
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(0, 0);
    oled.printf("%d %s\n", cmdActVal, UNIT_LABEL); // Row 0: who am I.
    if (WiFi.status() == WL_CONNECTED) {
        oled.printf("WiFi %s\n", WiFi.localIP().toString().c_str()); // Row 1.
    } else {
        oled.print("WiFi connecting...\n");
    }
    oled.printf("MQTT %s\n", mqttStatusText()); // Row 2.

    uint8_t row = 3;
    const uint8_t barX[CHANNELS_PER_ZONE] = {36, 59, 82, 105}; // Four 20-px bars, R G B W.
    if (ZONE_COUNT <= 3) { // Room for a header row of channel letters.
        const char letters[] = "RGBW";
        for (uint8_t channel = 0; channel < CHANNELS_PER_ZONE; channel++) {
            oled.setCursor(barX[channel] + 7, row * 8);
            oled.print(letters[channel]);
        }
        row++;
    }
    for (uint8_t zone = 0; zone < ZONE_COUNT && row < 7; zone++, row++) {
        oled.setCursor(0, row * 8);
        oled.printf("%.5s", ZONE_NAMES[zone]); // Zone name, up to 5 characters.
        for (uint8_t channel = 0; channel < CHANNELS_PER_ZONE; channel++) {
            oled.drawRect(barX[channel], row * 8 + 1, 20, 6, SSD1306_WHITE); // Bar outline.
            const uint8_t fill = analogLevels[zone][channel] * 18 / 255; // 0-18 px inside the outline.
            oled.fillRect(barX[channel] + 1, row * 8 + 2, fill, 4, SSD1306_WHITE);
        }
    }
    oled.setCursor(0, 56);
    oled.printf("Last %s", lastCommandText); // Row 7.
    oled.display(); // Sends the frame buffer.
} // Completes one OLED frame.

void lcdRow(uint8_t row, const char *text){
    char padded[17];
    snprintf(padded, sizeof(padded), "%-16.16s", text); // Exactly 16 characters, so old text is erased.
    if (strcmp(padded, lcdShown[row]) != 0) {
        lcd->setCursor(0, row);
        lcd->print(padded);
        strcpy(lcdShown[row], padded); // Only changed rows are rewritten, avoiding flicker.
    }
} // Completes one LCD row.

void drawLcd(){
    char top[24];
    char bottom[24];
    const uint8_t pageCount = 2 + ZONE_COUNT; // Network, one per zone, last command.
    if (lcdPage == 0) {
        if (WiFi.status() == WL_CONNECTED) {
            snprintf(top, sizeof(top), "IP %s", WiFi.localIP().toString().c_str());
        } else {
            snprintf(top, sizeof(top), "WiFi connecting");
        }
        snprintf(bottom, sizeof(bottom), "MQTT %s", mqttStatusText());
    } else if (lcdPage <= ZONE_COUNT) {
        const uint8_t zone = lcdPage - 1;
        snprintf(top, sizeof(top), "Z%u %s", zone + 1, ZONE_NAMES[zone]);
        snprintf(bottom, sizeof(bottom), "R%03uG%03uB%03uW%03u", analogLevels[zone][RED_CHANNEL],
                 analogLevels[zone][GREEN_CHANNEL], analogLevels[zone][BLUE_CHANNEL], analogLevels[zone][WHITE_CHANNEL]);
    } else {
        snprintf(top, sizeof(top), "Last command:");
        snprintf(bottom, sizeof(bottom), "%s", lastCommandText);
    }
    lcdRow(0, top);
    lcdRow(1, bottom);
    if (millis() - lcdPageStarted >= LCD_PAGE_MS) {
        lcdPage = (lcdPage + 1) % pageCount; // Moves to the next page.
        lcdPageStarted = millis();
    }
} // Completes one LCD check.

void updateDisplays(){
    if (!DISPLAYS_ENABLED) {
        return;
    }
    const uint32_t now = millis();
    if (oledFound && now - lastOledDraw >= OLED_MIN_REDRAW_MS && (displayDirty || now - lastOledDraw >= OLED_IDLE_REDRAW_MS)) {
        drawOled();
        lastOledDraw = now;
        displayDirty = false;
    }
    if (lcd && now - lastLcdCheck >= LCD_REFRESH_MS) {
        drawLcd();
        lastLcdCheck = now;
    }
} // Completes a rate-limited refresh of whichever displays are connected.
