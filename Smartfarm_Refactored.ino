#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <RTClib.h>
#include <EEPROM.h>
#include <DHT.h>
#include <ctype.h>
#include <string.h>

const char* mqtt_server = "650188a0ee2b4367b7c131fb385590a9.s1.eu.hivemq.cloud";
const int mqtt_port = 8883;
const char* mqtt_user = "smartfarm";
const char* mqtt_pass = "Kla12345";

const char* topic_pump = "farm/pump";
const char* topic_pump_status = "farm/pump/status";
const char* topic_pump_countdown = "farm/pump/countdown";
const char* topic_pump_countdown_start = "farm/pump/countdown/start";
const char* topic_pump_countdown_stop = "farm/pump/countdown/stop";
const char* topic_pump_duration = "farm/pump/duration";
const char* topic_status = "farm/status";
const char* topic_relay_set_prefix = "farm/relay/";
const char* topic_relay_set_suffix = "/set";
const char* topic_time = "farm/time";
const char* topic_mode = "farm/mode";
const char* topic_schedule = "farm/schedule";
const char* topic_schedule_status = "farm/schedule/status";
const char* topic_temperature = "farm/temp";
const char* topic_humidity = "farm/hum";
const char* topic_sensor_data = "farm/data";

const byte RELAY_COUNT = 4;
const byte RELAY_PINS[RELAY_COUNT] = {D5, D6, D7, D0};
const char* RELAY_NAMES[RELAY_COUNT] = {"pump", "zone1", "zone2", "zone3"};
const bool RELAY_ACTIVE_LOW = true;
#define DHT_PIN D4
#define DHTTYPE DHT11
#define I2C_SDA D2
#define I2C_SCL D1

const byte PUMP_RELAY_INDEX = 0;
const byte ZONE_COUNT = 4;
const byte MAX_SCHEDULES = 12;
const byte HISTORY_LIMIT = 12;
const char* FIRMWARE_VERSION = "SmartFarm Pro 8.0.0";
const unsigned int DEFAULT_PUMP_DURATION_MINUTES = 15;
const unsigned int MIN_PUMP_DURATION_MINUTES = 1;
const unsigned int MAX_PUMP_DURATION_MINUTES = 120;
const unsigned long HEARTBEAT_INTERVAL = 30000;
const unsigned long RTC_INTERVAL = 1000;
const unsigned long DHT_INTERVAL = 2000;
const unsigned long MQTT_RECONNECT_INTERVAL = 5000;
const unsigned long WIFI_RECONNECT_INTERVAL = 10000;
const unsigned long RTC_RETRY_INTERVAL = 10000;
const unsigned int MQTT_MESSAGE_BUFFER_SIZE = 180;

struct ScheduleData { char onTime1[6]; char offTime1[6]; char onTime2[6]; char offTime2[6]; };
enum OperatingMode { MODE_AUTO, MODE_MANUAL, MODE_COUNTDOWN };
struct ZoneConfig { char name[18]; byte relayIndex; bool enabled; unsigned int countdownMinutes; unsigned long runtimeToday; unsigned int activationCount; };
struct ProSchedule { bool enabled; byte zone; char startTime[6]; unsigned int durationMinutes; byte repeatDays; int lastStartDay; };
struct SmartFarmConfig { uint32_t magic; ScheduleData legacy; ZoneConfig zones[ZONE_COUNT]; ProSchedule schedules[MAX_SCHEDULES]; };

RTC_DS3231 rtc;
WiFiClientSecure espClient;
PubSubClient client(espClient);
DHT dht(DHT_PIN, DHTTYPE);

