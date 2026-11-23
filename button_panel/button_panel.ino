/*********
Button Panel (boxchat unit 32006)
ESP32 classic + LED&KEY (TM1638)

Normal mode:
  S1: Living Room Lights (32004) off
  S2: Living Room Lights (32004) on, full white on both zones
  S3 / S4 / S5: build a colour for All / Left / Right
Building a colour asks for red, then green, then blue ("rEd A---"); answer each with S1-S8
(S1 = 0 ... S8 = 255 in eight even steps). Each answer is sent at once (red also switches that
section's white off) and shown on the LEDs in binary. At the end the display shows the colour as
hex ("L FF2449") before returning to the idle readout.
When idle the display shows white on/off (1/0), the side (A = both, L, r) and RRGGBB in hex, from 32004's
retained state (Left/Right alternate when they differ). LED 0 = lights off, LED 1 = lights on.
*********/

#include <WiFi.h>
#include <PubSubClient.h>

// LED&KEY wiring (ESP32 classic GPIOs that are not boot-strapping, flash or input-only pins).
const uint8_t TM_STB_PIN = 25;
const uint8_t TM_CLK_PIN = 26;
const uint8_t TM_DIO_PIN = 27;

// Wi-Fi and MQTT (same network and broker as the MrBox units).
const char *ssid = "test";
const char *password = "classytulip154";
const char *mqtt_broker = "192.168.1.253";
const int mqtt_port = 1883;

const int PANEL_ID = 32006; // This panel's boxchat number (claimed in the NAS registry).
const char *const STATUS_TOPIC = "boxchat/32006/status"; // online / offline (last-will).
const char *const TARGET_SET_TOPIC = "boxchat/32004/set"; // Living Room Lights, direct commands.
const char *const TARGET_STATE_TOPIC = "boxchat/32004/state"; // Retained levels JSON from the unit.

// "On" = full white on both zones with red/green/blue cleared; "Off" = the all-off command 0.
const char *const LIGHTS_ON_COMMANDS[] = {"1000", "2000", "3000", "4255", "5000", "6000", "7000", "8255"};

// Colour-building targets. Zones follow the boxchat standard: zone z, channel c (R0 G1 B2 W3)
// uses command block 4 * (z - 1) + c + 1, so Left R/G/B/W = 1/2/3/4 and Right = 5/6/7/8.
struct Target {
    uint8_t button; // 0-based button index (S3 = 2).
    char side; // Display letter: A = All, L = Left, r = Right (7-segment friendly).
    uint8_t firstZone, lastZone; // Zones 1-2 this target covers.
};
const Target TARGETS[] = {{2, 'A', 1, 2}, {3, 'L', 1, 1}, {4, 'r', 2, 2}};
const char *const COLOUR_NAMES[3] = {"rEd", "Grn", "bLuE"}; // Left 4 digits while asking.
const uint8_t LEVELS[8] = {0, 36, 73, 109, 146, 182, 219, 255}; // S1-S8 -> round(n * 255 / 7).

const uint32_t BUTTON_POLL_MS = 20; // Key scan rate; also debounces.
const uint32_t MQTT_RETRY_MS = 5000;
const uint32_t STEP_TIMEOUT_MS = 5000; // A question left unanswered this long cancels the rest.
const uint32_t RESULT_SHOW_MS = 3000; // How long the hex summary stays before the on/off display returns.

WiFiClient espClient;
PubSubClient client(espClient);
String clientId;
uint8_t lastButtons = 0;
uint32_t lastPoll = 0;
uint32_t lastMqttTry = 0;
int lightsState = -1; // -1 unknown, 0 off, 1 on (from 32004's state).
uint8_t zoneLevels[2][4] = {}; // Left and Right r, g, b, w from 32004's state.
uint8_t zonesKnown = 0; // How many zones the last state message reported (0 = nothing yet).
uint8_t idleZone = 0; // Which zone the idle display shows when Left and Right differ.
uint32_t idlePageStarted = 0;
const uint32_t IDLE_PAGE_MS = 3000; // Left/Right alternate this often when they differ.
int targetIndex = -1; // Index into TARGETS while building a colour, else -1.
uint8_t step = 0; // 0 = asking red, 1 = green, 2 = blue.
uint8_t chosen[3] = {}; // Levels picked so far in this sequence.
uint32_t modeStarted = 0; // When the current question or result display began.
bool showingResult = false; // True while the hex summary (or "no nEt") is on the display.

uint8_t ledsForByte(uint8_t value){
    uint8_t mask = 0;
    for (uint8_t bit = 0; bit < 8; bit++) {
        if (value & (0x80 >> bit)) {
            mask |= 1 << bit; // LED 0 (leftmost) shows the top bit, LED 7 the lowest.
        }
    }
    return mask;
} // Completes the byte-to-LED mapping, e.g. 0xB6 -> on off on on off on on off.

