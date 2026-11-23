void callback(char *incomingTopic, byte *payload, unsigned int length) {
    Serial.print("Message arrived in topic: ");
    Serial.println(incomingTopic);
    Serial.print("Message: ");
    for (int i = 0; i < length; i++) {
        Serial.print((char) payload[i]);
    }
    Serial.println();
    Serial.println("-----------------------");

    const bool isDirectTopic = mqtt_direct_topic.equals(incomingTopic); // Shared-topic noise isn't shown on displays.
    char message[32];
    if (length == 0 || length >= sizeof(message)) {
        Serial.println("MQTT command rejected: payload must contain 1-31 characters");
        client.publish(mqtt_status_topic.c_str(), "invalid payload length");
        if (isDirectTopic) {
            setLastCommand("bad length"); // Shown as the last command on the displays.
        }
        return;
    }

    memcpy(message, payload, length);
    message[length] = '\0'; // Makes the MQTT byte payload safe for integer parsing.

    char *parseEnd = nullptr;
    const long parsedValue = strtol(message, &parseEnd, 10);
    while (parseEnd != nullptr && isspace(static_cast<unsigned char>(*parseEnd))) {
        parseEnd++; // Permits harmless spaces and line endings after the number.
    }

    if (parseEnd == message || (parseEnd != nullptr && *parseEnd != '\0')) {
        Serial.println("MQTT command rejected: expected a numeric command");
        client.publish(mqtt_status_topic.c_str(), "invalid numeric command");
        if (isDirectTopic) {
            setLastCommand("not a number");
        }
        return;
    }

    categorizer(static_cast<int>(parsedValue), isDirectTopic); // Shared-topic commands retain board activation.
} // Completes MQTT payload logging, parsing, and command dispatch.

void reconnectMQTT() {
    while (!client.connected()) {
        updateDisplays(); // Shows the current MQTT state while (re)connecting.
        Serial.printf("Connecting to MQTT as %s...", client_id.c_str());
        if (client.connect(client_id.c_str(), mqtt_status_topic.c_str(), 0, true, "offline")) {
            const bool legacySubscribed = client.subscribe(mqtt_command_topic);
            const bool directSubscribed = client.subscribe(mqtt_direct_topic.c_str());
            client.publish(mqtt_status_topic.c_str(), "online", true);
            publishAnalogState(); // Announces both connectivity and the current RGBW output values.
            Serial.printf(
                "connected; subscriptions: %s=%s, %s=%s\n",
                mqtt_command_topic,
                legacySubscribed ? "ok" : "failed",
                mqtt_direct_topic.c_str(),
                directSubscribed ? "ok" : "failed"
            );
        } else {
            Serial.print("MQTT reconnect failed, state=");
            Serial.println(client.state());
            updateDisplays(); // Shows the failure reason before the wait.
            for (int i = 0; i < 50 && !client.connected(); i++) {
                handleOTA();  // Keep OTA responsive during the retry wait, so a board with a down broker stays reflashable.
                delay(100);   // ~5 s total before the next reconnect attempt.
            }
        }
    }
} // Completes blocking MQTT reconnection and topic restoration.

