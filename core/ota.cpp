#include "ota.h"
#include <ArduinoOTA.h>
#include "logger.h"
#include "../config/config.h"
OtaService Ota;
void OtaService::begin(){ ArduinoOTA.setHostname(SMARTFARM_HOSTNAME); ArduinoOTA.onStart([](){Log.event("OTA start");}); ArduinoOTA.onEnd([](){Log.event("OTA success");}); ArduinoOTA.onError([](ota_error_t){Log.warn("OTA failed");}); ArduinoOTA.begin(); }
void OtaService::loop(){ ArduinoOTA.handle(); }
