//   terminal.ino
//   status bar, scrollback and command bar drawing. Forked from DOLL-OS's terminal.ino;
//   the ssh/telnet streaming API and the USB-mode banner were cut out with those features.

static int spriteCharWidth(LGFX_Sprite& sprite, char ch) {
    char glyph[2] = { ch, '\0' };
    return (int)sprite.textWidth(glyph);
}

static int spriteTextWidthRange(LGFX_Sprite& sprite, const String& text, int start, int end) {
    int clampedStart = constrain(start, 0, (int)text.length());
    int clampedEnd = constrain(end, clampedStart, (int)text.length());
    int width = 0;

    for (int i = clampedStart; i < clampedEnd; i++) {
        width += spriteCharWidth(sprite, text[i]);
    }
    return width;
}

static void drawSpriteTextRange(LGFX_Sprite& sprite, const String& text, int start, int x, int y, int maxWidth) {
    int cursorX = x;
    int limitX = x + maxWidth;

    for (int i = constrain(start, 0, (int)text.length()); i < text.length(); i++) {
        char glyph[2] = { text[i], '\0' };
        int glyphWidth = (int)sprite.textWidth(glyph);
        if (cursorX + glyphWidth > limitX) {
            break;
        }
        sprite.drawString(glyph, cursorX, y);
        cursorX += glyphWidth;
    }
}

//status bar management
void statusManagement(){
    //check battery if it's been more than 60 ticks
    if(refreshCounter >= 60){
        batteryPercent = readBatteryPercent();                              //get batteryPercent (hardware.ino)
        batteryMillivolts = M5Cardputer.Power.getBatteryVoltage();          //get battery voltage
        char batteryText[48];
        snprintf(batteryText, sizeof(batteryText), "FREEMEM:%luKB B:%d%% %.2fV",
            (unsigned long)(ESP.getFreeHeap() / 1000),
            batteryPercent,
            batteryMillivolts / 1000.0f);
        //draw Dollputer banner
        statusBarSprite.fillSprite(BLACK);                                  //fill sprite area with black
        statusBarSprite.setTextDatum(top_left);                             //text align top-left
        statusBarSprite.setTextColor(PINK,BLACK);                           //set text color
        statusBarSprite.drawString("DOLL-REMOTE",5,0);                //fork name in the header so it never gets mistaken for full DOLL-OS
        //draw battery percentage
        statusBarSprite.setTextDatum(top_right);
        statusBarSprite.drawString(batteryText, statusBarSprite.width() - 5, 0);
        //draw divider
        statusBarSprite.drawFastHLine(0, STATUS_BAR_HEIGHT - 2, statusBarSprite.width(), PINK);
        //push updated sprite
        statusBarSprite.pushSprite(0,0);
        //reset refresh counter
        refreshCounter = 0;
    }   else{
        refreshCounter++;
    }
}

//Terminal area code

//returns terminal area
int terminalAreaY(){
    return STATUS_BAR_HEIGHT;
}

//returns terminal like height
int terminalVisibleLines(){
    const int lineHeight = 12;                                                                  //line height of 12px
    const int availableHeight = max(0, (int)terminalSprite.height() - (TERMINAL_PADDING * 2));  //figure out the max available space by taking total pixel height of terminalSprite. TERMINAL_PADDING * 2 subtracts padding from both top and bottom and clamps result to minimum of 0
    return max(1, availableHeight / lineHeight);    //max 1 prevents 0 from being returned, takes available height of screen, divides by lineheight, and returns number of possible rows.                                           
}

static int historyPhysicalIndex(int logicalIndex) {
    return (historyHead + logicalIndex) % HISTORY_MAX_LINES;
}

static void copyHistoryText(char* dest, const String& src) {
    int copyLen = min((int)src.length(), HISTORY_ROW_MAX_CHARS - 1);

    for (int i = 0; i < copyLen; i++) {
        dest[i] = src[i];
    }
    dest[copyLen] = '\0';

    if (src.length() >= HISTORY_ROW_MAX_CHARS && HISTORY_ROW_MAX_CHARS > 4) {
        dest[HISTORY_ROW_MAX_CHARS - 4] = '.';
        dest[HISTORY_ROW_MAX_CHARS - 3] = '.';
        dest[HISTORY_ROW_MAX_CHARS - 2] = '.';
        dest[HISTORY_ROW_MAX_CHARS - 1] = '\0';
    }
}

static const char* historyRowText(int logicalIndex) {
    return historyRows[historyPhysicalIndex(logicalIndex)].text;
}

static uint16_t historyRowColor(int logicalIndex) {
    return historyRows[historyPhysicalIndex(logicalIndex)].color;
}

void addHistoryRow(const String& row, uint16_t color) {
    //a brand-new row is about to become "the last row" through the normal (non-streaming) path --
    //any stream that still thought it owned the previous last row doesn't anymore
    //(the stream-owner bookkeeping from DOLL-OS is gone with ssh/telnet -- every row here is a whole line)

    int slot;
    if (historyCount < HISTORY_MAX_LINES) {             //if history count is less than max lines, add to history.
        slot = historyPhysicalIndex(historyCount);
        historyCount++;
    } else {
        slot = historyHead;
        historyHead = (historyHead + 1) % HISTORY_MAX_LINES;
    }

    copyHistoryText(historyRows[slot].text, row);
    historyRows[slot].color = color;
    //scrollOffset is intentionally left untouched here -- forcing it to 0 on every row would
    //yank a user who's scrolled back to read history down to the bottom on every new line
}

//plain overload: existing call sites that don't care about color keep working unchanged
void addWrappedHistoryLine(const String& line) {
    addWrappedHistoryLine(line, WHITE);
}

