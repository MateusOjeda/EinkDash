#include "AirQualityManager.h"
#include "Secrets.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

bool AirQualityManager::begin()
{
  if (!GOOGLE_API_KEY[0])
  {
    Serial.println("AIR: GOOGLE_API_KEY nao configurada.");
    return false;
  }

  Serial.println("AIR: inicializado.");
  return true;
}

bool AirQualityManager::fetchAndLog(double latitude, double longitude)
{
  Serial.println("\n========== GOOGLE AIR QUALITY ==========");
  Serial.printf("Local: %.6f, %.6f\n", latitude, longitude);

  if (!GOOGLE_API_KEY[0] || WiFi.status() != WL_CONNECTED)
  {
    Serial.println("AIR: sem chave ou Wi-Fi desconectado.");
    return false;
  }
  if (!isfinite(latitude) || !isfinite(longitude) ||
      latitude < -90 || latitude > 90 || longitude < -180 || longitude > 180)
  {
    Serial.println("AIR: coordenadas invalidas.");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure(); // Prototipo: em producao, validar certificado TLS.

  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(12000);
  http.useHTTP10(true); // Evita corpo chunked ao desserializar o stream HTTPS.

  String url = "https://airquality.googleapis.com/v1/currentConditions:lookup?key=";
  url += GOOGLE_API_KEY;
  if (!http.begin(client, url))
  {
    Serial.println("AIR: falha em HTTPClient.begin().");
    return false;
  }

  http.addHeader("Content-Type", "application/json");
  char payload[512];
  const int length = snprintf(
      payload, sizeof(payload),
      R"json({
    "location": {"latitude": %.6f, "longitude": %.6f},
    "universalAqi": true,
    "extraComputations": [
      "LOCAL_AQI",
      "POLLUTANT_CONCENTRATION",
      "DOMINANT_POLLUTANT_CONCENTRATION",
      "HEALTH_RECOMMENDATIONS"
    ],
    "languageCode": "pt-BR"
  })json", latitude, longitude);
  if (length <= 0 || static_cast<size_t>(length) >= sizeof(payload))
  {
    Serial.println("AIR: erro ao montar JSON de localizacao.");
    http.end();
    return false;
  }

  const int status = http.POST(reinterpret_cast<uint8_t*>(payload),
                               static_cast<size_t>(length));
  Serial.printf("AIR: HTTP %d\n", status);
  if (status != HTTP_CODE_OK)
  {
    if (status < 0)
      Serial.printf("AIR: erro de conexao: %s\n", HTTPClient::errorToString(status).c_str());
    http.end();
    return false;
  }

  // Seleciona somente o indice brasileiro e as concentracoes PM2.5 / PM10.
  // Nao guarda a resposta JSON completa na RAM.
  DynamicJsonDocument filter(512);
  JsonObject indexFilter = filter["indexes"].to<JsonArray>().createNestedObject();
  indexFilter["code"] = true;
  indexFilter["aqi"] = true;
  indexFilter["category"] = true;
  JsonObject pollutantFilter = filter["pollutants"].to<JsonArray>().createNestedObject();
  pollutantFilter["code"] = true;
  pollutantFilter["concentration"]["value"] = true;

  DynamicJsonDocument doc(3072);
  const DeserializationError error = deserializeJson(
      doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end(); // Fecha HTTPS antes de liberar os objetos JSON temporarios.

  if (error || doc.overflowed())
  {
    Serial.printf("AIR: erro no JSON: %s (overflow=%d)\n",
                  error.c_str(), doc.overflowed());
    return false;
  }

  AirQualityData incoming;
  for (JsonObjectConst index : doc["indexes"].as<JsonArrayConst>())
  {
    const char* code = index["code"] | "";
    if (strcmp(code, "bra_saopaulo") != 0)
      continue;
    incoming.brazilAqi = index["aqi"] | -1;
    const char* category = index["category"] | "";
    snprintf(incoming.category, sizeof(incoming.category), "%s", category);
    break;
  }
  if (incoming.brazilAqi < 0)
  {
    Serial.println("AIR: indice brasileiro nao retornado; dados anteriores preservados.");
    return false;
  }

  for (JsonObjectConst pollutant : doc["pollutants"].as<JsonArrayConst>())
  {
    const char* code = pollutant["code"] | "";
    const float value = pollutant["concentration"]["value"] | -1.0f;
    if (!strcmp(code, "pm25")) incoming.pm25 = value;
    else if (!strcmp(code, "pm10")) incoming.pm10 = value;
  }

  _data = incoming;
  _hasData = true;
  Serial.printf("AIR: AQI BR=%d (%s) | PM2.5=%.2f | PM10=%.2f ug/m3\n",
                _data.brazilAqi, _data.category, _data.pm25, _data.pm10);
  Serial.println("AIR: OK");
  Serial.println("========================================");
  return true;
}
