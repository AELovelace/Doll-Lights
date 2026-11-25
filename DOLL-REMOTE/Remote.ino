//   Remote.ino
//   The Mnemerynne Media remote: pairs with the server like a TV with no keyboard,
//   then drives the owner's screens over the same remote-device API the ESP32 touch
//   panel (Mnemerynne-tab) uses. It is DOLL-REMOTE's default app: it opens at boot,
//   and the G0 button (or Fn+Q) drops to the shell; G0 or "remote" comes back.
//
//   API (Mnemerynne repo: mnemerynne/web/media.py, mnemerynne/media/remotes.py):
//     POST /media/remote/pair                {"speaker":false} -> {code, expires_in, url, secret}
//     POST /media/remote/pair/status         {"secret":...}    -> {state: waiting|paired|expired, key, name}
//     GET  /api/media/remote                 (Bearer)          -> {name, owner, screens:[{id,name,online,state,...}]}
//     GET  /api/media/remote/channels?offset=&limit=           -> {total, offset, channels:[...]}
//     POST /api/media/remote/screens/{id}/command  {action, ...}
//     DELETE /api/media/remote                                 -> the remote forgets itself
//   A 401 on any Bearer call means the owner removed this remote: the key is wiped
//   and the remote goes back to pairing.
//
//   The remote key is a bearer credential. It lives only in /remote.cfg and in RAM,
//   and nothing in this file prints, logs or draws it.

#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <LittleFS.h>
#include <time.h>
#include "ca_roots.h"

//   Settings (/remote.cfg on LittleFS), one "name=value" per line
const char* REMOTE_CFG_PATH = "/remote.cfg";
static String remoteServer = "";          //https://host, no trailing slash
static String remoteKey = "";             //bearer key from pairing; empty = not paired. NEVER print this.
static String remoteName = "";            //what the owner named this remote when approving it
static bool remoteSettingsLoaded = false; //lazy-load guard so every entry point sees the file once

//   Timing. The server saves last-seen at most once a minute however often we ask, and
//   the touch panel polls every few seconds; 5s keeps "what's playing" fresh without
//   making the radio work hard.
const unsigned long REMOTE_POLL_MS = 5000;            //screen/what's-playing poll in the list and control pages
const unsigned long REMOTE_POLL_AFTER_CMD_MS = 900;   //quick re-poll after a command so the page catches up
const unsigned long REMOTE_PAIR_POLL_MS = 4000;       //pairing status poll ("every few seconds")
const unsigned long REMOTE_CLOCK_TIMEOUT_MS = 8000;   //NTP wait; certificate dates can't be checked without a clock
const size_t REMOTE_MAX_RESPONSE = 16384;             //refuse bigger bodies rather than run the heap dry
const float REMOTE_VOLUME_STEP = 0.05f;               //volume +/- step (5%)
const int REMOTE_SKIP_SECONDS = 30;                   //[ and ] skip back/forward this far

//   HTTPS client, kept open for the whole session so requests reuse one TLS
//   connection (a fresh handshake on this chip costs a second or more). Heap-allocated:
//   the loop task's 8KB stack is too small to hold them, as Dapper found out.
static WiFiClientSecure* remoteTls = nullptr;
static HTTPClient* remoteHttp = nullptr;

//   Session state
static RemoteScreen remoteScreens[REMOTE_MAX_SCREENS];   //last poll's screens (own first, then guest TVs)
static int remoteScreenCount = 0;                       //valid entries in remoteScreens
static int remoteCursor = 0;                            //highlighted row in the screens list
static int remoteSelectedId = 0;                        //screen_id being controlled (0 = none yet)
static String remoteOwner = "";                         //owner's username from the last poll
static unsigned long remoteLastPollAt = 0;              //millis() of the last successful poll (drives the live progress clock)
static unsigned long remoteNextPollAt = 0;              //when the next poll is due
static bool remoteLastPollOk = false;                   //false shows "offline" on the pages
static RemoteMode remoteMode = RMODE_SCREENS;           //page on screen
static RemoteMode remoteModeBeforeHelp = RMODE_SCREENS; //page the help overlay returns to
static String remoteMessage = "";                       //transient line in the command bar
static unsigned long remoteMessageUntil = 0;            //when remoteMessage gives way to the key hints
static uint16_t remoteMessageColor = WHITE;             //green for sent, red for refused

static RemoteChannel remoteChannels[REMOTE_CHANNEL_PAGE]; //one page of the channel list
static int remoteChannelCount = 0;                       //entries on this page
static int remoteChannelTotal = 0;                       //all channels the server has
static int remoteChannelOffset = 0;                      //offset of remoteChannels[0] in the full list
static int remoteChannelCursor = 0;                      //highlighted row within the page

static uint16_t remotePadHeld = 0;                       //bitmask of pad buttons currently held (gamepad mode)
static DollApp remoteExitTo = APP_SHELL;                 //where to go when a remote page is left (G0 tap: lights)

// --------------------------------------------------------------------- settings

//reads /remote.cfg once; missing file = defaults (main server, unpaired)
void remoteLoadSettings() {
    if (remoteSettingsLoaded) {
        return;   //already read this boot
    }
    remoteSettingsLoaded = true;
    remoteServer = REMOTE_DEFAULT_SERVER;   //config.h default until the file says otherwise
    remoteKey = "";
    remoteName = "";

    File file = LittleFS.open(REMOTE_CFG_PATH, "r");
    if (!file) {
        return;   //first boot: nothing saved yet
    }
    while (file.available()) {
        String line = file.readStringUntil('\n');   //one "name=value" per line
        line.trim();
        int eq = line.indexOf('=');
        if (eq <= 0) {
            continue;   //blank or malformed line
        }
        String name = line.substring(0, eq);
        String value = line.substring(eq + 1);
        if (name == "server" && value.startsWith("https://")) {
            remoteServer = value;
        } else if (name == "key") {
            remoteKey = value;
        } else if (name == "name") {
            remoteName = value;
        }   //anything else (e.g. an old "autostart=" line) is ignored
    }
    file.close();
    if (remoteKey.length() == 0 && remoteServer == REMOTE_OLD_DEFAULT_SERVER) {
        remoteServer = REMOTE_DEFAULT_SERVER;   //unpaired on the old default: follow the new one (config.h)
    }
}

//writes every setting back to /remote.cfg (small file, rewritten whole)
bool remoteSaveSettings() {
    File file = LittleFS.open(REMOTE_CFG_PATH, "w");
    if (!file) {
        return false;   //LittleFS unmounted or full
    }
    file.println("server=" + remoteServer);
    file.println("key=" + remoteKey);          //written to flash only; never echoed anywhere
    file.println("name=" + remoteName);
    file.close();
    return true;
}

bool remoteIsPaired() {
    remoteLoadSettings();
    return remoteKey.length() > 0;   //a key means the server approved this remote at some point
}

//drops the key locally (server-side removal is "forget", or the owner's Media page)
void remoteForgetLocal() {
    remoteKey = "";
    remoteName = "";
    remoteSaveSettings();
}

// --------------------------------------------------------------------- text helpers

