//   global.h
//   shared state and sprites used across DOLL-REMOTE.
//
//   Forked from DOLL-OS's global.h and cut down hard: everything that only existed
//   for SD, ssh/telnet streaming, the .dapp runtime, the editor, Dapper and aliases is
//   gone. What's left is the screen, the keyboard buffers, the scrollback and the
//   remote's own types. Types that a function signature mentions still have to live
//   here (not in their .ino) because the Arduino builder hoists auto-generated
//   prototypes above every tab's own code -- same trap DOLL-OS documents.

#pragma once

#include <ArduinoJson.h>

#define DOLL_REMOTE_VERSION "R-0.1.0"   //shown on the boot logo and in "status"

// M5Cardputer contains the StampS3's single addressable RGB LED (Led.ino).
struct LedRgb {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
};

bool rearLedAvailable();                                    //true when the board reports an RGB LED
void rearLedSetRgb(uint8_t red, uint8_t green, uint8_t blue);
void rearLedOff();
void ledBegin();                                            //full brightness, LED off
void ledService();                                          //settles pulses back to idle, once per loop
void ledPulseNetwork();                                     //short teal blink per HTTPS request
void ledPulseInput();                                       //short purple blink per remote key
void ledPulseError();                                       //red blink when the server says no

//battery logic
int batteryPercent = 0;         //battery in percent
int batteryMillivolts = 0;      //battery in millivolts
int refreshCounter = 60;        //tick counter, statusManagement refreshes once this hits 60 (starts there so boot draws it at once)

//command logic
String currentCommand  = "";             //command buffer, filled by readKeyboard and cleared by commandProcessor
int commandCursorPos = 0;                //index within currentCommand where typing/deleting/cursor keys act
int commandScrollOffset = 0;             //first visible character index in the command bar when text overflows the screen
const int COMMAND_BAR_HEIGHT = 24;       //pixel height of the command bar at the bottom of the screen
const int COMMAND_BAR_PADDING = 4;       //pixel padding inside the command bar
LGFX_Sprite commandBarSprite(&M5Cardputer.Display);   //offscreen buffer the command bar gets drawn to before pushing

//command history (sent commands, recalled with fn+;/fn+. like a shell's up/down arrows)
const int COMMAND_HISTORY_MAX = 10;          //DOLL-OS keeps 30; a remote's shell is mostly "remote" and "wifi"
String commandHistory[COMMAND_HISTORY_MAX];  //ring buffer of sent commands
int commandHistoryCount = 0;                 //number of valid entries in commandHistory
int commandHistoryHead = 0;                  //physical slot of the oldest command in the ring buffer
int commandHistoryIndex = -1;                //entry currently recalled into the command bar; -1 means not recalling
String commandHistoryDraft = "";             //in-progress typing stashed when recall starts

//   Output color codes (Output.ino). ANSI SGR foreground codes mapped onto M5GFX
//   colors by ansiCodeToPixelColor(), kept so outLine(text, C_CYAN) reads the same as
//   in DOLL-OS and DS.
const int C_RESET   = 0;
const int C_WHITE   = 37;
const int C_BLACK   = 30;
const int C_RED     = 31;
const int C_GREEN   = 32;
const int C_YELLOW  = 33;
const int C_BLUE    = 34;
const int C_MAGENTA = 35;
const int C_CYAN    = 36;
const int C_PINK    = 95;   //bright magenta; stands in for the PINK accent color

//Motoko MQTT buffers (defined in motoko.ino), declared here so reserveHotStrings() can pre-size them
extern String motokoChannel;
extern String motokoInputBuffer;

int splitCommand(const String& input, String parts[], int maxParts);
bool keyboardEventIsDebounced(const Keyboard_Class::KeysState& keys);

//status bar
const int STATUS_BAR_HEIGHT = 14;   //pixel height of the top status bar
LGFX_Sprite statusBarSprite(&M5Cardputer.Display);   //offscreen buffer the status bar gets drawn to before pushing

//heap instrumentation (free.ino) -- the "free details" command prints these
const int HEAP_CHECKPOINT_MAX = 12;
struct HeapCheckpoint {
    const char* tag;
    uint32_t freeHeap;
    uint32_t largestBlock;
    uint32_t minFreeHeap;
};
HeapCheckpoint heapCheckpoints[HEAP_CHECKPOINT_MAX];
int heapCheckpointCount = 0;
int heapCheckpointHead = 0;

//terminal
const int TERMINAL_PADDING = 4;             //pixel padding around the terminal history text
const int HISTORY_MAX_LINES = 50;           //DOLL-OS keeps 120 rows; 50 is plenty for setup messages and saves ~7KB
const int HISTORY_ROW_MAX_CHARS = 48;       //one 240px row of 6px glyphs is 40 chars; DOLL-OS's 96 was sized for streams
LGFX_Sprite terminalSprite(&M5Cardputer.Display);   //offscreen buffer for the scrollback -- the remote draws its pages here too
struct HistoryRow {
    char text[HISTORY_ROW_MAX_CHARS];
    uint16_t color = WHITE;
};
HistoryRow historyRows[HISTORY_MAX_LINES];  //ring buffer of wrapped terminal history rows with inline storage
int historyCount = 0;                       //number of valid rows currently in historyRows
int historyHead = 0;                        //physical slot of the oldest logical history row in the ring buffer
int scrollOffset = 0;                       //how many rows back from the newest line the view is scrolled

