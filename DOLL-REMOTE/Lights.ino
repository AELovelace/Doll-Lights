//   Lights.ino
//   Doll-Lights controller: drives the boxchat MQTT lights (C:\Scripts\Doll-Lights)
//   from the Cardputer keyboard. One of the two G0 apps: a G0 tap switches to the
//   remote and back; holding G0 3s (or Fn+Q) drops to the shell. "lights" opens it too.
//
//   Ported from Doll-Lights' master keypad console (lcd_keypad_esp32: Console.h and
//   lights.ino), so it behaves the same way:
//     - commands use the boxchat zone standard: zone z (1-4), channel c (R G B W =
//       0-3), level v -> (4*(z-1) + c + 1) * 1000 + v, published to boxchat/<id>/set;
//       "0" turns every zone of a board off
//     - relays get "1" / "0"; 32002 only listens on the shared "boxchat" topic, so it
//       is armed with its activator number first
//     - changes land on the local copy first (keys feel instant), then go out at most
//       every LIGHTS_PUBLISH_GAP_MS per light, only for channels that changed
//     - retained boxchat/<id>/state fills in the real colours; our own echoes are
//       ignored for LIGHTS_HOLD_MS so they can't yank a level backwards
//   The Cardputer only listens and commands: it publishes no status of its own, so it
//   needs no boxchat number from the NAS registry.
#include <PubSubClient.h>

WiFiClient lightsWifiClient;                          //plain TCP to the LAN broker (boxchat has no TLS)
PubSubClient lightsMqtt(lightsWifiClient);

const unsigned long LIGHTS_PUBLISH_GAP_MS = 50;       //per light: up to 20 updates a second while a key repeats
const unsigned long LIGHTS_HOLD_MS = 800;             //ignore state echoes this long after our last change
const unsigned long LIGHTS_RETRY_MS = 3000;           //broker reconnect cadence (each try is bounded to ~2s)
const int LIGHTS_STEP = 16;                           //level change per left/right press (Fn: 4 for fine)

//   Channels the left/right keys adjust. BRI scales the whole colour; R/G/B/W set one level.
enum { LCH_BRI = 0, LCH_R, LCH_G, LCH_B, LCH_W, LCH_COUNT };
static const char* const LIGHTS_CHANNEL_NAMES[LCH_COUNT] = { "BRI", "R", "G", "B", "W" };

//   Scenes, picked from the scene menu (key 1) by number or with the arrows. The list
//   grows as rooms get lighting: add a line here and it's in the menu, numbered in order.
//   The first four are Light Console's presets (the keypad console's A-D).
struct LightScene {
    const char* name;
    LightRgbw colour;           //R, G, B, W 0-255
};
static const LightScene LIGHTS_SCENES[] = {
    { "WHITE",  { { 0, 0, 0, 220 } } },
    { "SUNSET", { { 255, 72, 12, 0 } } },
    { "VIOLET", { { 166, 0, 255, 0 } } },
    { "AQUA",   { { 0, 180, 255, 0 } } },
};
static const int LIGHTS_SCENE_COUNT = sizeof(LIGHTS_SCENES) / sizeof(LIGHTS_SCENES[0]);
static const LightRgbw LIGHTS_DEFAULT_ON = { { 0, 0, 0, 220 } };   //"on" for a light never seen lit: white

//   The lights, one per board (room), in list order: the same boards as Light Console's
//   config.json. Add a room by adding a line; the list scrolls. A light's relay is its
//   master power supply. Fields: boxchat id, name, zones (1-4), relay id (0 = none),
//   relay on the shared topic, zone names (optional).
LightUnit lightUnits[] = {
    { 32001, "Doll-Office", 1, 32002, true },
    { 32003, "Mnemerynne",  1, 0,     false },
    { 32004, "Living Room", 2, 32007, false, { "Left", "Right" } },
    { 32005, "Cync Strip",  1, 0,     false },
};
const int LIGHTS_COUNT = sizeof(lightUnits) / sizeof(lightUnits[0]);

