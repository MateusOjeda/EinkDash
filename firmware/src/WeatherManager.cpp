#include "WeatherManager.h"
#include "Secrets.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_heap_caps.h>
#include <algorithm>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <StreamString.h>

namespace
{
  constexpr uint32_t REFRESH_MS = 60UL * 60UL * 1000UL;
  constexpr uint32_t RETRY_MS = 5UL * 60UL * 1000UL;
  constexpr int REQUEST_CONNECT_MS = 6000;
  constexpr int REQUEST_TOTAL_MS = 12000;
  constexpr int32_t SAO_PAULO_OFFSET = -3 * 3600; // Bebedouro: UTC-03, sem horario de verao
  constexpr const char *API_HOST = "weather.googleapis.com";
  // Desative para testar apenas os tres endpoints estaveis.
  constexpr bool USE_MINUTE_FORECAST = true;

  void copyText(char *target, size_t capacity, const char *source)
  {
    snprintf(target, capacity, "%s", source ? source : "");
  }

  float numberOrNaN(JsonVariantConst value)
  {
    return value.isNull() ? NAN : value.as<float>();
  }

  float maxKnown(float a, float b)
  {
    if (!isfinite(a))
      return b;
    if (!isfinite(b))
      return a;
    return a > b ? a : b;
  }

  // Converte enum da Google Weather API para codigos WMO usados apenas pelos
  // icones existentes do usuario (WeatherIconIds.h). Nao confundir com origem Open-Meteo.
  int iconCode(const char *type)
  {
    if (!type)
      return -1;
    if (!strcmp(type, "CLEAR"))
      return 0;
    if (!strcmp(type, "MOSTLY_CLEAR"))
      return 1;
    if (!strcmp(type, "PARTLY_CLOUDY"))
      return 2;
    if (!strcmp(type, "MOSTLY_CLOUDY"))
      return 3;
    if (!strcmp(type, "CLOUDY") || !strcmp(type, "WINDY"))
      return 3;
    if (strstr(type, "FOG") || strstr(type, "HAZE") || strstr(type, "SMOKE") ||
        strstr(type, "DUST") || strstr(type, "MIST"))
      return 45;
    if (strstr(type, "THUNDER") || strstr(type, "TSTORM"))
      return strstr(type, "HEAVY") || strstr(type, "HAIL") ? 96 : 95;
    if (strstr(type, "HAIL"))
      return 96;
    if (strstr(type, "SNOW") || strstr(type, "BLIZZARD"))
      return 73;
    if (strstr(type, "FREEZING"))
      return 67;
    if (!strcmp(type, "LIGHT_RAIN") || !strcmp(type, "DRIZZLE"))
      return 51;
    if (!strcmp(type, "LIGHT_RAIN_SHOWERS") ||
        !strcmp(type, "CHANCE_OF_SHOWERS") || !strcmp(type, "SCATTERED_SHOWERS"))
      return 80;
    if (!strcmp(type, "HEAVY_RAIN_SHOWERS") || !strcmp(type, "HEAVY_RAIN") ||
        !strcmp(type, "MODERATE_TO_HEAVY_RAIN") ||
        !strcmp(type, "RAIN_PERIODICALLY_HEAVY"))
      return 82;
    if (strstr(type, "SHOWERS"))
      return 81;
    if (strstr(type, "RAIN"))
      return 63;
    return -1;
  }

  // Ex.: "2026-10-29T17:43:00Z"; nao depende de NTP para interpretar os horarios.
  int32_t daysFromCivil(int year, unsigned month, unsigned day)
  {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = unsigned(year - era * 400);
    const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    return era * 146097 + int32_t(yoe * 365 + yoe / 4 - yoe / 100 + doy);
  }

  bool utcEpoch(const char *iso, time_t &out)
  {
    if (!iso)
      return false;
    int y, m, d, hh, mm, ss;
    if (sscanf(iso, "%4d-%2d-%2dT%2d:%2d:%2d", &y, &m, &d, &hh, &mm, &ss) != 6)
      return false;
    if (y < 2020 || m < 1 || m > 12 || d < 1 || d > 31 ||
        hh > 23 || mm > 59 || ss > 60)
      return false;
    const int64_t days = int64_t(daysFromCivil(y, m, d)) - daysFromCivil(1970, 1, 1);
    out = time_t(days * 86400 + hh * 3600 + mm * 60 + ss);
    return true;
  }

