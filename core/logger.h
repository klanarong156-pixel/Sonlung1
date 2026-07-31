#pragma once
#include <Arduino.h>
#include "../config/config.h"

class Logger {
 public:
  void begin();
  void info(const __FlashStringHelper* msg);
  void info(const char* msg);
  void warn(const char* msg);
  void event(const char* msg);
  String json() const;
 private:
  char entries_[HISTORY_LIMIT][80]{};
  uint8_t head_ = 0;
  uint8_t count_ = 0;
  void push_(const char* level, const char* msg);
};
extern Logger Log;
