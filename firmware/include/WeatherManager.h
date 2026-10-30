#ifndef EINKDASH_WEATHER_MANAGER_H
#define EINKDASH_WEATHER_MANAGER_H

#include <Arduino.h>
#include <math.h>

// Tipos de tamanho fixo: mantidos em RAM para o DashboardData.
struct WeatherDay
{
  char date[11] = {};
  int weatherCode = -1;
  float minC = NAN;
  float maxC = NAN;
  float precipitationMm = NAN;
  float precipitationChancePct = NAN;
  float windMaxKmh = NAN;
  float gustMaxKmh = NAN;
  float uvMax = NAN;
};

struct WeatherData
{
  char observationTime[17] = {};    // horario local YYYY-MM-DDTHH:MM
  char currentDescription[72] = {}; // descricao pt-BR vinda do Google
  char currentType[40] = {};
  float currentTempC = NAN;
  float apparentTempC = NAN;
  int currentCode = -1;
  bool isDay = true;
  float cloudCoverPct = NAN; // cobertura de nuvens atual (%)
  WeatherDay days[6] = {};
  uint8_t dayCount = 0;

  // Primeiro horario futuro com previsao >= 0,1 mm/h, nas proximas 48 h.
  bool hasNextRain = false;
  bool rainNow = false; // faixa de chuva inclui o horario da consulta
  char nextRainTime[17] = {};
  float nextRainMm = 0; // intensidade media prevista, mm/h
  int nextRainProbabilityPct = -1;

  // Permite expirar a previsao experimental sem perder o fallback horario.
  bool minuteForecastUsed = false;
  bool hourlyHasNextRain = false;
  bool hourlyRainNow = false;
  char hourlyNextRainTime[17] = {};
  float hourlyNextRainMm = 0;
  int hourlyNextRainChance = -1;
};

class WeatherManager
{
public:
  bool begin();
  bool fetchAndLog(double latitude, double longitude);

  bool hasData() const { return _hasData; }
  const WeatherData &data() const { return _data; }

  static const char *conditionName(int weatherCode);
  static void print(const WeatherData &value);

private:
  bool fetch(WeatherData &output, const char *locationQuery);

  WeatherData _data;
  bool _hasData = false;
};

#endif
