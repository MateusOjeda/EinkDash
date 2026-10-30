#ifndef EINKDASH_DASHBOARD_DATA_H
#define EINKDASH_DASHBOARD_DATA_H

#include <Arduino.h>
#include <vector>
#include "Habit.h"

struct WeatherData;
struct AirQualityData;
struct PollenData;

// Pacote de informacoes que o main.cpp entrega ao DisplayManager.
// Ponteiro de leitura: os icones continuam na RAM do FirebaseManager;
// nao duplicamos a memoria da lista nem os 576 bytes por icone.
struct DashboardData
{
  const std::vector<Habit> *habits = nullptr;
  const WeatherData *weather = nullptr;
  const AirQualityData *airQuality = nullptr;
  const PollenData *pollen = nullptr;
  String todayISO; // YYYY-MM-DD, horario local; vazio se nao houve sincronizacao de relogio.

  // Futuramente: clima, agenda, tempo de tela e sensores.
};

#endif