static int lightsSel = 0;                 //selected light (index into lightUnits)
static int lightsZone = 0;                //0 = every zone together, else zone 1..zones
static bool lightsMenu = false;           //scene menu open
static int lightsSceneCursor = 0;         //highlighted scene in the menu
static String lightsSceneEntry = "";      //number typed in the menu ("" = none), applied with Enter
static int lightsChannel = LCH_BRI;       //what left/right adjusts
static String lightsMessage = "";         //transient command-bar line
static unsigned long lightsMessageUntil = 0;
static unsigned long lightsLastTry = 0;   //last broker connect attempt

// --------------------------------------------------------------------- colour helpers

//how bright a colour is: its biggest channel
static uint8_t lightsMax(const LightRgbw& c) {
    return max(max(c.v[0], c.v[1]), max(c.v[2], c.v[3]));
}

//on/off check for one zone
static bool lightsLit(const LightRgbw& c) {
    return lightsMax(c) > 0;
}

//scales a colour so its biggest channel equals brightness (keeps the hue)
static LightRgbw lightsScaled(const LightRgbw& c, uint8_t brightness) {
    uint8_t top = lightsMax(c);
    LightRgbw out = { { 0, 0, 0, 0 } };
    if (top == 0) {
        return out;
    }
    for (int i = 0; i < 4; i++) {
        out.v[i] = (uint8_t)((c.v[i] * (uint16_t)brightness + top / 2) / top);
    }
    return out;
}

//RGBW -> the panel's RGB565, with white added to every channel so a white strip shows white
static uint16_t lightsSwatch(const LightRgbw& c) {
    int r = min(255, c.v[0] + c.v[3]);
    int g = min(255, c.v[1] + c.v[3]);
    int b = min(255, c.v[2] + c.v[3]);
    return M5Cardputer.Display.color565(r, g, b);
}

//0-based first/last zone the selection covers on this light
static int lightsZoneFirst(const LightUnit& l) {
    return lightsZone == 0 ? 0 : min(lightsZone, (int)l.zones) - 1;
}
static int lightsZoneLast(const LightUnit& l) {
    return lightsZone == 0 ? l.zones - 1 : min(lightsZone, (int)l.zones) - 1;
}

static void lightsSay(const String& text) {
    lightsMessage = text;
    lightsMessageUntil = millis() + 2500;
}

// --------------------------------------------------------------------- changing lights

//one local zone change; lightsPublish() sends it
static void lightsSetZone(LightUnit& l, int z, const LightRgbw& colour, bool newHue) {
    l.zone[z] = colour;
    if (newHue && lightsLit(colour)) {
        l.hue[z] = lightsScaled(colour, 255);   //remember the shape at full brightness
    }
    l.dirty[z] = true;
    l.known = true;
    l.holdUntil = millis() + LIGHTS_HOLD_MS;
}

//left/right: nudges the selected channel on the selected light/zone(s)
static void lightsAdjust(int delta) {
    LightUnit& l = lightUnits[lightsSel];
    for (int z = lightsZoneFirst(l); z <= lightsZoneLast(l); z++) {
        LightRgbw c = l.zone[z];
        if (lightsChannel == LCH_BRI) {
            int bright = constrain((int)lightsMax(c) + delta, 0, 255);
            if (!lightsLit(l.hue[z])) {
                l.hue[z] = lightsScaled(LIGHTS_DEFAULT_ON, 255);   //never lit: start from white
            }
            lightsSetZone(l, z, lightsScaled(l.hue[z], bright), false);   //same hue, new brightness
        } else {
            int idx = lightsChannel - LCH_R;                      //R..W -> 0..3
            c.v[idx] = constrain((int)c.v[idx] + delta, 0, 255);
            lightsSetZone(l, z, c, true);
        }
    }
}

//the selection's current level of the adjusted channel (first covered zone)
static int lightsLevel() {
    const LightUnit& l = lightUnits[lightsSel];
    const LightRgbw& c = l.zone[lightsZoneFirst(l)];
    return lightsChannel == LCH_BRI ? lightsMax(c) : c.v[lightsChannel - LCH_R];
}

