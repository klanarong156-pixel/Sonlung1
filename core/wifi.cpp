#include "wifi.h"
#if defined(ESP8266)
#include <ESP8266WiFi.h>
#else
#include <WiFi.h>
#endif
#include <WiFiManager.h>
#include "../config/config.h"
#include "logger.h"
WifiService Wifi;
void WifiService::begin(){ WiFi.mode(WIFI_STA); WiFi.setHostname(SMARTFARM_HOSTNAME); WiFi.setAutoReconnect(true); WiFi.persistent(false); WiFiManager wm; wm.setConfigPortalBlocking(false); wm.setConfigPortalTimeout(180); if(!wm.autoConnect(SMARTFARM_AP_NAME)) Log.warn("WiFi portal started or connection pending"); }
void WifiService::loop(uint32_t now){ if(connected()) return; if(now-lastAttempt_>=WIFI_RECONNECT_INTERVAL_MS){ lastAttempt_=now; reconnects_++; WiFi.reconnect(); Log.warn("WiFi reconnect requested"); }}
bool WifiService::connected()const{ return WiFi.status()==WL_CONNECTED; }
int32_t WifiService::rssi()const{ return connected()?WiFi.RSSI():0; }
String WifiService::json()const{ return String("{\"connected\":")+(connected()?"true":"false")+",\"rssi\":"+String(rssi())+",\"reconnects\":"+String(reconnects_)+"}"; }
