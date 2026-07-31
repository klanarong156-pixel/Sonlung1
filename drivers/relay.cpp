#include "relay.h"
RelayController Relays;
void RelayController::begin(){ for(uint8_t i=0;i<RELAY_COUNT;i++){ pinMode(RELAY_PINS[i],OUTPUT); states_[i]=false; write_(i);} }
void RelayController::write_(uint8_t i){ bool level=RELAY_ACTIVE_LOW ? !states_[i] : states_[i]; digitalWrite(RELAY_PINS[i], level?HIGH:LOW); }
bool RelayController::set(uint8_t i,bool on){ if(i>=RELAY_COUNT||states_[i]==on) return false; states_[i]=on; write_(i); return true; }
bool RelayController::get(uint8_t i)const{ return i<RELAY_COUNT && states_[i]; }
String RelayController::json()const{ String s="["; for(uint8_t i=0;i<RELAY_COUNT;i++){ if(i)s+=','; s+=states_[i]?"true":"false";} return s+"]"; }
