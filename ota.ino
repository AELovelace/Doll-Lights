// Over-the-air firmware updates, so a wall-mounted board never needs USB again.
// IMPORTANT: OTA only works once this code is already running on the board, so the
// FIRST flash that installs it must still be over USB. After that, every later update
// can arrive over Wi-Fi. Two paths share one OTA-capable partition layout (two app
// slots, so a failed or interrupted upload rolls back to the firmware still running):
//   * ArduinoOTA - push a build over Wi-Fi with arduino-cli / espota (no web page).
//   * ElegantOTA - a browser page at http://<host>/update to upload a .bin by hand.
// Both are reached at the board's mDNS name, e.g. http://mrbox-32004.local/update.

#define ELEGANTOTA_USE_ASYNC_WEBSERVER 0 // Use the built-in sync WebServer, so no AsyncTCP dependency is needed.
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <ElegantOTA.h>

// CHANGE THIS before flashing (or pass -DOTA_PASSWORD=... at build time). It is the only
// thing stopping another device on the LAN from reflashing this board, so pick your own.
#ifndef OTA_PASSWORD
#define OTA_PASSWORD "boxchat-ota"
#endif
#define OTA_WEB_USER "doll" // Username for the ElegantOTA /update web page (password is OTA_PASSWORD).

WebServer otaServer(80); // Serves the ElegantOTA upload page on the usual HTTP port.
String otaHostname;      // Per-unit mDNS name, e.g. "mrbox-32004", so DHCP IPs don't matter.

void startOTA() {
    otaHostname = String("mrbox-") + cmdActVal; // cmdActVal is this unit's activator (BUILD_FOR_UNIT).

    ArduinoOTA.setHostname(otaHostname.c_str());
    ArduinoOTA.setPassword(OTA_PASSWORD); // arduino-cli / espota must supply the same password.
    ArduinoOTA.onStart([]() { Serial.println("ArduinoOTA: update starting"); });
    ArduinoOTA.onEnd([]() { Serial.println("ArduinoOTA: update finished, rebooting"); });
    ArduinoOTA.onError([](ota_error_t error) { Serial.printf("ArduinoOTA error %u\n", error); });
    ArduinoOTA.begin(); // Starts the push listener and advertises the board over mDNS.

    ElegantOTA.setAuth(OTA_WEB_USER, OTA_PASSWORD); // Password-protects the browser upload page.
    ElegantOTA.begin(&otaServer);                   // Mounts /update on the web server.
    otaServer.begin();

    Serial.printf("OTA ready: push 'arduino-cli upload -p %s.local' or open http://%s.local/update\n",
                  otaHostname.c_str(), otaHostname.c_str());
} // Completes OTA setup so future firmware arrives over Wi-Fi instead of USB.

void handleOTA() {
    ArduinoOTA.handle();      // Services an in-progress arduino-cli / espota push.
    otaServer.handleClient(); // Services the ElegantOTA web page and any upload in flight.
    ElegantOTA.loop();        // Finalizes a completed web upload and reboots into the new image.
} // Completes one OTA service pass; call it every loop and during any blocking wait.
