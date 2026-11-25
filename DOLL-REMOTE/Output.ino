//   Output.ino
//   The single line-output entry point for the shell, and the seam that lets code
//   ported from DS (../DS) drop in unchanged. (Forked into DOLL-REMOTE as-is.)
//
//   DS replaced DOLL-OS's sprite terminal with a telnet socket, and in doing so
//   renamed addWrappedHistoryLine(text[, color]) to outLine(text[, color]) and
//   swapped the per-row uint16_t color for an ANSI SGR integer. Every feature DS
//   grew since the fork -- the filesystem commands, the .dapp runtime, the editor --
//   is written against that spelling. Rather than rewrite each call site on the way
//   back, outLine() adopts DS's signature and forwards to DOLL-OS's existing
//   pixel-wrapped history. There is no socket here and no second surface: this is a
//   rename plus a color-space conversion, nothing more.
//
//   See docs/PORT-FROM-DS.md, Phase 2.

//maps an ANSI SGR foreground code (C_*, global.h) onto the M5GFX color the sprite
//actually draws with. Ported from DS's Display.ino, retargeted from TFT_eSPI's
//TFT_* constants to M5GFX's equivalents.
uint16_t ansiCodeToPixelColor(int code) {
    switch (code) {
        case C_BLACK:   return BLACK;
        case C_RED:     return RED;
        case C_GREEN:   return GREEN;
        case C_YELLOW:  return YELLOW;
        case C_BLUE:    return BLUE;
        case C_MAGENTA: return MAGENTA;
        case C_CYAN:    return CYAN;
        case C_PINK:    return PINK;
        default:        return WHITE;
    }
}

void outLine(const String& text) {
    outLine(text, C_WHITE);
}

void outLine(const String& text, int color) {
    addWrappedHistoryLine(text, ansiCodeToPixelColor(color));
}

//clears the terminal history. Was inline in commandProcessor()'s "clear" branch;
//lifted here so the .dapp runtime's CLEAR opcode and any future caller share it.
void outClearScreen() {
    historyCount = 0;
    historyHead = 0;
    scrollOffset = 0;
}

//   The shell prompt. DOLL-OS's is path-aware ("/sd/apps > "), but this fork has no
//   cd/ls and no SD card, so there is no "where" to show -- a fixed prompt does. Kept
//   as a function so the command bar and echoCommandLine() still share one source.
String shellPrompt() {
    return "remote > ";   //fixed prompt: the fork has no working directory to display
}

//echoes a submitted command into the scrollback so the transcript reads like a real
//shell instead of showing output with no record of what asked for it. The command bar
//is cleared on submit, so unlike a real terminal the typed line leaves no trace of its
//own -- this is what puts it in the history.
void echoCommandLine(const String& entered) {
    addWrappedHistoryLine(shellPrompt() + entered, ansiCodeToPixelColor(C_CYAN));
}
