#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <RTClib.h>
#include <EEPROM.h>
#include <DHT.h>

// ==========================================
// การตั้งค่า MQTT (HiveMQ Cloud)
// ==========================================
const char* mqtt_server = "650188a0ee2b4367b7c131fb385590a9.s1.eu.hivemq.cloud";
const int mqtt_port = 8883;
const char* mqtt_user = "smartfarm";
const char* mqtt_pass = "Kla12345";

// MQTT Topics
const char* topic_pump     = "farm/pump"; // Legacy: relay 1
const char* topic_status   = "farm/status";
const char* topic_relay_set_prefix = "farm/relay/";
const char* topic_relay_set_suffix = "/set";
const char* topic_time     = "farm/time";
const char* topic_mode     = "farm/mode";
const char* topic_schedule = "farm/schedule";
const char* topic_schedule_status = "farm/schedule/status";
const char* topic_temperature = "farm/temp";
const char* topic_humidity = "farm/hum";
const char* topic_sensor_data = "farm/data";

// ==========================================
// การตั้งค่า Hardware
// ==========================================
const byte RELAY_COUNT = 4;
const byte RELAY_PINS[RELAY_COUNT] = {D5, D6, D7, D0}; // รีเลย์ 4 ช่อง: ปั๊มน้ำ, โซน 1, โซน 2, ไฟศาลา
const char* RELAY_NAMES[RELAY_COUNT] = {"pump", "zone1", "zone2", "pavilionLight"};
const bool RELAY_ACTIVE_LOW = true; // true = Active LOW, false = Active HIGH
#define DHT_PIN D4   // GPIO2 ว่างจาก RTC/รีเลย์ และเหมาะกับ DHT11 พร้อม pull-up
#define DHTTYPE DHT11
#define I2C_SDA D2   // ขา SDA ของ RTC
#define I2C_SCL D1   // ขา SCL ของ RTC

// ==========================================
// โครงสร้างข้อมูลสำหรับ Schedule
// ==========================================
struct ScheduleData {
  char onTime1[6];  // "HH:MM"
  char offTime1[6]; // "HH:MM"
  char onTime2[6];  // "HH:MM"
  char offTime2[6]; // "HH:MM"
};

ScheduleData schedules;
byte lastScheduleAction = 0; // 0=None, 1=S1ON, 2=S1OFF, 3=S2ON, 4=S2OFF

// ==========================================
// ตัวแปรระบบ
// ==========================================
bool isAutoMode = true; // โหมดการทำงาน (true = Auto, false = Manual)
bool relayStates[RELAY_COUNT] = {false, false, false, false}; // สถานะรีเลย์แต่ละช่อง (true = ON, false = OFF)
float temperature = NAN;
float humidity = NAN;
bool rtcAvailable = false;
unsigned long lastWiFiReconnect = 0;

// ตัวแปรสำหรับจัดการเวลา (ไม่ต้องใช้ delay)
unsigned long lastHeartbeat = 0;
unsigned long lastRTCUpdate = 0;
unsigned long lastDHTRead = 0;
unsigned long lastMQTTReconnect = 0;
const unsigned long HEARTBEAT_INTERVAL = 30000; // 30 วินาที
const unsigned long RTC_INTERVAL = 1000;        // 1 วินาที
const unsigned long DHT_INTERVAL = 2000;        // 2 วินาที
const unsigned long MQTT_RECONNECT_INTERVAL = 5000; // 5 วินาที
const unsigned long WIFI_RECONNECT_INTERVAL = 10000; // 10 วินาที
const byte DHT_MAX_RETRIES = 3;
const float DHT_MIN_TEMPERATURE = -10.0;
const float DHT_MAX_TEMPERATURE = 60.0;
const float DHT_MIN_HUMIDITY = 0.0;
const float DHT_MAX_HUMIDITY = 100.0;

// ==========================================
// ออบเจ็กต์ต่างๆ
// ==========================================
RTC_DS3231 rtc;
WiFiClientSecure espClient;
PubSubClient client(espClient);
DHT dht(DHT_PIN, DHTTYPE);

const int RELAY_STATE_EEPROM_ADDR = sizeof(ScheduleData);
const int EEPROM_SIZE = sizeof(ScheduleData) + RELAY_COUNT;

void publishSensorData();
void publishScheduleStatus();