//   Mnemerynne remote (Remote.ino). The one screen and one channel row the remote
//   shows, sized for this 240x135 panel. Lists are capped: the server allows at most
//   10 screens per owner, plus TVs they're a guest on.
const int REMOTE_MAX_SCREENS = 16;          //owner's screens + guest TVs kept from one poll
const int REMOTE_CHANNEL_PAGE = 6;          //channels fetched (and shown) per page; the server caps a page at 20

struct RemoteScreen {
    int id = 0;                 //screen_id used in /api/media/remote/screens/{id}/command
    String name;                //what the owner called it ("Living room")
    String host;                //a guest TV's owner; empty for the remote owner's own screens
    bool online = false;        //the TV page is open and connected right now
    bool speaker = false;       //a speaker screen (plays what is cast to it)
    bool hasState = false;      //false when the screen has reported nothing yet
    String kind;                //idle, video, live, youtube, audio, radio, pictures, ...
    String title;               //what is playing
    String sub;                 //second line (album, channel, episode)
    float position = 0;         //seconds into the item
    float duration = 0;         //seconds long; 0 for live
    bool paused = false;
    float volume = -1;          //0..1, -1 when the TV hasn't said
    bool muted = false;
    float night = 0;            //night filter level, 0..1
};

//   App switcher (System.ino). The G0 button: a tap switches remote <-> lights, a 3s
//   hold drops to the shell, a tap in the shell goes back to the remote. Each app's
//   loop returns the app to run next.
enum DollApp {
    APP_REMOTE,      //Mnemerynne remote (Remote.ino), the default
    APP_LIGHTS,      //boxchat lights (Lights.ino)
    APP_SHELL        //the command line
};

enum G0Event {
    G0_NONE,         //nothing this update
    G0_TAP,          //pressed and let go before 3s
    G0_HOLD          //held for 3s (fires once, while still held)
};

const uint32_t G0_HOLD_MS = 3000;   //how long G0 is held for the shell

//which page the remote is showing (Remote.ino's modal loop switches between them)
enum RemoteMode {
    RMODE_SCREENS,   //list of the owner's screens and guest TVs
    RMODE_CONTROL,   //one screen: what's playing, transport/volume/arrow keys
    RMODE_CHANNELS,  //live streams and radio stations to start on the screen
    RMODE_PAD,       //gamepad mode: every key goes down and up as a SNES-style pad button
    RMODE_HELP       //key map overlay; any key returns to the page underneath
};

//what a poll of /api/media/remote came back with
enum RemoteResult {
    RRES_OK,         //200 and parsed
    RRES_UNPAIRED,   //401: the owner removed this remote, the key is gone
    RRES_FAILED      //network/TLS/server trouble; try again later
};

//   Doll-Lights (Lights.ino): boxchat MQTT lights, ported from Doll-Lights' master
//   keypad console (C:\Scripts\Doll-Lights\lcd_keypad\lcd_keypad_esp32\Console.h).
struct LightRgbw {
    uint8_t v[4];               //red, green, blue, white (0-255 each)
};

const int LIGHT_MAX_ZONES = 4;  //a Dig-Quad has four outputs; the boxchat zone standard allows 1-4

struct LightUnit {
    uint16_t id;                //boxchat activator number (32001...)
    const char* name;           //shown in the list (the room)
    uint8_t zones;              //1-4 (Living Room: 1 = Left, 2 = Right)
    uint16_t relayId;           //master power relay board, 0 for none
    bool relayShared;           //32002 only listens on the shared "boxchat" topic (arm, then command)
    const char* zoneNames[LIGHT_MAX_ZONES];   //optional names per zone ("Left", "Right"); nullptr = "Z1", "Z2"...
    LightRgbw zone[LIGHT_MAX_ZONES];          //what each zone shows (or will, once our commands land)
    LightRgbw hue[LIGHT_MAX_ZONES];           //each zone's colour at full brightness (BRI scales this)
    LightRgbw sent[LIGHT_MAX_ZONES];          //what we last published per zone
    bool dirty[LIGHT_MAX_ZONES];              //zone changed here but not published yet
    bool known;                 //a state message arrived (or we set it ourselves)
    bool online;                //from boxchat/<id>/status
    int8_t relayOn;             //relay: 1 on, 0 off, -1 unknown (relays report nothing back)
    unsigned long holdUntil;    //ignore state echoes until then: our own change is in flight
    unsigned long lastPublish;  //per-light publish rate limit
};

struct RemoteChannel {
    bool station = false;       //true: radio station (play_station + key); false: live stream (play_source + id)
    int sourceId = 0;           //stream id for play_source
    String key;                 //station key for play_station
    String name;                //shown in the list
};
