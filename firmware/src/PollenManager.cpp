#include "PollenManager.h"
#include "Secrets.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

bool PollenManager::begin()
{
  if (!GOOGLE_API_KEY[0])
  {
    Serial.println("POLLEN: GOOGLE_API_KEY nao configurada.");
    return false;
  }

  Serial.println("POLLEN: inicializado.");
  return true;
}

bool PollenManager::fetchAndLog(double latitude, double longitude)
{
  Serial.println("\n============ GOOGLE POLLEN =============");
  Serial.printf("Local: %.6f, %.6f\n", latitude, longitude);
  Serial.println("Previsao solicitada: proximos 5 dias.");

  if (!GOOGLE_API_KEY[0] || WiFi.status() != WL_CONNECTED)
  {
    Serial.println("POLLEN: sem chave ou Wi-Fi desconectado.");
    return false;
  }
  if (!isfinite(latitude) || !isfinite(longitude) ||
      latitude < -90 || latitude > 90 || longitude < -180 || longitude > 180)
  {
    Serial.println("POLLEN: coordenadas invalidas.");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure(); // Prototipo: em producao, validar certificado TLS.

  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(12000);
  http.useHTTP10(true); // Evita corpo chunked ao desserializar o stream HTTPS.

  String url = "https://pollen.googleapis.com/v1/forecast:lookup?key=";
  url += GOOGLE_API_KEY;
  url += "&location.latitude=";
  url += String(latitude, 6);
  url += "&location.longitude=";
  url += String(longitude, 6);
  url += "&days=5&languageCode=pt-BR&plantsDescription=false";

  if (!http.begin(client, url))
  {
    Serial.println("POLLEN: falha em HTTPClient.begin().");
    return false;
  }

  const int status = http.GET();
  Serial.printf("POLLEN: HTTP %d\n", status);
  if (status != HTTP_CODE_OK)
  {
    if (status < 0)
      Serial.printf("POLLEN: erro de conexao: %s\n", HTTPClient::errorToString(status).c_str());
    http.end();
    return false;
  }

  // Le apenas codigo e indice por tipo de polen. O retorno continua sendo
  // de 5 dias, mas por ora armazenamos apenas o primeiro no painel.
  DynamicJsonDocument filter(384);
  JsonObject dailyFilter = filter["dailyInfo"].to<JsonArray>().createNestedObject();
  JsonObject pollenFilter = dailyFilter["pollenTypeInfo"].to<JsonArray>().createNestedObject();
  pollenFilter["code"] = true;
  pollenFilter["indexInfo"]["value"] = true;
  pollenFilter["indexInfo"]["category"] = true;

  DynamicJsonDocument doc(3072);
  const DeserializationError error = deserializeJson(
      doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();

  if (error || doc.overflowed())
  {
    Serial.printf("POLLEN: erro no JSON: %s (overflow=%d)\n",
                  error.c_str(), doc.overflowed());
    return false;
  }

  PollenData incoming;
  JsonArrayConst days = doc["dailyInfo"].as<JsonArrayConst>();
  if (!days.isNull() && days.size() > 0)
  {
    for (JsonObjectConst type : days[0]["pollenTypeInfo"].as<JsonArrayConst>())
    {
      const char* code = type["code"] | "";
      if (strcmp(code, "GRASS") != 0)
        continue;
      incoming.grassIndex = type["indexInfo"]["value"] | -1;
      const char* category = type["indexInfo"]["category"] | "";
      snprintf(incoming.category, sizeof(incoming.category), "%s", category);
      break;
    }
  }

  _data = incoming; // -1 significa que gramíneas nao vieram na resposta.
  _hasData = true;
  if (_data.grassIndex >= 0)
    Serial.printf("POLLEN: gramineas UPI=%d (%s)\n", _data.grassIndex, _data.category);
  else
    Serial.println("POLLEN: dados de gramineas indisponiveis.");
  Serial.println("POLLEN: OK");
  Serial.println("========================================");
  return true;
}