bool zonesMatch(){
    return zonesKnown < 2 || memcmp(zoneLevels[0], zoneLevels[1], 4) == 0;
} // Completes the check for "both sides show the same colour".

void showStatus(){
    tmSetLeds(lightsState == 1 ? 0b10 : lightsState == 0 ? 0b01 : 0); // LED 0 = off, LED 1 = on.
    if (zonesKnown == 0) {
        tmShowText("    ----"); // 32004's state hasn't arrived yet.
        return;
    }
    // Idle readout: white on (1) / off (0), side (A = both the same, L, r), then RRGGBB in hex.
    const uint8_t zone = zonesMatch() ? 0 : idleZone;
    const char side = zonesMatch() ? (zonesKnown == 2 ? 'A' : 'L') : (zone == 0 ? 'L' : 'r');
    const uint8_t *levels = zoneLevels[zone];
    char text[9];
    snprintf(text, sizeof(text), "%c%c%02X%02X%02X", levels[3] ? '1' : '0', side, levels[0], levels[1], levels[2]);
    tmShowText(text); // e.g. "1AFFFFFF" or "0LFF006D".
} // Completes the idle status display.

void setLightsState(int state){
    lightsState = state;
    if (targetIndex < 0 && !showingResult) {
        showStatus(); // A question or result on screen keeps priority; the status returns afterwards.
    }
} // Completes a lights-state update.

void onMessage(char *topic, byte *payload, unsigned int length){
    if (strcmp(topic, TARGET_STATE_TOPIC) != 0) {
        return;
    }
    // The state JSON's numbers come in a fixed order: top-level r,g,b,w (= zone 1), then each zone's
    // r,g,b,w from the "zones" list, e.g. {"r":1,"g":2,"b":3,"w":4,"zones":[{...},{...}]}.
    uint8_t numbers[12];
    uint8_t count = 0;
    long value = 0;
    bool inNumber = false;
    for (unsigned int i = 0; i <= length && count < 12; i++) {
        const char c = i < length ? payload[i] : ' ';
        if (c >= '0' && c <= '9') {
            value = value * 10 + (c - '0');
            inNumber = true;
        } else if (inNumber) {
            numbers[count++] = (uint8_t)constrain(value, 0, 255);
            value = 0;
            inNumber = false;
        }
    }
    if (count < 4) {
        return; // Not a levels message.
    }
    zonesKnown = count >= 12 ? 2 : 1; // Older single-zone firmware sends only the first four.
    const uint8_t first = count >= 12 ? 4 : 0; // Skip the top-level copy of zone 1 when "zones" is present.
    bool anyLit = false;
    for (uint8_t zone = 0; zone < zonesKnown; zone++) {
        for (uint8_t channel = 0; channel < 4; channel++) {
            zoneLevels[zone][channel] = numbers[first + zone * 4 + channel];
            anyLit |= zoneLevels[zone][channel] > 0;
        }
    }
    setLightsState(anyLit ? 1 : 0);
} // Completes one state update from 32004.

bool publishCommand(const char *command){
    if (!client.connected()) {
        tmShowText("no nEt  "); // Nothing was sent; the display says why.
        return false;
    }
    client.publish(TARGET_SET_TOPIC, command);
    return true;
} // Completes one command publish.

bool publishLevel(uint8_t zone, uint8_t channel, uint8_t level){
    char command[8];
    snprintf(command, sizeof(command), "%u", (4 * (zone - 1) + channel + 1) * 1000 + level); // e.g. 5255 = right red 255.
    return publishCommand(command);
} // Completes one zone-standard channel command.

void sendLights(bool on){
    if (on) {
        for (const char *command : LIGHTS_ON_COMMANDS) {
            if (!publishCommand(command)) {
                return;
            }
        }
    } else if (!publishCommand("0")) {
        return;
    }
    Serial.printf("Button: Living Room %s\n", on ? "on" : "off");
    setLightsState(on ? 1 : 0); // Shown at once; 32004's state message confirms it moments later.
} // Completes an on/off button action.

void askStep(){
    char text[9];
    snprintf(text, sizeof(text), "%-4s%c---", COLOUR_NAMES[step], TARGETS[targetIndex].side); // e.g. "Grn A---".
    tmShowText(text);
    modeStarted = millis(); // Each question gets its own timeout.
} // Completes one question on the display.

void startTarget(int index){
    targetIndex = index;
    step = 0;
    showingResult = false;
    tmSetLeds(1 << TARGETS[index].button); // The chosen target's LED until the first answer.
    askStep();
} // Completes the start of a colour sequence.

