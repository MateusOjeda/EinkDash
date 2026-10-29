#ifndef EINKDASH_FIREBASE_MANAGER_H
#define EINKDASH_FIREBASE_MANAGER_H

#include <Arduino.h>
#include <Preferences.h>
#include <Firebase_ESP_Client.h>
#include <vector>

#include "Habit.h"
#include "FlashManager.h"

class FirebaseManager {
 public:
  // Preserva as assinaturas usadas no firmware atual.
  void begin(const char* apiKey, const char* projectId, Stream& serial);
  bool createAnonymousSession(String& uid);
  bool authenticate();

  // Sincroniza e substitui os dados SOMENTE se todas as consultas derem certo.
  bool syncHabits();
  // Escreve ou remove somente a conclusao de hoje. Nao altera _habits; chame syncHabits depois.
  bool setHabitCompletedToday(const String& habitId, const String& todayISO, bool completed);
  const std::vector<Habit>& getHabits() const { return _habits; }

  // Apenas altera os dados de tela; nao grava na rede.
  bool setLocalHabitCompletedToday(const String& habitId, const String& todayISO, bool completed);
  bool restoreLocalHabitDates(const String& habitId, const String& lastISO, const String& previousISO);

 private:
  FirebaseConfig _config;
  FirebaseAuth _auth;
  FirebaseData _fbdo;
  Preferences _preferences;
  String _projectId;
  Stream* _serial = nullptr;
  std::vector<Habit> _habits;
  FlashManager _iconFlash;

  bool awaitReady(uint32_t timeoutMs = 20000);
  bool readLastCompletionDate(const String& habitId, String& outDate, String& outPrevious);
  bool decodeIcon(const String& base64, Habit& habit);
};

#endif
