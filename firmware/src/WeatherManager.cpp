#include "WeatherManager.h"

#include <ArduinoJson.h>
#include <algorithm>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace {
constexpr uint32_t REFRESH_MS = 60UL * 60UL * 1000UL;
constexpr uint32_t RETRY_MS = 5UL * 60UL * 1000UL;
constexpr int REQUEST_CONNECT_MS = 6000;
constexpr int REQUEST_TOTAL_MS = 10000;

// Coordenadas da sede de Bebedouro-SP segundo o IBGE (-20.949, -48.479).
// forecast_hours=48: serie horaria a partir da hora atual.
// forecast_days=6: hoje e os proximos 5 dias.
static const char* API_URL =
    "https://api.open-meteo.com/v1/forecast"
    "?latitude=-20.949&longitude=-48.479"
    "&timezone=America%2FSao_Paulo"
    "&temperature_unit=celsius&wind_speed_unit=kmh&precipitation_unit=mm"
    "&current=temperature_2m,apparent_temperature,weather_code,is_day,cloud_cover"
    "&daily=weather_code,temperature_2m_max,temperature_2m_min,"
    "precipitation_sum,precipitation_probability_max,"
    "wind_speed_10m_max,wind_gusts_10m_max,uv_index_max"
    "&hourly=precipitation,precipitation_probability"
    "&forecast_hours=48&forecast_days=6";

void copyText(char* destination, size_t size, const char* source) {
  if (!source) source = "";
  snprintf(destination, size, "%s", source);
}

float numberOrNaN(JsonVariantConst v) {
  return v.isNull() ? NAN : v.as<float>();
}

// Evita comparar previsoes anteriores ao horario atual da resposta.
// Ambos usam YYYY-MM-DDTHH:MM, no mesmo fuso definido na URL.
bool notBefore(const char* datetime, const char* currentTime) {
  return datetime && currentTime && strcmp(datetime, currentTime) >= 0;
}
}

bool WeatherManager::begin() {
  if (_results) return true;
  _results = xQueueCreate(1, sizeof(Result));
  if (!_results) {
    Serial.println("WEATHER: nao foi possivel alocar a fila.");
    return false;
  }
  Serial.println("WEATHER: gerenciador iniciado (Open-Meteo; Bebedouro-SP).");
  return true;
}

void WeatherManager::requestNow() {
  _force = true;
  Serial.println("WEATHER: atualizacao solicitada.");
}

void WeatherManager::poll(bool allowNewRequest) {
  if (!_results) return;

  // A tarefa so envia um resultado e termina; este metodo e o UNICO
  // que altera _data, portanto a interface futura a le no loop com seguranca.
  Result result;
  if (xQueueReceive(_results, &result, 0) == pdTRUE) {
    _running = false;
    _lastAttemptOk = result.ok;
    if (result.ok) {
      _data = result.data;
      _hasData = true;
      Serial.println("WEATHER: dados atualizados.");
      print(_data);
    } else {
      Serial.println("WEATHER: consulta falhou; mantendo ultimo dado valido.");
    }
  }

  if (_running || !allowNewRequest || WiFi.status() != WL_CONNECTED)
    return;

  const uint32_t now = millis();
  const uint32_t interval = _lastAttemptOk ? REFRESH_MS : RETRY_MS;
  if (!_force && (uint32_t)(now - _lastAttemptAt) < interval)
    return;

  _force = false;
  _lastAttemptAt = now;
  // 10 KB: TLS e JSON usam heap, e nao um grande array na stack.
  // Core 0, prioridade baixa. O loop e o touch seguem independentes.
  BaseType_t task = xTaskCreatePinnedToCore(
      taskEntry, "open-meteo", 10 * 1024, this, 1, nullptr, 0);
  if (task == pdPASS) {
    _running = true;
  } else {
    Serial.println("WEATHER: falha ao criar tarefa (heap insuficiente?).");
    _lastAttemptOk = false;
  }
}

void WeatherManager::taskEntry(void* context) {
  static_cast<WeatherManager*>(context)->worker();
  vTaskDelete(nullptr);
}

void WeatherManager::worker() {
  Result result;
  result.ok = fetch(result.data);
  if (xQueueSend(_results, &result, pdMS_TO_TICKS(500)) != pdTRUE)
    Serial.println("WEATHER: fila de resultado ocupada.");
}

