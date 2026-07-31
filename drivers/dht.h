#pragma once
#include <Arduino.h>
struct SensorReading{ float temperature=NAN; float humidity=NAN; uint32_t updatedAt=0; bool valid=false;};
class DhtDriver{ public: void begin(); bool poll(uint32_t now); const SensorReading& reading()const{return reading_;} private: SensorReading reading_; uint32_t last_=0;};
extern DhtDriver DhtSensor;
