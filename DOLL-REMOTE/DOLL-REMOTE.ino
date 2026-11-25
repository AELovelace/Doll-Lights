//   DOLL-REMOTE.ino
//   entry point for DOLL-REMOTE: a slimmed fork of DOLL-OS that turns the Cardputer
//   ADV into a pocket remote for Mnemerynne's Media screens (Remote.ino).
//
//   What came over from DOLL-OS: the sprite UI (status bar / scrollback / command bar),
//   keyboard input, Wi-Fi scan/connect/save, LittleFS settings, the LED, and a small
//   shell for setup. What was cut to free RAM: SD card, FTP, USB mass storage, the
//   Bluetooth keyboard, ssh, telnet, Motoko MQTT, the .dapp runtime, the editor, Dapper
//   and aliases. See docs/REMOTE.md.
#include <M5Cardputer.h>
#include <WiFi.h>
#include <FS.h>
#include "config.h"
#include "global.h"

void setup() {
  recordHeapCheckpoint("setup start");

  auto cfg = M5.config();                     //config cardputer, auto-detects the board type (ADV keyboard included)
  M5Cardputer.begin(cfg, true);               //start cardputer hardware, true == keyboard on
  ledBegin();                                 //LED to full brightness, then off
  reserveHotStrings();                        //pre-size the buffers typed into most

  M5Cardputer.Display.fillScreen(BLACK);      //clear to black on boot
  M5Cardputer.Display.setTextSize(1);         //base text size for the display
  M5Cardputer.Display.setTextColor(WHITE);    //base text color for the display

  //status bar sprite, sits at the very top of the screen
  statusBarSprite.setColorDepth(16);
  statusBarSprite.createSprite(M5Cardputer.Display.width(), STATUS_BAR_HEIGHT);
  //terminal sprite: scrollback in the shell, and the remote's pages while it runs
  terminalSprite.setColorDepth(16);
  terminalSprite.createSprite(M5Cardputer.Display.width(), M5Cardputer.Display.height() - STATUS_BAR_HEIGHT - COMMAND_BAR_HEIGHT);
  terminalSprite.setTextColor(WHITE, BLACK);
  //command bar sprite, sits at the very bottom of the screen
  commandBarSprite.setColorDepth(16);
  commandBarSprite.createSprite(M5Cardputer.Display.width(), COMMAND_BAR_HEIGHT);
  commandBarSprite.setTextColor(WHITE, BLACK);
  recordHeapCheckpoint("after sprites");

  drawBootLogo();                             //"DOLL-REMOTE" splash (terminal.ino)
  delay(800);
  initStorage();                              //LittleFS for /wifi.cfg and /remote.cfg (System.ino)
  statusManagement();                         //draw the status bar now rather than 60 loops from now

  outLine("DOLL-REMOTE " DOLL_REMOTE_VERSION, C_PINK);
  outLine("Type 'help' for commands.");

  //   Unlike DOLL-OS, the remote joins its saved network at boot: it's an appliance,
  //   and a remote that needs typing before every use isn't one. Only a network saved
  //   with "wifi save" is used -- nothing is joined if there isn't one.
  String ssid, password;
  if (loadWifiCredentials(ssid, password)) {
    connectWifiNetwork(ssid, password);       //blocks up to 15s, prints the result
  } else {
    outLine("No saved Wi-Fi. Run: wifi connect <ssid> <pass>", C_YELLOW);
    outLine("then: wifi save", C_YELLOW);
  }

  //   The remote is the default app: it always opens at boot. With no Wi-Fi it waits on
  //   its own "no Wi-Fi" page (retrying the saved network); unpaired, it shows pairing.
  //   G0: tap switches remote <-> lights, hold 3s drops to this shell, tap here goes back.
  M5Cardputer.BtnA.setHoldThresh(G0_HOLD_MS);   //"hold" = 3s on G0
  recordHeapCheckpoint("setup done");
  runApps(APP_REMOTE);
}

void loop() {
  M5Cardputer.update();             //update cardputer status (keyboard scan + G0 button)
  if (g0Poll() == G0_TAP) {         //G0 tap in the shell: back to the remote (a hold's release is not a tap)
    runApps(APP_REMOTE);
    return;                         //runApps() redrew the shell on its way out
  }
  statusManagement();               //status bar: battery + free memory
  drawTerminalHistory();            //scrollback
  drawCommandBar(shellPrompt(), currentCommand);   //typing line
  keyboardLogic();                  //keys -> command buffer -> commandProcessor on Enter
  maintainInternetConnection();     //bounded reconnect tick if the AP went away (wifi.ino)
  ledService();                     //settle LED pulses
}
