#pragma once
#include <Arduino.h>
class TelegramService{ public: void begin(){} void loop(uint32_t){} void notify(const char*){} bool connected()const{return false;} }; extern TelegramService Telegram;