bool schedulesAreEqual(const ScheduleData& left, const ScheduleData& right) {
  return strncmp(left.onTime1, right.onTime1, sizeof(left.onTime1)) == 0 &&
         strncmp(left.offTime1, right.offTime1, sizeof(left.offTime1)) == 0 &&
         strncmp(left.onTime2, right.onTime2, sizeof(left.onTime2)) == 0 &&
         strncmp(left.offTime2, right.offTime2, sizeof(left.offTime2)) == 0;
}

// ==========================================
// ฟังก์ชันอ่าน/เขียน EEPROM
// ==========================================
bool isValidScheduleTime(const char* value) {
  if (strlen(value) != 5 || value[2] != ':') return false;
  if (!isDigit(value[0]) || !isDigit(value[1]) || !isDigit(value[3]) || !isDigit(value[4])) return false;

  byte hour = ((value[0] - '0') * 10) + (value[1] - '0');
  byte minute = ((value[3] - '0') * 10) + (value[4] - '0');
  return hour <= 23 && minute <= 59;
}

bool isValidScheduleData(const ScheduleData& data) {
  return isValidScheduleTime(data.onTime1) && isValidScheduleTime(data.offTime1) &&
         isValidScheduleTime(data.onTime2) && isValidScheduleTime(data.offTime2);
}

bool isValidRTCDateTime(const DateTime& value) {
  return value.year() >= 2024 && value.year() <= 2099 &&
         value.month() >= 1 && value.month() <= 12 &&
         value.day() >= 1 && value.day() <= 31 &&
         value.hour() <= 23 && value.minute() <= 59 && value.second() <= 59;
}

void setDefaultSchedule() {
  strcpy(schedules.onTime1, "06:00");
  strcpy(schedules.offTime1, "06:10");
  strcpy(schedules.onTime2, "17:00");
  strcpy(schedules.offTime2, "17:10");
}

void loadScheduleFromEEPROM() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(0, schedules);

  // ตรวจสอบข้อมูลขยะ ถ้าใช่ให้ตั้งค่าเริ่มต้น
  if (!isValidScheduleData(schedules)) {
    setDefaultSchedule();
    EEPROM.put(0, schedules);
    EEPROM.commit();
    Serial.println("Initialized Default Schedule in EEPROM");
  } else {
    Serial.println("Loaded Schedule from EEPROM");
  }
  EEPROM.end();
}

void saveScheduleToEEPROM() {
  ScheduleData savedSchedule;
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(0, savedSchedule);

  if (isValidScheduleData(savedSchedule) && schedulesAreEqual(savedSchedule, schedules)) {
    EEPROM.end();
    Serial.println("Schedule unchanged, skipped EEPROM write");
    return;
  }

  EEPROM.put(0, schedules);
  EEPROM.commit();
  EEPROM.end();
  Serial.println("Saved Schedule to EEPROM");
}


void loadRelayStatesFromEEPROM() {
  EEPROM.begin(EEPROM_SIZE);
  for (byte i = 0; i < RELAY_COUNT; i++) {
    byte savedState = EEPROM.read(RELAY_STATE_EEPROM_ADDR + i);
    relayStates[i] = savedState == 1;
  }
  EEPROM.end();
  Serial.println("Loaded Relay States from EEPROM");
}

void saveRelayStateToEEPROM(byte relayIndex) {
  if (relayIndex >= RELAY_COUNT) return;

  byte newState = relayStates[relayIndex] ? 1 : 0;
  EEPROM.begin(EEPROM_SIZE);
  if (EEPROM.read(RELAY_STATE_EEPROM_ADDR + relayIndex) == newState) {
    EEPROM.end();
    return;
  }

  EEPROM.write(RELAY_STATE_EEPROM_ADDR + relayIndex, newState);
  EEPROM.commit();
  EEPROM.end();
}

void writeRelayPin(byte relayIndex) {
  if (relayIndex >= RELAY_COUNT) return;

  bool outputLevel = RELAY_ACTIVE_LOW ? !relayStates[relayIndex] : relayStates[relayIndex];
  digitalWrite(RELAY_PINS[relayIndex], outputLevel ? HIGH : LOW);
}

// ==========================================
// ฟังก์ชันควบคุมรีเลย์
// ==========================================
void publishRelayStatus(byte relayIndex) {
  if (relayIndex >= RELAY_COUNT) return;

  char topicBuffer[32];
  snprintf(topicBuffer, sizeof(topicBuffer), "farm/relay/%d/status", relayIndex + 1);
  client.publish(topicBuffer, relayStates[relayIndex] ? "ON" : "OFF", true);

  // ส่งสถานะปั๊มน้ำช่อง 1 ไปยัง topic เดิม เพื่อให้ dashboard รุ่นเก่ายังใช้งานได้
  if (relayIndex == 0) {
    client.publish(topic_status, relayStates[relayIndex] ? "ON" : "OFF", true);
  }
}

