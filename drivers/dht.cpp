#include "dht.h"
#include <DHT.h>
#include "../config/config.h"
static DHT dht(DHT_PIN,DHTTYPE); DhtDriver DhtSensor;
void DhtDriver::begin(){ dht.begin(); }
bool DhtDriver::poll(uint32_t now){ if(now-last_<SENSOR_INTERVAL_MS) return false; last_=now; float h=dht.readHumidity(), t=dht.readTemperature(); if(isnan(h)||isnan(t)){ reading_.valid=false; return false;} reading_.humidity=h; reading_.temperature=t; reading_.updatedAt=now; reading_.valid=true; return true; }
