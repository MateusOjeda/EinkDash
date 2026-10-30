#ifndef EINKDASH_FIREBASE_MANAGER_H
#define EINKDASH_FIREBASE_MANAGER_H

#include <Arduino.h>
#include <Preferences.h>
#include <Firebase_ESP_Client.h>
#include <vector>

#include "Habit.h"
#include "FlashManager.h"
#include "CalendarData.h"
#include "ScreenTimeDetails.h"

struct DeviceSettings
{
  double latitude = 0;
  double longitude = 0;
  bool hasLocation = false;
};

class FirebaseManager
{
public:
  // Preserva as assinaturas usadas no firmware atual.
  void begin(const char *apiKey, const char *projectId, Stream &serial);
  bool createAnonymousSession(String &uid);
  bool authenticate();

  // Sincroniza e substitui os dados SOMENTE se todas as consultas derem certo.
  bool syncHabits();
  // Escreve ou remove somente a conclusao de hoje. Nao altera _habits; chame syncHabits depois.
  bool setHabitCompletedToday(const String &habitId, const String &todayISO, bool completed);
  const std::vector<Habit> &getHabits() const { return _habits; }

  // Apenas altera os dados de tela; nao grava na rede.
  bool setLocalHabitCompletedToday(const String &habitId, const String &todayISO, bool completed);
  bool restoreLocalHabitDates(const String &habitId, const String &lastISO, const String &previousISO);

  bool syncSettings(const char *documentPath);

  // Consulta calendar/AAAA-MM-DD (documento gravado pelo Apps Script).
  bool readCalendarDay(const char *date, CalendarDay &out);

  // Busca SOMENTE birthdays/MM-DD para a data solicitada.
  // Documento ausente (404) significa nenhum aniversario naquele dia.
  bool readBirthdaysForDay(const char *isoDate, std::vector<CalendarBirthday> &out);

  bool readScreenTime(
      const char *deviceId,
      const char *date,
      uint32_t &totalTV,
      uint32_t &totalPC);

  // Consulta UNICA sob toque; traz categorias tv/pc (nao faz polling).
  bool readScreenTimeDetails(const char *deviceId, const char *date,
                             ScreenTimeDetails &out);

  const DeviceSettings &getSettings() const
  {
    return _settings;
  }

private:
  FirebaseConfig _config;
  FirebaseAuth _auth;
  FirebaseData _fbdo;
  Preferences _preferences;
  String _projectId;
  Stream *_serial = nullptr;
  std::vector<Habit> _habits;
  FlashManager _iconFlash;
  DeviceSettings _settings;

  bool awaitReady(uint32_t timeoutMs = 20000);
  bool readLastCompletionDate(const String &habitId, String &outDate, String &outPrevious);
  bool decodeIcon(const String &base64, Habit &habit);
};

#endif