ScheduleData schedules;
SmartFarmConfig farmConfig;
OperatingMode currentMode = MODE_AUTO;
bool isAutoMode = true;
bool relayStates[RELAY_COUNT] = {false, false, false, false};
bool zoneStates[ZONE_COUNT] = {false, false, false, false};
bool scheduleActive[MAX_SCHEDULES] = {false};
unsigned long scheduleFinishAt[MAX_SCHEDULES] = {0};
float temperature = NAN, humidity = NAN;
bool rtcAvailable = false;
unsigned long lastWiFiReconnect = 0, lastRTCRetry = 0, lastHeartbeat = 0, lastRTCUpdate = 0, lastDHTRead = 0, lastMQTTReconnect = 0;
unsigned long pumpCountdownStartedAt = 0, lastPumpCountdownTick = 0, pumpCountdownDurationSeconds = DEFAULT_PUMP_DURATION_MINUTES * 60UL, pumpCountdownRemainingSeconds = 0;
bool pumpCountdownActive = false;
byte countdownZoneMask = 0x01;
unsigned long pumpOnMillis = 0, pumpRuntimeTodaySeconds = 0;
char lastScheduleAction[6] = "";
char history[HISTORY_LIMIT][48]; byte historyHead = 0;
const uint32_t CONFIG_MAGIC = 0x53465038UL;
const int RELAY_STATE_EEPROM_ADDR = sizeof(SmartFarmConfig);
const int EEPROM_SIZE = sizeof(SmartFarmConfig) + RELAY_COUNT + 16;

bool isDigitChar(char c) { return c >= '0' && c <= '9'; }
bool isValidScheduleTime(const char* v) { if (strlen(v) != 5 || v[2] != ':') return false; if (!isDigitChar(v[0]) || !isDigitChar(v[1]) || !isDigitChar(v[3]) || !isDigitChar(v[4])) return false; byte h=(v[0]-'0')*10+v[1]-'0', m=(v[3]-'0')*10+v[4]-'0'; return h < 24 && m < 60; }
bool isValidScheduleData(const ScheduleData& d) { return isValidScheduleTime(d.onTime1)&&isValidScheduleTime(d.offTime1)&&isValidScheduleTime(d.onTime2)&&isValidScheduleTime(d.offTime2); }
void setDefaultSchedule(){ strcpy(schedules.onTime1,"06:00"); strcpy(schedules.offTime1,"06:10"); strcpy(schedules.onTime2,"17:00"); strcpy(schedules.offTime2,"17:10"); }
const char* modeToText(){ return currentMode==MODE_AUTO?"AUTO":(currentMode==MODE_COUNTDOWN?"COUNTDOWN":"MANUAL"); }
void addHistory(const char* e){ snprintf(history[historyHead], sizeof(history[historyHead]), "%lu:%s", millis()/1000UL, e); historyHead=(historyHead+1)%HISTORY_LIMIT; }