void addWrappedHistoryLine(const String& line, uint16_t color) {
    const int maxWidth = (int)terminalSprite.width() - (TERMINAL_PADDING * 2);  //keep test in terminal sprite padding
    if (maxWidth <0){
        return;
    }

    String row = "";    //build temporary string to store row in.
    row.reserve(min((int)line.length(), HISTORY_ROW_MAX_CHARS - 1));
    int rowWidth = 0;

    for(int i = 0; i < line.length(); i++){
        char ch = line[i]; //get character at index i
        int charWidth = spriteCharWidth(terminalSprite, ch);

        //if adding this character exceeds the row width
        if(row.length() > 0 && rowWidth + charWidth > maxWidth){
            addHistoryRow(row, color);
            row = "";
            rowWidth = 0;

            //skip leading space on next row
            if (ch == ' '){
                continue;
            }
        }

        row += ch;
        rowWidth += charWidth;
    }

    addHistoryRow(row, color); //add any remaining text as a new row
}


void scrollHistory(int delta) {
    const int maxScroll = max(0, historyCount - terminalVisibleLines());
    scrollOffset = constrain(scrollOffset + delta, 0, maxScroll);
}

void drawTerminalHistory() {
    terminalSprite.fillSprite(BLACK);
    terminalSprite.setTextDatum(top_left);

    if (historyCount == 0) {
        terminalSprite.pushSprite(0, terminalAreaY());
        return;
    }

    const int lineHeight = 12;
    const int visibleLines = terminalVisibleLines();
    const int maxScroll = max(0, historyCount - visibleLines);

    scrollOffset = constrain(scrollOffset, 0, maxScroll);

    int lastLine = historyCount - 1 - scrollOffset;
    int firstLine = max(0, lastLine - visibleLines + 1);

    int y = TERMINAL_PADDING;
    for (int i = firstLine; i <= lastLine; i++) {
        terminalSprite.setTextColor(historyRowColor(i), BLACK);
        terminalSprite.drawString(historyRowText(i), TERMINAL_PADDING, y);
        y += lineHeight;
    }
    terminalSprite.setTextColor(WHITE, BLACK);   //reset so anything drawing without its own color call gets the default
    terminalSprite.pushSprite(0, terminalAreaY());

}


//boot logo
void drawBootLogo() {
    M5Cardputer.Display.setTextSize(2);
    const int top = terminalAreaY();
    const int height = M5Cardputer.Display.height() - top - COMMAND_BAR_HEIGHT;
    M5Cardputer.Display.fillRect(0, top, M5Cardputer.Display.width(), height, CYAN);
    M5Cardputer.Display.setTextDatum(middle_center);
    M5Cardputer.Display.setTextColor(PINK, BLACK);
    M5Cardputer.Display.drawString("DOLL-REMOTE", M5Cardputer.Display.width() / 2, top + height / 2 - 8);
    M5Cardputer.Display.drawString(DOLL_REMOTE_VERSION, M5Cardputer.Display.width() / 2, top + height / 2 + 8);
    M5Cardputer.Display.setTextSize(1);
}
//Command Bar Area
int commandBarY(){
    return M5Cardputer.Display.height() - COMMAND_BAR_HEIGHT;
}

//plain overload: existing call sites that just show "> " over currentCommand keep working unchanged
void drawCommandBar(const String& text) {
    drawCommandBar("> ", text);
}

//Draws command bar at bottom of the screen. `prompt` is a fixed, non-editable label (e.g. "> ",
//"channel> ", "password> ") pinned at the left edge and drawn in full every time -- it never
//scrolls out of view. `text` is the live-editable buffer; commandCursorPos/commandScrollOffset
//(maintained by readKeyboard) index directly into it, so callers should pass their raw input
//buffer here rather than pre-concatenating the prompt onto it -- concatenating would shift the
//buffer's character indices and make the cursor land in the wrong place.
void drawCommandBar(const String& prompt, const String& text) {
    //get location of y from commandBarY();
    commandBarSprite.fillSprite(BLACK);
    commandBarSprite.drawFastHLine(0,0,commandBarSprite.width(), WHITE);
    commandBarSprite.drawString(prompt, COMMAND_BAR_PADDING,COMMAND_BAR_PADDING);

    const int textX = COMMAND_BAR_PADDING + (int)commandBarSprite.textWidth(prompt);
    const int maxWidth = max(0, (int)commandBarSprite.width() - textX - COMMAND_BAR_PADDING);

    commandCursorPos = constrain(commandCursorPos, 0, (int)text.length());

    //scroll left so the cursor is never left of the visible window
    if (commandCursorPos < commandScrollOffset) {
        commandScrollOffset = commandCursorPos;
    }
    //scroll right so the cursor is never right of the visible window
    while (spriteTextWidthRange(commandBarSprite, text, commandScrollOffset, commandCursorPos) > maxWidth) {
        commandScrollOffset++;
    }
    //pull the window back left while there's still room, so deleting text reveals earlier characters again
    while (commandScrollOffset > 0 &&
           spriteTextWidthRange(commandBarSprite, text, commandScrollOffset - 1, commandCursorPos) <= maxWidth) {
        commandScrollOffset--;
    }
    commandScrollOffset = constrain(commandScrollOffset, 0, (int)text.length());

    drawSpriteTextRange(commandBarSprite, text, commandScrollOffset, textX, COMMAND_BAR_PADDING, maxWidth);

    //draw a beam cursor at the current position within the visible text
    int cursorX = textX + spriteTextWidthRange(commandBarSprite, text, commandScrollOffset, commandCursorPos);
    commandBarSprite.drawFastVLine(cursorX, COMMAND_BAR_PADDING, 10, WHITE);

    commandBarSprite.pushSprite(0,commandBarY());
}

