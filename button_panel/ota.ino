// Over-the-air firmware updates for the button panel, so it never needs USB again.
// OTA only works once this code is already running, so the FIRST flash that installs it
// must still be over USB; every later update can then arrive over Wi-Fi. Two paths share
// one OTA-capable partition (two app slots, so a bad upload rolls back):
//   * ArduinoOTA - push a build with arduino-cli / espota (no web page).
//   * ElegantOTA - a browser page at http://btnpanel-32006.local/update to upload a .bin.

#define ELEGANTOTA_USE_ASYNC_WEBSERVER 0 // Built-in sync WebServer, so no AsyncTCP dependency is needed.
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <ElegantOTA.h>

// CHANGE THIS before flashing (or pass -DOTA_PASSWORD=... at build time): the only thing
// stopping another LAN device from reflashing this panel. Match the MrBox boards' password.
#ifndef OTA_PASSWORD
#define OTA_PASSWORD "boxchat-ota"
#endif
#define OTA_WEB_USER "doll" // Username for the ElegantOTA /update web page.

WebServer otaServer(80); // Serves the ElegantOTA upload page.
String otaHostname;      // mDNS name "btnpanel-32006", so the DHCP IP doesn't matter.

void startOTA() {
    otaHostname = String("btnpanel-") + PANEL_ID; // PANEL_ID is this panel's boxchat number.

    ArduinoOTA.setHostname(otaHostname.c_str());
    ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.onStart([]() { Serial.println("ArduinoOTA: update starting"); tmShowText("UPdAtE  "); });
    ArduinoOTA.onEnd([]() { Serial.println("ArduinoOTA: update finished, rebooting"); });
    ArduinoOTA.onError([](ota_error_t error) { Serial.printf("ArduinoOTA error %u\n", error); });
    ArduinoOTA.begin(); // Starts the push listener and advertises the panel over mDNS.

    ElegantOTA.setAuth(OTA_WEB_USER, OTA_PASSWORD); // Password-protects the web upload page.
    ElegantOTA.begin(&otaServer);                   // Mounts /update on the web server.
    otaServer.begin();

    Serial.printf("OTA ready: push 'arduino-cli upload -p %s.local' or open http://%s.local/update\n",
                  otaHostname.c_str(), otaHostname.c_str());
} // Completes OTA setup so future firmware arrives over Wi-Fi instead of USB.

void handleOTA() {
    ArduinoOTA.handle();      // Services an in-progress arduino-cli / espota push.
    otaServer.handleClient(); // Services the ElegantOTA web page and any upload in flight.
    ElegantOTA.loop();        // Finalizes a completed web upload and reboots into the new image.
} // Completes one OTA service pass; call it every loop.