//the panel's GLCD font is ASCII; turn each UTF-8 character into one '?' so names line up
static String remoteAscii(const String& text) {
    String out;
    out.reserve(text.length());
    for (int i = 0; i < (int)text.length(); i++) {
        uint8_t ch = (uint8_t)text[i];
        if (ch < 0x80) {
            out += (ch >= 0x20) ? (char)ch : ' ';   //printable ASCII kept, control chars become spaces
        } else if ((ch & 0xC0) == 0xC0) {
            out += '?';                              //lead byte of a multi-byte character: one placeholder
        }                                            //continuation bytes (10xxxxxx) are dropped
    }
    return out;
}

//cuts text to fit maxChars columns, with "~" marking the cut
static String remoteFit(const String& text, int maxChars) {
    if ((int)text.length() <= maxChars) {
        return text;
    }
    return text.substring(0, max(0, maxChars - 1)) + "~";
}

//seconds -> "m:ss" or "h:mm:ss"
static String remoteClock(float seconds) {
    if (seconds < 0) {
        seconds = 0;
    }
    unsigned long s = (unsigned long)seconds;
    char buf[16];
    if (s >= 3600) {
        snprintf(buf, sizeof(buf), "%lu:%02lu:%02lu", s / 3600, (s % 3600) / 60, s % 60);
    } else {
        snprintf(buf, sizeof(buf), "%lu:%02lu", s / 60, s % 60);
    }
    return String(buf);
}

//shows a line in the command bar for a few seconds (then the key hints come back)
static void remoteSay(const String& text, uint16_t color) {
    remoteMessage = remoteAscii(text);
    remoteMessageColor = color;
    remoteMessageUntil = millis() + 3500;
}

// --------------------------------------------------------------------- HTTPS

//true once the clock is plausible (after 2023); TLS needs it to check certificate dates
static bool remoteClockReady() {
    return time(nullptr) > 1700000000;
}

//syncs the clock over NTP once per boot, keeping the screen alive while it waits
static bool remoteEnsureClock() {
    if (remoteClockReady()) {
        return true;
    }
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");   //UTC is all TLS needs
    unsigned long started = millis();
    while (!remoteClockReady() && millis() - started < REMOTE_CLOCK_TIMEOUT_MS) {
        M5Cardputer.update();
        ledService();
        delay(50);
    }
    return remoteClockReady();
}

//allocates the TLS + HTTP clients for this session (pinned ISRG roots, keep-alive on)
static bool remoteHttpOpen() {
    if (remoteHttp) {
        return true;   //already open
    }
    remoteTls = new WiFiClientSecure();
    remoteHttp = new HTTPClient();
    if (!remoteTls || !remoteHttp) {
        delete remoteHttp;
        delete remoteTls;
        remoteHttp = nullptr;
        remoteTls = nullptr;
        return false;   //out of heap
    }
    remoteTls->setCACert(REMOTE_CA_PEM);       //validate the server certificate against ISRG Root X1/X2 (ca_roots.h)
    remoteHttp->setReuse(true);                //keep the TLS connection open between requests
    remoteHttp->setConnectTimeout(10000);      //TCP + TLS handshake budget
    remoteHttp->setTimeout(8000);              //read budget; the touch panel waits 7s for the server
    remoteHttp->setUserAgent("DOLL-REMOTE/" DOLL_REMOTE_VERSION);
    recordHeapCheckpoint("remote http open");
    return true;
}

//closes the connection and frees the clients (on leaving the remote)
static void remoteHttpClose() {
    if (remoteHttp) {
        remoteHttp->setReuse(false);   //so end() really closes the socket
        remoteHttp->end();
    }
    if (remoteTls) {
        remoteTls->stop();
    }
    delete remoteHttp;
    delete remoteTls;
    remoteHttp = nullptr;
    remoteTls = nullptr;
}

//   One HTTPS request. method is "GET"/"POST"/"DELETE"; body is JSON or empty; auth
//   adds the Bearer key. Returns the HTTP status (or a negative HTTPClient error) and
//   fills response with the body. A dead kept-alive socket is retried once on a fresh
//   connection, since the server may have closed it between polls.
static int remoteRequest(const char* method, const String& path, const String& body, bool auth, String& response) {
    response = "";
    if (WiFi.status() != WL_CONNECTED) {
        response = "Wi-Fi is not connected";
        return -100;   //our own code: no network at all
    }
    if (!remoteEnsureClock()) {
        response = "clock sync failed (needed for HTTPS)";
        return -101;
    }
    if (!remoteHttpOpen()) {
        response = "not enough memory for HTTPS";
        return -102;
    }

    int code = 0;
    for (int attempt = 0; attempt < 2; attempt++) {
        ledPulseNetwork();
        if (!remoteHttp->begin(*remoteTls, remoteServer + path)) {
            response = "bad server address";
            return -103;
        }
        if (auth) {
            remoteHttp->addHeader("Authorization", "Bearer " + remoteKey);   //bearer credential; headers are never logged here
        }
        if (body.length() > 0) {
            remoteHttp->addHeader("Content-Type", "application/json");
        }
        code = remoteHttp->sendRequest(method, body);   //body may be empty (GET/DELETE)
        if (code > 0) {
            break;   //got an HTTP answer
        }
        remoteHttp->end();   //socket was dead: drop it and try once more on a new connection
        remoteTls->stop();
    }

    if (code <= 0) {
        char tlsError[96] = "";
        int tlsCode = remoteTls->lastError(tlsError, sizeof(tlsError));   //certificate / handshake reason, if any
        response = HTTPClient::errorToString(code);
        if (tlsCode != 0) {
            response += String(" (TLS: ") + tlsError + ")";
        }
        remoteHttp->end();
        remoteTls->stop();
        ledPulseError();
        Serial.printf("[remote] %s %s -> %d %s\n", method, path.c_str(), code, response.c_str());   //transport error
        return code;
    }

    int size = remoteHttp->getSize();   //-1 for chunked replies
    if (size > (int)REMOTE_MAX_RESPONSE) {
        response = "reply too large";
        remoteHttp->end();
        remoteTls->stop();
        return -104;
    }
    response = remoteHttp->getString();   //small JSON bodies only (screens list, one channel page)
    remoteHttp->end();                    //keeps the socket open for reuse (setReuse)
    //   USB serial log for debugging: method, path, status, and the body only for a command
    //   (never for pairing: its request carries the secret and its reply the key) and the
    //   reply only on an error (error replies carry no credentials).
    bool isCommand = path.endsWith("/command");
    Serial.printf("[remote] %s %s %s-> %d %s\n", method, path.c_str(), isCommand ? body.c_str() : "", code,
                  code >= 400 ? response.c_str() : "");
    if (code >= 400) {
        ledPulseError();
    }
    return code;
}

//pulls FastAPI's {"detail": "..."} out of an error body, else "HTTP <code>"
static String remoteErrorText(int code, const String& response) {
    if (code <= 0) {
        return response;   //transport error text from remoteRequest
    }
    JsonDocument doc;
    if (deserializeJson(doc, response) == DeserializationError::Ok && doc["detail"].is<const char*>()) {
        return String(doc["detail"].as<const char*>());   //e.g. "Living room isn't connected: is Media open on it?"
    }
    return "HTTP " + String(code);
}

// --------------------------------------------------------------------- pairing