void categorizer(int _val, bool directCommand){
    if (!directCommand && _val > 32000) {
        cmdAct = (_val == cmdActVal); // Arms only the addressed unit and disarms it for every other unit number.
        if (cmdAct) {
            Serial.printf("Board %d armed\n", cmdActVal);
            client.publish(mqtt_status_topic.c_str(), "ready");
            char text[17];
            snprintf(text, sizeof(text), "%d armed", cmdActVal);
            setLastCommand(text);
        } else {
            Serial.printf("Ignoring activation for another board: %d\n", _val);
        }
        return;
    }

    if (!directCommand && !cmdAct) {
        Serial.printf("Ignoring unarmed shared command: %d\n", _val);
        return;
    }

    if (!directCommand) {
        cmdAct = false; // Shared-topic activation applies to exactly one following command.
    }

    if (_val == 0) {
        setAllZones(0, 0, 0, 0); // Provides a simple emergency/all-off command for every zone.
        Serial.println("All zones: 0");
        setLastCommand("0 all off");
        publishAnalogState();
        return;
    }

    // boxchat zone standard: command = block * 1000 + level, where block 1-16 covers zones 1-4.
    // Each zone owns four consecutive blocks (R, G, B, W): zone 1 = 1000-4255, zone 2 = 5000-8255,
    // zone 3 = 9000-12255, zone 4 = 13000-16255. Zone 1 is the original single-zone protocol.
    const int block = _val / 1000; // Which channel of which zone, counted from 1.
    const int ledVal = _val % 1000; // The last three digits are the 0-255 level.
    if (block < 1 || block > ZONE_COUNT * CHANNELS_PER_ZONE || ledVal > 255) {
        Serial.printf("Unknown MQTT command: %d\n", _val); // Includes zones this board does not have.
        client.publish(mqtt_status_topic.c_str(), "unknown command");
        char text[17];
        snprintf(text, sizeof(text), "%d unknown", _val);
        setLastCommand(text);
        return;
    }

    const uint8_t zone = (block - 1) / CHANNELS_PER_ZONE; // 0-based zone index.
    const AnalogChannel channel = static_cast<AnalogChannel>((block - 1) % CHANNELS_PER_ZONE);
    Serial.printf("Zone %u (%s) %s: %d\n", zone + 1, ZONE_NAMES[zone], CHANNEL_NAMES[channel], ledVal);
    char text[17];
    snprintf(text, sizeof(text), "%d %.5s %c%d", _val, ZONE_NAMES[zone], "RGBW"[channel], ledVal); // e.g. "5128 Right R128".
    setLastCommand(text);
    setZoneChannel(zone, channel, ledVal); // Updates one output while retaining every other value.
    publishAnalogState(); // Acknowledges the applied command with the complete resulting state.
} // Completes numeric-command routing for every RGBW channel of every zone.

int appendZoneJson(char *buffer, size_t size, uint8_t zone){
    return snprintf(
        buffer,
        size,
        "{\"r\":%u,\"g\":%u,\"b\":%u,\"w\":%u}",
        analogLevels[zone][RED_CHANNEL],
        analogLevels[zone][GREEN_CHANNEL],
        analogLevels[zone][BLUE_CHANNEL],
        analogLevels[zone][WHITE_CHANNEL]
    ); // Writes one zone as {"r":..,"g":..,"b":..,"w":..}.
} // Completes the JSON object for a single zone.

void publishAnalogState(){
    if (!client.connected()) {
        return; // Startup hardware tests occur before MQTT and do not need an acknowledgement.
    }

    // Top-level r/g/b/w repeat zone 1 so single-zone listeners keep working;
    // "zones" lists every zone in order, e.g. {"r":1,...,"zones":[{"r":1,...},{"r":9,...}]}.
    char state[384];
    size_t used = 0;
    used += snprintf(state + used, sizeof(state) - used, "{\"r\":%u,\"g\":%u,\"b\":%u,\"w\":%u,\"zones\":[",
        analogLevels[0][RED_CHANNEL],
        analogLevels[0][GREEN_CHANNEL],
        analogLevels[0][BLUE_CHANNEL],
        analogLevels[0][WHITE_CHANNEL]);
    for (uint8_t zone = 0; zone < ZONE_COUNT && used < sizeof(state); zone++) {
        if (zone > 0) {
            used += snprintf(state + used, sizeof(state) - used, ","); // Separates zone objects.
        }
        used += appendZoneJson(state + used, sizeof(state) - used, zone);
    }
    if (used < sizeof(state)) {
        snprintf(state + used, sizeof(state) - used, "]}"); // Closes the zones array and the object.
    }
    client.publish(mqtt_state_topic.c_str(), state, true);
} // Completes retained MQTT state acknowledgement publishing for every zone.