void writeRelayPin(byte i){ if(i>=RELAY_COUNT)return; bool level=RELAY_ACTIVE_LOW ? !relayStates[i] : relayStates[i]; digitalWrite(RELAY_PINS[i], level?HIGH:LOW); }
void publishPumpStatus(); void publishPumpCountdown(); void publishSensorData(); void publishMode(); void publishScheduleStatus();
void saveConfigToEEPROM(){ EEPROM.begin(EEPROM_SIZE); EEPROM.put(0,farmConfig); EEPROM.commit(); EEPROM.end(); }
void setDefaultConfig(){ memset(&farmConfig,0,sizeof(farmConfig)); farmConfig.magic=CONFIG_MAGIC; setDefaultSchedule(); farmConfig.legacy=schedules; const char* n[ZONE_COUNT]={"Greenhouse","Vegetables","Garden","Field"}; for(byte i=0;i<ZONE_COUNT;i++){ strncpy(farmConfig.zones[i].name,n[i],sizeof(farmConfig.zones[i].name)-1); farmConfig.zones[i].relayIndex=i; farmConfig.zones[i].enabled=true; farmConfig.zones[i].countdownMinutes=DEFAULT_PUMP_DURATION_MINUTES; } farmConfig.schedules[0]={true,0,"06:00",10,0x7F,-1}; farmConfig.schedules[1]={true,0,"12:00",5,0x7F,-1}; farmConfig.schedules[2]={true,0,"18:30",15,0x7F,-1}; }
void loadScheduleFromEEPROM(){ EEPROM.begin(EEPROM_SIZE); EEPROM.get(0,farmConfig); EEPROM.end(); if(farmConfig.magic!=CONFIG_MAGIC || !isValidScheduleData(farmConfig.legacy)){ setDefaultConfig(); saveConfigToEEPROM(); Serial.println("Initialized SmartFarm Pro config"); } schedules=farmConfig.legacy; }
void saveScheduleToEEPROM(){ farmConfig.legacy=schedules; saveConfigToEEPROM(); Serial.println("Saved Schedule to EEPROM"); }
void loadRelayStatesFromEEPROM(){ EEPROM.begin(EEPROM_SIZE); for(byte i=0;i<RELAY_COUNT;i++) relayStates[i]=EEPROM.read(RELAY_STATE_EEPROM_ADDR+i)==1; EEPROM.end(); }
void saveRelayStateToEEPROM(byte i){ if(i>=RELAY_COUNT)return; EEPROM.begin(EEPROM_SIZE); EEPROM.write(RELAY_STATE_EEPROM_ADDR+i, relayStates[i]?1:0); EEPROM.commit(); EEPROM.end(); }

void publishZoneStatus(byte z){ if(!client.connected()||z>=ZONE_COUNT)return; char t[32]; snprintf(t,sizeof(t),"farm/zone%d/status",z+1); client.publish(t, zoneStates[z]?"ON":"OFF", true); }
void publishAllZoneStatus(){ for(byte i=0;i<ZONE_COUNT;i++) publishZoneStatus(i); }
void publishZoneConfig(){ if(!client.connected())return; char j[360]; snprintf(j,sizeof(j),"{\"z1\":\"%s\",\"z2\":\"%s\",\"z3\":\"%s\",\"z4\":\"%s\"}",farmConfig.zones[0].name,farmConfig.zones[1].name,farmConfig.zones[2].name,farmConfig.zones[3].name); client.publish("farm/zones/config",j,true); }
void publishRelayStatus(byte i){ if(i>=RELAY_COUNT)return; char t[32]; snprintf(t,sizeof(t),"farm/relay/%d/status",i+1); client.publish(t, relayStates[i]?"ON":"OFF",true); if(i==PUMP_RELAY_INDEX){ publishPumpStatus(); client.publish(topic_status, relayStates[i]?"ON":"OFF",true);} }
void publishAllRelayStatus(){ for(byte i=0;i<RELAY_COUNT;i++) publishRelayStatus(i); }
void setRelay(byte i,bool st){ if(i>=RELAY_COUNT || relayStates[i]==st)return; relayStates[i]=st; writeRelayPin(i); saveRelayStateToEEPROM(i); if(i==PUMP_RELAY_INDEX){ if(st) pumpOnMillis=millis(); else if(pumpOnMillis) { pumpRuntimeTodaySeconds+=(millis()-pumpOnMillis)/1000UL; pumpOnMillis=0; } addHistory(st?"Pump ON":"Pump OFF"); } publishRelayStatus(i); publishSensorData(); }
void recomputePump(){ bool any=false; for(byte i=0;i<ZONE_COUNT;i++) if(zoneStates[i]) any=true; setRelay(PUMP_RELAY_INDEX, any); }
void setZone(byte z,bool st){ if(z>=ZONE_COUNT || !farmConfig.zones[z].enabled)return; if(zoneStates[z]==st)return; zoneStates[z]=st; byte r=farmConfig.zones[z].relayIndex; if(r<RELAY_COUNT && r!=PUMP_RELAY_INDEX) setRelay(r,st); if(st){ farmConfig.zones[z].activationCount++; addHistory("Zone Started"); } else addHistory("Zone Finished"); publishZoneStatus(z); recomputePump(); }
void allZonesOff(){ for(byte i=0;i<ZONE_COUNT;i++) setZone(i,false); recomputePump(); }

