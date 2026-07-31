#include "rtc.h"
#include <Wire.h>
#include "../config/config.h"
static RTC_DS3231 rtc; RtcClock Clock;
void RtcClock::begin(){ Wire.begin(I2C_SDA_PIN,I2C_SCL_PIN); available_=rtc.begin(); if(available_&&rtc.lostPower()) rtc.adjust(DateTime(F(__DATE__),F(__TIME__))); }
void RtcClock::poll(uint32_t nowMs){ if(available_||nowMs-lastRetry_<10000UL)return; lastRetry_=nowMs; available_=rtc.begin(); }
DateTime RtcClock::now()const{ return available_?rtc.now():DateTime(__DATE__,__TIME__); }
