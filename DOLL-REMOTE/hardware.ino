//   hardware.ino
//   handles keyboard input and battery reads for DollOS
// wrote this myself. 
//Keyboard Management

const unsigned long KEYBOARD_DEBOUNCE_MS = 45;

static unsigned long lastKeyboardEventAt = 0;
static uint32_t lastKeyboardEventSignature = 0;

static uint32_t keyboardEventSignature(const Keyboard_Class::KeysState& keys) {
    uint32_t signature = 2166136261u;

    auto mixByte = [&signature](uint8_t value) {
        signature ^= value;
        signature *= 16777619u;
    };

    mixByte(keys.fn ? 1 : 0);
    mixByte(keys.ctrl ? 1 : 0);
    mixByte(keys.del ? 1 : 0);
    mixByte(keys.enter ? 1 : 0);

    for (char ch : keys.word) {
        mixByte((uint8_t)ch);
    }

    return signature;
}

bool keyboardEventIsDebounced(const Keyboard_Class::KeysState& keys) {
    unsigned long now = millis();
    uint32_t signature = keyboardEventSignature(keys);

    if (signature == lastKeyboardEventSignature &&
        now - lastKeyboardEventAt < KEYBOARD_DEBOUNCE_MS) {
        return true;
    }

    lastKeyboardEventAt = now;
    lastKeyboardEventSignature = signature;
    return false;
}

//polls the keyboard once per loop and redraws/dispatches on change
void keyboardLogic(){
    int previousLength = currentCommand.length();        //snapshot enough state to detect an edit without copying the buffer
    int previousCursorPos = commandCursorPos;
    bool enterPressed = readKeyboard(currentCommand);     //read keyboard, updates currentCommand in place
    if(currentCommand.length() != previousLength || commandCursorPos != previousCursorPos){
        drawCommandBar(currentCommand);
    }

    if (enterPressed) {                //enter was hit, hand the finished command off to the processor
        commandProcessor(currentCommand);
    }
}

//checks whether target char is present in this keystate's word buffer
bool keysContainChar(const Keyboard_Class::KeysState& keys, char target) {
    for (char ch : keys.word) {   //loop over every character reported this keystate
        if (ch == target) {
            return true;
        }
    }
    return false;
}

static bool keysContainAnyChar(const Keyboard_Class::KeysState& keys, char first, char second) {
    return keysContainChar(keys, first) || keysContainChar(keys, second);
}

//read the keyboard, appends to text in place and returns true once enter is pressed
bool readKeyboard(String& text) {
    //M5Cardputer.update();

    if (!M5Cardputer.Keyboard.isChange()) {   //nothing changed since last poll, bail early
        return false;
    }

    if (!M5Cardputer.Keyboard.isPressed()) {   //change was a key release, not a press
        return false;
    }

    Keyboard_Class::KeysState keys = M5Cardputer.Keyboard.keysState();
    if (keyboardEventIsDebounced(keys)) {
        return false;
    }

    if (keys.fn) {                              //fn held, treat this as a local shortcut combo instead of text entry
        if (keysContainAnyChar(keys, ';', ':')) {   //fn + up recalls older commands, regardless of shifted report variant
            recallCommandHistory(-1, text);
            commandCursorPos = text.length();   //jump cursor to end of recalled text, like a shell
            return false;
        }

        if (keysContainAnyChar(keys, '.', '>')) {   //fn + down recalls newer commands, regardless of shifted report variant
            recallCommandHistory(1, text);
            commandCursorPos = text.length();   //jump cursor to end of recalled text, like a shell
            return false;
        }

        if (keysContainChar(keys, ',')) {       //fn + , moves the cursor left within the command buffer
            if (commandCursorPos > 0) {
                commandCursorPos--;
            }
            return false;
        }

        if (keysContainChar(keys, '/')) {       //fn + / moves the cursor right within the command buffer
            if (commandCursorPos < text.length()) {
                commandCursorPos++;
            }
            return false;
        }
        return false; // Ignore other function key combinations for now
    }

    if (keys.ctrl) {                            //ctrl held, use the physical up/down keys to scroll terminal history
        if (keysContainAnyChar(keys, ';', ':')) {   //ctrl + up scrolls the terminal history down
            scrollHistory(1);
            return false;
        }

        if (keysContainAnyChar(keys, '.', '>')) {   //ctrl + down scrolls the terminal history up
            scrollHistory(-1);
            return false;
        }
        return false; // Ignore other ctrl combinations for now
    }

    // Exclusive branch: on this keyboard, .word can carry stray characters alongside
    // a backspace press, so handle del on its own and skip the append below.
    if (keys.del) {
        if (commandCursorPos > 0) {
            text.remove(commandCursorPos - 1, 1);   //remove the character just before the cursor
            commandCursorPos--;
        }
    } else {
        // Insert normal characters at the cursor position.
        text.reserve(text.length() + keys.word.size());
        for (char character : keys.word) {
            int oldLength = text.length();
            text += ' ';
            for (int i = oldLength; i > commandCursorPos; i--) {
                text.setCharAt(i, text[i - 1]);
            }
            text.setCharAt(commandCursorPos, character);
            commandCursorPos++;
        }
    }

    // Signal that the command is complete.
    return keys.enter;
}

//   Battery reads. These were TODO stubs that fell through without returning a value
//   (undefined behaviour if anything had ever called them); statusManagement() was reading
//   M5Cardputer.Power inline instead. Named to match DS's SysInfo.ino so the code ported
//   from there -- the "battery" command and the .dapp runtime's $battery builtin -- calls
//   the same thing the status bar does.
//
//   Unlike DS, which has no fuel-gauge chip and estimates percent off a divided ADC pin,
//   the Cardputer's M5Unified power driver reports both directly, so these are thin wrappers
//   rather than approximations.
int readBatteryPercent() {
    return M5Cardputer.Power.getBatteryLevel();
}

//volts, to match DS's signature -- M5Unified reports millivolts
float readBatteryVoltage() {
    return M5Cardputer.Power.getBatteryVoltage() / 1000.0f;
}