//paints the selection one colour (scenes)
static void lightsColour(const LightRgbw& colour) {
    LightUnit& l = lightUnits[lightsSel];
    for (int z = lightsZoneFirst(l); z <= lightsZoneLast(l); z++) {
        lightsSetZone(l, z, colour, true);
    }
}

//Space/Enter: off if any covered zone is lit, else back on at its last colour (or white)
static void lightsToggle() {
    LightUnit& l = lightUnits[lightsSel];
    bool anyLit = false;
    for (int z = lightsZoneFirst(l); z <= lightsZoneLast(l); z++) {
        anyLit = anyLit || lightsLit(l.zone[z]);
    }
    for (int z = lightsZoneFirst(l); z <= lightsZoneLast(l); z++) {
        if (anyLit) {
            lightsSetZone(l, z, LightRgbw{ { 0, 0, 0, 0 } }, false);   //off; the hue is kept for "on"
        } else {
            lightsSetZone(l, z, lightsLit(l.hue[z]) ? l.hue[z] : LIGHTS_DEFAULT_ON, true);
        }
    }
    lightsSay(anyLit ? "off" : "on");
}

//switches a light's master-power relay
static void lightsSetRelay(LightUnit& l, bool on) {
    if (!l.relayId || !lightsMqtt.connected()) {
        return;
    }
    String id = String(l.relayId);
    if (l.relayShared) {   //shared protocol: arm with the activator, then exactly one command
        lightsMqtt.publish(LIGHTS_BASE_TOPIC, id.c_str());
        lightsMqtt.publish(LIGHTS_BASE_TOPIC, on ? "1" : "0");
    } else {
        lightsMqtt.publish((String(LIGHTS_BASE_TOPIC) + "/" + id + "/set").c_str(), on ? "1" : "0");
    }
    l.relayOn = on ? 1 : 0;   //relays report nothing back, so remember what we sent
}

//ALL OFF: every board to "0" (its all-zones-off command) and every relay off
static void lightsAllOff() {
    for (int i = 0; i < LIGHTS_COUNT; i++) {
        LightUnit& l = lightUnits[i];
        for (int z = 0; z < l.zones; z++) {
            l.zone[z] = LightRgbw{ { 0, 0, 0, 0 } };
            l.sent[z] = l.zone[z];   //"0" below already does it
            l.dirty[z] = false;
        }
        l.holdUntil = millis() + LIGHTS_HOLD_MS;
        if (lightsMqtt.connected()) {
            lightsMqtt.publish((String(LIGHTS_BASE_TOPIC) + "/" + l.id + "/set").c_str(), "0");
        }
        lightsSetRelay(l, false);
    }
    lightsSay("ALL OFF");
}

// --------------------------------------------------------------------- MQTT

//sends pending changes as zone-standard commands, rate-limited per light
static void lightsPublish() {
    if (!lightsMqtt.connected()) {
        return;   //changes wait (dirty) until the broker is back
    }
    unsigned long now = millis();
    for (int i = 0; i < LIGHTS_COUNT; i++) {
        LightUnit& l = lightUnits[i];
        if (now - l.lastPublish < LIGHTS_PUBLISH_GAP_MS) {
            continue;
        }
        String topic = String(LIGHTS_BASE_TOPIC) + "/" + l.id + "/set";
        for (int z = 0; z < l.zones; z++) {
            if (!l.dirty[z]) {
                continue;
            }
            for (int c = 0; c < 4; c++) {
                if (l.zone[z].v[c] == l.sent[z].v[c]) {
                    continue;   //only what changed
                }
                long command = (4L * z + c + 1) * 1000 + l.zone[z].v[c];
                lightsMqtt.publish(topic.c_str(), String(command).c_str());
                l.sent[z].v[c] = l.zone[z].v[c];
                ledPulseNetwork();
            }
            l.dirty[z] = false;
            l.lastPublish = now;
        }
    }
}

