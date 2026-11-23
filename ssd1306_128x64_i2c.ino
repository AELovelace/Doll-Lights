/*********
MrBox V2
ESP32 Style
*********/

#include <WiFi.h>
#include <PubSubClient.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <Wire.h> // Optional status displays share one I2C bus (display.ino).
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h> // 0.96" 128x64 OLED (e.g. Elegoo), I2C address 0x3C.
#include <LiquidCrystal_I2C.h> // 16x2 character LCD on a PCF8574 I2C backpack (0x27/0x3F).
// No NeoPixel library: led.ino drives each zone's WS2814F (Dig2Analog) directly with the ESP32 core's
// RMT API. Adafruit_NeoPixel's default constructor never initialises its ESP32 send lock (frames were
// silently dropped), and it shares one RMT pin between all strips, re-creating it on every zone switch.

// Each lighting zone is one QuinLED-Dig-Quad LED output wired to its own QuinLED-Dig2Analog.
// One converter behaves as one addressable RGBW pixel; its MOSFET outputs drive that zone's strip.
// The LED output GPIOs depend on the Dig-Quad revision, so check the board before adding a unit:
//   v2/v3 (QuinLED-ESP32, WROOM-style): LED1 = GPIO16, LED2 = GPIO3 (RX), LED3 = GPIO1 (TX), LED4 = GPIO4.
//   v4 (QuinLED ESP32 v4 = ESP32-PICO-V3-02 with 8MB flash + 2MB PSRAM): LED1 = GPIO2, LED2 = GPIO4,
//      LED3 = GPIO12, LED4 = GPIO14. GPIO16/17 belong to the built-in PSRAM there; driving them hangs the
//      chip (watchdog reset loop), which is how the first 32004 flash failed.

// Pick which physical unit this build is for. Each unit has its own activator number and zone layout.
#define BUILD_FOR_UNIT 32004

#if BUILD_FOR_UNIT == 32004
// 32004: the left/right room unit on a Dig-Quad v4. Two Dig2Analog converters, one per output.
const uint8_t ZONE_COUNT = 2; // The boxchat standard allows 1-4 zones.
const uint8_t ZONE_DATA_PINS[] = {2, 4}; // Zone 1 (left) on v4 LED1 = GPIO2, zone 2 (right) on v4 LED2 = GPIO4.
const char *const ZONE_NAMES[] = {"Left", "Right"}; // Serial Monitor labels, in zone order.
const char *const UNIT_LABEL = "Living Room"; // Shown on the status displays.
// Status displays (0.96" OLED + 16x2 LCD) on their own I2C bus, on v4 header pins Q2/Q3. These are
// bidirectional and not boot-strapping pins, and they avoid Q1 (GPIO32 is shared with the Q1R relay
// output) and GPIO1/3 (USB serial). Missing displays are skipped at boot.
const bool DISPLAYS_ENABLED = true;
const uint8_t DISPLAY_SDA_PIN = 33; // Dig-Quad v4 header Q2.
const uint8_t DISPLAY_SCL_PIN = 13; // Dig-Quad v4 header Q3.
#elif BUILD_FOR_UNIT == 32001
// 32001: the original single-zone RGBW Light (Dig-Quad v2/v3). One Dig2Analog on LED1 = GPIO16.
const uint8_t ZONE_COUNT = 1;
const uint8_t ZONE_DATA_PINS[] = {16};
const char *const ZONE_NAMES[] = {"Main"};
const char *const UNIT_LABEL = "RGBW Light";
const bool DISPLAYS_ENABLED = false; // No displays fitted; the pins below are unused.
const uint8_t DISPLAY_SDA_PIN = 0;
const uint8_t DISPLAY_SCL_PIN = 0;
#else
#error "Add a zone layout for this BUILD_FOR_UNIT (and its row in the README and the NAS boxchat reference)"
#endif

static_assert(ZONE_COUNT >= 1 && ZONE_COUNT <= 4, "the boxchat protocol defines zones 1 through 4");
static_assert(sizeof(ZONE_DATA_PINS) / sizeof(ZONE_DATA_PINS[0]) == ZONE_COUNT, "one data pin per zone");
static_assert(sizeof(ZONE_NAMES) / sizeof(ZONE_NAMES[0]) == ZONE_COUNT, "one name per zone");

const uint16_t DIG2ANALOG_BOARD_COUNT = 1; // Converters per zone; extra daisy-chained ones mirror the first.
// Each converter is one 800 kHz SK6812-style RGBW pixel. WLED's "BRG plus W/G swap" setting for the
// WS2814F resolves to the byte order W, R, G, B on the wire, which showZone() sends.
const bool RUN_STARTUP_LIGHT_TEST = true;
bool zoneOutputReady[ZONE_COUNT] = {}; // Set by startPins() once a zone's RMT output exists.

enum AnalogChannel : uint8_t {
    RED_CHANNEL = 0,
    GREEN_CHANNEL = 1,
    BLUE_CHANNEL = 2,
    WHITE_CHANNEL = 3
}; // Names the four Dig2Analog outputs inside each zone instead of using bare numbers.

const uint8_t CHANNELS_PER_ZONE = 4; // R, G, B and W: four command blocks of 1000 per zone.
uint8_t analogLevels[ZONE_COUNT][CHANNELS_PER_ZONE] = {}; // Retains every zone's channels when one MQTT value changes.

//wifi
const char* ssid = "test";
const char* password = "classytulip154";

// MQTT Broker
const char *mqtt_broker = "192.168.1.253";
const char *mqtt_command_topic = "boxchat";
const int mqtt_port = 1883;
const uint16_t MQTT_BUFFER_SIZE = 512; // Room for the per-zone state JSON of up to four zones.
String client_id;
String mqtt_direct_topic;
String mqtt_status_topic;
String mqtt_state_topic;
const int cmdActVal = BUILD_FOR_UNIT; // Unique activator above 32000; chosen by BUILD_FOR_UNIT above.
bool cmdAct = false;

WiFiClient espClient;
PubSubClient client(espClient);

void setup() {
    Serial.begin(115200); // Starts before startPins() so a v2/v3 LED2 strip can take over GPIO3 (RX) afterwards.
    startPins();
    startDisplays(); // Finds whichever status displays are connected; skips any that are missing.
    if (RUN_STARTUP_LIGHT_TEST) {
        startupLightTest(); // Proves every zone's Dig-Quad data path before Wi-Fi or MQTT can block startup.
    }
    startWiFi();
    startOTA();  // Before MQTT, so the board stays reflashable over Wi-Fi even if the broker is down.
    startMQTT();
} // Completes hardware diagnostics and network service initialization.

void loop() {
    if (!client.connected()) {
        reconnectMQTT();
    }
    client.loop();
    handleOTA(); // Services ArduinoOTA pushes and the ElegantOTA web page every loop.
    updateDisplays(); // Rate-limited inside, so calling it every loop is cheap.
} // Completes MQTT servicing, reconnect handling and display refresh.