//full-panel pairing screen: QR of the claim URL on the left, the code big on the right
static void remoteDrawPairing(const String& code, const String& url, long secondsLeft, const String& note) {
    M5Cardputer.Display.startWrite();
    const int top = STATUS_BAR_HEIGHT;                                         //keep the status bar
    const int height = M5Cardputer.Display.height() - top - COMMAND_BAR_HEIGHT; //area between the bars
    M5Cardputer.Display.fillRect(0, top, M5Cardputer.Display.width(), height, BLACK);

    //QR code of the claim URL (scan it on the phone instead of typing the code)
    const int qrSize = min(height - 4, 100);
    if (url.length() > 0) {
        M5Cardputer.Display.fillRect(2, top + 2, qrSize, qrSize, WHITE);   //quiet zone behind the code
        M5Cardputer.Display.qrcode(url.c_str(), 2, top + 2, qrSize, 1);  //version grows to fit the URL
    }

    const int textX = qrSize + 10;   //text column right of the QR
    M5Cardputer.Display.setTextDatum(top_left);
    M5Cardputer.Display.setTextColor(PINK, BLACK);
    M5Cardputer.Display.drawString("Pair this remote", textX, top + 2);
    M5Cardputer.Display.setTextSize(2);
    M5Cardputer.Display.setTextColor(WHITE, BLACK);
    int dash = code.indexOf('-');
    if (dash > 0) {
        M5Cardputer.Display.drawString(code.substring(0, dash + 1), textX, top + 16);   //"ABCD-" on one line
        M5Cardputer.Display.drawString(code.substring(dash + 1), textX, top + 34);      //"EFGH" under it
    } else {
        M5Cardputer.Display.drawString(code, textX, top + 16);
    }
    M5Cardputer.Display.setTextSize(1);
    M5Cardputer.Display.setTextColor(CYAN, BLACK);
    M5Cardputer.Display.drawString("Phone: Media>Screens", textX, top + 56);
    M5Cardputer.Display.drawString("> Pair a TV or remote", textX, top + 66);
    M5Cardputer.Display.setTextColor(note.length() ? YELLOW : 0x8410, BLACK);   //grey when just counting down
    String footer = note.length() ? note : (remoteClock(secondsLeft) + " left");
    M5Cardputer.Display.drawString(remoteFit(footer, 20), textX, top + 80);
    M5Cardputer.Display.endWrite();
}

//   Runs pairing until approved (true) or cancelled with Fn+Q / backspace (false).
//   Asks for a code, shows it, and polls the status with the secret every few seconds.
//   The secret stays in this function's RAM: it's what collects the key, so it's never shown.
bool remotePairFlow() {
    remoteLoadSettings();
    remoteExitTo = APP_SHELL;   //a failure lands in the shell, where the error is printed
    outLine("Pairing with " + remoteServer, C_CYAN);
    drawTerminalHistory();

    while (true) {   //one iteration per pairing code (a new one after expiry, on Enter)
        String response;
        drawCommandBar("pair> ", "asking for a code...");
        int code = remoteRequest("POST", "/media/remote/pair", "{\"speaker\":false}", false, response);
        if (code != 200) {
            outLine("pair: " + remoteErrorText(code, response), C_RED);
            remoteHttpClose();
            return false;
        }
        JsonDocument doc;
        if (deserializeJson(doc, response) != DeserializationError::Ok) {
            outLine("pair: unreadable reply from server", C_RED);
            remoteHttpClose();
            return false;
        }
        String pairCode = doc["code"] | "";            //shown as "ABCD-EFGH"
        String pairUrl = doc["url"] | "";              //…/media/pair?code=… for the QR
        String secret = doc["secret"] | "";            //collects the key; never displayed
        long expiresIn = doc["expires_in"] | 600;
        doc.clear();
        if (pairCode.length() == 0 || secret.length() == 0) {
            outLine("pair: server sent no code", C_RED);
            remoteHttpClose();
            return false;
        }

        unsigned long expiresAt = millis() + (unsigned long)expiresIn * 1000UL;
        unsigned long nextPollAt = millis() + REMOTE_PAIR_POLL_MS;
        long shownSeconds = -1;
        String note = "";
        bool expired = false;
        drawCommandBar("pair> ", "G0 or Fn+Q: shell");

        while (true) {   //wait for the owner to approve this code
            M5Cardputer.update();
            maintainInternetConnection();
            ledService();
            statusManagement();

            long left = (long)((expiresAt - millis()) / 1000);
            if ((long)(expiresAt - millis()) <= 0) {
                left = 0;
            }
            G0Event g0 = g0Poll();
            if (g0 != G0_NONE) {   //G0 tap: lights, hold: shell (pairing resumes next time)
                remoteExitTo = (g0 == G0_TAP) ? APP_LIGHTS : APP_SHELL;
                remoteHttpClose();
                return false;
            }
            if (left != shownSeconds) {   //redraw once a second (countdown)
                shownSeconds = left;
                remoteDrawPairing(pairCode, pairUrl, left, note);
            }

            if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
                Keyboard_Class::KeysState keys = M5Cardputer.Keyboard.keysState();
                bool quit = (keys.fn && (keysContainChar(keys, 'q') || keysContainChar(keys, 'Q'))) || keys.del ||
                            keysContainChar(keys, '`');
                if (quit) {
                    remoteExitTo = APP_SHELL;
                    remoteHttpClose();
                    outLine("pair: cancelled", C_YELLOW);
                    return false;
                }
            }

            if ((long)(millis() - nextPollAt) >= 0) {
                nextPollAt = millis() + REMOTE_PAIR_POLL_MS;
                JsonDocument body;
                body["secret"] = secret;
                String json;
                serializeJson(body, json);
                int status = remoteRequest("POST", "/media/remote/pair/status", json, false, response);
                if (status != 200) {
                    note = remoteFit(remoteAscii(remoteErrorText(status, response)), 20);   //keep waiting; show why
                    shownSeconds = -1;
                    continue;
                }
                JsonDocument reply;
                if (deserializeJson(reply, response) != DeserializationError::Ok) {
                    continue;   //garbled reply: just ask again next time
                }
                String state = reply["state"] | "";
                if (state == "paired") {
                    remoteKey = reply["key"] | "";
                    remoteName = reply["name"] | "";
                    if (remoteKey.length() == 0) {
                        note = "no key in reply";
                        shownSeconds = -1;
                        continue;
                    }
                    bool saved = remoteSaveSettings();
                    outLine("Paired as \"" + remoteAscii(remoteName) + "\"", C_GREEN);
                    if (!saved) {
                        outLine("warning: could not save the key; pair again after reboot", C_YELLOW);
                    }
                    return true;   //the HTTPS session stays open for the remote
                }
                if (state == "expired") {
                    expired = true;
                    break;
                }
                note = "";   //"waiting": keep counting down
            }
            delay(20);
        }

        if (expired) {   //code ran out: offer a new one rather than spamming the server with codes
            remoteDrawPairing(pairCode, "", 0, "expired: Enter=new");
            drawCommandBar("pair> ", "Enter: new code  G0: lights");
            while (true) {
                M5Cardputer.update();
                ledService();
                G0Event g0 = g0Poll();
                if (g0 != G0_NONE) {   //G0 tap: lights, hold: shell
                    remoteExitTo = (g0 == G0_TAP) ? APP_LIGHTS : APP_SHELL;
                    remoteHttpClose();
                    return false;
                }
                if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
                    Keyboard_Class::KeysState keys = M5Cardputer.Keyboard.keysState();
                    if (keys.enter) {
                        break;   //outer loop asks for a fresh code
                    }
                    if ((keys.fn && keysContainChar(keys, 'q')) || keys.del || keysContainChar(keys, '`')) {
                        remoteExitTo = APP_SHELL;
                        remoteHttpClose();
                        outLine("pair: cancelled", C_YELLOW);
                        return false;
                    }
                }
                delay(20);
            }
        }
    }
}

