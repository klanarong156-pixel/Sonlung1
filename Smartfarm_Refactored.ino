#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <RTClib.h>
#include <EEPROM.h>
#include <DHT.h>
#include <ctype.h>
#include <string.h>

// ==========================================
// การตั้งค่า MQTT (HiveMQ Cloud)
// ==========================================
const char* mqtt_server = "650188a0ee2b4367b7c131fb385590a9.s1.eu.hivemq.cloud";
const int mqtt_port = 8883;
const char* mqtt_user = "smartfarm";
const char* mqtt_pass = "Kla12345";

// MQTT Topics
const char* topic_pump     = "farm/pump"; // Legacy: relay 1
const char* topic_pump_status = "farm/pump/status";
const char* topic_pump_countdown = "farm/pump/countdown";
const char* topic_pump_countdown_start = "farm/pump/countdown/start";
const char* topic_pump_countdown_stop = "farm/pump/countdown/stop";
const char* topic_pump_duration = "farm/pump/duration";
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
char lastScheduleAction[6] = "";

// ==========================================
// ตัวแปรระบบ
// ==========================================
bool isAutoMode = true; // โหมดการทำงาน (true = Auto, false = Manual)
bool relayStates[RELAY_COUNT] = {false, false, false, false}; // สถานะรีเลย์แต่ละช่อง (true = ON, false = OFF)
float temperature = NAN;
float humidity = NAN;
bool rtcAvailable = false;
unsigned long lastWiFiReconnect = 0;
unsigned long lastRTCRetry = 0;

// ตัวแปรสำหรับจัดการเวลา (ไม่ต้องใช้ delay)
const unsigned int DEFAULT_PUMP_DURATION_MINUTES = 15;
const unsigned int MIN_PUMP_DURATION_MINUTES = 1;
const unsigned int MAX_PUMP_DURATION_MINUTES = 120;

unsigned long lastHeartbeat = 0;
unsigned long lastRTCUpdate = 0;
unsigned long lastDHTRead = 0;
unsigned long lastMQTTReconnect = 0;
unsigned long pumpCountdownStartedAt = 0;
unsigned long lastPumpCountdownTick = 0;
unsigned long pumpCountdownDurationSeconds = DEFAULT_PUMP_DURATION_MINUTES * 60UL;
unsigned long pumpCountdownRemainingSeconds = 0;
bool pumpCountdownActive = false;
const unsigned long HEARTBEAT_INTERVAL = 30000; // 30 วินาที
const unsigned long RTC_INTERVAL = 1000;        // 1 วินาที
const unsigned long DHT_INTERVAL = 2000;        // 2 วินาที
const unsigned long MQTT_RECONNECT_INTERVAL = 5000; // 5 วินาที
const unsigned long WIFI_RECONNECT_INTERVAL = 10000; // 10 วินาที
const unsigned long RTC_RETRY_INTERVAL = 10000;  // 10 วินาที
const unsigned int MQTT_MESSAGE_BUFFER_SIZE = 32;

void publishSensorData();
void publishScheduleStatus();
void publishPumpStatus();
void publishPumpCountdown();
void startPumpCountdown(unsigned int durationMinutes);
void stopPumpCountdown(bool turnPumpOff);
void updatePumpCountdown();
void checkRTC();

// ==========================================
// ออบเจ็กต์ต่างๆ
// ==========================================
RTC_DS3231 rtc;
WiFiClientSecure espClient;
PubSubClient client(espClient);
DHT dht(DHT_PIN, DHTTYPE);

const int RELAY_STATE_EEPROM_ADDR = sizeof(ScheduleData);
const int EEPROM_SIZE = sizeof(ScheduleData) + RELAY_COUNT;

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
  EEPROM.begin(EEPROM_SIZE);
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

  EEPROM.begin(EEPROM_SIZE);
  EEPROM.write(RELAY_STATE_EEPROM_ADDR + relayIndex, relayStates[relayIndex] ? 1 : 0);
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
    publishPumpStatus();
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
  if (!state && pumpCountdownActive) {
    stopPumpCountdown(false);
  }
}

// ==========================================
// ฟังก์ชัน Pump Countdown Timer
// ==========================================
void formatCountdown(unsigned long totalSeconds, char* buffer, size_t bufferSize) {
  unsigned int minutes = totalSeconds / 60UL;
  unsigned int seconds = totalSeconds % 60UL;
  snprintf(buffer, bufferSize, "%02u:%02u", minutes, seconds);
}

void publishPumpStatus() {
  if (!client.connected()) return;
  client.publish(topic_pump_status, relayStates[0] ? "ON" : "OFF", true);
}

