#pragma once
#include <Arduino.h>
#include "../config/config.h"
struct ZoneConfig{ char name[18]; uint8_t relayIndex; bool enabled; uint16_t countdownMinutes;};
struct FarmSchedule{ bool enabled; uint8_t zone; char startTime[6]; uint16_t durationMinutes; uint8_t repeatDays; int16_t lastStartDay;};
struct FarmConfig{ uint32_t magic; ZoneConfig zones[ZONE_COUNT]; FarmSchedule schedules[MAX_SCHEDULES]; bool holidayMode;};
class Storage{ public: void begin(); FarmConfig& config(){return config_;} void save(); String exportJson()const; void factoryReset(); private: FarmConfig config_; void defaults_();}; extern Storage Store;