// --------------------------------------------------------------------- polling

//finds a screen in the last poll by id (-1 if it's gone)
static int remoteIndexOf(int screenId) {
    for (int i = 0; i < remoteScreenCount; i++) {
        if (remoteScreens[i].id == screenId) {
            return i;
        }
    }
    return -1;
}

//the screen being controlled, or nullptr if it vanished from the list
static RemoteScreen* remoteSelected() {
    int index = remoteIndexOf(remoteSelectedId);
    return index >= 0 ? &remoteScreens[index] : nullptr;
}

//GET /api/media/remote: name, owner, and every screen with what it's playing
static RemoteResult remoteRefresh() {
    String response;
    int code = remoteRequest("GET", "/api/media/remote", "", true, response);
    if (code == 401) {
        return RRES_UNPAIRED;   //owner removed this remote (or left the screens group)
    }
    if (code != 200) {
        remoteLastPollOk = false;
        remoteSay(remoteErrorText(code, response), RED);
        return RRES_FAILED;
    }
    JsonDocument doc;
    if (deserializeJson(doc, response) != DeserializationError::Ok) {
        remoteLastPollOk = false;
        remoteSay("unreadable reply from server", RED);
        return RRES_FAILED;
    }
    response = "";   //free the text before filling the structs

    remoteOwner = doc["owner"] | "";
    String name = doc["name"] | "";
    if (name.length() && name != remoteName) {   //the owner renamed it: remember the new name
        remoteName = name;
        remoteSaveSettings();
    }

    remoteScreenCount = 0;
    for (JsonObject s : doc["screens"].as<JsonArray>()) {
        if (remoteScreenCount >= REMOTE_MAX_SCREENS) {
            break;   //more than we have room for; the rest aren't shown
        }
        RemoteScreen& r = remoteScreens[remoteScreenCount++];
        r.id = s["id"] | 0;
        r.name = remoteAscii(s["name"] | "?");
        bool guest = s["guest"] | false;                                    //a TV someone made us a guest on
        r.host = guest ? remoteAscii(s["host"] | "") : String("");          //whose TV it is; empty for our own
        r.online = s["online"] | false;
        r.speaker = s["speaker"] | false;
        JsonObject st = s["state"];
        r.hasState = !st.isNull();
        r.kind = remoteAscii(st["kind"] | "");
        r.title = remoteAscii(st["title"] | "");
        r.sub = remoteAscii(st["sub"] | "");
        r.position = st["position"] | 0.0f;
        r.duration = st["duration"] | 0.0f;
        r.paused = st["paused"] | false;
        r.volume = st["volume"] | -1.0f;   //null until the TV reports it
        r.muted = st["muted"] | false;
        r.night = st["night"] | 0.0f;
    }
    remoteCursor = constrain(remoteCursor, 0, max(0, remoteScreenCount - 1));
    remoteLastPollAt = millis();
    remoteLastPollOk = true;
    return RRES_OK;
}

//GET one page of /api/media/remote/channels starting at offset
static bool remoteLoadChannels(int offset) {
    String response;
    String path = "/api/media/remote/channels?offset=" + String(max(0, offset)) + "&limit=" + String(REMOTE_CHANNEL_PAGE);
    int code = remoteRequest("GET", path, "", true, response);
    if (code == 401) {
        remoteForgetLocal();   //caller sees the empty key and goes back to pairing
        return false;
    }
    if (code != 200) {
        remoteSay(remoteErrorText(code, response), RED);
        return false;
    }
    JsonDocument doc;
    if (deserializeJson(doc, response) != DeserializationError::Ok) {
        remoteSay("unreadable channel list", RED);
        return false;
    }
    remoteChannelTotal = doc["total"] | 0;
    remoteChannelOffset = doc["offset"] | offset;
    remoteChannelCount = 0;
    for (JsonObject c : doc["channels"].as<JsonArray>()) {
        if (remoteChannelCount >= REMOTE_CHANNEL_PAGE) {
            break;
        }
        RemoteChannel& ch = remoteChannels[remoteChannelCount++];
        String kind = c["kind"] | "";
        ch.station = (kind == "station");              //"stream" (live source) or "station" (radio)
        ch.sourceId = c["id"] | 0;
        ch.key = c["key"] | "";
        ch.name = remoteAscii(c["name"] | "?");
    }
    return true;
}

// --------------------------------------------------------------------- commands

//   POST /api/media/remote/screens/{id}/command with an already-built body. Reports
//   the result in the command bar; a 401 wipes the key (the loop then re-pairs).
//   quiet=true skips the "sent" message (gamepad presses would flood it).
static bool remoteSend(JsonDocument& body, bool quiet) {
    RemoteScreen* screen = remoteSelected();
    if (!screen) {
        remoteSay("pick a screen first", YELLOW);
        return false;
    }
    String json;
    serializeJson(body, json);
    String response;
    int code = remoteRequest("POST", "/api/media/remote/screens/" + String(screen->id) + "/command", json, true, response);
    if (code == 401) {
        remoteForgetLocal();
        return false;
    }
    if (code != 200) {
        remoteSay(remoteErrorText(code, response), RED);   //409: "<TV> isn't connected: is Media open on it?"
        return false;
    }
    if (!quiet) {
        remoteSay(String(body["action"].as<const char*>()) + " sent", GREEN);
    }
    remoteNextPollAt = millis() + REMOTE_POLL_AFTER_CMD_MS;   //look again soon to show the effect
    return true;
}

//a command that is just {"action": "..."} (toggle, mute, next, prev, stop, night, audio_track, subtitles)
static void remoteAction(const char* action) {
    JsonDocument body;
    body["action"] = action;
    remoteSend(body, false);
}

//press: up/down/left/right/enter, which the TV page replays as its own arrow keys
static void remotePress(const char* button) {
    JsonDocument body;
    body["action"] = "press";
    body["button"] = button;
    if (remoteSend(body, true)) {
        remoteSay(String(button) + " sent", GREEN);   //short confirmation so a lost press is visible
    }
}

//skip by +/- seconds within what's playing
static void remoteSkip(int seconds) {
    JsonDocument body;
    body["action"] = "skip";
    body["by"] = seconds;
    remoteSend(body, false);
}

//volume +/- one step from the last known level (the API takes an absolute 0..1 level)
static void remoteVolumeStep(int direction) {
    RemoteScreen* screen = remoteSelected();
    if (!screen) {
        return;
    }
    float current = screen->volume >= 0 ? screen->volume : 0.5f;   //TV hasn't said yet: start from the middle
    float level = constrain(current + direction * REMOTE_VOLUME_STEP, 0.0f, 1.0f);
    JsonDocument body;
    body["action"] = "volume";
    body["level"] = roundf(level * 100.0f) / 100.0f;   //two decimals is plenty
    if (remoteSend(body, true)) {
        screen->volume = level;                          //show it now; the next poll confirms
        remoteSay("volume " + String((int)roundf(level * 100)) + "%", GREEN);
    }
}

