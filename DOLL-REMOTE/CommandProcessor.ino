//   CommandProcessor.ino
//   parses and runs shell commands for DOLL-REMOTE. Forked from DOLL-OS's dispatcher
//   (same tokenizer, history and table), with the command table cut to setup and the remote.

//split commands for ingestion into command subsystem; "quoted words" stay one token
int splitCommand(const String& input, String parts[], int maxParts) {
    String working = input;   //working copy of input
    working.trim();           //remove leading/trailing spaces

    int count = 0;            //tokens stored so far
    int start = 0;            //scan position
    int len = working.length();
    while (start < len && count < maxParts) {
        while (start < len && working[start] == ' ') {
            start++;          //skip runs of spaces
        }
        if (start >= len) {
            break;
        }
        char c = working[start];
        if (c == '"' || c == '\'') {                     //quoted token: up to the matching quote, quotes stripped
            char quote = c;
            start++;
            int end = working.indexOf(quote, start);
            if (end == -1) {                             //no closing quote: take the rest as-is
                parts[count++] = working.substring(start);
                break;
            }
            parts[count++] = working.substring(start, end);
            start = end + 1;
            continue;
        }
        int end = working.indexOf(' ', start);           //next space ends this word
        if (end == -1) {
            parts[count++] = working.substring(start);   //last token
            break;
        }
        parts[count++] = working.substring(start, end);
        start = end + 1;
    }
    return count;
}

static int commandHistoryPhysicalIndex(int logicalIndex) {
    return (commandHistoryHead + logicalIndex) % COMMAND_HISTORY_MAX;
}

//remembers a sent command, evicting the oldest entry once full
void addCommandHistory(const String& cmd) {
    if (cmd.length() == 0) {
        return;
    }
    if (commandHistoryCount < COMMAND_HISTORY_MAX) {
        commandHistory[commandHistoryPhysicalIndex(commandHistoryCount)] = cmd;
        commandHistoryCount++;
    } else {
        commandHistory[commandHistoryHead] = cmd;
        commandHistoryHead = (commandHistoryHead + 1) % COMMAND_HISTORY_MAX;
    }
    commandHistoryIndex = -1;   //sending a command always ends any in-progress recall
}

//steps through sent commands (Fn+; older, Fn+. newer, then back to the stashed draft)
void recallCommandHistory(int step, String& text) {
    if (commandHistoryCount == 0) {
        return;
    }
    if (commandHistoryIndex == -1) {
        if (step > 0) {
            return;                          //already on the live draft
        }
        commandHistoryDraft = text;          //stash in-progress typing
        commandHistoryIndex = commandHistoryCount - 1;
    } else {
        int newIndex = commandHistoryIndex + step;
        if (newIndex < 0) {
            newIndex = 0;
        } else if (newIndex >= commandHistoryCount) {
            commandHistoryIndex = -1;
            text = commandHistoryDraft;      //ran past the newest: restore the draft
            return;
        }
        commandHistoryIndex = newIndex;
    }
    text = commandHistory[commandHistoryPhysicalIndex(commandHistoryIndex)];
}

//dispatch table entry: command name -> handler
struct CommandEntry {
    const char* name;
    void (*handler)(const String parts[], int partCount);
};

void helpCommandHandler(const String parts[], int partCount) {
    outLine("Remote: remote, pair, server, forget", C_CYAN);
    outLine("G0: tap remote<->lights, hold 3s shell", C_CYAN);
    outLine("Lights: lights, motoko [broker] [port]", C_CYAN);
    outLine("System: wifi, status, battery, free,");
    outLine("        uptime, reboot, clear, help");
}

//sorted alphabetically; lookup is a linear scan since the table is tiny
static const CommandEntry commandTable[] = {
    { "battery",   handleBatteryCommand },
    { "forget",    handleForgetCommand },
    { "free",      handleFreeCommand },
    { "help",      helpCommandHandler },
    { "lights",    handleLightsCommand },   //boxchat light controller (Lights.ino)
    { "motoko",    handleMotokoCommand },   //MQTT client for the lights (motoko.ino)
    { "pair",      handlePairCommand },
    { "reboot",    handleRebootCommand },
    { "remote",    handleRemoteCommand },
    { "server",    handleServerCommand },
    { "status",    handleStatusCommand },
    { "uptime",    handleUptimeCommand },
    { "wifi",      handleWifiCommand },
};
static const int commandTableSize = sizeof(commandTable) / sizeof(commandTable[0]);

//takes the finished command buffer, runs it, and clears it for the next entry
void commandProcessor(String& command) {
    if (command.length() == 0) {
        return;                       //nothing typed
    }
    String entered = command;         //copy off the buffer before clearing it
    command = "";
    commandCursorPos = 0;
    commandScrollOffset = 0;

    entered.trim();
    addCommandHistory(entered);       //remember for Fn+;/Fn+. recall

    String parts[6];
    int partCount = splitCommand(entered, parts, 6);
    if (partCount == 0) {
        return;
    }
    if (parts[0] == "clear") {        //clear wipes history without echoing itself
        outClearScreen();
        return;
    }
    echoCommandLine(entered);         //put the typed line in the scrollback

    for (int i = 0; i < commandTableSize; i++) {
        if (parts[0] == commandTable[i].name) {
            commandTable[i].handler(parts, partCount);
            return;
        }
    }
    outLine("Unknown command: " + parts[0], C_RED);
}