void publishAllRelayStatus() {
  for (byte i = 0; i < RELAY_COUNT; i++) {
    publishRelayStatus(i);
  }
}

void setRelay(byte relayIndex, bool state) {
  if (relayIndex >= RELAY_COUNT) return;

  // ป้องกันการเปิด/ปิดซ้ำ
  if (relayStates[relayIndex] == state) return;

  relayStates[relayIndex] = state;
  writeRelayPin(relayIndex);
  saveRelayStateToEEPROM(relayIndex);

  Serial.print("Relay ");
  Serial.print(relayIndex + 1);
  Serial.print(" (");
  Serial.print(RELAY_NAMES[relayIndex]);
  Serial.print(")");
  Serial.print(" turned ");
  Serial.println(state ? "ON" : "OFF");

  // ส่งสถานะไปยัง MQTT
  publishRelayStatus(relayIndex);
  publishSensorData();
}

void setPump(bool state) {
  setRelay(0, state);
}

// ==========================================
// ฟังก์ชันส่งข้อมูลผ่าน MQTT (Publish)
// ==========================================
void publishTime() {
  if (!rtcAvailable) return;

  DateTime now = rtc.now();
  char timeString[20];
  sprintf(timeString, "%02d:%02d:%02d", now.hour(), now.minute(), now.second());
  client.publish(topic_time, timeString, true);
}

void publishHeartbeat() {
  client.publish(topic_status, "ONLINE", true);
  Serial.println("Heartbeat sent: ONLINE");
}

void publishMode() {
  client.publish(topic_mode, isAutoMode ? "AUTO" : "MANUAL", true);
}

void publishScheduleStatus() {
  char scheduleBuffer[24];
  snprintf(scheduleBuffer, sizeof(scheduleBuffer), "%s,%s,%s,%s",
           schedules.onTime1, schedules.offTime1, schedules.onTime2, schedules.offTime2);
  client.publish(topic_schedule_status, scheduleBuffer, true);
}

void publishSensorData() {
  if (!client.connected() || isnan(temperature) || isnan(humidity)) return;

  char tempBuffer[12];
  char humBuffer[12];
  dtostrf(temperature, 4, 1, tempBuffer);
  dtostrf(humidity, 4, 1, humBuffer);
  client.publish(topic_temperature, tempBuffer, true);
  client.publish(topic_humidity, humBuffer, true);

  char jsonBuffer[180];
  snprintf(jsonBuffer, sizeof(jsonBuffer),
           "{\"temperature\":%.1f,\"humidity\":%.1f,\"pump\":\"%s\",\"zone1\":\"%s\",\"zone2\":\"%s\",\"pavilionLight\":\"%s\"}",
           temperature, humidity,
           relayStates[0] ? "ON" : "OFF",
           relayStates[1] ? "ON" : "OFF",
           relayStates[2] ? "ON" : "OFF",
           relayStates[3] ? "ON" : "OFF");
  client.publish(topic_sensor_data, jsonBuffer, true);
}

void readDHTSensor() {
  float newHumidity = NAN;
  float newTemperature = NAN;

  for (byte attempt = 0; attempt < DHT_MAX_RETRIES; attempt++) {
    newHumidity = dht.readHumidity();
    newTemperature = dht.readTemperature();
    if (!isnan(newHumidity) && !isnan(newTemperature)) break;
    yield();
  }

  if (isnan(newHumidity) || isnan(newTemperature)) {
    Serial.println("WARNING: Failed to read from DHT11 sensor after retries");
    return;
  }

  if (newTemperature < DHT_MIN_TEMPERATURE || newTemperature > DHT_MAX_TEMPERATURE ||
      newHumidity < DHT_MIN_HUMIDITY || newHumidity > DHT_MAX_HUMIDITY) {
    Serial.println("WARNING: Ignored out-of-range DHT11 sensor value");
    return;
  }

  temperature = newTemperature;
  humidity = newHumidity;

  Serial.print("DHT11 Temperature: ");
  Serial.print(temperature);
  Serial.print(" C, Humidity: ");
  Serial.print(humidity);
  Serial.println(" %");

  publishSensorData();
}