//starts the highlighted channel on the selected screen
static void remotePlayChannel(const RemoteChannel& ch) {
    JsonDocument body;
    if (ch.station) {
        body["action"] = "play_station";
        body["key"] = ch.key;
    } else {
        body["action"] = "play_source";
        body["source_id"] = ch.sourceId;
    }
    if (remoteSend(body, true)) {
        remoteSay("playing " + ch.name, GREEN);
    }
}

// --------------------------------------------------------------------- gamepad mode

//   SNES-shaped pad, buttons named by place (A right, B bottom, X top, Y left), the
//   server's PAD_BUTTONS. The D-pad is the Cardputer's printed arrows; the face
//   buttons are an E/A/S/D diamond under the left hand (E, not W, is above S/D on
//   this staggered keyboard), with Q and R as the shoulders.
struct RemotePadKey {
    char key;              //character from keysState().word (lowercase)
    const char* button;    //PAD_BUTTONS name sent to the server
};
static const RemotePadKey REMOTE_PAD_KEYS[] = {
    { ';', "up" },    { '.', "down" },  { ',', "left" },  { '/', "right" },   //printed arrows
    { 'd', "a" },     { 's', "b" },     { 'e', "x" },     { 'a', "y" },       //face buttons by place; the rows are
                                                                              //staggered, so E (not W) sits above S/D
    { 'q', "l" },     { 'r', "r" },                                           //shoulders
    { ' ', "select" },                                                        //Enter is start (keys.enter)
};
static const int REMOTE_PAD_KEY_COUNT = sizeof(REMOTE_PAD_KEYS) / sizeof(REMOTE_PAD_KEYS[0]);
static const int REMOTE_PAD_START_BIT = REMOTE_PAD_KEY_COUNT;   //bit for Enter -> start

//which pad buttons the keyboard is holding right now, as a bitmask
static uint16_t remotePadMask(const Keyboard_Class::KeysState& keys) {
    uint16_t mask = 0;
    for (char ch : keys.word) {
        char lower = tolower((unsigned char)ch);
        if (lower == ':') lower = ';';   //shifted arrow reports still mean the arrow
        if (lower == '>') lower = '.';
        if (lower == '<') lower = ',';
        if (lower == '?') lower = '/';
        for (int i = 0; i < REMOTE_PAD_KEY_COUNT; i++) {
            if (REMOTE_PAD_KEYS[i].key == lower) {
                mask |= (1u << i);
            }
        }
    }
    if (keys.enter) {
        mask |= (1u << REMOTE_PAD_START_BIT);
    }
    return mask;
}

//sends one pad button going down or up
static void remotePadSend(int bit, bool down) {
    JsonDocument body;
    body["action"] = "pad";
    body["button"] = bit == REMOTE_PAD_START_BIT ? "start" : REMOTE_PAD_KEYS[bit].button;
    body["down"] = down;
    remoteSend(body, true);
}

//sends a down or up for every button whose state changed since the last report
static void remotePadUpdate(uint16_t nowHeld) {
    uint16_t changed = nowHeld ^ remotePadHeld;
    for (int bit = 0; bit <= REMOTE_PAD_START_BIT; bit++) {
        if (changed & (1u << bit)) {
            remotePadSend(bit, (nowHeld & (1u << bit)) != 0);
            ledPulseInput();
        }
    }
    remotePadHeld = nowHeld;
}

//lets go of every held button (leaving gamepad mode mid-press must not leave one stuck down)
static void remotePadReleaseAll() {
    remotePadUpdate(0);
}

// --------------------------------------------------------------------- drawing

const int REMOTE_ROW_H = 12;    //row pitch on the canvas (matches the terminal)
const int REMOTE_COLS = 38;     //characters that fit across 240px at 6px with padding

//one text row on the canvas (terminalSprite, 240x97 between the two bars)
static void remoteRow(int row, const String& text, uint16_t color) {
    terminalSprite.setTextColor(color, BLACK);
    terminalSprite.drawString(remoteFit(text, REMOTE_COLS), TERMINAL_PADDING, TERMINAL_PADDING + row * REMOTE_ROW_H);
}

//highlighted list row (inverted)
static void remoteRowSelected(int row, const String& text, uint16_t color) {
    int y = TERMINAL_PADDING + row * REMOTE_ROW_H - 2;
    terminalSprite.fillRect(0, y, terminalSprite.width(), REMOTE_ROW_H, color);
    terminalSprite.setTextColor(BLACK, color);
    terminalSprite.drawString(remoteFit(text, REMOTE_COLS), TERMINAL_PADDING, y + 2);
}

//short "what's on" line for the screens list
static String remoteSummary(const RemoteScreen& s) {
    if (!s.online) {
        return "offline";
    }
    if (!s.hasState || s.kind.length() == 0 || s.kind == "idle") {
        return "idle";
    }
    return (s.paused ? "|| " : "> ") + (s.title.length() ? s.title : s.kind);
}

//page: list of screens
static void remoteDrawScreens() {
    String header = remoteName.length() ? remoteName : String("Remote");
    if (remoteOwner.length()) {
        header += " (" + remoteAscii(remoteOwner) + ")";
    }
    remoteRow(0, header, PINK);
    if (remoteScreenCount == 0) {
        remoteRow(2, remoteLastPollOk ? "No screens yet. Pair a TV under" : "Can't reach the server...", YELLOW);
        remoteRow(3, remoteLastPollOk ? "Media > Screens on your phone." : remoteServer, YELLOW);
        return;
    }
    const int visible = 6;                                                //rows 1..6 under the header
    int first = constrain(remoteCursor - visible + 1, 0, max(0, remoteScreenCount - visible));
    for (int i = 0; i < visible && first + i < remoteScreenCount; i++) {
        const RemoteScreen& s = remoteScreens[first + i];
        String name = s.name;
        if (s.host.length()) name += " @" + s.host;                       //guest TV: whose it is
        if (s.speaker) name += " [spk]";
        String line = remoteFit(name, 18);
        while (line.length() < 19) line += ' ';                           //column for the summary
        line += remoteSummary(s);
        if (first + i == remoteCursor) {
            remoteRowSelected(1 + i, line, s.online ? CYAN : 0x8410);
        } else {
            remoteRow(1 + i, line, s.online ? WHITE : 0x8410);            //grey when the TV page isn't open
        }
    }
}