void publishPumpCountdown() {
  if (!client.connected()) return;
  char countdownBuffer[8];
  formatCountdown(pumpCountdownRemainingSeconds, countdownBuffer, sizeof(countdownBuffer));
  client.publish(topic_pump_countdown, countdownBuffer, true);
}

void startPumpCountdown(unsigned int durationMinutes) {
  if (durationMinutes < MIN_PUMP_DURATION_MINUTES) durationMinutes = MIN_PUMP_DURATION_MINUTES;
  if (durationMinutes > MAX_PUMP_DURATION_MINUTES) durationMinutes = MAX_PUMP_DURATION_MINUTES;

  pumpCountdownDurationSeconds = durationMinutes * 60UL;
  pumpCountdownRemainingSeconds = pumpCountdownDurationSeconds;
  pumpCountdownStartedAt = millis();
  lastPumpCountdownTick = pumpCountdownStartedAt;
  pumpCountdownActive = true;

  setRelay(0, true);
  publishPumpStatus();
  publishPumpCountdown();

  Serial.print("Pump countdown started for ");
  Serial.print(durationMinutes);
  Serial.println(" minute(s)");
}

void stopPumpCountdown(bool turnPumpOff) {
  pumpCountdownActive = false;
  pumpCountdownRemainingSeconds = 0;

  if (turnPumpOff) {
    setRelay(0, false);
  }

  publishPumpCountdown();
  publishPumpStatus();
  Serial.println("Pump countdown stopped");
}

void updatePumpCountdown() {
  if (!pumpCountdownActive) return;

  unsigned long currentMillis = millis();
  unsigned long elapsedSeconds = (currentMillis - pumpCountdownStartedAt) / 1000UL;
  unsigned long newRemaining = elapsedSeconds >= pumpCountdownDurationSeconds ? 0 : pumpCountdownDurationSeconds - elapsedSeconds;

  if (currentMillis - lastPumpCountdownTick >= 1000UL || newRemaining != pumpCountdownRemainingSeconds) {
    lastPumpCountdownTick = currentMillis;
    pumpCountdownRemainingSeconds = newRemaining;
    publishPumpCountdown();

    if (pumpCountdownRemainingSeconds == 0) {
      pumpCountdownActive = false;
      setRelay(0, false);
      publishPumpStatus();
      publishPumpCountdown();
      Serial.println("Pump countdown finished: pump OFF");
    }
  }
}

// ==========================================
// ฟังก์ชันส่งข้อมูลผ่าน MQTT (Publish)
// ==========================================
void checkRTC() {
  if (rtcAvailable) return;

  unsigned long currentMillis = millis();
  if (currentMillis - lastRTCRetry < RTC_RETRY_INTERVAL) return;
  lastRTCRetry = currentMillis;

  rtcAvailable = rtc.begin();
  if (rtcAvailable) {
    Serial.println("RTC recovered");
    if (rtc.lostPower()) {
      Serial.println("RTC lost power, let's set the time!");
      rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    }
  }
}

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
  float newHumidity = dht.readHumidity();
  float newTemperature = dht.readTemperature();

  if (isnan(newHumidity) || isnan(newTemperature)) {
    Serial.println("WARNING: Failed to read from DHT11 sensor");
    return;
  }

  humidity = newHumidity;
  temperature = newTemperature;

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
bool copyTrimmedPayload(char* destination, size_t destinationSize, const byte* payload, unsigned int length) {
  if (destinationSize == 0 || length >= destinationSize) return false;

  memcpy(destination, payload, length);
  destination[length] = '\0';

  char* start = destination;
  while (*start && isspace((unsigned char)*start)) start++;

  char* end = start + strlen(start);
  while (end > start && isspace((unsigned char)*(end - 1))) end--;
  *end = '\0';

  if (start != destination) memmove(destination, start, end - start + 1);
  return true;
}

bool copyScheduleToken(char* destination, size_t destinationSize, const char* start, size_t length) {
  while (length > 0 && isspace((unsigned char)*start)) {
    start++;
    length--;
  }
  while (length > 0 && isspace((unsigned char)start[length - 1])) {
    length--;
  }

  if (length != 5 || destinationSize < 6) return false;
  memcpy(destination, start, length);
  destination[length] = '\0';
  return isValidScheduleTime(destination);
}

