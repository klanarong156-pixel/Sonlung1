#pragma once
#include <Arduino.h>
#include "../config/config.h"
class RelayController{ public: void begin(); bool set(uint8_t idx,bool on); bool get(uint8_t idx)const; String json()const; private: bool states_[RELAY_COUNT]{}; void write_(uint8_t idx);};
extern RelayController Relays;