//page: one screen's now-playing panel
static void remoteDrawControl() {
    RemoteScreen* s = remoteSelected();
    if (!s) {
        remoteRow(0, "That screen is gone.", YELLOW);
        remoteRow(1, "Del: back to the list", WHITE);
        return;
    }
    String header = s->name + (s->online ? "" : "  (offline)");
    if (!remoteLastPollOk) header += "  ?";                               //last poll failed: may be stale
    remoteRow(0, header, s->online ? PINK : 0x8410);

    if (!s->hasState || s->kind.length() == 0 || s->kind == "idle") {
        remoteRow(2, s->online ? "Nothing playing." : "Open Media on the TV first.", WHITE);
        remoteRow(3, "0: channels   g: gamepad   h: keys", CYAN);
    } else {
        remoteRow(1, "[" + s->kind + "]", CYAN);
        String title = s->title.length() ? s->title : s->kind;
        remoteRow(2, title.substring(0, REMOTE_COLS), WHITE);              //title over two rows if long
        if ((int)title.length() > REMOTE_COLS) {
            remoteRow(3, title.substring(REMOTE_COLS), WHITE);
        }
        remoteRow(4, s->sub, 0xBDF7);                                     //light grey second line

        //live progress: advance the reported position by the time since that poll
        float position = s->position;
        if (!s->paused && remoteLastPollOk) {
            position += (millis() - remoteLastPollAt) / 1000.0f;
        }
        if (s->duration > 0) {
            position = min(position, s->duration);
            String times = remoteClock(position) + " / " + remoteClock(s->duration);
            remoteRow(5, times, WHITE);
            int barX = TERMINAL_PADDING + (times.length() + 1) * 6;      //bar fills the rest of the row
            int barW = terminalSprite.width() - barX - TERMINAL_PADDING;
            int barY = TERMINAL_PADDING + 5 * REMOTE_ROW_H + 2;
            if (barW > 10) {
                terminalSprite.drawRect(barX, barY, barW, 5, 0x8410);
                terminalSprite.fillRect(barX + 1, barY + 1, (int)((barW - 2) * (position / s->duration)), 3, PINK);
            }
        } else {
            remoteRow(5, s->kind == "live" || s->kind == "radio" ? "live" : remoteClock(position), WHITE);
        }
    }

    String levels = s->paused ? "paused  " : "playing  ";
    levels += "vol " + (s->volume >= 0 ? String((int)roundf(s->volume * 100)) + "%" : String("?"));
    if (s->muted) levels += " MUTED";
    if (s->night > 0) levels += "  night " + String((int)roundf(s->night * 100)) + "%";
    remoteRow(6, levels, s->muted ? YELLOW : GREEN);
}

//page: one page of channels
static void remoteDrawChannels() {
    String header = "Channels";
    if (remoteChannelTotal > 0) {
        header += " " + String(remoteChannelOffset + remoteChannelCursor + 1) + "/" + String(remoteChannelTotal);
    }
    RemoteScreen* s = remoteSelected();
    if (s) header += " -> " + s->name;
    remoteRow(0, header, PINK);
    if (remoteChannelCount == 0) {
        remoteRow(2, "No live streams or stations.", YELLOW);
        return;
    }
    for (int i = 0; i < remoteChannelCount; i++) {
        const RemoteChannel& ch = remoteChannels[i];
        String line = String(ch.station ? "radio " : "live  ") + ch.name;
        if (i == remoteChannelCursor) {
            remoteRowSelected(1 + i, line, CYAN);
        } else {
            remoteRow(1 + i, line, WHITE);
        }
    }
}

//page: gamepad key map
static void remoteDrawPad() {
    RemoteScreen* s = remoteSelected();
    remoteRow(0, "GAMEPAD -> " + (s ? s->name : String("?")), PINK);
    remoteRow(1, "D-pad: arrow keys ; . , /", WHITE);
    remoteRow(2, "     E=X                  Q=L  R=R", WHITE);
    remoteRow(3, "A=Y   D=A    Enter=Start", WHITE);
    remoteRow(4, "    S=B      Space=Select", WHITE);
    String held = "";
    for (int bit = 0; bit <= REMOTE_PAD_START_BIT; bit++) {
        if (remotePadHeld & (1u << bit)) {
            held += String(bit == REMOTE_PAD_START_BIT ? "start" : REMOTE_PAD_KEYS[bit].button) + " ";
        }
    }
    remoteRow(5, "held: " + held, held.length() ? GREEN : 0x8410);
    remoteRow(6, "Fn+Q or ` : leave gamepad", CYAN);
}

//page: key map
static void remoteDrawHelp() {
    remoteRow(0, "arrows ; . , /  Enter OK  spc play", PINK);
    remoteRow(1, "` list  1 stop  2 prev  3 next", WHITE);
    remoteRow(2, "4 -30s  5 +30s  6 mute  7 night", WHITE);
    remoteRow(3, "8 audio  9 subs  0 channels", WHITE);
    remoteRow(4, "- vol down   = vol up", WHITE);
    remoteRow(5, "g gamepad  r refresh  Del list", WHITE);
    remoteRow(6, "G0 tap: lights  hold G0 3s: shell", CYAN);
}

//hints shown in the command bar when there's no message
static String remoteHints() {
    switch (remoteMode) {
        case RMODE_SCREENS:  return "Enter pick  r refresh  G0 lights";
        case RMODE_CONTROL:  return "spc play 1 stop 0 chan h keys";
        case RMODE_CHANNELS: return "Enter play  Del back";
        case RMODE_PAD:      return "gamepad live  ` leave";
        default:             return "any key: back";
    }
}

//redraws the canvas and command bar for the current page
static void remoteDraw() {
    terminalSprite.fillSprite(BLACK);
    terminalSprite.setTextDatum(top_left);
    switch (remoteMode) {
        case RMODE_SCREENS:  remoteDrawScreens(); break;
        case RMODE_CONTROL:  remoteDrawControl(); break;
        case RMODE_CHANNELS: remoteDrawChannels(); break;
        case RMODE_PAD:      remoteDrawPad(); break;
        case RMODE_HELP:     remoteDrawHelp(); break;
    }
    terminalSprite.setTextColor(WHITE, BLACK);
    terminalSprite.pushSprite(0, terminalAreaY());

    bool showMessage = remoteMessage.length() && (long)(remoteMessageUntil - millis()) > 0;
    commandBarSprite.fillSprite(BLACK);
    commandBarSprite.drawFastHLine(0, 0, commandBarSprite.width(), WHITE);
    commandBarSprite.setTextColor(showMessage ? remoteMessageColor : 0xBDF7, BLACK);
    commandBarSprite.drawString(remoteFit(showMessage ? remoteMessage : remoteHints(), REMOTE_COLS),
                                COMMAND_BAR_PADDING, COMMAND_BAR_PADDING);
    commandBarSprite.setTextColor(WHITE, BLACK);
    commandBarSprite.pushSprite(0, commandBarY());
}

// --------------------------------------------------------------------- keys

//true if this keystate carries either character (shifted and unshifted reports differ)
static bool remoteKey2(const Keyboard_Class::KeysState& keys, char a, char b) {
    return keysContainChar(keys, a) || keysContainChar(keys, b);
}

//arrow from the printed-arrow keys, with or without Fn: 0 none, else 'u','d','l','r'
static char remoteArrow(const Keyboard_Class::KeysState& keys) {
    if (remoteKey2(keys, ';', ':')) return 'u';
    if (remoteKey2(keys, '.', '>')) return 'd';
    if (remoteKey2(keys, ',', '<')) return 'l';
    if (remoteKey2(keys, '/', '?')) return 'r';
    return 0;
}

//opens the channel page (first page) for the selected screen
static void remoteOpenChannels() {
    if (!remoteSelected()) {
        remoteSay("pick a screen first", YELLOW);
        return;
    }
    remoteChannelCursor = 0;
    if (remoteLoadChannels(0)) {
        remoteMode = RMODE_CHANNELS;
    }
}