bool parseScheduleMessage(const char* msg, ScheduleData& parsedSchedule) {
  const char* firstComma = strchr(msg, ',');
  if (!firstComma) return false;

  const char* secondComma = strchr(firstComma + 1, ',');
  if (!secondComma) return false;

  const char* thirdComma = strchr(secondComma + 1, ',');
  if (!thirdComma) return false;

  if (strchr(thirdComma + 1, ',')) return false;

  return copyScheduleToken(parsedSchedule.onTime1, sizeof(parsedSchedule.onTime1), msg, firstComma - msg) &&
         copyScheduleToken(parsedSchedule.offTime1, sizeof(parsedSchedule.offTime1), firstComma + 1, secondComma - firstComma - 1) &&
         copyScheduleToken(parsedSchedule.onTime2, sizeof(parsedSchedule.onTime2), secondComma + 1, thirdComma - secondComma - 1) &&
         copyScheduleToken(parsedSchedule.offTime2, sizeof(parsedSchedule.offTime2), thirdComma + 1, strlen(thirdComma + 1));
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  char msg[MQTT_MESSAGE_BUFFER_SIZE];
  if (!copyTrimmedPayload(msg, sizeof(msg), payload, length)) {
    Serial.println("Ignored: MQTT payload too long");
    return;
  }

  Serial.println("=== MQTT Message Received ===");
  Serial.print("Topic: "); Serial.println(topic);
  Serial.print("Message: "); Serial.println(msg);

  // 1. ควบคุมปั๊มน้ำช่อง 1 แบบ Manual (topic เดิม)
  if (strcmp(topic, topic_pump) == 0) {
    if (!isAutoMode) {
      if (strcmp(msg, "ON") == 0) startPumpCountdown(pumpCountdownDurationSeconds / 60UL);
      else if (strcmp(msg, "OFF") == 0) stopPumpCountdown(true);
    } else {
      Serial.println("Ignored: System is in AUTO mode");
    }
  }
  // 1.1 ควบคุมรีเลย์ 4 ช่องแบบ Manual (farm/relay/1/set ... farm/relay/4/set)
  else if (strncmp(topic, topic_relay_set_prefix, strlen(topic_relay_set_prefix)) == 0) {
    size_t topicLength = strlen(topic);
    size_t prefixLength = strlen(topic_relay_set_prefix);
    size_t suffixLength = strlen(topic_relay_set_suffix);

    if (topicLength > prefixLength + suffixLength &&
        strcmp(topic + topicLength - suffixLength, topic_relay_set_suffix) == 0) {
      if (!isAutoMode) {
        char relayNumberBuffer[4];
        size_t relayNumberLength = topicLength - prefixLength - suffixLength;
        if (relayNumberLength == 0 || relayNumberLength >= sizeof(relayNumberBuffer)) {
          Serial.println("Ignored: Invalid relay number");
          return;
        }
        memcpy(relayNumberBuffer, topic + prefixLength, relayNumberLength);
        relayNumberBuffer[relayNumberLength] = '\0';

        int relayNumber = atoi(relayNumberBuffer);
        if (relayNumber < 1 || relayNumber > RELAY_COUNT) {
          Serial.println("Ignored: Invalid relay number");
          return;
        }

        byte relayIndex = relayNumber - 1;
        if (relayIndex == 0 && strcmp(msg, "ON") == 0) startPumpCountdown(pumpCountdownDurationSeconds / 60UL);
        else if (relayIndex == 0 && strcmp(msg, "OFF") == 0) stopPumpCountdown(true);
        else if (strcmp(msg, "ON") == 0) setRelay(relayIndex, true);
        else if (strcmp(msg, "OFF") == 0) setRelay(relayIndex, false);
      } else {
        Serial.println("Ignored: System is in AUTO mode");
      }
    }
  }
  // 1.2 ตั้งเวลาและควบคุม Countdown Timer
  else if (strcmp(topic, topic_pump_duration) == 0) {
    int durationMinutes = atoi(msg);
    if (durationMinutes >= MIN_PUMP_DURATION_MINUTES && durationMinutes <= MAX_PUMP_DURATION_MINUTES) {
      pumpCountdownDurationSeconds = durationMinutes * 60UL;
      Serial.println("Pump countdown duration updated");
    } else {
      Serial.println("Ignored: Pump duration must be 1-120 minutes");
    }
  }
  else if (strcmp(topic, topic_pump_countdown_start) == 0) {
    int durationMinutes = atoi(msg);
    if (durationMinutes == 0) durationMinutes = pumpCountdownDurationSeconds / 60UL;
    startPumpCountdown(durationMinutes);
  }
  else if (strcmp(topic, topic_pump_countdown_stop) == 0) {
    stopPumpCountdown(true);
  }
  // 2. เปลี่ยนโหมด Auto/Manual
  else if (strcmp(topic, topic_mode) == 0) {
    if (strcmp(msg, "AUTO") == 0) {
      isAutoMode = true;
      Serial.println("Mode changed to AUTO");
    } else if (strcmp(msg, "MANUAL") == 0) {
      isAutoMode = false;
      Serial.println("Mode changed to MANUAL");
    }
    publishMode();
  }
  // 3. ตั้งค่า Schedule (รูปแบบ: HH:MM,HH:MM,HH:MM,HH:MM)
  else if (strcmp(topic, topic_schedule) == 0) {
    ScheduleData parsedSchedule;
    if (parseScheduleMessage(msg, parsedSchedule)) {
      if (memcmp(&schedules, &parsedSchedule, sizeof(ScheduleData)) != 0) {
        schedules = parsedSchedule;
        saveScheduleToEEPROM();
      }
      publishScheduleStatus();
      Serial.println("Schedule updated via MQTT");
    } else {
      Serial.println("Ignored: Invalid schedule format. Use HH:MM,HH:MM,HH:MM,HH:MM");
      publishScheduleStatus();
    }
  }
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
      client.subscribe(topic_pump);
      for (byte i = 0; i < RELAY_COUNT; i++) {
        char relayTopic[32];
        snprintf(relayTopic, sizeof(relayTopic), "farm/relay/%d/set", i + 1);
        client.subscribe(relayTopic);
      }
      client.subscribe(topic_mode);
      client.subscribe(topic_schedule);
      client.subscribe(topic_pump_duration);
      client.subscribe(topic_pump_countdown_start);
      client.subscribe(topic_pump_countdown_stop);

      // ส่งสถานะเริ่มต้น
      publishAllRelayStatus();
      publishMode();
      publishScheduleStatus();
      publishSensorData();
      publishPumpStatus();
      publishPumpCountdown();
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
  char buf[6];
  sprintf(buf, "%02d:%02d", now.hour(), now.minute());
  String currentTime = String(buf);

  // Schedule 1 ON
  if (currentTime == schedules.onTime1 && strcmp(lastScheduleAction, "S1ON") != 0) {
    startPumpCountdown(pumpCountdownDurationSeconds / 60UL);
    Serial.println("Auto: Schedule 1 ON");
    strcpy(lastScheduleAction, "S1ON");
  }
  // Schedule 1 OFF
  else if (currentTime == schedules.offTime1 && strcmp(lastScheduleAction, "S1OFF") != 0) {
    stopPumpCountdown(true);
    Serial.println("Auto: Schedule 1 OFF");
    strcpy(lastScheduleAction, "S1OFF");
  }
  // Schedule 2 ON
  else if (currentTime == schedules.onTime2 && strcmp(lastScheduleAction, "S2ON") != 0) {
    startPumpCountdown(pumpCountdownDurationSeconds / 60UL);
    Serial.println("Auto: Schedule 2 ON");
    strcpy(lastScheduleAction, "S2ON");
  }
  // Schedule 2 OFF
  else if (currentTime == schedules.offTime2 && strcmp(lastScheduleAction, "S2OFF") != 0) {
    stopPumpCountdown(true);
    Serial.println("Auto: Schedule 2 OFF");
    strcpy(lastScheduleAction, "S2OFF");
  }

  // รีเซ็ตสถานะเมื่อผ่านไป 1 นาที (เพื่อรองรับวันถัดไป)
  if (currentTime != schedules.onTime1 && currentTime != schedules.offTime1 &&
      currentTime != schedules.onTime2 && currentTime != schedules.offTime2) {
    lastScheduleAction[0] = '\0';
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
  // รีเซ็ตค่า WiFi หากต้องการ (wm.resetSettings();)
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  Serial.println("Connecting to WiFi...");
  if (!wm.autoConnect("SmartFarm_Setup")) {
    Serial.println("Failed to connect WiFi, restarting...");
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
  ESP.wdtEnable(8000); // ESP8266 watchdog timeout in milliseconds (8 วินาที)

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
    return; // ข้ามการทำงานส่วนอื่นไปก่อน
  }

  // จัดการ MQTT
  if (!client.connected()) {
    connectMQTT();
  } else {
    client.loop();
  }

  unsigned long currentMillis = millis();

  // 1. ตรวจสอบเวลาทุก 1 วินาที (สำหรับส่งเวลาและเช็คตารางเวลา)
  if (currentMillis - lastRTCUpdate >= RTC_INTERVAL) {
    lastRTCUpdate = currentMillis;
    checkRTC();
    if (client.connected()) publishTime();
    checkSchedule();
  }

  updatePumpCountdown();

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