// ==========================================
// ฟังก์ชันรับข้อมูลจาก MQTT (Callback)
// ==========================================
bool parseScheduleMessage(const char* msg, ScheduleData& parsedSchedule) {
  char buffer[32];
  strlcpy(buffer, msg, sizeof(buffer));

  char* tokens[4] = {nullptr, nullptr, nullptr, nullptr};
  byte tokenCount = 0;
  char* savePtr = nullptr;
  char* token = strtok_r(buffer, ",", &savePtr);

  while (token != nullptr && tokenCount < 4) {
    while (*token == ' ') token++;
    char* endPtr = token + strlen(token);
    while (endPtr > token && *(endPtr - 1) == ' ') {
      *(--endPtr) = '\0';
    }
    tokens[tokenCount++] = token;
    token = strtok_r(nullptr, ",", &savePtr);
  }

  if (token != nullptr || tokenCount != 4) return false;

  strlcpy(parsedSchedule.onTime1, tokens[0], sizeof(parsedSchedule.onTime1));
  strlcpy(parsedSchedule.offTime1, tokens[1], sizeof(parsedSchedule.offTime1));
  strlcpy(parsedSchedule.onTime2, tokens[2], sizeof(parsedSchedule.onTime2));
  strlcpy(parsedSchedule.offTime2, tokens[3], sizeof(parsedSchedule.offTime2));

  return isValidScheduleData(parsedSchedule);
}

bool payloadEquals(const byte* payload, unsigned int length, const char* expected) {
  size_t expectedLength = strlen(expected);
  return length == expectedLength && memcmp(payload, expected, expectedLength) == 0;
}

bool copyPayload(char* destination, size_t destinationSize, const byte* payload, unsigned int length) {
  if (destinationSize == 0 || length >= destinationSize) return false;
  memcpy(destination, payload, length);
  destination[length] = '\0';
  return true;
}

bool parseRelayTopic(const char* topic, byte& relayIndex) {
  size_t prefixLength = strlen(topic_relay_set_prefix);
  size_t suffixLength = strlen(topic_relay_set_suffix);
  size_t topicLength = strlen(topic);

  if (topicLength <= prefixLength + suffixLength) return false;
  if (strncmp(topic, topic_relay_set_prefix, prefixLength) != 0) return false;
  if (strcmp(topic + topicLength - suffixLength, topic_relay_set_suffix) != 0) return false;

  char relayNumberBuffer[4];
  size_t relayNumberLength = topicLength - prefixLength - suffixLength;
  if (relayNumberLength == 0 || relayNumberLength >= sizeof(relayNumberBuffer)) return false;

  memcpy(relayNumberBuffer, topic + prefixLength, relayNumberLength);
  relayNumberBuffer[relayNumberLength] = '\0';

  for (size_t i = 0; i < relayNumberLength; i++) {
    if (!isDigit(relayNumberBuffer[i])) return false;
  }

  int relayNumber = atoi(relayNumberBuffer);
  if (relayNumber < 1 || relayNumber > RELAY_COUNT) return false;

  relayIndex = relayNumber - 1;
  return true;
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  char msg[32];
  bool hasTextPayload = copyPayload(msg, sizeof(msg), payload, length);

  Serial.println("=== MQTT Message Received ===");
  Serial.print("Topic: "); Serial.println(topic);
  Serial.print("Payload length: "); Serial.println(length);

  // 1. ควบคุมปั๊มน้ำช่อง 1 แบบ Manual (topic เดิม)
  if (strcmp(topic, topic_pump) == 0) {
    if (!isAutoMode) {
      if (payloadEquals(payload, length, "ON")) setPump(true);
      else if (payloadEquals(payload, length, "OFF")) setPump(false);
      else Serial.println("Ignored: Invalid pump command");
    } else {
      Serial.println("Ignored: System is in AUTO mode");
    }
  }
  // 1.1 ควบคุมรีเลย์ 4 ช่องแบบ Manual (farm/relay/1/set ... farm/relay/4/set)
  else {
    byte relayIndex = 0;
    if (parseRelayTopic(topic, relayIndex)) {
      if (!isAutoMode) {
        if (payloadEquals(payload, length, "ON")) setRelay(relayIndex, true);
        else if (payloadEquals(payload, length, "OFF")) setRelay(relayIndex, false);
        else Serial.println("Ignored: Invalid relay command");
      } else {
        Serial.println("Ignored: System is in AUTO mode");
      }
    }
    // 2. เปลี่ยนโหมด Auto/Manual
    else if (strcmp(topic, topic_mode) == 0) {
      bool modeChanged = false;
      if (payloadEquals(payload, length, "AUTO")) {
        modeChanged = !isAutoMode;
        isAutoMode = true;
        Serial.println("Mode changed to AUTO");
      } else if (payloadEquals(payload, length, "MANUAL")) {
        modeChanged = isAutoMode;
        isAutoMode = false;
        Serial.println("Mode changed to MANUAL");
      } else {
        Serial.println("Ignored: Invalid mode command");
      }

      if (modeChanged) publishMode();
    }
    // 3. ตั้งค่า Schedule (รูปแบบ: HH:MM,HH:MM,HH:MM,HH:MM)
    else if (strcmp(topic, topic_schedule) == 0) {
      ScheduleData parsedSchedule;
      if (hasTextPayload && parseScheduleMessage(msg, parsedSchedule)) {
        if (!schedulesAreEqual(schedules, parsedSchedule)) {
          schedules = parsedSchedule;
          saveScheduleToEEPROM();
          lastScheduleAction = 0;
        } else {
          Serial.println("Schedule unchanged");
        }
        publishScheduleStatus();
        Serial.println("Schedule updated via MQTT");
      } else {
        Serial.println("Ignored: Invalid schedule format. Use HH:MM,HH:MM,HH:MM,HH:MM");
        publishScheduleStatus();
      }
    }
  }
  yield();
}

