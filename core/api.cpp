#include "api.h"
#if defined(ESP8266)
#include <ESP8266WebServer.h>
static ESP8266WebServer server(80);
#else
#include <WebServer.h>
static WebServer server(80);
#endif
#include "../config/config.h"
#include "../drivers/relay.h"
#include "../drivers/dht.h"
#include "wifi.h"
#include "mqtt.h"
#include "storage.h"
#include "logger.h"
ApiServer Api;
static void cors(){ server.sendHeader("Access-Control-Allow-Origin","*"); server.sendHeader("Cache-Control","no-store"); }
static void json(int code,const String& body){ cors(); server.send(code,"application/json",body); }
static bool requireAuth(){ String key=server.header("X-Auth-Token"); if(key=="admin"||server.arg("token")=="admin") return true; json(401,"{\"error\":\"auth required\"}"); return false; }
void ApiServer::begin(){ server.on("/api/status",HTTP_GET,[]{ const SensorReading&r=DhtSensor.reading(); json(200,String("{\"version\":\"") + SMARTFARM_VERSION + "\",\"wifi\":"+Wifi.json()+",\"mqtt\":"+(Mqtt.connected()?"true":"false")+",\"heap\":"+String(ESP.getFreeHeap())+",\"uptime\":"+String(millis()/1000UL)+",\"temperature\":"+String(r.temperature,1)+",\"humidity\":"+String(r.humidity,1)+",\"relays\":"+Relays.json()+"}"); }); server.on("/api/control",HTTP_POST,[]{ if(!requireAuth())return; uint8_t relay=server.arg("relay").toInt(); bool on=server.arg("state")=="ON"||server.arg("state")=="1"; if(relay<1||relay>RELAY_COUNT){ json(400,"{\"error\":\"invalid relay\"}"); return;} Relays.set(relay-1,on); Mqtt.publishRelay(relay-1); json(200,"{\"ok\":true}"); }); server.on("/api/config",HTTP_GET,[]{ json(200,Store.exportJson()); }); server.on("/api/schedule",HTTP_GET,[]{ json(200,"{\"schedules\":[]}"); }); server.on("/api/log",HTTP_GET,[]{ json(200,Log.json()); }); server.on("/api/history",HTTP_GET,[]{ json(200,Log.json()); }); server.on("/api/reboot",HTTP_POST,[]{ if(!requireAuth())return; json(200,"{\"rebooting\":true}"); ESP.restart(); }); server.on("/api/restart",HTTP_POST,[]{ if(!requireAuth())return; json(200,"{\"restarting\":true}"); ESP.restart(); }); server.begin(); }
void ApiServer::loop(){ server.handleClient(); }
