//   config.h
//   build-time configuration for DOLL-REMOTE
#pragma once

//   Wi-Fi
//   First-boot defaults only, and blank on purpose: set the network on the device with
//   "wifi connect <ssid> <pass>" then "wifi save", which writes /wifi.cfg (LittleFS).
//   (DOLL-OS keeps a default network here; the fork doesn't ship a password in source.)
const char* STA_DEFAULT_SSID = "";
const char* STA_DEFAULT_PASSWORD = "";

//   Mnemerynne server the remote pairs with. Change it on the device with
//   "server https://other.host" (saved in /remote.cfg). Must be https://.
const char* REMOTE_DEFAULT_SERVER = "https://beepsquad.wtf";
//   The address builds before 2026-11-23 defaulted to. An unpaired remote that saved it
//   is moved to the default above (paired ones keep theirs: their key belongs to it).
const char* REMOTE_OLD_DEFAULT_SERVER = "https://erynne.eternalhotels.com";

//   Motoko MQTT (motoko.ino) -- the lights. Same defaults as DOLL-OS;
//   "motoko [broker-ip] [port]" overrides them for one session.
const char* MOTOKO_DEFAULT_BROKER = "192.168.44.4";
const int MOTOKO_DEFAULT_PORT = 1883;
const char* MOTOKO_CLIENT_ID = "MOTOKO-Cardputer";

//   Doll-Lights boxchat broker (Lights.ino), same as Light Console's config.json and
//   the master keypad console. "lights <broker-ip> [port]" overrides it for one session.
const char* LIGHTS_MQTT_HOST = "192.168.1.253";
const int LIGHTS_MQTT_PORT = 1883;
const char* LIGHTS_BASE_TOPIC = "boxchat";