//the light a board (or its relay) belongs to, -1 if none
static int lightsIndexOf(uint16_t id) {
    for (int i = 0; i < LIGHTS_COUNT; i++) {
        if (lightUnits[i].id == id || lightUnits[i].relayId == id) {
            return i;
        }
    }
    return -1;
}

//PubSubClient callback: boxchat/<id>/status and boxchat/<id>/state
void lightsOnMessage(char* topic, byte* payload, unsigned int length) {
    String t(topic);
    int slash1 = t.indexOf('/');
    int slash2 = t.indexOf('/', slash1 + 1);
    if (slash1 < 0 || slash2 < 0) {
        return;   //not boxchat/<id>/<kind>
    }
    uint16_t id = t.substring(slash1 + 1, slash2).toInt();
    String kind = t.substring(slash2 + 1);
    int index = lightsIndexOf(id);
    if (index < 0) {
        return;   //a board we don't show
    }
    LightUnit& l = lightUnits[index];

    if (kind == "status") {
        String status;
        for (unsigned int i = 0; i < length; i++) status += (char)payload[i];
        if (id == l.id) {   //only the light board's own status counts as "online"
            if (status == "online" || status == "ready") l.online = true;
            else if (status == "offline") l.online = false;   //other texts are command errors
        }
        return;
    }
    if (kind != "state" || id != l.id) {
        return;
    }
    if ((long)(millis() - l.holdUntil) < 0) {
        return;   //our own change is in flight: ignore the echo
    }

    JsonDocument doc;
    if (deserializeJson(doc, payload, length)) {
        return;   //not valid JSON
    }
    JsonArray zones = doc["zones"].as<JsonArray>();
    for (int z = 0; z < l.zones; z++) {
        JsonVariant src;                                 //this zone's {"r","g","b","w"}
        if (!zones.isNull()) src = zones[z];             //multi-zone firmware lists every zone...
        else if (z == 0) src = doc.as<JsonVariant>();    //...single-zone firmware sends the flat object
        if (src.isNull()) continue;
        LightRgbw c;
        const char* keys[4] = { "r", "g", "b", "w" };
        for (int i = 0; i < 4; i++) {
            c.v[i] = (uint8_t)constrain((int)(src[keys[i]] | 0), 0, 255);
        }
        l.zone[z] = c;
        l.sent[z] = c;      //the light already has it: nothing to publish
        l.dirty[z] = false;
        if (lightsLit(c)) l.hue[z] = lightsScaled(c, 255);
    }
    l.known = true;
}

//keeps the broker connection up without blocking long; true while connected
static bool lightsNetService() {
    if (lightsMqtt.connected()) {
        lightsMqtt.loop();   //receive state, keep the connection alive
        lightsPublish();
        return true;
    }
    if (WiFi.status() != WL_CONNECTED || millis() - lightsLastTry < LIGHTS_RETRY_MS) {
        return false;
    }
    lightsLastTry = millis();
    String clientId = "DollRemote-" + WiFi.macAddress();   //unique, so it never kicks another board off
    if (lightsMqtt.connect(clientId.c_str())) {
        lightsMqtt.subscribe("boxchat/+/state");    //every light's retained colour...
        lightsMqtt.subscribe("boxchat/+/status");   //...and whether it's online
        lightsSay("boxchat connected");
        return true;
    }
    return false;
}

// --------------------------------------------------------------------- drawing

//one text row on the canvas
static void lightsRow(int row, const String& text, uint16_t fg, uint16_t bg) {
    terminalSprite.setTextColor(fg, bg);
    terminalSprite.drawString(text, TERMINAL_PADDING, TERMINAL_PADDING + row * 12);
}

//the selected zone's label: "all" (one-zone light), "ALL", or its name ("Left") / "Z3"
static String lightsZoneLabel(const LightUnit& l) {
    if (l.zones < 2) return "all";
    if (lightsZone == 0) return "ALL";
    const char* name = l.zoneNames[lightsZone - 1];
    return name ? String(name) : "Z" + String(lightsZone);
}

const int LIGHTS_VISIBLE = 4;   //light rows on screen (rows 1-4); the list scrolls past that