bool WeatherManager::fetch(WeatherData& output) {
  if (WiFi.status() != WL_CONNECTED) return false;
  const uint32_t started = millis();
  Serial.printf("WEATHER: consultando Open-Meteo (heap=%lu).\n",
                static_cast<unsigned long>(ESP.getFreeHeap()));
  Serial.printf("WEATHER NET: WiFi=%d RSSI=%d dBm | heap8=%u | maior bloco8=%u\n",
                static_cast<int>(WiFi.status()), WiFi.RSSI(),
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));

  IPAddress forecastIP;
  const bool dnsOK = WiFi.hostByName("api.open-meteo.com", forecastIP) == 1;
  if (dnsOK) {
    Serial.printf("WEATHER NET: DNS api.open-meteo.com = %s\n",
                  forecastIP.toString().c_str());
  } else {
    Serial.println("WEATHER NET: FALHA DNS api.open-meteo.com (nao inicia HTTPS).");
    return false;
  }


  Serial.print("WEATHER URL: ");
  Serial.println(API_URL);
  WiFiClientSecure client;
  // TESTE INICIAL: sem verificacao do certificado TLS, sem chaves/senhas na URL.
  // Para uso definitivo, configurar validacao da CA do endpoint.
  client.setInsecure();
  HTTPClient http;
  http.setConnectTimeout(REQUEST_CONNECT_MS);
  http.setTimeout(REQUEST_TOTAL_MS);
  http.useHTTP10(true);  // permite ler o corpo como stream sem chunked encoding.

  if (!http.begin(client, API_URL)) {
    Serial.println("WEATHER: falha ao iniciar HTTPS.");
    return false;
  }
  http.addHeader("Accept-Encoding", "identity");
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("WEATHER: HTTP %d (%s), tempo=%lu ms.\n", code,
                  HTTPClient::errorToString(code).c_str(),
                  static_cast<unsigned long>(millis() - started));
    // Obter erro da camada TLS ANTES de liberar o cliente.
    char tlsDetails[160] = {};
    const int tlsError = client.lastError(tlsDetails, sizeof(tlsDetails));
    Serial.printf("WEATHER TLS: lastError=%d | detalhe=%s\n",
                  tlsError, tlsDetails[0] ? tlsDetails : "(nao informado)");
    http.end();
    client.stop();
    Serial.printf("WEATHER NET: depois do HTTPS | heap8=%u | maior bloco8=%u\n",
                  static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
                  static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));

    // Somente diagnostico apos falha HTTPS: separa alcance TCP de TLS.
    // Nao envia requisicao nem modifica estado do servidor.
    WiFiClient tcp;
    const unsigned long tcpStart = millis();
    const bool tcpOK = tcp.connect(forecastIP, 443, 3000);
    tcp.stop();
    Serial.printf("WEATHER NET: TCP 443 = %s (%lu ms)\n",
                  tcpOK ? "OK" : "FALHOU",
                  static_cast<unsigned long>(millis() - tcpStart));
    return false;
  }

  // Copiar somente os campos realmente usados. Assim, mesmo que a API
  // retorne mais metadados, o JSON em memoria permanece pequeno.
  DynamicJsonDocument filter(1536);
  JsonObject fc = filter["current"].to<JsonObject>();
  fc["time"] = true;
  fc["temperature_2m"] = true;
  fc["apparent_temperature"] = true;
  fc["weather_code"] = true;
  fc["is_day"] = true;
  fc["cloud_cover"] = true;

  JsonObject fd = filter["daily"].to<JsonObject>();
  fd["time"] = true;
  fd["weather_code"] = true;
  fd["temperature_2m_max"] = true;
  fd["temperature_2m_min"] = true;
  fd["precipitation_sum"] = true;
  fd["precipitation_probability_max"] = true;
  fd["wind_speed_10m_max"] = true;
  fd["wind_gusts_10m_max"] = true;
  fd["uv_index_max"] = true;

  JsonObject fh = filter["hourly"].to<JsonObject>();
  fh["time"] = true;
  fh["precipitation"] = true;
  fh["precipitation_probability"] = true;

  // O TLS permanece ativo durante o parse: reservar um bloco menor
  // do que o maior bloco livre observado (8692 B no diagnostico).
  // O filtro acima mantem somente os campos usados pelo EinkDash.
  DynamicJsonDocument doc(7168);
  Serial.printf(
      "WEATHER JSON ANTES: capacidade=%u | heap=%u | maior bloco=%u\n",
      (unsigned)doc.capacity(),
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  if (doc.capacity() == 0) {
    Serial.println("WEATHER: nao foi possivel alocar o documento JSON.");
    http.end();
    return false;
  }
  DeserializationError error = deserializeJson(
      doc, http.getStream(), DeserializationOption::Filter(filter));
  Serial.printf(
      "WEATHER JSON DEPOIS: erro=%s | usado=%u/%u | overflow=%d | maior bloco=%u\n",
      error.c_str(),
      (unsigned)doc.memoryUsage(),
      (unsigned)doc.capacity(),
      doc.overflowed() ? 1 : 0,
      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  http.end();

  if (error) {
    Serial.printf("WEATHER: JSON invalido: %s (heap=%lu).\n",
                  error.c_str(), static_cast<unsigned long>(ESP.getFreeHeap()));
    return false;
  }

  JsonObjectConst current = doc["current"].as<JsonObjectConst>();
  JsonObjectConst daily = doc["daily"].as<JsonObjectConst>();
  JsonObjectConst hourly = doc["hourly"].as<JsonObjectConst>();
  if (current.isNull() || daily.isNull() || hourly.isNull() ||
      current["temperature_2m"].isNull()) {
    Serial.println("WEATHER: JSON sem campos obrigatorios.");
    return false;
  }

  WeatherData data;
  copyText(data.observationTime, sizeof(data.observationTime),
           current["time"] | "");
  data.currentTempC = numberOrNaN(current["temperature_2m"]);
  data.apparentTempC = numberOrNaN(current["apparent_temperature"]);
  data.currentCode = current["weather_code"] | -1;
  data.isDay = (current["is_day"] | 1) != 0;
  data.cloudCoverPct = numberOrNaN(current["cloud_cover"]);

  JsonArrayConst dates = daily["time"].as<JsonArrayConst>();
  if (dates.isNull() || dates.size() < 1) {
    Serial.println("WEATHER: previsao diaria vazia.");
    return false;
  }
  const size_t count = std::min<size_t>(dates.size(), sizeof(data.days) / sizeof(data.days[0]));
  data.dayCount = (uint8_t)count;
  for (size_t i = 0; i < count; ++i) {
    WeatherDay& day = data.days[i];
    copyText(day.date, sizeof(day.date), daily["time"][i] | "");
    day.weatherCode = daily["weather_code"][i] | -1;
    day.minC = numberOrNaN(daily["temperature_2m_min"][i]);
    day.maxC = numberOrNaN(daily["temperature_2m_max"][i]);
    day.precipitationMm = numberOrNaN(daily["precipitation_sum"][i]);
    day.precipitationChancePct = numberOrNaN(daily["precipitation_probability_max"][i]);
    day.windMaxKmh = numberOrNaN(daily["wind_speed_10m_max"][i]);
    day.gustMaxKmh = numberOrNaN(daily["wind_gusts_10m_max"][i]);
    day.uvMax = numberOrNaN(daily["uv_index_max"][i]);
  }

  JsonArrayConst times = hourly["time"].as<JsonArrayConst>();
  JsonArrayConst mm = hourly["precipitation"].as<JsonArrayConst>();
  JsonArrayConst pct = hourly["precipitation_probability"].as<JsonArrayConst>();
  if (!times.isNull() && !mm.isNull()) {
    const size_t n = times.size() < mm.size() ? times.size() : mm.size();
    for (size_t i = 0; i < n; ++i) {
      const char* hour = times[i] | "";
      const float expectedMm = numberOrNaN(mm[i]);
      if (notBefore(hour, data.observationTime) &&
          !isnan(expectedMm) && expectedMm >= 0.1f) {
        data.hasNextRain = true;
        copyText(data.nextRainTime, sizeof(data.nextRainTime), hour);
        data.nextRainMm = expectedMm;
        if (!pct.isNull() && i < pct.size() && !pct[i].isNull())
          data.nextRainProbabilityPct = (int)roundf(pct[i].as<float>());
        break;
      }
    }
  }

  output = data;
  Serial.printf("WEATHER: HTTP 200; tempo=%lu ms; heap=%lu bytes.\n",
                static_cast<unsigned long>(millis() - started),
                static_cast<unsigned long>(ESP.getFreeHeap()));
  return true;
}

const char* WeatherManager::conditionName(int c) {
  switch (c) {
    case 0: return "Ceu limpo";
    case 1: return "Predom. de sol";
    case 2: return "Parc. nublado";
    case 3: return "Nublado";
    case 45: case 48: return "Nevoeiro";
    case 51: case 53: case 55: return "Garoa";
    case 56: case 57: return "Garoa congelante";
    case 61: case 63: case 65: return "Chuva";
    case 66: case 67: return "Chuva congelante";
    case 71: case 73: case 75: case 77: return "Neve";
    case 80: case 81: case 82: return "Pancadas de chuva";
    case 85: case 86: return "Pancadas de neve";
    case 95: case 96: case 99: return "Trovoadas";
    default: return "Indefinido";
  }
}

void WeatherManager::print(const WeatherData& w) {
  Serial.println();
  Serial.println("=== WEATHER: BEBEDOURO-SP ===");
  Serial.printf("Horario da API: %s\n", w.observationTime);
  Serial.printf("Agora: %.1f C | Sensacao: %.1f C | %s | %s\n",
                w.currentTempC, w.apparentTempC,
                conditionName(w.currentCode), w.isDay ? "dia" : "noite");
  for (uint8_t i = 0; i < w.dayCount; ++i) {
    const WeatherDay& d = w.days[i];
    Serial.printf("%s: %.1f..%.1f C | %s | chuva %.1f mm (max %.0f%%) | vento %.0f km/h | rajadas %.0f km/h | UV %.1f\n",
                  d.date, d.minC, d.maxC, conditionName(d.weatherCode),
                  d.precipitationMm, d.precipitationChancePct,
                  d.windMaxKmh, d.gustMaxKmh, d.uvMax);
  }
  if (w.hasNextRain) {
    Serial.printf("Proxima chuva prevista: %s (%.1f mm/h; prob. %d%%)\n",
                  w.nextRainTime, w.nextRainMm, w.nextRainProbabilityPct);
  } else {
    Serial.println("Proxima chuva: nao prevista nas proximas 48 h acima de 0,1 mm/h.");
  }
  Serial.println("=============================");
}
