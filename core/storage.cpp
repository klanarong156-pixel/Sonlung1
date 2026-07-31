#include "storage.h"
#if defined(ESP8266)
#include <LittleFS.h>
#define FSYS LittleFS
#else
#include <LittleFS.h>
#define FSYS LittleFS
#endif
#include "logger.h"
Storage Store; static const uint32_t MAGIC=0x53465039UL;
void Storage::begin(){ FSYS.begin(); File f=FSYS.open("/config.bin","r"); if(!f||f.readBytes((char*)&config_,sizeof(config_))!=sizeof(config_)||config_.magic!=MAGIC){ defaults_(); save(); } }
void Storage::defaults_(){ memset(&config_,0,sizeof(config_)); config_.magic=MAGIC; const char* names[ZONE_COUNT]={"Pump","Greenhouse","Vegetables","Garden"}; for(uint8_t i=0;i<ZONE_COUNT;i++){ strncpy(config_.zones[i].name,names[i],sizeof(config_.zones[i].name)-1); config_.zones[i].relayIndex=i; config_.zones[i].enabled=true; config_.zones[i].countdownMinutes=DEFAULT_PUMP_DURATION_MINUTES;} config_.schedules[0]={true,0,"06:00",10,0x7F,-1}; config_.schedules[1]={true,0,"18:00",10,0x7F,-1}; }
void Storage::save(){ File b=FSYS.open("/config.bak","w"); if(b){ b.write((uint8_t*)&config_,sizeof(config_)); b.close(); } File f=FSYS.open("/config.bin","w"); if(f){ f.write((uint8_t*)&config_,sizeof(config_)); f.close(); }}
String Storage::exportJson()const{ String s="{\"version\":\"" SMARTFARM_VERSION "\",\"holiday\":"; s+=config_.holidayMode?"true":"false"; s+=",\"zones\":["; for(uint8_t i=0;i<ZONE_COUNT;i++){ if(i)s+=','; s += "{\"name\":\""; s += config_.zones[i].name; s += "\",\"enabled\":"; s += config_.zones[i].enabled ? "true" : "false"; s += "}";} return s+"]}"; }
void Storage::factoryReset(){ defaults_(); save(); }