static void lightsDraw(bool connected) {
    terminalSprite.fillSprite(BLACK);
    terminalSprite.setTextDatum(top_left);
    String header = "LIGHTS " + String(lightsSel + 1) + "/" + String(LIGHTS_COUNT) + "  ";
    lightsRow(0, header + (connected ? "boxchat ok" : "connecting..."), PINK, BLACK);

    //rows 1-4: a window of the list that keeps the selected light in view
    int first = constrain(lightsSel - (LIGHTS_VISIBLE - 1), 0, max(0, LIGHTS_COUNT - LIGHTS_VISIBLE));
    for (int row = 0; row < LIGHTS_VISIBLE && first + row < LIGHTS_COUNT; row++) {
        int i = first + row;
        const LightUnit& l = lightUnits[i];
        int y = TERMINAL_PADDING + (row + 1) * 12;
        bool sel = (i == lightsSel);
        uint16_t bg = sel ? 0x2104 : BLACK;
        if (sel) terminalSprite.fillRect(0, y - 2, terminalSprite.width(), 12, bg);   //dark highlight bar
        terminalSprite.fillCircle(TERMINAL_PADDING + 2, y + 3, 2, l.online ? GREEN : 0x8410);   //online dot
        terminalSprite.setTextColor(sel ? WHITE : 0xBDF7, bg);
        terminalSprite.drawString(String(l.name).substring(0, 16), TERMINAL_PADDING + 8, y);
        int w = l.zones <= 2 ? 18 : 10;        //narrower swatches when a room has 3-4 zones
        for (int z = 0; z < l.zones && z < LIGHT_MAX_ZONES; z++) {   //swatch per zone, outlined when selected
            int x = 110 + z * (w + 4);
            terminalSprite.fillRect(x, y - 1, w, 10, l.known ? lightsSwatch(l.zone[z]) : BLACK);
            bool zoneSel = sel && (lightsZone == 0 || lightsZone == z + 1);
            terminalSprite.drawRect(x, y - 1, w, 10, zoneSel ? WHITE : 0x8410);
        }
        if (l.relayId) {                       //power relay: on, off or unknown
            const char* pwr = l.relayOn == 1 ? "PWR on" : (l.relayOn == 0 ? "PWR off" : "PWR ?");
            terminalSprite.setTextColor(l.relayOn == 1 ? GREEN : 0x8410, bg);
            terminalSprite.drawString(pwr, 172, y);
        }
    }
    if (first > 0) terminalSprite.fillTriangle(230, 16, 238, 16, 234, 12, 0x8410);                        //more above
    if (first + LIGHTS_VISIBLE < LIGHTS_COUNT) terminalSprite.fillTriangle(230, 58, 238, 58, 234, 62, 0x8410); //more below

    //row 5: zone + channel being adjusted, with its level
    const LightUnit& l = lightUnits[lightsSel];
    lightsRow(5, "zone " + lightsZoneLabel(l) + "  adjust " + LIGHTS_CHANNEL_NAMES[lightsChannel] + " " +
              String(lightsLevel()), CYAN, BLACK);

    //row 6: level bars of the first selected zone, the adjusted one outlined
    const LightRgbw& c = l.zone[lightsZoneFirst(l)];
    int values[LCH_COUNT] = { lightsMax(c), c.v[0], c.v[1], c.v[2], c.v[3] };
    uint16_t colours[LCH_COUNT] = { WHITE, RED, GREEN, BLUE, 0xFFDF };
    int y = TERMINAL_PADDING + 6 * 12;
    for (int ch = 0; ch < LCH_COUNT; ch++) {
        int x = TERMINAL_PADDING + ch * 46;
        terminalSprite.setTextColor(0xBDF7, BLACK);
        terminalSprite.drawString(LIGHTS_CHANNEL_NAMES[ch], x, y);
        int barX = x + 20;
        terminalSprite.drawRect(barX, y, 22, 8, ch == lightsChannel ? WHITE : 0x4208);
        terminalSprite.fillRect(barX + 1, y + 1, (values[ch] * 20) / 255, 6, colours[ch]);
    }
    terminalSprite.setTextColor(WHITE, BLACK);
    terminalSprite.pushSprite(0, terminalAreaY());

    bool showMessage = lightsMessage.length() && (long)(lightsMessageUntil - millis()) > 0;
    drawCommandBar("lights> ", showMessage ? lightsMessage : String("1 scenes spc on/off </> lvl h help"));
}

