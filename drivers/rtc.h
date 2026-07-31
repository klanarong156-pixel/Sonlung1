#pragma once
#include <Arduino.h>
#include <RTClib.h>
class RtcClock{ public: void begin(); void poll(uint32_t now); bool available()const{return available_;} DateTime now()const; private: bool available_=false; uint32_t lastRetry_=0;};
extern RtcClock Clock;
