#pragma once
#include <Arduino.h>
class MqttService{ public: void begin(); void loop(uint32_t now); bool connected()const; void publishStatus(); void publishRelay(uint8_t i); void publishSensor(); void publishEvent(const char* event); private: uint32_t lastConnect_=0,lastHeartbeat_=0; void subscribe_();}; extern MqttService Mqtt;