void formatCountdown(unsigned long s,char* b,size_t n){ snprintf(b,n,"%02u:%02u",(unsigned int)(s/60UL),(unsigned int)(s%60UL)); }
void publishPumpStatus(){ if(client.connected()) client.publish(topic_pump_status, relayStates[PUMP_RELAY_INDEX]?"ON":"OFF", true); }
void publishPumpCountdown(){ if(!client.connected())return; char b[8]; formatCountdown(pumpCountdownRemainingSeconds,b,sizeof(b)); client.publish(topic_pump_countdown,b,true); }
void startPumpCountdown(unsigned int min){ if(currentMode!=MODE_COUNTDOWN){ Serial.println("Ignored: countdown only runs in COUNTDOWN mode"); return; } if(min<MIN_PUMP_DURATION_MINUTES)min=MIN_PUMP_DURATION_MINUTES; if(min>MAX_PUMP_DURATION_MINUTES)min=MAX_PUMP_DURATION_MINUTES; pumpCountdownDurationSeconds=min*60UL; pumpCountdownRemainingSeconds=pumpCountdownDurationSeconds; pumpCountdownStartedAt=lastPumpCountdownTick=millis(); pumpCountdownActive=true; for(byte i=0;i<ZONE_COUNT;i++) if(countdownZoneMask&(1<<i)) setZone(i,true); setRelay(PUMP_RELAY_INDEX,true); publishPumpCountdown(); publishPumpStatus(); addHistory("Countdown Started"); }
void stopPumpCountdown(bool off){ pumpCountdownActive=false; pumpCountdownRemainingSeconds=0; if(off) allZonesOff(); publishPumpCountdown(); publishPumpStatus(); addHistory("Countdown Finished"); }
void updatePumpCountdown(){ if(!pumpCountdownActive)return; unsigned long now=millis(), elapsed=(now-pumpCountdownStartedAt)/1000UL; unsigned long rem=elapsed>=pumpCountdownDurationSeconds?0:pumpCountdownDurationSeconds-elapsed; if(now-lastPumpCountdownTick>=1000UL || rem!=pumpCountdownRemainingSeconds){ lastPumpCountdownTick=now; pumpCountdownRemainingSeconds=rem; publishPumpCountdown(); if(rem==0) stopPumpCountdown(true); } }

