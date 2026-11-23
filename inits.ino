void startWiFi(){
    WiFi.mode(WIFI_STA); // Keeps the product in client mode without creating an access point.
    WiFi.begin(ssid, password);

    while(WiFi.status() != WL_CONNECTED){
        Serial.print('.'); // Reports connection progress on USB serial...
        updateDisplays(); // ...and on the status displays, if fitted.
        delay(250);
    }
    Serial.println();
    Serial.print("WiFi connected. IP: ");
    Serial.println(WiFi.localIP());
} // Completes station-mode Wi-Fi connection and Serial reporting.

void startMQTT(){
    client.setServer(mqtt_broker, mqtt_port);
    client.setBufferSize(MQTT_BUFFER_SIZE); // The default 256 bytes is too small for four zones of state JSON.
    client.setCallback(callback);
    client_id = "MrBox-" + WiFi.macAddress(); // Prevents duplicate client IDs from disconnecting each other.
    client_id.replace(":", "");
    const String deviceTopic = String(mqtt_command_topic) + "/" + cmdActVal;
    mqtt_direct_topic = deviceTopic + "/set"; // Allows optional direct commands addressed only to this board.
    mqtt_status_topic = deviceTopic + "/status";
    mqtt_state_topic = deviceTopic + "/state";
    reconnectMQTT(); // Connects once and installs every required subscription.
} // Completes MQTT setup, connection, and topic subscription.

void startPins(){
    for (uint8_t zone = 0; zone < ZONE_COUNT; zone++) {
        zoneOutputReady[zone] = beginZoneOutput(zone); // Gives the zone its own RMT channel, idling low.
        Serial.printf("Zone %u (%s) data on GPIO %u: %s\n", zone + 1, ZONE_NAMES[zone], ZONE_DATA_PINS[zone],
                      zoneOutputReady[zone] ? "ready" : "RMT INIT FAILED");
        showZone(zone); // Transmits the all-off RGBW frame so the converter starts dark.
    }
} // Completes Dig2Analog data-output initialization for every zone.

