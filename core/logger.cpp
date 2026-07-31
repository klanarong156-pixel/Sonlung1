#include "logger.h"
Logger Log;
void Logger::begin(){ Serial.begin(115200); Serial.println(); }
void Logger::info(const __FlashStringHelper* msg){ Serial.println(msg); }
void Logger::info(const char* msg){ push_("INFO", msg); }
void Logger::warn(const char* msg){ push_("WARN", msg); }
void Logger::event(const char* msg){ push_("EVENT", msg); }
void Logger::push_(const char* level,const char* msg){ snprintf(entries_[head_],sizeof(entries_[head_]),"%lu [%s] %s",millis()/1000UL,level,msg); Serial.println(entries_[head_]); head_=(head_+1)%HISTORY_LIMIT; if(count_<HISTORY_LIMIT) count_++; }
String Logger::json() const{ String out="["; for(uint8_t i=0;i<count_;i++){ uint8_t idx=(head_+HISTORY_LIMIT-count_+i)%HISTORY_LIMIT; if(i) out+=','; out+='"'; for(const char* p=entries_[idx];*p;p++){ if(*p=='"'||*p=='\\') out+='\\'; out+=*p; } out+='"'; } out+=']'; return out; }