//page: the scene menu. Pick by typing its number + Enter, or with up/down + Enter.
const int LIGHTS_SCENE_ROWS = 6;   //scene rows on screen (rows 1-6); the menu scrolls past that

static void lightsDrawMenu() {
    terminalSprite.fillSprite(BLACK);
    terminalSprite.setTextDatum(top_left);
    const LightUnit& l = lightUnits[lightsSel];
    String header = "SCENE -> " + String(l.name) + (l.zones > 1 ? " " + lightsZoneLabel(l) : String(""));
    lightsRow(0, header.substring(0, 38), PINK, BLACK);
    int first = constrain(lightsSceneCursor - (LIGHTS_SCENE_ROWS - 1), 0, max(0, LIGHTS_SCENE_COUNT - LIGHTS_SCENE_ROWS));
    for (int row = 0; row < LIGHTS_SCENE_ROWS && first + row < LIGHTS_SCENE_COUNT; row++) {
        int s = first + row;
        int y = TERMINAL_PADDING + (row + 1) * 12;
        bool sel = (s == lightsSceneCursor);
        uint16_t bg = sel ? 0x2104 : BLACK;
        if (sel) terminalSprite.fillRect(0, y - 2, terminalSprite.width(), 12, bg);
        terminalSprite.setTextColor(sel ? WHITE : 0xBDF7, bg);
        String num = String(s + 1);
        while (num.length() < 3) num = " " + num;                           //right-aligned numbers
        terminalSprite.drawString(num + "  " + LIGHTS_SCENES[s].name, TERMINAL_PADDING, y);
        terminalSprite.fillRect(200, y - 1, 30, 10, lightsSwatch(LIGHTS_SCENES[s].colour));   //what it looks like
        terminalSprite.drawRect(200, y - 1, 30, 10, 0x8410);
    }
    terminalSprite.pushSprite(0, terminalAreaY());
    drawCommandBar("scene# ", lightsSceneEntry.length() ? lightsSceneEntry
                                                        : String("number+Enter or arrows  ` back"));
}

//applies a scene to the selected light/zone(s) and closes the menu (index is 0-based)
static void lightsApplyScene(int index) {
    if (index < 0 || index >= LIGHTS_SCENE_COUNT) {
        lightsSay("no scene " + String(index + 1));
        return;
    }
    lightsColour(LIGHTS_SCENES[index].colour);
    lightsSay(String(index + 1) + " " + LIGHTS_SCENES[index].name);
    lightsMenu = false;
}

//keys in the scene menu: digits type a number, arrows move, Enter applies, ` / Del close
static void lightsMenuKey(const Keyboard_Class::KeysState& keys) {
    auto has = [&keys](char a, char b) { return keysContainChar(keys, a) || keysContainChar(keys, b); };
    if (keys.enter) {                                    //typed number wins over the highlighted row
        lightsApplyScene(lightsSceneEntry.length() ? lightsSceneEntry.toInt() - 1 : lightsSceneCursor);
        lightsSceneEntry = "";
        return;
    }
    if (keys.del) {                                      //Del: rub out a digit, else close
        if (lightsSceneEntry.length()) lightsSceneEntry.remove(lightsSceneEntry.length() - 1);
        else lightsMenu = false;
        return;
    }
    if (keysContainChar(keys, '`')) { lightsMenu = false; lightsSceneEntry = ""; return; }
    if (has(';', ':')) { lightsSceneCursor = max(0, lightsSceneCursor - 1); lightsSceneEntry = ""; return; }
    if (has('.', '>')) { lightsSceneCursor = min(LIGHTS_SCENE_COUNT - 1, lightsSceneCursor + 1); lightsSceneEntry = ""; return; }
    for (char ch : keys.word) {                          //digits build the number (up to 3)
        if (ch >= '0' && ch <= '9' && lightsSceneEntry.length() < 3) {
            lightsSceneEntry += ch;
            int n = lightsSceneEntry.toInt();
            if (n >= 1 && n <= LIGHTS_SCENE_COUNT) lightsSceneCursor = n - 1;   //highlight follows the number
        }
    }
}