  void formatLocal(time_t utc, char (&out)[17])
  {
    const time_t local = utc + SAO_PAULO_OFFSET;
    struct tm parts{};
    gmtime_r(&local, &parts);
    snprintf(out, sizeof(out), "%04d-%02d-%02dT%02d:%02d",
             parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday,
             parts.tm_hour, parts.tm_min);
  }

  // Compara intervalos de tempo reais: a primeira faixa horaria pode ja ter iniciado.
  void recordRain(WeatherData &data, time_t start, time_t end,
                  time_t now, float mmPerHour, int chance)
  {
    if (!isfinite(mmPerHour) || mmPerHour < 0.1f || end <= now)
      return;
    data.hasNextRain = true;
    data.rainNow = start <= now;
    formatLocal(start, data.nextRainTime);
    data.nextRainMm = mmPerHour;
    data.nextRainProbabilityPct = chance;
  }

  // Faz JSON filtrado diretamente do stream HTTPS sem armazenar o corpo inteiro.
  // A chave nunca e impressa no Serial. Certificado TLS: prototipo (setInsecure).
  bool googleGet(const char *resource, const char *parameters,
                 DynamicJsonDocument &doc, DynamicJsonDocument &filter,
                 const char *locationQuery)
  {
    if (WiFi.status() != WL_CONNECTED)
      return false;
    WiFiClientSecure client;
    client.setInsecure(); // TODO producao: validar certificado/CA
    HTTPClient http;
    http.setConnectTimeout(REQUEST_CONNECT_MS);
    http.setTimeout(REQUEST_TOTAL_MS);
    http.useHTTP10(true);

    String url = String("https://") + API_HOST + "/v1/" + resource +
                 "?key=" + GOOGLE_API_KEY + locationQuery + parameters;
    if (!doc.capacity() || !filter.capacity() || filter.overflowed())
    {
      Serial.printf("WEATHER GOOGLE: sem memoria para JSON de %s\n", resource);
      return false;
    }
    if (!http.begin(client, url))
    {
      Serial.printf("WEATHER GOOGLE: falha ao iniciar %s\n", resource);
      return false;
    }
    http.addHeader("Accept-Encoding", "identity");
    const uint32_t started = millis();
    const int status = http.GET();
    if (status != HTTP_CODE_OK)
    {
      Serial.printf("WEATHER GOOGLE: %s HTTP %d (%lu ms)\n",
                    resource, status, (unsigned long)(millis() - started));
      http.end();
      return false;
    }
    DeserializationError error;
    if (strcmp(resource, "forecast/minutes:lookup") == 0 ||
        strcmp(resource, "forecast/days:lookup") == 0)
    {
      // A resposta de minutos e maior. Ler o corpo inteiro pelo HTTPClient
      // evita que uma interrupcao temporaria do stream seja confundida com EOF.
      // Somente este endpoint usa RAM adicional; os outros seguem filtrados
      // diretamente do stream, como antes.
      const int expectedBytes = http.getSize();

      StreamString body;

      const int received = http.writeToStream(&body);

      Serial.printf(
          "WEATHER GOOGLE: %s leitura=%d; body=%u; esperado=%d; maior bloco=%u\n",
          resource,
          received,
          (unsigned)body.length(),
          expectedBytes,
          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

      if (received < 0)
      {
        Serial.printf("WEATHER GOOGLE: ERRO HTTP: %s\n",
                      HTTPClient::errorToString(received).c_str());
        http.end();
        return false;
      }

      if (expectedBytes >= 0 && body.length() != (size_t)expectedBytes)
      {
        Serial.println("WEATHER GOOGLE: resposta incompleta.");
        http.end();
        return false;
      }

      body.trim();

      if (!body.endsWith("}"))
      {
        Serial.printf("WEATHER GOOGLE: JSON truncado; ultimos bytes: %.50s\n",
                      body.c_str() + (body.length() > 50 ? body.length() - 50 : 0));
        http.end();
        return false;
      }

      error = deserializeJson(
          doc,
          body.c_str(),
          DeserializationOption::Filter(filter));
    }
    else
    {
      Stream &stream = http.getStream();
      stream.setTimeout(12000);

      error = deserializeJson(
          doc,
          stream,
          DeserializationOption::Filter(filter));
    }
    http.end();
    if (error || doc.overflowed())
    {
      Serial.printf("WEATHER GOOGLE: %s JSON=%s overflow=%d (usado=%u/%u)\n",
                    resource, error.c_str(), doc.overflowed(),
                    (unsigned)doc.memoryUsage(), (unsigned)doc.capacity());
      return false;
    }
    Serial.printf("WEATHER GOOGLE: %s OK %lu ms; JSON=%u/%u; maior bloco=%u\n",
                  resource, (unsigned long)(millis() - started),
                  (unsigned)doc.memoryUsage(), (unsigned)doc.capacity(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    return true;
  }

  bool readCurrent(WeatherData &output, time_t &nowUtc, const char *locationQuery)
  {
    DynamicJsonDocument filter(896);
    filter["currentTime"] = true;
    filter["isDaytime"] = true;
    filter["weatherCondition"]["type"] = true;
    filter["weatherCondition"]["description"]["text"] = true;
    filter["temperature"]["degrees"] = true;
    filter["feelsLikeTemperature"]["degrees"] = true;
    filter["cloudCover"] = true;
    DynamicJsonDocument doc(1600);
    if (!googleGet("currentConditions:lookup", "&languageCode=pt-BR", doc, filter, locationQuery))
      return false;
    if (doc["temperature"]["degrees"].isNull() ||
        !utcEpoch(doc["currentTime"] | "", nowUtc))
      return false;

    formatLocal(nowUtc, output.observationTime);
    output.currentTempC = numberOrNaN(doc["temperature"]["degrees"]);
    output.apparentTempC = numberOrNaN(doc["feelsLikeTemperature"]["degrees"]);
    output.cloudCoverPct = numberOrNaN(doc["cloudCover"]);
    output.isDay = doc["isDaytime"] | true;
    output.currentCode = iconCode(doc["weatherCondition"]["type"] | "");
    copyText(
        output.currentType,
        sizeof(output.currentType),
        doc["weatherCondition"]["type"] | "");
    copyText(output.currentDescription, sizeof(output.currentDescription),
             doc["weatherCondition"]["description"]["text"] | "");
    return true;
  }

  bool readDaily(WeatherData &output, const char *locationQuery)
  {
    DynamicJsonDocument filter(1800);
    JsonObject day = filter["forecastDays"].to<JsonArray>().createNestedObject();
    day["displayDate"]["year"] = true;
    day["displayDate"]["month"] = true;
    day["displayDate"]["day"] = true;
    day["maxTemperature"]["degrees"] = true;
    day["minTemperature"]["degrees"] = true;
    const char *periods[] = {"daytimeForecast", "nighttimeForecast"};
    for (const char *part : periods)
    {
      day[part]["weatherCondition"]["type"] = true;
      day[part]["precipitation"]["qpf"]["quantity"] = true;
      day[part]["precipitation"]["probability"]["percent"] = true;
      day[part]["wind"]["speed"]["value"] = true;
      day[part]["wind"]["gust"]["value"] = true;
      day[part]["uvIndex"] = true;
    }
    DynamicJsonDocument doc(6900);
    if (!googleGet("forecast/days:lookup", "&days=6&pageSize=6&languageCode=pt-BR", doc, filter, locationQuery))
      return false;
    JsonArrayConst days = doc["forecastDays"].as<JsonArrayConst>();
    if (days.isNull() || days.size() < 6)
    {
      Serial.printf("WEATHER GOOGLE: dias insuficientes (%u/6)\n", (unsigned)days.size());
      return false;
    }
    output.dayCount = 6;
    for (size_t i = 0; i < 6; ++i)
    {
      JsonObjectConst item = days[i].as<JsonObjectConst>();
      WeatherDay &out = output.days[i];
      int year = item["displayDate"]["year"] | 0;
      int month = item["displayDate"]["month"] | 0;
      int date = item["displayDate"]["day"] | 0;
      if (year < 2020 || month < 1 || month > 12 || date < 1 || date > 31)
        return false;
      snprintf(out.date, sizeof(out.date), "%04d-%02d-%02d", year, month, date);
      out.maxC = numberOrNaN(item["maxTemperature"]["degrees"]);
      out.minC = numberOrNaN(item["minTemperature"]["degrees"]);
      JsonObjectConst daytime = item["daytimeForecast"].as<JsonObjectConst>();
      JsonObjectConst nighttime = item["nighttimeForecast"].as<JsonObjectConst>();
      const char *condition = daytime["weatherCondition"]["type"] | "";
      if (!condition[0])
        condition = nighttime["weatherCondition"]["type"] | "";
      out.weatherCode = iconCode(condition);
      float rainDay = numberOrNaN(daytime["precipitation"]["qpf"]["quantity"]);
      float rainNight = numberOrNaN(nighttime["precipitation"]["qpf"]["quantity"]);
      out.precipitationMm = (isfinite(rainDay) ? rainDay : 0) +
                            (isfinite(rainNight) ? rainNight : 0);
      out.precipitationChancePct = maxKnown(
          numberOrNaN(daytime["precipitation"]["probability"]["percent"]),
          numberOrNaN(nighttime["precipitation"]["probability"]["percent"]));
      out.windMaxKmh = maxKnown(numberOrNaN(daytime["wind"]["speed"]["value"]),
                                numberOrNaN(nighttime["wind"]["speed"]["value"]));
      out.gustMaxKmh = maxKnown(numberOrNaN(daytime["wind"]["gust"]["value"]),
                                numberOrNaN(nighttime["wind"]["gust"]["value"]));
      out.uvMax = maxKnown(numberOrNaN(daytime["uvIndex"]),
                           numberOrNaN(nighttime["uvIndex"]));
    }
    return true;
  }

  // Parametro de paginacao: escapamento URL para tokens que contenham + / =.
  String urlEncode(const char *input)
  {
    String encoded;
    if (!input)
      return encoded;
    const char *hex = "0123456789ABCDEF";
    for (const uint8_t *s = (const uint8_t *)input; *s; ++s)
    {
      const uint8_t c = *s;
      if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
          (c >= 'A' && c <= 'Z') || c == '-' || c == '_' ||
          c == '.' || c == '~')
      {
        encoded += char(c);
      }
      else
      {
        encoded += '%';
        encoded += hex[c >> 4];
        encoded += hex[c & 0xF];
      }
    }
    return encoded;
  }

  // Consulta no maximo duas paginas de 24h, preservando o horizonte anterior de 48h.
  // Se achar chuva na primeira pagina, evita a segunda chamada.
  bool readHourly(WeatherData &output, time_t nowUtc, const char *locationQuery)
  {
    DynamicJsonDocument filter(750);
    filter["nextPageToken"] = true;
    JsonObject item = filter["forecastHours"].to<JsonArray>().createNestedObject();
    item["interval"]["startTime"] = true;
    item["interval"]["endTime"] = true;
    item["precipitation"]["qpf"]["quantity"] = true;
    item["precipitation"]["probability"]["percent"] = true;
    DynamicJsonDocument doc(6000);
    String pageToken;
    bool gotFirstPage = false;
    for (int page = 0; page < 2; ++page)
    {
      String parameters = "&hours=48&pageSize=24";
      if (page > 0)
      {
        if (pageToken.isEmpty())
          break;
        parameters += "&pageToken=" + urlEncode(pageToken.c_str());
      }
      doc.clear();
      if (!googleGet("forecast/hours:lookup", parameters.c_str(), doc, filter, locationQuery))
        return gotFirstPage;
      JsonArrayConst hours = doc["forecastHours"].as<JsonArrayConst>();
      if (hours.isNull() || hours.size() == 0)
        return gotFirstPage;
      gotFirstPage = true;
      for (JsonVariantConst value : hours)
      {
        JsonObjectConst hour = value.as<JsonObjectConst>();
        time_t start, end;
        if (!utcEpoch(hour["interval"]["startTime"] | "", start) ||
            !utcEpoch(hour["interval"]["endTime"] | "", end) || end <= start)
          continue;
        float quantity = numberOrNaN(hour["precipitation"]["qpf"]["quantity"]);
        const float mmPerHour = quantity * (3600.0f / float(end - start));
        if (isfinite(mmPerHour) && mmPerHour >= 0.1f && end > nowUtc)
        {
          recordRain(output, start, end, nowUtc, mmPerHour,
                     hour["precipitation"]["probability"]["percent"] | -1);
          return true;
        }
      }
      pageToken = doc["nextPageToken"] | "";
    }
    return gotFirstPage;
  }

  // Dados experimentais com horizonte de ate 6h. Cada segmento pode durar
  // 2, 5, 15... minutos; qpf e convertido corretamente para mm/h.
  // Se nao houver chuva nesta primeira pagina, preserva a previsao horaria.
  bool readMinutes(WeatherData &output, time_t nowUtc, const char *locationQuery)
  {
    DynamicJsonDocument filter(650);
    JsonObject segment = filter["segments"].to<JsonArray>().createNestedObject();
    segment["timeFrame"]["startTime"] = true;
    segment["timeFrame"]["endTime"] = true;
    segment["type"] = true;
    segment["qpf"]["quantity"] = true;
    segment["probability"] = true;
    DynamicJsonDocument doc(4500);
    if (!googleGet("forecast/minutes:lookup", "&pageSize=24", doc, filter, locationQuery))
      return false;
    JsonArrayConst segments = doc["segments"].as<JsonArrayConst>();
    if (segments.isNull() || segments.size() == 0)
      return false;
    for (JsonVariantConst value : segments)
    {
      JsonObjectConst segment = value.as<JsonObjectConst>();
      const char *type = segment["type"] | "";
      if (strcmp(type, "RAIN") != 0)
        continue;
      time_t start, end;
      if (!utcEpoch(segment["timeFrame"]["startTime"] | "", start) ||
          !utcEpoch(segment["timeFrame"]["endTime"] | "", end) || end <= start)
        continue;
      float quantity = numberOrNaN(segment["qpf"]["quantity"]);
      const float mmPerHour = quantity * (3600.0f / float(end - start));
      if (isfinite(mmPerHour) && mmPerHour >= 0.1f && end > nowUtc)
      {
        // Guardar uma alternativa horaria para descartar o cache experimental apos 1h.
        output.hourlyHasNextRain = output.hasNextRain;
        output.hourlyRainNow = output.rainNow;
        copyText(output.hourlyNextRainTime, sizeof(output.hourlyNextRainTime), output.nextRainTime);
        output.hourlyNextRainMm = output.nextRainMm;
        output.hourlyNextRainChance = output.nextRainProbabilityPct;
        recordRain(output, start, end, nowUtc, mmPerHour,
                   segment["probability"] | -1);
        output.minuteForecastUsed = true;
        Serial.println("WEATHER GOOGLE: chuva por minutos aplicada.");
        return true;
      }
    }
    return true;
  }
} // namespace

bool WeatherManager::begin()
{
  if (!GOOGLE_API_KEY[0])
  {
    Serial.println("WEATHER: GOOGLE_API_KEY nao configurada.");
    return false;
  }

  Serial.println("WEATHER: inicializado.");
  return true;
}

bool WeatherManager::fetchAndLog(double latitude, double longitude)
{
  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("WEATHER: Wi-Fi desconectado.");
    return false;
  }

  if (!isfinite(latitude) || !isfinite(longitude) ||
      latitude < -90 || latitude > 90 ||
      longitude < -180 || longitude > 180)
  {
    Serial.println("WEATHER: coordenadas invalidas.");
    return false;
  }

  char locationQuery[128];
  const int length = snprintf(
      locationQuery, sizeof(locationQuery),
      "&location.latitude=%.6f&location.longitude=%.6f&unitsSystem=METRIC",
      latitude, longitude);
  if (length <= 0 || static_cast<size_t>(length) >= sizeof(locationQuery))
  {
    Serial.println("WEATHER: erro ao montar coordenadas.");
    return false;
  }

  Serial.println("WEATHER: iniciando consulta sincrona...");
  Serial.printf("WEATHER: local=%.6f, %.6f\n", latitude, longitude);

  WeatherData result;

  if (!fetch(result, locationQuery))
  {
    Serial.println("WEATHER: consulta falhou.");
    return false;
  }

  _data = result;
  _hasData = true;

  Serial.println("WEATHER: consulta concluida.");
  print(_data);

  return true;
}

bool WeatherManager::fetch(WeatherData &output, const char *locationQuery)
{
  if (WiFi.status() != WL_CONNECTED)
    return false;
  const uint32_t started = millis();
  Serial.printf("WEATHER GOOGLE: consultando (heap=%u)\n", ESP.getFreeHeap());
  WeatherData data;
  time_t nowUtc;
  if (!readCurrent(data, nowUtc, locationQuery))
    return false;
  if (!readDaily(data, locationQuery))
    return false;
  if (!readHourly(data, nowUtc, locationQuery))
    Serial.println("WEATHER GOOGLE: previsao horaria indisponivel (dados atuais e diarios OK).");
  if (USE_MINUTE_FORECAST && !readMinutes(data, nowUtc, locationQuery))
    Serial.println("WEATHER GOOGLE: previsao por minuto indisponivel (horaria preservada).");
  output = data;
  Serial.printf("WEATHER GOOGLE: atualizacao concluida em %lu ms.\n",
                (unsigned long)(millis() - started));
  return true;
}

const char *WeatherManager::conditionName(int c)
{
  switch (c)
  {
  case 0:
    return "Céu limpo";
  case 1:
    return "Predom. de sol";
  case 2:
    return "Parc. nublado";
  case 3:
    return "Nublado";
  case 45:
  case 48:
    return "Nevoeiro";
  case 51:
  case 53:
  case 55:
    return "Garoa";
  case 56:
  case 57:
    return "Garoa congelante";
  case 61:
  case 63:
  case 65:
    return "Chuva";
  case 66:
  case 67:
    return "Chuva congelante";
  case 71:
  case 73:
  case 75:
  case 77:
    return "Neve";
  case 80:
  case 81:
  case 82:
    return "Pancadas de chuva";
  case 85:
  case 86:
    return "Pancadas de neve";
  case 95:
  case 96:
  case 99:
    return "Trovoadas";
  default:
    return "Indefinido";
  }
}

void WeatherManager::print(const WeatherData &w)
{
  Serial.println("\n=== WEATHER: GOOGLE ===");
  Serial.printf("Horario da API: %s\n", w.observationTime);
  Serial.printf("Agora: %.1f C | Sensacao: %.1f C | %s | %s | nuvens %.0f%%\n",
                w.currentTempC, w.apparentTempC,
                w.currentDescription[0] ? w.currentDescription : conditionName(w.currentCode),
                w.isDay ? "dia" : "noite", w.cloudCoverPct);
  for (uint8_t i = 0; i < w.dayCount; ++i)
  {
    const WeatherDay &d = w.days[i];
    Serial.printf("%s: %.1f..%.1f C | %s | chuva %.2f mm (max %.0f%%) | vento %.0f km/h | rajadas %.0f km/h | UV %.0f\n",
                  d.date, d.minC, d.maxC, conditionName(d.weatherCode),
                  d.precipitationMm, d.precipitationChancePct,
                  d.windMaxKmh, d.gustMaxKmh, d.uvMax);
  }
  if (w.hasNextRain)
  {
    Serial.printf("Chuva prevista: %s (%s; %.2f mm/h; prob. %d%%)\n",
                  w.nextRainTime, w.rainNow ? "em andamento" : "futura",
                  w.nextRainMm, w.nextRainProbabilityPct);
  }
  else
  {
    Serial.println("Chuva: nao encontrada no horizonte consultado (horaria 48h / minutos 1a pagina).");
  }
  Serial.println("=====================================");
}