//keys on the screens list
static void remoteKeysScreens(const Keyboard_Class::KeysState& keys) {
    char arrow = remoteArrow(keys);
    if (arrow == 'u') remoteCursor = max(0, remoteCursor - 1);
    if (arrow == 'd') remoteCursor = min(max(0, remoteScreenCount - 1), remoteCursor + 1);
    if (keys.enter && remoteScreenCount > 0) {
        remoteSelectedId = remoteScreens[remoteCursor].id;   //control this one
        remoteMode = RMODE_CONTROL;
    }
    if (remoteKey2(keys, 'r', 'R')) remoteNextPollAt = 0;    //refresh now
    if (remoteKey2(keys, 'h', 'H')) { remoteModeBeforeHelp = remoteMode; remoteMode = RMODE_HELP; }
}

//   keys on the control page. TV functions live on the number row (Doll, 2026-11-23):
//     `  back to the list   1 stop        2 prev       3 next      4 back 30s   5 fwd 30s
//     6  mute               7 night       8 audio trk  9 subtitles 0 channels   - / = volume
//   plus Space play/pause, Enter OK, and the printed arrows. Letters only open pages
//   (g gamepad, h help, r refresh), so nothing on the TV hides behind a letter.
static void remoteKeysControl(const Keyboard_Class::KeysState& keys) {
    char arrow = remoteArrow(keys);
    if (arrow == 'u') { remotePress("up"); return; }
    if (arrow == 'd') { remotePress("down"); return; }
    if (arrow == 'l') { remotePress("left"); return; }
    if (arrow == 'r') { remotePress("right"); return; }
    if (keys.enter)   { remotePress("enter"); return; }                                 //OK
    if (keys.del || keysContainChar(keys, '`')) { remoteMode = RMODE_SCREENS; return; } //back to the screens list
    if (keysContainChar(keys, ' '))         { remoteAction("toggle"); return; }         //play/pause
    if (remoteKey2(keys, '=', '+'))         { remoteVolumeStep(1); return; }            //volume up
    if (remoteKey2(keys, '-', '_'))         { remoteVolumeStep(-1); return; }           //volume down
    if (remoteKey2(keys, '1', '!'))         { remoteAction("stop"); return; }
    if (remoteKey2(keys, '2', '@'))         { remoteAction("prev"); return; }
    if (remoteKey2(keys, '3', '#'))         { remoteAction("next"); return; }
    if (remoteKey2(keys, '4', '$'))         { remoteSkip(-REMOTE_SKIP_SECONDS); return; }
    if (remoteKey2(keys, '5', '%'))         { remoteSkip(REMOTE_SKIP_SECONDS); return; }
    if (remoteKey2(keys, '6', '^'))         { remoteAction("mute"); return; }
    if (remoteKey2(keys, '7', '&'))         { remoteAction("night"); return; }          //next night-filter step
    if (remoteKey2(keys, '8', '*'))         { remoteAction("audio_track"); return; }    //next sound track
    if (remoteKey2(keys, '9', '('))         { remoteAction("subtitles"); return; }      //next subtitle track
    if (remoteKey2(keys, '0', ')'))         { remoteOpenChannels(); return; }           //live streams + radio
    if (remoteKey2(keys, 'r', 'R'))         { remoteNextPollAt = 0; return; }           //refresh now
    if (remoteKey2(keys, 'g', 'G'))         { remotePadHeld = 0; remoteMode = RMODE_PAD; return; }
    if (remoteKey2(keys, 'h', 'H'))         { remoteModeBeforeHelp = remoteMode; remoteMode = RMODE_HELP; return; }
}

//keys on the channel page (fetches the next/previous page at the ends)
static void remoteKeysChannels(const Keyboard_Class::KeysState& keys) {
    char arrow = remoteArrow(keys);
    if (keys.del || keysContainChar(keys, '`')) { remoteMode = RMODE_CONTROL; return; }
    if (arrow == 'u') {
        if (remoteChannelCursor > 0) {
            remoteChannelCursor--;
        } else if (remoteChannelOffset > 0 && remoteLoadChannels(remoteChannelOffset - REMOTE_CHANNEL_PAGE)) {
            remoteChannelCursor = remoteChannelCount - 1;   //previous page, bottom row
        }
    }
    if (arrow == 'd') {
        if (remoteChannelCursor < remoteChannelCount - 1) {
            remoteChannelCursor++;
        } else if (remoteChannelOffset + remoteChannelCount < remoteChannelTotal &&
                   remoteLoadChannels(remoteChannelOffset + REMOTE_CHANNEL_PAGE)) {
            remoteChannelCursor = 0;                        //next page, top row
        }
    }
    if (keys.enter && remoteChannelCount > 0) {
        remotePlayChannel(remoteChannels[remoteChannelCursor]);
        remoteMode = RMODE_CONTROL;                         //back to now-playing to watch it start
    }
}

// --------------------------------------------------------------------- the modal loop

//   No network yet (AP down at boot, or never set up): explains what to do and keeps
//   retrying the saved network. True once online; false when G0 / Fn+Q asks for the shell.
static bool remoteWaitForWifi() {
    if (WiFi.status() == WL_CONNECTED) {
        return true;
    }
    String ssid, password;
    bool haveSaved = loadWifiCredentials(ssid, password);
    if (haveSaved) {
        wifiArmReconnect();   //maintainInternetConnection() now retries it every 10s
    }
    password = "";            //only the name is shown
    unsigned long lastDrawAt = 0;
    while (WiFi.status() != WL_CONNECTED) {
        M5Cardputer.update();
        maintainInternetConnection();
        ledService();
        statusManagement();
        G0Event g0 = g0Poll();
        if (g0 != G0_NONE) {
            remoteExitTo = (g0 == G0_TAP) ? APP_LIGHTS : APP_SHELL;   //hold: shell, to run "wifi connect"
            return false;
        }
        if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
            Keyboard_Class::KeysState keys = M5Cardputer.Keyboard.keysState();
            if (keys.fn && (keysContainChar(keys, 'q') || keysContainChar(keys, 'Q'))) {
                remoteExitTo = APP_SHELL;
                return false; //Fn+Q works too
            }
        }
        if (lastDrawAt == 0 || millis() - lastDrawAt > 1000) {   //once a second is plenty
            lastDrawAt = millis();
            terminalSprite.fillSprite(BLACK);
            terminalSprite.setTextDatum(top_left);
            remoteRow(0, "No Wi-Fi", PINK);
            if (haveSaved) {
                remoteRow(1, "Trying \"" + remoteAscii(ssid) + "\"...", YELLOW);
            } else {
                remoteRow(1, "No network saved yet.", YELLOW);
            }
            remoteRow(3, "Hold G0 3s for the shell, then:", WHITE);
            remoteRow(4, "  wifi connect \"name\" \"password\"", CYAN);
            remoteRow(5, "  wifi save", CYAN);
            remoteRow(6, "Tap G0 there to come back.", WHITE);
            terminalSprite.pushSprite(0, terminalAreaY());
            drawCommandBar("remote> ", "waiting for Wi-Fi  hold G0: shell");
        }
        delay(20);
    }
    return true;
}