static void lightsDrawHelp() {
    terminalSprite.fillSprite(BLACK);
    terminalSprite.setTextDatum(top_left);
    lightsRow(0, "Lights keys (arrows = ; . , /)", PINK, BLACK);
    lightsRow(1, "up/down light   left/right level", WHITE, BLACK);
    lightsRow(2, "Fn+left/right fine   spc/Enter on/off", WHITE, BLACK);
    lightsRow(3, "1 scene menu (pick by number)", WHITE, BLACK);
    lightsRow(4, "2 zone   3 channel (or Tab)", WHITE, BLACK);
    lightsRow(5, "4 power relay   0 ALL OFF", WHITE, BLACK);
    lightsRow(6, "G0 tap: remote  hold G0 3s: shell", CYAN, BLACK);
    terminalSprite.pushSprite(0, terminalAreaY());
    drawCommandBar("lights> ", "any key: back");
}

// --------------------------------------------------------------------- the modal loop

//handles one key press; returns false when the user asked to leave
static bool lightsHandleKey(const Keyboard_Class::KeysState& keys) {
    auto has = [&keys](char a, char b) { return keysContainChar(keys, a) || keysContainChar(keys, b); };
    if (keys.fn && has('q', 'Q')) return false;          //Fn+Q: shell, even from the menu
    if (lightsMenu) { lightsMenuKey(keys); return true; }
    if (keysContainChar(keys, '`')) return false;

    LightUnit& l = lightUnits[lightsSel];
    int step = keys.fn ? 4 : LIGHTS_STEP;   //Fn+arrow: fine steps
    if (has(';', ':')) { lightsSel = (lightsSel + LIGHTS_COUNT - 1) % LIGHTS_COUNT; lightsZone = 0; }
    else if (has('.', '>')) { lightsSel = (lightsSel + 1) % LIGHTS_COUNT; lightsZone = 0; }
    else if (has(',', '<')) lightsAdjust(-step);
    else if (has('/', '?')) lightsAdjust(step);
    //number row (Doll, 2026-11-23): 1 scene menu, 2 zone, 3 channel, 4 power relay, 0 ALL OFF
    else if (has('1', '!')) { lightsMenu = true; lightsSceneEntry = ""; }               //scene menu
    else if (has('2', '@')) lightsZone = l.zones < 2 ? 0 : (lightsZone + 1) % (l.zones + 1);   //ALL, zone 1..n
    else if (keys.tab || has('3', '#')) lightsChannel = (lightsChannel + 1) % LCH_COUNT;   //BRI R G B W
    else if (has('4', '$')) {                                                     //power relay
        if (!l.relayId) lightsSay(String(l.name) + " has no relay");
        else { lightsSetRelay(l, l.relayOn != 1); lightsSay(l.relayOn == 1 ? "power on" : "power off"); }
    }
    else if (has('0', ')')) lightsAllOff();                                       //ALL OFF
    else if (keys.enter || keysContainChar(keys, ' ')) lightsToggle();
    return true;
}

static String lightsBroker = "";          //broker host/IP for this boot ("" = config.h default)
static int lightsPort = 0;                //broker port (0 = config.h default)
static bool lightsServerReady = false;    //setServer() done with a resolved address

//   Points the MQTT client at the broker. An IP needs no DNS; a hostname is resolved
//   once (only while on Wi-Fi), so reconnect attempts never wait on DNS.
static bool lightsResolveBroker() {
    String broker = lightsBroker.length() ? lightsBroker : String(LIGHTS_MQTT_HOST);
    int port = lightsPort > 0 ? lightsPort : LIGHTS_MQTT_PORT;
    IPAddress brokerIp;
    if (!brokerIp.fromString(broker)) {                   //not a dotted IP: look the name up
        if (WiFi.status() != WL_CONNECTED || !WiFi.hostByName(broker.c_str(), brokerIp)) {
            return false;                                 //try again on a later pass
        }
    }
    lightsMqtt.setServer(brokerIp, port);
    lightsServerReady = true;
    return true;
}

