#pragma once
#include <Arduino.h>
class WifiService{ public: void begin(); void loop(uint32_t now); bool connected()const; int32_t rssi()const; uint32_t reconnects()const{return reconnects_;} String json()const; private: uint32_t lastAttempt_=0,reconnects_=0;}; extern WifiService Wifi;
