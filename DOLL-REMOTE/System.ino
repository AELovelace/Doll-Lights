//   System.ino
//   storage mount, heap instrumentation, and the small system commands (battery,
//   free, uptime, reboot, status). Collected from DOLL-OS's storage.ino, free.ino and
//   CommandProcessor.ino, minus everything that was about SD, ssh, telnet or Motoko.
#include <LittleFS.h>
#include <esp_heap_caps.h>

//mounts LittleFS (settings only -- no SD card in this fork)
void initStorage() {
    //begin(true) formats a blank partition on first boot; a half-written one is forced
    //through an explicit format so settings storage is never left dead (as DOLL-OS does)
    if (!LittleFS.begin(true)) {
        if (LittleFS.format() && LittleFS.begin(false)) {
            outLine("LittleFS: was corrupt -- reformatted (settings reset)", C_YELLOW);
        } else {
            outLine("LittleFS: mount failed -- settings won't persist", C_RED);
        }
    }
}

//   G0 button, read once per loop right after M5Cardputer.update(). M5Unified reports a
//   short press as "clicked" and a press past the hold threshold (set to G0_HOLD_MS in
//   setup) as "hold", so letting go after a hold is never mistaken for a tap.
G0Event g0Poll() {
    if (M5Cardputer.BtnA.wasHold()) {
        return G0_HOLD;      //3s reached: fires while still held, so the shell appears without letting go
    }
    if (M5Cardputer.BtnA.wasClicked()) {
        return G0_TAP;       //short press, released
    }
    return G0_NONE;
}

//   Runs apps until one asks for the shell: remote -> (G0 tap) -> lights -> (G0 tap)
//   -> remote ... Each app's loop returns where to go next.
void runApps(DollApp app) {
    while (app != APP_SHELL) {
        app = (app == APP_LIGHTS) ? runLights() : runRemote();
    }
    outLine("Shell. Tap G0 for the remote.", C_CYAN);
    drawTerminalHistory();
}

//remembers free/largest/min heap at a named point; "free details" prints the ring
void recordHeapCheckpoint(const char* tag) {
    int slot;
    if (heapCheckpointCount < HEAP_CHECKPOINT_MAX) {
        slot = (heapCheckpointHead + heapCheckpointCount) % HEAP_CHECKPOINT_MAX;
        heapCheckpointCount++;
    } else {
        slot = heapCheckpointHead;                                   //full: overwrite the oldest
        heapCheckpointHead = (heapCheckpointHead + 1) % HEAP_CHECKPOINT_MAX;
    }
    HeapCheckpoint& checkpoint = heapCheckpoints[slot];
    checkpoint.tag = tag;
    checkpoint.freeHeap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    checkpoint.largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    checkpoint.minFreeHeap = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
}

//pre-sizes the strings typed into, so typing doesn't fragment the heap
void reserveHotStrings() {
    currentCommand.reserve(96);
    commandHistoryDraft.reserve(96);
    motokoChannel.reserve(64);        //MQTT topic being chatted on (motoko.ino)
    motokoInputBuffer.reserve(256);   //MQTT message being typed
    for (int i = 0; i < COMMAND_HISTORY_MAX; i++) {
        commandHistory[i].reserve(48);
    }
}

//"battery": percent and volts
void handleBatteryCommand(const String parts[], int partCount) {
    outLine("Battery: " + String(readBatteryPercent()) + "% (" + String(readBatteryVoltage(), 2) + "V)", C_CYAN);
}

//"free" / "free details": heap now, and the checkpoints recorded since boot
void handleFreeCommand(const String parts[], int partCount) {
    outLine("FREE: " + String(ESP.getFreeHeap()) + "  LARGEST: " + String(ESP.getMaxAllocHeap()), C_CYAN);
    outLine("MIN EVER: " + String(ESP.getMinFreeHeap()), C_CYAN);
    if (partCount > 1 && parts[1] == "details") {
        for (int i = 0; i < heapCheckpointCount; i++) {
            const HeapCheckpoint& c = heapCheckpoints[(heapCheckpointHead + i) % HEAP_CHECKPOINT_MAX];
            outLine(String(c.tag) + " f=" + String(c.freeHeap) + " l=" + String(c.largestBlock), C_CYAN);
        }
    }
}

//"uptime": days, hh:mm:ss since boot
void handleUptimeCommand(const String parts[], int partCount) {
    unsigned long totalSeconds = millis() / 1000;
    char buf[48];
    snprintf(buf, sizeof(buf), "Uptime: %lu days, %02lu:%02lu:%02lu", totalSeconds / 86400,
             (totalSeconds % 86400) / 3600, (totalSeconds % 3600) / 60, totalSeconds % 60);
    outLine(String(buf));
}

//"reboot": paint the message first, since ESP.restart() never returns to loop()
void handleRebootCommand(const String parts[], int partCount) {
    outLine("Restarting...");
    drawTerminalHistory();
    delay(500);
    ESP.restart();
}

//"status": one-screen glance at Wi-Fi and pairing
void handleStatusCommand(const String parts[], int partCount) {
    outLine("DOLL-REMOTE " DOLL_REMOTE_VERSION, C_PINK);
    if (wifiIsConnected() == 1) {
        outLine("Wi-Fi: " + WiFi.SSID() + " " + WiFi.localIP().toString() + " " + String(WiFi.RSSI()) + "dBm");
    } else {
        outLine("Wi-Fi: disconnected");
    }
    String noArgs[1] = { "server" };
    handleServerCommand(noArgs, 1);   //server address + paired yes/no (never the key)
}
