#ifndef EINKDASH_WEATHER_MANAGER_H
#define EINKDASH_WEATHER_MANAGER_H

#include <Arduino.h>
#include <math.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// Tudo em tipos de tamanho fixo: pode atravessar uma fila FreeRTOS sem
// alocar String e fica disponivel em RAM para o futuro DashboardData.
struct WeatherDay {
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

struct WeatherData {
  char observationTime[17] = {};
  float currentTempC = NAN;
  float apparentTempC = NAN;
  int currentCode = -1;
  bool isDay = true;
  float cloudCoverPct = NAN;  // cobertura de nuvens atual (%)
  WeatherDay days[6] = {};
  uint8_t dayCount = 0;

  // Primeiro horario futuro com previsao >= 0,1 mm/h, nas proximas 48 h.
  bool hasNextRain = false;
  char nextRainTime[17] = {};
  float nextRainMm = 0;
  int nextRainProbabilityPct = -1;
};

class WeatherManager {
public:
  // Nao realiza HTTP em begin(). Apenas cria a fila.
  bool begin();

  // Deve ser chamada no loop. HTTP ocorre numa tarefa independente;
  // allowNewRequest=false impede iniciar novas chamadas durante gravacoes.
  void poll(bool allowNewRequest = true);

  // Forca a proxima requisicao quando nao houver outra em andamento.
  void requestNow();

  bool hasData() const { return _hasData; }
  const WeatherData& data() const { return _data; }
  static const char* conditionName(int weatherCode);
  static void print(const WeatherData& value);

private:
  struct Result {
    bool ok = false;
    WeatherData data;
  };

  static void taskEntry(void* context);
  void worker();
  bool fetch(WeatherData& output);

  QueueHandle_t _results = nullptr;
  WeatherData _data;
  bool _hasData = false;
  bool _running = false;
  bool _force = true;
  uint32_t _lastAttemptAt = 0;
  bool _lastAttemptOk = false;
};

#endif