//   The remote's own loop: owns screen and keyboard until G0 (tap: lights, hold 3s:
//   shell) or Fn+Q (shell). Waits for Wi-Fi, pairs first if there's no key, and goes
//   back to pairing whenever the server says 401. Returns the app to run next.
DollApp runRemote() {
    remoteLoadSettings();
    remoteExitTo = APP_SHELL;
    if (!remoteWaitForWifi() || (remoteKey.length() == 0 && !remotePairFlow())) {
        return remoteExitTo;   //G0 tap/hold, Fn+Q, or pairing failed (message already printed)
    }

    remoteMode = RMODE_SCREENS;
    remoteScreenCount = 0;
    remoteSelectedId = 0;
    remoteNextPollAt = 0;          //poll straight away
    remoteLastPollOk = true;       //don't flash "can't reach" before the first answer
    remotePadHeld = 0;
    remoteMessage = "";
    drawCommandBar("remote> ", "connecting...");
    unsigned long lastDrawAt = 0;
    bool dirty = true;

    while (true) {
        M5Cardputer.update();
        maintainInternetConnection();   //re-join the saved AP if it drops
        ledService();
        statusManagement();

        G0Event g0 = g0Poll();
        if (g0 != G0_NONE) {   //G0 tap: lights, hold 3s: shell (held pad buttons are released below)
            remoteExitTo = (g0 == G0_TAP) ? APP_LIGHTS : APP_SHELL;
            break;
        }

        //a 401 anywhere wiped the key: pair again (or leave if that's cancelled)
        if (remoteKey.length() == 0) {
            remotePadHeld = 0;
            outLine("This remote was removed on the server.", C_YELLOW);
            if (!remotePairFlow()) {
                break;
            }
            remoteMode = RMODE_SCREENS;
            remoteNextPollAt = 0;
            dirty = true;
        }

        //poll what's playing on the list and control pages only (gamepad/channel pages stay snappy)
        if ((remoteMode == RMODE_SCREENS || remoteMode == RMODE_CONTROL) && (long)(millis() - remoteNextPollAt) >= 0) {
            remoteNextPollAt = millis() + REMOTE_POLL_MS;
            if (remoteRefresh() == RRES_UNPAIRED) {
                remoteForgetLocal();
                continue;   //top of loop handles re-pairing
            }
            if (remoteMode == RMODE_SCREENS && remoteSelectedId == 0 && remoteScreenCount == 1) {
                remoteSelectedId = remoteScreens[0].id;   //only one screen: go straight to it
                remoteMode = RMODE_CONTROL;
            }
            dirty = true;
        }

        if (M5Cardputer.Keyboard.isChange()) {
            Keyboard_Class::KeysState keys = M5Cardputer.Keyboard.keysState();
            bool pressed = M5Cardputer.Keyboard.isPressed();

            if (pressed && keys.fn && remoteKey2(keys, 'q', 'Q')) {   //Fn+Q: leave gamepad, or leave the remote
                if (remoteMode == RMODE_PAD) {
                    remotePadReleaseAll();
                    remoteMode = RMODE_CONTROL;
                    dirty = true;
                    continue;
                }
                remoteExitTo = APP_SHELL;
                break;
            }

            if (remoteMode == RMODE_PAD) {
                //gamepad: every change (presses AND releases) becomes pad down/up events
                if (pressed && keysContainChar(keys, '`')) {
                    remotePadReleaseAll();
                    remoteMode = RMODE_CONTROL;
                } else {
                    remotePadUpdate(pressed ? remotePadMask(keys) : 0);
                }
                dirty = true;
            } else if (pressed && !keyboardEventIsDebounced(keys)) {
                ledPulseInput();
                switch (remoteMode) {
                    case RMODE_SCREENS:  remoteKeysScreens(keys); break;
                    case RMODE_CONTROL:  remoteKeysControl(keys); break;
                    case RMODE_CHANNELS: remoteKeysChannels(keys); break;
                    case RMODE_HELP:     remoteMode = remoteModeBeforeHelp; break;   //any key closes help
                    default: break;
                }
                dirty = true;
            }
        }

        //redraw on change, and twice a second for the progress clock and message timeout
        if (dirty || millis() - lastDrawAt > 500) {
            remoteDraw();
            lastDrawAt = millis();
            dirty = false;
        }
        delay(5);
    }

    remotePadReleaseAll();   //never leave a pad button held on the TV
    remoteHttpClose();       //frees the TLS client before lights opens its MQTT one
    return remoteExitTo;
}

// --------------------------------------------------------------------- shell commands

//"remote" opens the remote; "remote help" lists the related commands
void handleRemoteCommand(const String parts[], int partCount) {
    if (partCount > 1 && parts[1] == "help") {
        outLine("remote          open the remote (pairs first if needed)");
        outLine("pair            pair again (gets a new key)");
        outLine("server [url]    show/set the Mnemerynne address");
        outLine("forget          unpair here and on the server");
        outLine("G0: tap remote<->lights, hold 3s shell");
        return;
    }
    runApps(APP_REMOTE);   //G0 taps switch to lights and back from here too
}

//"pair": drop any local key and run pairing (the owner re-approves on their phone)
void handlePairCommand(const String parts[], int partCount) {
    remoteLoadSettings();
    if (WiFi.status() != WL_CONNECTED) {
        outLine("pair: Wi-Fi not connected. Run 'wifi connect' first.", C_RED);
        return;
    }
    remoteForgetLocal();   //a re-pair under the same name replaces the key server-side anyway
    if (remotePairFlow()) {
        runApps(APP_REMOTE);          //go straight into the remote once approved
    } else if (remoteExitTo != APP_SHELL) {
        runApps(remoteExitTo);        //G0 tapped during pairing: lights
    } else {
        drawTerminalHistory();
    }
}

//"server" shows the address; "server <https://host>" changes it (and unpairs: keys are per server)
void handleServerCommand(const String parts[], int partCount) {
    remoteLoadSettings();
    if (partCount < 2) {
        outLine("Server: " + remoteServer, C_CYAN);
        outLine(String("Paired: ") + (remoteKey.length() ? "yes, as \"" + remoteAscii(remoteName) + "\"" : "no"));
        return;
    }
    String url = parts[1];
    url.trim();
    while (url.endsWith("/")) {
        url.remove(url.length() - 1);   //"https://host/" -> "https://host"
    }
    if (!url.startsWith("https://") || url.length() <= 8) {
        outLine("server: must be an https:// address", C_RED);
        return;
    }
    if (url == remoteServer) {
        outLine("server: unchanged");
        return;
    }
    remoteServer = url;
    remoteKey = "";        //the old server's key means nothing to a new one
    remoteName = "";
    if (remoteSaveSettings()) {
        outLine("Server set to " + remoteServer + ". Run 'pair'.", C_GREEN);
    } else {
        outLine("server: could not save /remote.cfg", C_RED);
    }
}

//"forget": tell the server to drop this remote, then wipe the key here ("forget local" skips the server)
void handleForgetCommand(const String parts[], int partCount) {
    remoteLoadSettings();
    if (remoteKey.length() == 0) {
        outLine("forget: not paired");
        return;
    }
    bool localOnly = partCount > 1 && parts[1] == "local";
    if (!localOnly) {
        String response;
        int code = remoteRequest("DELETE", "/api/media/remote", "", true, response);
        remoteHttpClose();
        if (code != 200 && code != 401) {   //401: already gone server-side, fine
            outLine("forget: server said " + remoteErrorText(code, response), C_RED);
            outLine("Use 'forget local' to wipe it here anyway.", C_YELLOW);
            return;
        }
    }
    remoteForgetLocal();
    outLine(localOnly ? "Key wiped here. Remove it on your phone too." : "Remote forgotten.", C_GREEN);
}