void publishTime(){ if(!rtcAvailable||!client.connected())return; DateTime n=rtc.now(); char t[20]; sprintf(t,"%02d:%02d:%02d",n.hour(),n.minute(),n.second()); client.publish(topic_time,t,true); }
void publishHeartbeat(){ client.publish(topic_status,"ONLINE",true); }
void publishMode(){ if(client.connected()) client.publish(topic_mode,modeToText(),true); }
void publishScheduleStatus(){ if(!client.connected())return; char b[420]; int o=snprintf(b,sizeof(b),"legacy=%s,%s,%s,%s;",schedules.onTime1,schedules.offTime1,schedules.onTime2,schedules.offTime2); for(byte i=0;i<MAX_SCHEDULES&&o<(int)sizeof(b)-28;i++) if(farmConfig.schedules[i].enabled) o+=snprintf(b+o,sizeof(b)-o,"%u,%s,%u,%u|",farmConfig.schedules[i].zone+1,farmConfig.schedules[i].startTime,farmConfig.schedules[i].durationMinutes,farmConfig.schedules[i].repeatDays); client.publish(topic_schedule_status,b,true); }
void publishSensorData(){ if(!client.connected())return; if(!isnan(temperature)){ char tb[12]; dtostrf(temperature,4,1,tb); client.publish(topic_temperature,tb,true);} if(!isnan(humidity)){ char hb[12]; dtostrf(humidity,4,1,hb); client.publish(topic_humidity,hb,true);} char j[260]; snprintf(j,sizeof(j),"{\"temperature\":%.1f,\"humidity\":%.1f,\"pump\":\"%s\",\"zone1\":\"%s\",\"zone2\":\"%s\",\"zone3\":\"%s\",\"zone4\":\"%s\",\"mode\":\"%s\",\"runtimeToday\":%lu}",temperature,humidity,relayStates[0]?"ON":"OFF",zoneStates[0]?"ON":"OFF",zoneStates[1]?"ON":"OFF",zoneStates[2]?"ON":"OFF",zoneStates[3]?"ON":"OFF",modeToText(),pumpRuntimeTodaySeconds); client.publish(topic_sensor_data,j,true); }
void readDHTSensor(){ float h=dht.readHumidity(), t=dht.readTemperature(); if(isnan(h)||isnan(t)){ Serial.println("WARNING: Failed to read DHT11"); return;} humidity=h; temperature=t; publishSensorData(); }
void checkRTC(){ if(rtcAvailable)return; unsigned long now=millis(); if(now-lastRTCRetry<RTC_RETRY_INTERVAL)return; lastRTCRetry=now; rtcAvailable=rtc.begin(); if(rtcAvailable && rtc.lostPower()) rtc.adjust(DateTime(F(__DATE__),F(__TIME__))); }

bool copyTrimmedPayload(char* d,size_t n,const byte* p,unsigned int l){ if(n==0||l>=n)return false; memcpy(d,p,l); d[l]='\0'; char* s=d; while(*s&&isspace((unsigned char)*s))s++; char* e=s+strlen(s); while(e>s&&isspace((unsigned char)*(e-1)))e--; *e='\0'; if(s!=d) memmove(d,s,e-s+1); return true; }
bool copyScheduleToken(char* d,size_t n,const char* s,size_t l){ while(l&&isspace((unsigned char)*s)){s++;l--;} while(l&&isspace((unsigned char)s[l-1]))l--; if(l!=5||n<6)return false; memcpy(d,s,l); d[l]='\0'; return isValidScheduleTime(d); }
bool parseScheduleMessage(const char* m,ScheduleData& ps){ const char* a=strchr(m,','); if(!a)return false; const char* b=strchr(a+1,','); if(!b)return false; const char* c=strchr(b+1,','); if(!c||strchr(c+1,','))return false; return copyScheduleToken(ps.onTime1,6,m,a-m)&&copyScheduleToken(ps.offTime1,6,a+1,b-a-1)&&copyScheduleToken(ps.onTime2,6,b+1,c-b-1)&&copyScheduleToken(ps.offTime2,6,c+1,strlen(c+1)); }

void setMode(OperatingMode m){ if(currentMode==m)return; if(currentMode==MODE_COUNTDOWN) stopPumpCountdown(true); if(m==MODE_MANUAL) { pumpCountdownActive=false; allZonesOff(); } currentMode=m; isAutoMode=(m==MODE_AUTO); publishMode(); addHistory(modeToText()); }
void handleZoneTopic(char* topic,const char* msg){ int z=topic[9]-'1'; if(z<0||z>=ZONE_COUNT)return; if(strstr(topic,"/name/set")){ strncpy(farmConfig.zones[z].name,msg,sizeof(farmConfig.zones[z].name)-1); farmConfig.zones[z].name[sizeof(farmConfig.zones[z].name)-1]='\0'; saveConfigToEEPROM(); publishZoneConfig(); return; } if(strstr(topic,"/enable/set")){ farmConfig.zones[z].enabled=strcmp(msg,"OFF")!=0; saveConfigToEEPROM(); publishZoneConfig(); return; } if(strstr(topic,"/set") && currentMode==MODE_MANUAL){ if(strcmp(msg,"ON")==0)setZone(z,true); else if(strcmp(msg,"OFF")==0)setZone(z,false); } }

