#ifndef EINKDASH_CALENDAR_DATA_H
#define EINKDASH_CALENDAR_DATA_H

#include <Arduino.h>
#include <vector>

// Eventos em calendar/AAAA-MM-DD; aniversarios em birthdays/MM-DD.
struct CalendarEvent
{
  String title;
  String start; // HH:mm
  String end;   // HH:mm
  bool allDay = false;
};

// Apenas as pessoas da data consultada em birthdays/MM-DD.
struct CalendarBirthday
{
  String name;
  uint8_t month = 0;
  uint8_t day = 0;
};

struct CalendarDay
{
  String date; // AAAA-MM-DD
  std::vector<CalendarEvent> events;
  bool hasMore = false; // Mais de seis eventos
  bool found = false;   // Documento existe no Firestore
  bool loadError = false;
};

#endif
