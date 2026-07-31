#include "scheduler.h"
#include "storage.h"
#include "../drivers/rtc.h"
#include "../drivers/relay.h"
#include "logger.h"
FarmScheduler Scheduler; static bool active[MAX_SCHEDULES]; static uint32_t finishAt[MAX_SCHEDULES];
void FarmScheduler::loop(uint32_t nowMs){ static uint32_t last=0; if(nowMs-last<SCHEDULE_INTERVAL_MS||Store.config().holidayMode) return; last=nowMs; Clock.poll(nowMs); if(!Clock.available()) return; DateTime n=Clock.now(); char cur[6]; snprintf(cur,sizeof(cur),"%02u:%02u",n.hour(),n.minute()); for(uint8_t i=0;i<MAX_SCHEDULES;i++){ FarmSchedule&s=Store.config().schedules[i]; if(s.enabled&&strcmp(cur,s.startTime)==0&&s.lastStartDay!=n.day()&&(s.repeatDays&(1<<n.dayOfTheWeek()))){ s.lastStartDay=n.day(); active[i]=true; finishAt[i]=nowMs+s.durationMinutes*60000UL; Relays.set(Store.config().zones[s.zone].relayIndex,true); Log.event("Schedule started"); Store.save(); } if(active[i]&&(int32_t)(nowMs-finishAt[i])>=0){ active[i]=false; Relays.set(Store.config().zones[s.zone].relayIndex,false); Log.event("Schedule finished"); } } }
