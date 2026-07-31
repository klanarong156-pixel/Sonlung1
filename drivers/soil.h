#pragma once
#include <Arduino.h>
class SoilSensor{ public: void begin(){} int readPercent(uint8_t){return -1;} }; extern SoilSensor Soil;