// ==========================================
// ฟังก์ชันเชื่อมต่อ MQTT
// ==========================================
void connectMQTT() {
  if (client.connected()) return;

  unsigned long currentMillis = millis();
  if (currentMillis - lastMQTTReconnect >= MQTT_RECONNECT_INTERVAL) {
    lastMQTTReconnect = currentMillis;

    Serial.print("Connecting to MQTT...");
    // กำหนด Last Will and Testament (LWT) สำหรับแจ้ง Offline
    if (client.connect("ESP8266Farm", mqtt_user, mqtt_pass, topic_status, 0, true, "OFFLINE")) {
      Serial.println("Connected!");

      // สมัครรับข้อมูล Topics ที่ต้องการ
      bool subscribed = client.subscribe(topic_pump);
      for (byte i = 0; i < RELAY_COUNT; i++) {
        char relayTopic[32];
        snprintf(relayTopic, sizeof(relayTopic), "farm/relay/%d/set", i + 1);
        subscribed = client.subscribe(relayTopic) && subscribed;
        yield();
      }
      subscribed = client.subscribe(topic_mode) && subscribed;
      subscribed = client.subscribe(topic_schedule) && subscribed;
      Serial.println(subscribed ? "MQTT subscriptions OK" : "WARNING: MQTT subscription failed");

      // ส่งสถานะเริ่มต้น
      publishAllRelayStatus();
      publishMode();
      publishScheduleStatus();
      publishSensorData();
    } else {
      Serial.print("Failed, rc=");
      Serial.println(client.state());
    }
  }
}

// ==========================================
// ฟังก์ชันตรวจสอบตารางเวลา (Schedule)
// ==========================================
void checkSchedule() {
  if (!isAutoMode || !rtcAvailable) return;

  DateTime now = rtc.now();
  if (!isValidRTCDateTime(now)) {
    Serial.println("WARNING: RTC returned invalid time");
    return;
  }

  char currentTime[6];
  snprintf(currentTime, sizeof(currentTime), "%02d:%02d", now.hour(), now.minute());

  // Schedule 1 ON
  if (strcmp(currentTime, schedules.onTime1) == 0 && lastScheduleAction != 1) {
    setPump(true);
    Serial.println("Auto: Schedule 1 ON");
    lastScheduleAction = 1;
  }
  // Schedule 1 OFF
  else if (strcmp(currentTime, schedules.offTime1) == 0 && lastScheduleAction != 2) {
    setPump(false);
    Serial.println("Auto: Schedule 1 OFF");
    lastScheduleAction = 2;
  }
  // Schedule 2 ON
  else if (strcmp(currentTime, schedules.onTime2) == 0 && lastScheduleAction != 3) {
    setPump(true);
    Serial.println("Auto: Schedule 2 ON");
    lastScheduleAction = 3;
  }
  // Schedule 2 OFF
  else if (strcmp(currentTime, schedules.offTime2) == 0 && lastScheduleAction != 4) {
    setPump(false);
    Serial.println("Auto: Schedule 2 OFF");
    lastScheduleAction = 4;
  }

  // รีเซ็ตสถานะเมื่อผ่านไป 1 นาที (เพื่อรองรับวันถัดไป)
  if (strcmp(currentTime, schedules.onTime1) != 0 && strcmp(currentTime, schedules.offTime1) != 0 &&
      strcmp(currentTime, schedules.onTime2) != 0 && strcmp(currentTime, schedules.offTime2) != 0) {
    lastScheduleAction = 0;
  }
}