void cancelSequence(bool showNoNet){
    targetIndex = -1;
    if (showNoNet) {
        showingResult = true; // Keeps "no nEt" up briefly before the status returns.
        modeStarted = millis();
    } else {
        showStatus();
    }
} // Completes leaving a sequence early.

void answerStep(uint8_t levelButton){
    const Target &target = TARGETS[targetIndex];
    const uint8_t level = LEVELS[levelButton];
    chosen[step] = level;
    for (uint8_t zone = target.firstZone; zone <= target.lastZone; zone++) {
        if (step == 0 && !publishLevel(zone, 3, 0)) { // Red also switches white off, so the colour isn't washed out.
            cancelSequence(true);
            return;
        }
        if (!publishLevel(zone, step, level)) {
            cancelSequence(true);
            return;
        }
    }
    Serial.printf("Button: %s %c -> %u\n", COLOUR_NAMES[step], target.side, level);
    tmSetLeds(ledsForByte(level)); // The answer in binary: left four LEDs = high hex digit, right four = low.
    if (step < 2) {
        step++;
        askStep(); // On to green, then blue.
        return;
    }
    char text[9];
    snprintf(text, sizeof(text), "%c %02X%02X%02X", target.side, chosen[0], chosen[1], chosen[2]); // e.g. "L FF2449".
    tmShowText(text);
    targetIndex = -1;
    showingResult = true;
    modeStarted = millis();
} // Completes one answer; after blue, shows the colour as hex.

void handlePress(uint8_t button){
    if (targetIndex >= 0) {
        answerStep(button); // While building a colour, every button (S1-S8) is a level key.
        return;
    }
    showingResult = false;
    if (button == 0) {
        sendLights(false); // S1: off.
    } else if (button == 1) {
        sendLights(true); // S2: on.
    } else {
        for (int i = 0; i < (int)(sizeof(TARGETS) / sizeof(TARGETS[0])); i++) {
            if (TARGETS[i].button == button) {
                startTarget(i); // S3 / S4 / S5: All / Left / Right.
                return;
            }
        }
        showStatus(); // S6-S8 have no job outside a sequence.
    }
} // Completes one button press.

void connectMqtt(){
    tmShowText("Conn    ");
    if (client.connect(clientId.c_str(), STATUS_TOPIC, 0, true, "offline")) {
        client.publish(STATUS_TOPIC, "online", true);
        client.subscribe(TARGET_STATE_TOPIC); // The retained state arrives immediately.
        Serial.println("MQTT connected");
        showStatus();
    } else {
        Serial.printf("MQTT connect failed, state=%d\n", client.state());
        tmShowText("Err     ");
    }
} // Completes one MQTT connection attempt.

void setup(){
    Serial.begin(115200);
    tmBegin();
    tmShowText("boot    ");
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    tmShowText("UUiFi   "); // Shown while joining Wi-Fi.
    while (WiFi.status() != WL_CONNECTED) {
        delay(250);
    }
    Serial.printf("WiFi connected. IP: %s\n", WiFi.localIP().toString().c_str());
    startOTA();  // Before MQTT, so the panel stays reflashable over Wi-Fi even if the broker is down.
    clientId = "ButtonPanel-" + WiFi.macAddress();
    clientId.replace(":", "");
    client.setServer(mqtt_broker, mqtt_port);
    client.setCallback(onMessage);
    connectMqtt();
} // Completes hardware, Wi-Fi and MQTT start-up.

void loop(){
    if (!client.connected() && millis() - lastMqttTry >= MQTT_RETRY_MS) {
        lastMqttTry = millis();
        connectMqtt(); // Non-blocking retry, so the buttons keep working meanwhile.
    }
    client.loop();
    handleOTA(); // Services ArduinoOTA pushes and the ElegantOTA web page.

    if (targetIndex >= 0 && millis() - modeStarted >= STEP_TIMEOUT_MS) {
        cancelSequence(false); // Unanswered question: stop here; colours already sent stay.
    }
    if (showingResult && millis() - modeStarted >= RESULT_SHOW_MS) {
        showingResult = false;
        showStatus();
    }
    if (targetIndex < 0 && !showingResult && !zonesMatch() && millis() - idlePageStarted >= IDLE_PAGE_MS) {
        idlePageStarted = millis();
        idleZone ^= 1; // Alternates the idle readout between Left and Right.
        showStatus();
    }

    if (millis() - lastPoll >= BUTTON_POLL_MS) {
        lastPoll = millis();
        const uint8_t buttons = tmReadButtons();
        const uint8_t pressed = buttons & ~lastButtons; // Acts once per press, not while held.
        lastButtons = buttons;
        for (uint8_t button = 0; button < 8; button++) {
            if (pressed & (1 << button)) {
                handlePress(button);
                break; // One press per scan keeps each answer unambiguous.
            }
        }
    }
} // Completes MQTT servicing, question timeouts and button handling.
