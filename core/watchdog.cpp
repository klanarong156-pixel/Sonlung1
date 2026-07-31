#include "watchdog.h"
#include <Arduino.h>
Watchdog Wdt;
void Watchdog::begin(){
#if defined(ESP8266)
  ESP.wdtEnable(8000);
#endif
}
void Watchdog::feed(){
#if defined(ESP8266)
  ESP.wdtFeed();
#else
  yield();
#endif
}