// ==========================================
// Setup Function
// ==========================================
void setup() {
  Serial.begin(115200);
  Serial.println("\n===== SMART FARM SYSTEM STARTING =====");

  // ตั้งค่า Relay (ปิดปั๊มเป็นค่าเริ่มต้น)
  for (byte i = 0; i < RELAY_COUNT; i++) {
    pinMode(RELAY_PINS[i], OUTPUT);
    relayStates[i] = false;
    writeRelayPin(i);
  }

  // โหลด Schedule และสถานะรีเลย์จาก EEPROM
  loadScheduleFromEEPROM();
  loadRelayStatesFromEEPROM();
  for (byte i = 0; i < RELAY_COUNT; i++) {
    writeRelayPin(i);
  }

  // ตั้งค่า WiFiManager (Auto Reconnect อยู่ในตัวแล้ว)
  WiFiManager wm;
  wm.setConnectTimeout(30);
  wm.setConfigPortalTimeout(180);
  // รีเซ็ตค่า WiFi หากต้องการ (wm.resetSettings();)
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  Serial.println("Connecting to WiFi...");
  if (!wm.autoConnect("SmartFarm_Setup")) {
    Serial.println("Failed to connect WiFi, restarting...");
    delay(3000);
    ESP.restart();
  }
  Serial.println("WiFi Connected!");
  Serial.print("IP Address: "); Serial.println(WiFi.localIP());

  // ตั้งค่า RTC
  Wire.begin(I2C_SDA, I2C_SCL);
  rtcAvailable = rtc.begin();
  if (!rtcAvailable) {
    Serial.println("WARNING: RTC NOT FOUND!");
  } else {
    Serial.println("RTC OK");
    if (rtc.lostPower()) {
      Serial.println("RTC lost power, let's set the time!");
      rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    }
  }

  // ตั้งค่า DHT11
  dht.begin();
  Serial.println("DHT11 Initialized");

  // ตั้งค่า MQTT
  espClient.setInsecure(); // ไม่ตรวจสอบ Certificate
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(mqttCallback);

  // เปิดใช้งาน Watchdog Timer
  ESP.wdtEnable(WDTO_8S); // รีเซ็ตบอร์ดถ้าค้างเกิน 8 วินาที

  Serial.println("System Initialized.");
}

// ==========================================
// Loop Function
// ==========================================
void loop() {
  // รีเซ็ต Watchdog Timer ทุกรอบ
  ESP.wdtFeed();

  // ตรวจสอบการเชื่อมต่อ WiFi (Auto Reconnect)
  if (WiFi.status() != WL_CONNECTED) {
    unsigned long currentMillis = millis();
    if (currentMillis - lastWiFiReconnect >= WIFI_RECONNECT_INTERVAL) {
      lastWiFiReconnect = currentMillis;
      Serial.println("WiFi disconnected, reconnecting...");
      WiFi.reconnect();
    }
    yield();
    return; // ข้ามการทำงานส่วนอื่นไปก่อน
  }

  // จัดการ MQTT
  if (!client.connected()) {
    connectMQTT();
  } else {
    client.loop();
    yield();
  }

  unsigned long currentMillis = millis();

  // 1. ตรวจสอบเวลาทุก 1 วินาที (สำหรับส่งเวลาและเช็คตารางเวลา)
  if (currentMillis - lastRTCUpdate >= RTC_INTERVAL) {
    lastRTCUpdate = currentMillis;
    if (client.connected()) publishTime();
    checkSchedule();
  }

  // 2. อ่าน DHT11 ทุก 2 วินาทีแบบ Non-blocking
  if (currentMillis - lastDHTRead >= DHT_INTERVAL) {
    lastDHTRead = currentMillis;
    readDHTSensor();
  }

  // 3. ส่ง Heartbeat ทุก 30 วินาที
  if (currentMillis - lastHeartbeat >= HEARTBEAT_INTERVAL) {
    lastHeartbeat = currentMillis;
    if (client.connected()) publishHeartbeat();
  }
}