//"lights [broker-ip] [port]": opens the light controller (G0 tap then switches to the remote)
void handleLightsCommand(const String parts[], int partCount) {
    if (partCount > 1 && parts[1] == "help") {
        outLine("Usage: lights [broker-ip] [port]");
        outLine("Controls the boxchat lights (Doll-Lights).");
        outLine("G0 tap: remote. Hold G0 3s or Fn+Q: shell.");
        return;
    }
    if (partCount > 1) {                  //a broker given here sticks until reboot
        lightsBroker = parts[1];
        lightsPort = partCount > 2 ? parts[2].toInt() : 0;
        lightsServerReady = false;
        lightsMqtt.disconnect();
    }
    runApps(APP_LIGHTS);
}

//   The light controller's loop: owns screen and keyboard until G0 (tap: remote, hold
//   3s: shell) or Fn+Q / ` (shell). Works without Wi-Fi too: it shows "connecting..."
//   and keeps retrying, since the remote may hand over before the network is up.
DollApp runLights() {
    String ssid, password;
    if (WiFi.status() != WL_CONNECTED && loadWifiCredentials(ssid, password)) {
        wifiArmReconnect();               //keep trying the saved network in the background
    }
    lightsWifiClient.setTimeout(2);       //seconds; bounds the TCP connect
    lightsMqtt.setSocketTimeout(2);       //seconds; bounds the CONNACK wait
    lightsMqtt.setCallback(lightsOnMessage);
    lightsMqtt.setBufferSize(512);        //room for a 4-zone state JSON
    lightsLastTry = 0;                    //connect straight away
    lightsMenu = false;                   //always open on the light list
    lightsSceneEntry = "";
    static bool relaysInitialised = false;
    for (int i = 0; i < LIGHTS_COUNT; i++) {
        lightUnits[i].online = false;     //re-learned from retained status on connect
        if (!relaysInitialised) {
            lightUnits[i].relayOn = -1;   //relays report nothing: unknown until we switch one (kept between sessions)
        }
    }
    relaysInitialised = true;

    DollApp next = APP_SHELL;
    bool help = false;
    unsigned long lastDrawAt = 0;
    bool dirty = true;
    while (true) {
        M5Cardputer.update();
        maintainInternetConnection();
        ledService();
        statusManagement();

        G0Event g0 = g0Poll();
        if (g0 != G0_NONE) {              //G0 tap: remote, hold 3s: shell
            next = (g0 == G0_TAP) ? APP_REMOTE : APP_SHELL;
            break;
        }

        if (!lightsServerReady) {
            lightsResolveBroker();        //cheap when the broker is an IP; waits for Wi-Fi for a hostname
        }
        bool connected = lightsServerReady && lightsNetService();

        if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
            Keyboard_Class::KeysState keys = M5Cardputer.Keyboard.keysState();
            if (!keyboardEventIsDebounced(keys)) {
                ledPulseInput();
                if (help) {
                    help = false;                               //any key closes help
                } else if (keysContainChar(keys, 'h') || keysContainChar(keys, 'H')) {
                    help = true;
                } else if (!lightsHandleKey(keys)) {
                    next = APP_SHELL;                           //Fn+Q / `
                    break;
                }
                dirty = true;
            }
        }

        if (dirty || millis() - lastDrawAt > 300) {             //state messages arrive any time
            if (help) lightsDrawHelp();
            else if (lightsMenu) lightsDrawMenu();      //scene menu
            else lightsDraw(connected);
            lastDrawAt = millis();
            dirty = false;
        }
        delay(5);
    }

    lightsPublish();                      //flush a last pending change if the gap allows
    lightsMqtt.disconnect();              //frees the socket before the remote opens TLS
    return next;
}