void mqttCallback(char* topic, byte* payload, unsigned int length){ char msg[MQTT_MESSAGE_BUFFER_SIZE]; if(!copyTrimmedPayload(msg,sizeof(msg),payload,length)){ Serial.println("Ignored: MQTT payload too long"); return; } Serial.print(topic); Serial.print(" => "); Serial.println(msg);
  if(strncmp(topic,"farm/zone",9)==0){ handleZoneTopic(topic,msg); return; }
  if(strcmp(topic,topic_pump)==0){ if(currentMode==MODE_MANUAL){ if(strcmp(msg,"ON")==0)setRelay(PUMP_RELAY_INDEX,true); else if(strcmp(msg,"OFF")==0)setRelay(PUMP_RELAY_INDEX,false); } else Serial.println("Ignored pump manual command outside MANUAL"); }
  else if(strncmp(topic,topic_relay_set_prefix,strlen(topic_relay_set_prefix))==0){ if(currentMode!=MODE_MANUAL){ Serial.println("Ignored relay manual command outside MANUAL"); return;} size_t tl=strlen(topic), pl=strlen(topic_relay_set_prefix), sl=strlen(topic_relay_set_suffix); if(tl>pl+sl && strcmp(topic+tl-sl,topic_relay_set_suffix)==0){ int rn=atoi(topic+pl); if(rn>=1&&rn<=RELAY_COUNT){ if(strcmp(msg,"ON")==0)setRelay(rn-1,true); else if(strcmp(msg,"OFF")==0)setRelay(rn-1,false); } } }
  else if(strcmp(topic,topic_pump_duration)==0){ int m=atoi(msg); if(m>=1&&m<=120) pumpCountdownDurationSeconds=m*60UL; }
  else if(strcmp(topic,"farm/countdown/zones")==0){ byte mask=0; for(unsigned int i=0;i<strlen(msg);i++) if(msg[i]>='1'&&msg[i]<='4') mask|=1<<(msg[i]-'1'); if(mask) countdownZoneMask=mask; }
  else if(strcmp(topic,topic_pump_countdown_start)==0){ if(currentMode!=MODE_COUNTDOWN) setMode(MODE_COUNTDOWN); int m=atoi(msg); startPumpCountdown(m?m:pumpCountdownDurationSeconds/60UL); }
  else if(strcmp(topic,topic_pump_countdown_stop)==0) stopPumpCountdown(true);
  else if(strcmp(topic,topic_mode)==0){ if(strcmp(msg,"AUTO")==0)setMode(MODE_AUTO); else if(strcmp(msg,"MANUAL")==0)setMode(MODE_MANUAL); else if(strcmp(msg,"COUNTDOWN")==0)setMode(MODE_COUNTDOWN); }
  else if(strcmp(topic,topic_schedule)==0){ ScheduleData ps; if(parseScheduleMessage(msg,ps)){ schedules=ps; farmConfig.legacy=ps; saveConfigToEEPROM(); publishScheduleStatus(); } }
}

void connectMQTT(){ if(client.connected())return; unsigned long now=millis(); if(now-lastMQTTReconnect<MQTT_RECONNECT_INTERVAL)return; lastMQTTReconnect=now; if(client.connect("ESP8266FarmPro",mqtt_user,mqtt_pass,topic_status,0,true,"OFFLINE")){ client.subscribe(topic_pump); client.subscribe(topic_mode); client.subscribe(topic_schedule); client.subscribe(topic_pump_duration); client.subscribe(topic_pump_countdown_start); client.subscribe(topic_pump_countdown_stop); client.subscribe("farm/countdown/zones"); for(byte i=0;i<RELAY_COUNT;i++){ char t[32]; snprintf(t,sizeof(t),"farm/relay/%d/set",i+1); client.subscribe(t);} for(byte i=0;i<ZONE_COUNT;i++){ char t[36]; snprintf(t,sizeof(t),"farm/zone%d/#",i+1); client.subscribe(t);} publishAllRelayStatus(); publishAllZoneStatus(); publishMode(); publishScheduleStatus(); publishZoneConfig(); publishSensorData(); publishPumpCountdown(); client.publish("farm/firmware/version",FIRMWARE_VERSION,true); } else { Serial.print("MQTT failed rc="); Serial.println(client.state()); } }

