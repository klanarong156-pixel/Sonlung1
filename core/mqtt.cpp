#include "mqtt.h"
#if defined(ESP8266)
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#else
#include <WiFi.h>
#include <WiFiClientSecure.h>
#endif
#include <PubSubClient.h>
#include "../config/config.h"
#include "../drivers/relay.h"
#include "../drivers/dht.h"
#include "wifi.h"
#include "logger.h"
static WiFiClientSecure tls; static PubSubClient client(tls);
static void callback(char* topic, byte* payload, unsigned int len){ char msg[32]; if(len>=sizeof(msg)) return; memcpy(msg,payload,len); msg[len]=0; if(strncmp(topic,"farm/relay/",11)==0){ uint8_t id=atoi(topic+11); if(id>=1&&id<=RELAY_COUNT){ bool on=strcasecmp(msg,"ON")==0||strcmp(msg,"1")==0; Relays.set(id-1,on); Mqtt.publishRelay(id-1); }} if(strcmp(topic,"farm/pump")==0){ Relays.set(PUMP_RELAY_INDEX,strcasecmp(msg,"ON")==0); Mqtt.publishRelay(PUMP_RELAY_INDEX); }}
MqttService Mqtt;
void MqttService::begin(){ tls.setInsecure(); client.setServer(MQTT_HOST,MQTT_PORT); client.setCallback(callback); client.setBufferSize(512); }
bool MqttService::connected()const{ return client.connected(); }
void MqttService::subscribe_(){ client.subscribe("farm/pump",1); for(uint8_t i=0;i<RELAY_COUNT;i++){ char t[24]; snprintf(t,sizeof(t),"farm/relay/%u/set",i+1); client.subscribe(t,1);} }
void MqttService::loop(uint32_t now){
  if(!Wifi.connected()) return;
  if(!client.connected()){
    if(now-lastConnect_<MQTT_RECONNECT_INTERVAL_MS) return;
    lastConnect_=now;
#if defined(ESP8266)
    String id=String(SMARTFARM_HOSTNAME)+"-"+String(ESP.getChipId(),HEX);
#else
    String id=String(SMARTFARM_HOSTNAME)+"-"+String((uint32_t)ESP.getEfuseMac(),HEX);
#endif
    if(client.connect(id.c_str(),MQTT_USER,MQTT_PASS,"farm/status",1,true,"OFFLINE")){
      subscribe_();
      client.publish("farm/status","ONLINE",true);
      client.publish("farm/birth",SMARTFARM_VERSION,true);
      publishStatus();
      Log.event("MQTT connected");
    } else {
      Log.warn("MQTT reconnect failed");
    }
    return;
  }
  client.loop();
  if(now-lastHeartbeat_>=HEARTBEAT_INTERVAL_MS){
    lastHeartbeat_=now;
    client.publish("farm/heartbeat","1",false);
    publishStatus();
  }
}
void MqttService::publishRelay(uint8_t i){ if(!connected())return; char t[28]; snprintf(t,sizeof(t),"farm/relay/%u/status",i+1); client.publish(t,Relays.get(i)?"ON":"OFF",true); if(i==PUMP_RELAY_INDEX) client.publish("farm/pump/status",Relays.get(i)?"ON":"OFF",true); }
void MqttService::publishSensor(){ if(!connected())return; const SensorReading&r=DhtSensor.reading(); char j[240]; snprintf(j,sizeof(j),"{\"temperature\":%.1f,\"humidity\":%.1f,\"wifiRssi\":%ld,\"uptime\":%lu,\"heap\":%lu,\"relays\":%s}",r.temperature,r.humidity,(long)Wifi.rssi(),millis()/1000UL,(unsigned long)ESP.getFreeHeap(),Relays.json().c_str()); client.publish("farm/data",j,true); }
void MqttService::publishStatus(){ publishSensor(); for(uint8_t i=0;i<RELAY_COUNT;i++) publishRelay(i); }
void MqttService::publishEvent(const char* event){ if(connected()) client.publish("farm/event",event,false); }
