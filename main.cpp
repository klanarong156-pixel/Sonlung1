#include <Arduino.h>
#include "config/config.h"
#include "core/logger.h"
#include "core/watchdog.h"
#include "core/storage.h"
#include "core/wifi.h"
#include "core/mqtt.h"
#include "core/api.h"
#include "core/ota.h"
#include "core/telegram.h"
#include "core/scheduler.h"
#include "drivers/relay.h"
#include "drivers/dht.h"
#include "drivers/rtc.h"

void setup() {
  Log.begin();
  Log.info(F("===== SMART FARM PROFESSIONAL STARTING ====="));
  Wdt.begin();
  Store.begin();
  Relays.begin();
  Clock.begin();
  DhtSensor.begin();
  Wifi.begin();
  Mqtt.begin();
  Api.begin();
  Ota.begin();
  Telegram.begin();
  Log.event("Boot complete");
}

void loop() {
  const uint32_t now = millis();
  Wdt.feed();
  Wifi.loop(now);
  Api.loop();
  Ota.loop();
  Mqtt.loop(now);
  Telegram.loop(now);
  Scheduler.loop(now);
  if (DhtSensor.poll(now)) {
    Mqtt.publishSensor();
  }
}
