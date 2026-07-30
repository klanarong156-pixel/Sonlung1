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
String lastScheduleAction = "";

// ==========================================
// ตัวแปรระบบ
// ==========================================
bool isAutoMode = true; // โหมดการทำงาน (true = Auto, false = Manual)
bool relayStates[RELAY_COUNT] = {false, false, false, false}; // สถานะรีเลย์แต่ละช่อง (true = ON, false = OFF)
float temperature = NAN;
float humidity = NAN;

// ตัวแปรสำหรับจัดการเวลา (ไม่ต้องใช้ delay)
unsigned long lastHeartbeat = 0;
unsigned long lastRTCUpdate = 0;
unsigned long lastDHTRead = 0;
unsigned long lastMQTTReconnect = 0;
const unsigned long HEARTBEAT_INTERVAL = 30000; // 30 วินาที
const unsigned long RTC_INTERVAL = 1000;        // 1 วินาที
const unsigned long DHT_INTERVAL = 2000;        // 2 วินาที
const unsigned long MQTT_RECONNECT_INTERVAL = 5000; // 5 วินาที

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

// ==========================================
// ฟังก์ชันอ่าน/เขียน EEPROM
// ==========================================
void loadScheduleFromEEPROM() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(0, schedules);

  // ตรวจสอบข้อมูลขยะ ถ้าใช่ให้ตั้งค่าเริ่มต้น
  if (schedules.onTime1[2] != ':' || schedules.offTime1[2] != ':') {
    strcpy(schedules.onTime1, "06:00");
    strcpy(schedules.offTime1, "06:10");
    strcpy(schedules.onTime2, "17:00");
    strcpy(schedules.offTime2, "17:10");
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
  if (!rtc.begin()) return;

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
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (unsigned int i = 0; i < length; i++) {
    msg += (char)payload[i];
  }

  Serial.println("=== MQTT Message Received ===");
  Serial.print("Topic: "); Serial.println(topic);
  Serial.print("Message: "); Serial.println(msg);

  // 1. ควบคุมปั๊มน้ำช่อง 1 แบบ Manual (topic เดิม)
  if (String(topic) == topic_pump) {
    if (!isAutoMode) {
      if (msg == "ON") setPump(true);
      else if (msg == "OFF") setPump(false);
    } else {
      Serial.println("Ignored: System is in AUTO mode");
    }
  }
  // 1.1 ควบคุมรีเลย์ 4 ช่องแบบ Manual (farm/relay/1/set ... farm/relay/4/set)
  else if (String(topic).startsWith(topic_relay_set_prefix) && String(topic).endsWith(topic_relay_set_suffix)) {
    if (!isAutoMode) {
      byte relayIndex = String(topic).substring(strlen(topic_relay_set_prefix), String(topic).length() - strlen(topic_relay_set_suffix)).toInt() - 1;
      if (msg == "ON") setRelay(relayIndex, true);
      else if (msg == "OFF") setRelay(relayIndex, false);
    } else {
      Serial.println("Ignored: System is in AUTO mode");
    }
  }
  // 2. เปลี่ยนโหมด Auto/Manual
  else if (String(topic) == topic_mode) {
    if (msg == "AUTO") {
      isAutoMode = true;
      Serial.println("Mode changed to AUTO");
    } else if (msg == "MANUAL") {
      isAutoMode = false;
      Serial.println("Mode changed to MANUAL");
    }
    publishMode();
  }
  // 3. ตั้งค่า Schedule (รูปแบบ: HH:MM,HH:MM,HH:MM,HH:MM)
  else if (String(topic) == topic_schedule) {
    // แยกข้อความด้วยเครื่องหมาย comma
    int firstComma = msg.indexOf(',');
    int secondComma = msg.indexOf(',', firstComma + 1);
    int thirdComma = msg.indexOf(',', secondComma + 1);

    if (firstComma > 0 && secondComma > 0 && thirdComma > 0) {
      msg.substring(0, firstComma).toCharArray(schedules.onTime1, 6);
      msg.substring(firstComma + 1, secondComma).toCharArray(schedules.offTime1, 6);
      msg.substring(secondComma + 1, thirdComma).toCharArray(schedules.onTime2, 6);
      msg.substring(thirdComma + 1).toCharArray(schedules.offTime2, 6);

      saveScheduleToEEPROM();
      Serial.println("Schedule updated via MQTT");
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

      // ส่งสถานะเริ่มต้น
      publishAllRelayStatus();
      publishMode();
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
  if (!isAutoMode || !rtc.begin()) return;

  DateTime now = rtc.now();
  char buf[6];
  sprintf(buf, "%02d:%02d", now.hour(), now.minute());
  String currentTime = String(buf);

  // Schedule 1 ON
  if (currentTime == schedules.onTime1 && lastScheduleAction != "S1ON") {
    setPump(true);
    Serial.println("Auto: Schedule 1 ON");
    lastScheduleAction = "S1ON";
  }
  // Schedule 1 OFF
  else if (currentTime == schedules.offTime1 && lastScheduleAction != "S1OFF") {
    setPump(false);
    Serial.println("Auto: Schedule 1 OFF");
    lastScheduleAction = "S1OFF";
  }
  // Schedule 2 ON
  else if (currentTime == schedules.onTime2 && lastScheduleAction != "S2ON") {
    setPump(true);
    Serial.println("Auto: Schedule 2 ON");
    lastScheduleAction = "S2ON";
  }
  // Schedule 2 OFF
  else if (currentTime == schedules.offTime2 && lastScheduleAction != "S2OFF") {
    setPump(false);
    Serial.println("Auto: Schedule 2 OFF");
    lastScheduleAction = "S2OFF";
  }

  // รีเซ็ตสถานะเมื่อผ่านไป 1 นาที (เพื่อรองรับวันถัดไป)
  if (currentTime != schedules.onTime1 && currentTime != schedules.offTime1 &&
      currentTime != schedules.onTime2 && currentTime != schedules.offTime2) {
    lastScheduleAction = "";
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
  if (!rtc.begin()) {
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
