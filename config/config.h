#pragma once
#include <Arduino.h>

#define SMARTFARM_VERSION "SmartFarm Pro 9.0.0"
#define SMARTFARM_HOSTNAME "smartfarm-pro"
#define SMARTFARM_AP_NAME "SmartFarm_Setup"

static const char MQTT_HOST[] PROGMEM = "650188a0ee2b4367b7c131fb385590a9.s1.eu.hivemq.cloud";
static const uint16_t MQTT_PORT = 8883;
static const char MQTT_USER[] PROGMEM = "smartfarm";
static const char MQTT_PASS[] PROGMEM = "Kla12345";
static const char MQTT_BASE[] PROGMEM = "farm";

static const uint8_t RELAY_COUNT = 4;
static const uint8_t ZONE_COUNT = 4;
static const uint8_t MAX_SCHEDULES = 16;
static const uint8_t HISTORY_LIMIT = 24;
static const bool RELAY_ACTIVE_LOW = true;

#if defined(ESP8266)
static const uint8_t RELAY_PINS[RELAY_COUNT] = {D5, D6, D7, D0};
static const uint8_t DHT_PIN = D4;
static const uint8_t I2C_SDA_PIN = D2;
static const uint8_t I2C_SCL_PIN = D1;
#else
static const uint8_t RELAY_PINS[RELAY_COUNT] = {18, 19, 23, 5};
static const uint8_t DHT_PIN = 4;
static const uint8_t I2C_SDA_PIN = 21;
static const uint8_t I2C_SCL_PIN = 22;
#endif

#define DHTTYPE DHT11
static const uint8_t PUMP_RELAY_INDEX = 0;
static const uint16_t DEFAULT_PUMP_DURATION_MINUTES = 15;
static const uint16_t MIN_PUMP_DURATION_MINUTES = 1;
static const uint16_t MAX_PUMP_DURATION_MINUTES = 120;
static const uint32_t WIFI_RECONNECT_INTERVAL_MS = 10000UL;
static const uint32_t MQTT_RECONNECT_INTERVAL_MS = 5000UL;
static const uint32_t HEARTBEAT_INTERVAL_MS = 30000UL;
static const uint32_t SENSOR_INTERVAL_MS = 2500UL;
static const uint32_t SCHEDULE_INTERVAL_MS = 1000UL;
static const uint32_t TELEGRAM_INTERVAL_MS = 2000UL;
static const uint32_t SESSION_TIMEOUT_MS = 30UL * 60UL * 1000UL;

static const char ADMIN_PASSWORD_HASH[] PROGMEM = "240be518fabd2724d244a39c94f857b2"; // MD5("1234") legacy default; change in production.