void startSchedule(byte i){ if(i>=MAX_SCHEDULES)return; ProSchedule &s=farmConfig.schedules[i]; if(!s.enabled||s.zone>=ZONE_COUNT||!farmConfig.zones[s.zone].enabled)return; scheduleActive[i]=true; scheduleFinishAt[i]=millis()+s.durationMinutes*60000UL; setZone(s.zone,true); addHistory("Schedule Started"); }
void checkSchedule(){ if(currentMode!=MODE_AUTO||!rtcAvailable)return; DateTime n=rtc.now(); char cur[6]; sprintf(cur,"%02d:%02d",n.hour(),n.minute()); byte dow=n.dayOfTheWeek(); for(byte i=0;i<MAX_SCHEDULES;i++){ ProSchedule &s=farmConfig.schedules[i]; if(s.enabled && strcmp(cur,s.startTime)==0 && s.lastStartDay!=n.day() && (s.repeatDays&(1<<dow))){ s.lastStartDay=n.day(); startSchedule(i); } if(scheduleActive[i] && (long)(millis()-scheduleFinishAt[i])>=0){ scheduleActive[i]=false; bool zoneStillNeeded=false; for(byte j=0;j<MAX_SCHEDULES;j++) if(scheduleActive[j]&&farmConfig.schedules[j].zone==s.zone) zoneStillNeeded=true; if(!zoneStillNeeded) setZone(s.zone,false); addHistory("Schedule Finished"); } } }

void setup(){ Serial.begin(115200); Serial.println("\n===== SMART FARM PROFESSIONAL STARTING ====="); for(byte i=0;i<RELAY_COUNT;i++){ pinMode(RELAY_PINS[i],OUTPUT); relayStates[i]=false; writeRelayPin(i);} loadScheduleFromEEPROM(); loadRelayStatesFromEEPROM(); for(byte i=0;i<RELAY_COUNT;i++) writeRelayPin(i); WiFiManager wm; WiFi.mode(WIFI_STA); WiFi.setAutoReconnect(true); WiFi.persistent(false); if(!wm.autoConnect("SmartFarm_Setup")) ESP.restart(); Wire.begin(I2C_SDA,I2C_SCL); rtcAvailable=rtc.begin(); if(rtcAvailable&&rtc.lostPower()) rtc.adjust(DateTime(F(__DATE__),F(__TIME__))); dht.begin(); espClient.setInsecure(); client.setServer(mqtt_server,mqtt_port); client.setCallback(mqttCallback); ESP.wdtEnable(WDTO_8S); }
void loop(){ ESP.wdtFeed(); if(WiFi.status()!=WL_CONNECTED){ unsigned long now=millis(); if(now-lastWiFiReconnect>=WIFI_RECONNECT_INTERVAL){ lastWiFiReconnect=now; WiFi.reconnect(); } return; } if(!client.connected()) connectMQTT(); else client.loop(); unsigned long now=millis(); if(now-lastRTCUpdate>=RTC_INTERVAL){ lastRTCUpdate=now; checkRTC(); publishTime(); checkSchedule(); } updatePumpCountdown(); if(now-lastDHTRead>=DHT_INTERVAL){ lastDHTRead=now; readDHTSensor(); } if(now-lastHeartbeat>=HEARTBEAT_INTERVAL){ lastHeartbeat=now; if(client.connected()) publishHeartbeat(); } }
