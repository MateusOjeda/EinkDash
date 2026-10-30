#include "FirebaseManager.h"

#include <WiFi.h>
#include <algorithm>
#include <utility>
#include <mbedtls/base64.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <stdlib.h>

void FirebaseManager::begin(const char *apiKey,
                            const char *projectId,
                            Stream &serial)
{
  _projectId = projectId;
  _serial = &serial;
  _config.api_key = apiKey;
}

bool FirebaseManager::awaitReady(uint32_t timeoutMs)
{
  const uint32_t startedAt = millis();
  while (!Firebase.ready())
  {
    if (millis() - startedAt >= timeoutMs)
    {
      _serial->println("FIREBASE: timeout aguardando Firebase.ready().");
      return false;
    }
    delay(100);
  }
  return true;
}

bool FirebaseManager::createAnonymousSession(String &uid)
{
  uid = "";
  _serial->println("FIREBASE: criando sessao anonima...");
  Firebase.reconnectNetwork(true);

  if (!Firebase.signUp(&_config, &_auth, "", ""))
  {
    _serial->print("FIREBASE: falha no cadastro anonimo: ");
    _serial->println(_config.signer.signupError.message.c_str());
    return false;
  }

  uid = _auth.token.uid.c_str();
  Firebase.begin(&_config, &_auth);
  if (!awaitReady())
    return false;

  const char *refreshToken = Firebase.getRefreshToken();
  if (!refreshToken || !strlen(refreshToken))
  {
    _serial->println("FIREBASE: refresh token vazio.");
    return false;
  }

  if (!_preferences.begin("firebase", false))
  {
    _serial->println("FIREBASE: nao foi possivel abrir NVS.");
    return false;
  }
  const size_t saved = _preferences.putString("refreshToken", refreshToken);
  _preferences.end();
  if (!saved)
  {
    _serial->println("FIREBASE: falha ao salvar refresh token na NVS.");
    return false;
  }
  _serial->println("FIREBASE: token persistido na NVS (conteudo omitido).");
  return true;
}

bool FirebaseManager::authenticate()
{
  if (!_preferences.begin("firebase", true))
  {
    _serial->println("FIREBASE: nao foi possivel abrir NVS.");
    return false;
  }
  const String refreshToken = _preferences.getString("refreshToken", "");
  _preferences.end();

  if (refreshToken.isEmpty())
  {
    _serial->println("FIREBASE: sessao ausente. Configure pelo app uma vez.");
    return false;
  }

  Firebase.reconnectNetwork(true);
  Firebase.setIdToken(&_config, "", 3600, refreshToken.c_str());
  Firebase.begin(&_config, &_auth);
  if (!awaitReady())
    return false;

  _serial->println("FIREBASE: autenticado com refresh token da NVS.");
  return true;
}

// Converte o Base64 do aplicativo diretamente para bytes gray2bpp.
// 48 x 48 -> 576 bytes; nao guarda a string Base64 depois de ler o documento.
bool FirebaseManager::decodeIcon(const String &base64, Habit &habit)
{
  if (habit.iconWidth == 0 || habit.iconHeight == 0 ||
      habit.iconWidth > 128 || habit.iconHeight > 128)
  {
    return false;
  }

  const size_t pixelCount = static_cast<size_t>(habit.iconWidth) * habit.iconHeight;
  const size_t expectedBytes = (pixelCount + 3) / 4;

  habit.iconBytes.resize(expectedBytes);
  size_t decodedBytes = 0;

  const int result = mbedtls_base64_decode(
      habit.iconBytes.data(), habit.iconBytes.size(), &decodedBytes,
      reinterpret_cast<const unsigned char *>(base64.c_str()), base64.length());

  if (result != 0 || decodedBytes != expectedBytes)
  {
    habit.iconBytes.clear();
    habit.iconWidth = 0;
    habit.iconHeight = 0;
    return false;
  }
  // A assinatura permite invalidar a imagem-base quando o icone muda,
  // sem precisar guardar seus 576 bytes em RAM depois da sincronizacao.
  uint32_t h = 2166136261UL;
  auto hashBytes = [&h](const uint8_t *p, size_t n)
  {
    for (size_t i = 0; i < n; ++i)
    {
      h ^= p[i];
      h *= 16777619UL;
    }
  };
  hashBytes(reinterpret_cast<const uint8_t *>(habit.id.c_str()), habit.id.length());
  hashBytes(reinterpret_cast<const uint8_t *>(&habit.iconWidth), sizeof(habit.iconWidth));
  hashBytes(reinterpret_cast<const uint8_t *>(&habit.iconHeight), sizeof(habit.iconHeight));
  hashBytes(habit.iconBytes.data(), habit.iconBytes.size());
  habit.iconHash = h;
  return true;
}

bool FirebaseManager::syncSettings(const char *documentPath)
{
  if (WiFi.status() != WL_CONNECTED)
  {
    _serial->println("SETTINGS: Wi-Fi desconectado.");
    return false;
  }

  if (!documentPath || !documentPath[0])
  {
    _serial->println("SETTINGS: caminho do documento invalido.");
    return false;
  }

  if (!awaitReady(5000))
    return false;

  _serial->println("SETTINGS: buscando configuracoes...");

  // Uma unica requisicao sincrona ao Firestore.
  if (!Firebase.Firestore.getDocument(
          &_fbdo,
          _projectId.c_str(),
          "",
          documentPath,
          ""))
  {
    _serial->print("SETTINGS: erro: ");
    _serial->println(_fbdo.errorReason());
    return false;
  }

  FirebaseJson json;

  if (!json.setJsonData(_fbdo.payload()))
  {
    _serial->println("SETTINGS: JSON invalido.");
    return false;
  }

  FirebaseJsonData latData;
  FirebaseJsonData lonData;

  if (!json.get(latData, "fields/latitude/doubleValue") ||
      !json.get(lonData, "fields/longitude/doubleValue"))
  {
    _serial->println("SETTINGS: latitude ou longitude ausente.");
    return false;
  }

  const double latitude = latData.to<double>();
  const double longitude = lonData.to<double>();

  if (!isfinite(latitude) || !isfinite(longitude) ||
      latitude < -90 || latitude > 90 ||
      longitude < -180 || longitude > 180)
  {
    _serial->println("SETTINGS: coordenadas invalidas.");
    return false;
  }

  // So substitui os dados em RAM se tudo estiver correto.
  _settings.latitude = latitude;
  _settings.longitude = longitude;
  _settings.hasLocation = true;

  _serial->println("SETTINGS: configuracoes carregadas.");
  _serial->printf(
      "SETTINGS: latitude=%.6f longitude=%.6f\n",
      latitude,
      longitude);

  return true;
}

bool FirebaseManager::readScreenTime(
    const char *deviceId,
    const char *date,
    uint32_t &totalTV,
    uint32_t &totalPC)
{
  if (!Firebase.ready())
  {
    Serial.println("SCREEN TIME: Firebase indisponivel.");
    return false;
  }

  String path = "screenTime/";
  path += deviceId;
  path += "/days/";
  path += date;

  Serial.printf("SCREEN TIME: consultando %s\n", path.c_str());

  const char *mask = "totalSecondsTV,totalSecondsPC";

  if (!Firebase.Firestore.getDocument(
          &_fbdo,
          _projectId.c_str(),
          "",
          path.c_str(),
          mask))
  {
    // Dia sem registros ainda.
    if (_fbdo.httpCode() == 404)
    {
      totalTV = 0;
      totalPC = 0;
      Serial.println("SCREEN TIME: sem registros hoje.");
      return true;
    }

    Serial.printf(
        "SCREEN TIME: erro: %s\n",
        _fbdo.errorReason().c_str());

    return false;
  }

  FirebaseJson json;
  json.setJsonData(_fbdo.payload());

  FirebaseJsonData tv;
  FirebaseJsonData pc;

  if (!json.get(tv, "fields/totalSecondsTV/integerValue") ||
      !json.get(pc, "fields/totalSecondsPC/integerValue"))
  {
    Serial.println("SCREEN TIME: campos ausentes.");
    return false;
  }

  totalTV = (uint32_t)tv.to<String>().toInt();
  totalPC = (uint32_t)pc.to<String>().toInt();

  Serial.printf(
      "SCREEN TIME: TV=%lu s | PC=%lu s\n",
      (unsigned long)totalTV,
      (unsigned long)totalPC);

  return true;
}

// Converte campos dinâmicos da estrutura Firestore "pc"/"tv" em lista.
// Ex.: fields/pc/mapValue/fields/vscode/integerValue = "5400".
// A quantidade em RAM fica limitada aos 24 maiores aplicativos por grupo.
static void parseScreenTimeGroup(FirebaseJson &document,
                                 const char *field,
                                 std::vector<ScreenTimeEntry> &out)
{
  FirebaseJsonData map;
  const String path = String("fields/") + field + "/mapValue/fields";
  if (!document.get(map, path)) return;

  FirebaseJson fields;
  if (!fields.setJsonData(map.stringValue)) return;

  const size_t count = fields.iteratorBegin();
  for (size_t i = 0; i < count; ++i)
  {
    int type = 0;
    String key, unusedValue;
    fields.iteratorGet(i, type, key, unusedValue);
    if (key.isEmpty() || key == "totalSeconds") continue;

    FirebaseJsonData seconds;
    if (!fields.get(seconds, key + "/integerValue")) continue;
    const uint32_t value = static_cast<uint32_t>(strtoul(seconds.to<String>().c_str(), nullptr, 10));
    if (value == 0) continue;
    ScreenTimeEntry item;
    item.name = key;
    item.seconds = value;
    out.push_back(std::move(item));
  }
  fields.iteratorEnd();

  std::sort(out.begin(), out.end(), [](const ScreenTimeEntry &a, const ScreenTimeEntry &b)
            { return a.seconds > b.seconds; });
  // So os maiores sao necessarios para o card; evita manter dezenas de apps.
  if (out.size() > 24) out.resize(24);
}

bool FirebaseManager::readScreenTimeDetails(const char *deviceId,
                                             const char *date,
                                             ScreenTimeDetails &out)
{
  if (!deviceId || !date || strlen(date) != 10 ||
      WiFi.status() != WL_CONNECTED || !awaitReady(5000))
    return false;

  const String path = String("screenTime/") + deviceId + "/days/" + date;
  _serial->printf("SCREEN DETAILS: toque -> consultando %s\n", path.c_str());
  // Campos filtrados: nao busca metadados nao utilizados.
  if (!Firebase.Firestore.getDocument(&_fbdo, _projectId.c_str(), "",
                                      path.c_str(), "pc,tv,totalSecondsPC,totalSecondsTV"))
  {
    if (_fbdo.httpCode() == 404)
    {
      ScreenTimeDetails empty;
      empty.date = date;
      empty.dataAvailable = true;
      out = std::move(empty);
      return true;
    }
    _serial->printf("SCREEN DETAILS: HTTP %d: %s\n",
                    _fbdo.httpCode(), _fbdo.errorReason().c_str());
    return false;
  }

  FirebaseJson document;
  if (!document.setJsonData(_fbdo.payload()))
  {
    _serial->println("SCREEN DETAILS: JSON invalido.");
    return false;
  }
  ScreenTimeDetails fresh;
  fresh.date = date;
  fresh.dataAvailable = true;
  FirebaseJsonData total;
  if (document.get(total, "fields/totalSecondsPC/integerValue"))
    fresh.totalPC = static_cast<uint32_t>(strtoul(total.to<String>().c_str(), nullptr, 10));
  if (document.get(total, "fields/totalSecondsTV/integerValue"))
    fresh.totalTV = static_cast<uint32_t>(strtoul(total.to<String>().c_str(), nullptr, 10));

  parseScreenTimeGroup(document, "pc", fresh.pc);
  parseScreenTimeGroup(document, "tv", fresh.tv);
  _serial->printf("SCREEN DETAILS: %u categorias PC, %u TV.\n",
                  (unsigned)fresh.pc.size(), (unsigned)fresh.tv.size());
  out = std::move(fresh);
  return true;
}

// Um documento por data, com no maximo seis eventos materializados na RAM.
// A estrutura corresponde diretamente ao Apps Script: fields/events/arrayValue.
bool FirebaseManager::readCalendarDay(const char *date, CalendarDay &out)
{
  if (!date || strlen(date) != 10 || date[4] != '-' || date[7] != '-')
    return false;
  if (WiFi.status() != WL_CONNECTED || !awaitReady(5000))
  {
    _serial->println("CALENDAR: Wi-Fi ou Firebase indisponivel.");
    return false;
  }

  const String path = String("calendar/") + date;
  _serial->printf("CALENDAR: consultando %s\n", path.c_str());
  if (!Firebase.Firestore.getDocument(
          &_fbdo, _projectId.c_str(), "", path.c_str(), "events"))
  {
    if (_fbdo.httpCode() == 404)
    {
      out.date = date;
      out.found = false;
      out.events.clear();
      out.hasMore = false;
      _serial->println("CALENDAR: documento ainda nao sincronizado.");
      return true;
    }
    _serial->printf("CALENDAR: erro HTTP %d: %s\n",
                    _fbdo.httpCode(), _fbdo.errorReason().c_str());
    return false;
  }

  FirebaseJson json;
  if (!json.setJsonData(_fbdo.payload()))
  {
    _serial->println("CALENDAR: resposta JSON invalida.");
    return false;
  }

  CalendarDay incoming;
  incoming.date = date;
  incoming.found = true;
  incoming.events.reserve(6);

  // Os dias vazios podem ter arrayValue sem o campo 'values'.
  for (uint8_t i = 0; i < 7; ++i)
  {
    const String base = String("fields/events/arrayValue/values/[") + String(i) +
                        "]/mapValue/fields/";
    FirebaseJsonData title;
    if (!json.get(title, base + "title/stringValue"))
      break;
    if (i == 6)
    {
      incoming.hasMore = true;
      break;
    }
    CalendarEvent event;
    event.title = title.to<String>();
    FirebaseJsonData start, end, allDay;
    if (json.get(start, base + "start/stringValue"))
      event.start = start.to<String>();
    if (json.get(end, base + "end/stringValue"))
      event.end = end.to<String>();
    if (json.get(allDay, base + "allDay/booleanValue"))
      event.allDay = allDay.to<bool>();
    incoming.events.push_back(std::move(event));
  }

  out = std::move(incoming);
  _serial->printf("CALENDAR: %u evento(s)%s.\n",
                  (unsigned)out.events.size(),
                  out.hasMore ? " (ha mais)" : "");
  return true;
}

// Consulta pequena: apenas o dia/mes da data selecionada, birthdays/MM-DD.
// A ausencia do documento significa que nao ha aniversarios naquele dia.
bool FirebaseManager::readBirthdaysForDay(const char *isoDate,
                                          std::vector<CalendarBirthday> &out)
{
  if (!isoDate || strlen(isoDate) != 10 || isoDate[4] != '-' || isoDate[7] != '-')
    return false;
  const int month = String(isoDate).substring(5, 7).toInt();
  const int day = String(isoDate).substring(8, 10).toInt();
  if (month < 1 || month > 12 || day < 1 || day > 31)
    return false;
  if (WiFi.status() != WL_CONNECTED || !awaitReady(5000))
    return false;

  char key[6];
  snprintf(key, sizeof(key), "%02d-%02d", month, day);
  const String path = String("birthdays/") + key;
  _serial->printf("BIRTHDAYS: consultando %s\n", path.c_str());
  if (!Firebase.Firestore.getDocument(
          &_fbdo, _projectId.c_str(), "", path.c_str(), "names"))
  {
    if (_fbdo.httpCode() == 404)
    {
      out.clear();
      _serial->println("BIRTHDAYS: nenhum aniversario nesta data.");
      return true;
    }
    _serial->printf("BIRTHDAYS: erro HTTP %d: %s\n",
                    _fbdo.httpCode(), _fbdo.errorReason().c_str());
    return false;
  }

  FirebaseJson json;
  if (!json.setJsonData(_fbdo.payload()))
  {
    _serial->println("BIRTHDAYS: JSON invalido.");
    return false;
  }
  FirebaseJsonData field;
  if (!json.get(field, "fields/names/arrayValue"))
  {
    _serial->println("BIRTHDAYS: campo names ausente no documento.");
    return false;
  }

  std::vector<CalendarBirthday> incoming;
  for (uint8_t i = 0; i < 32; ++i)
  {
    const String fieldPath = String("fields/names/arrayValue/values/[") +
                             String(i) + "]/stringValue";
    FirebaseJsonData value;
    if (!json.get(value, fieldPath))
      break;
    CalendarBirthday birthday;
    birthday.name = value.to<String>();
    birthday.month = static_cast<uint8_t>(month);
    birthday.day = static_cast<uint8_t>(day);
    incoming.push_back(std::move(birthday));
  }
  out.swap(incoming);
  _serial->printf("BIRTHDAYS: %u aniversario(s) em %s.\n",
                  (unsigned)out.size(), key);
  return true;
}

// Consulta estruturada testada com sucesso: 1 documento por habito,
// ordenado pelo campo "date" DESCENDING.
bool FirebaseManager::readLastCompletionDate(const String &habitId,
                                             String &outDate, String &outPrevious)
{
  outDate = "";
  outPrevious = "";
  String parentPath = "habits/";
  parentPath += habitId;

  FirebaseJson query;
  query.set("from/collectionId", "completions");
  query.set("from/allDescendants", false);
  query.set("select/fields/[0]/fieldPath", "date");
  query.set("orderBy/field/fieldPath", "date");
  query.set("orderBy/direction", "DESCENDING");
  query.set("limit", 2);

  if (!Firebase.Firestore.runQuery(
          &_fbdo, _projectId.c_str(), "", parentPath.c_str(), &query))
  {
    _serial->print("COMPLETIONS: erro no habito ");
    _serial->println(habitId);
    _serial->println(_fbdo.errorReason());
    return false;
  }

  FirebaseJsonArray results;
  if (!results.setJsonArrayData(_fbdo.payload()))
  {
    _serial->println("COMPLETIONS: resposta inesperada.");
    return false;
  }

  if (results.size() == 0)
    return true;

  // Ultimas duas datas: se hoje for desmarcado, a tela recupera
  // imediatamente a conclusao anterior, mesmo sem acesso a rede.
  for (int i = 0; i < 2 && i < static_cast<int>(results.size()); ++i)
  {
    FirebaseJsonData rowData;
    if (!results.get(rowData, i))
      return false;
    FirebaseJson row;
    if (!rowData.getJSON(row))
      return false;
    FirebaseJsonData dateData;
    if (!row.get(dateData, "document/fields/date/stringValue"))
      continue;
    const String dateValue = dateData.to<String>();
    if (i == 0)
      outDate = dateValue;
    else
      outPrevious = dateValue;
  }

  return true;
}

bool FirebaseManager::syncHabits()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    _serial->println("HABITS: sem conexao Wi-Fi.");
    return false;
  }
  _serial->println("HABITS: buscando habits no Firestore...");

  std::vector<Habit> incoming;
  String pageToken;

  do
  {
    if (!Firebase.Firestore.listDocuments(
            &_fbdo, _projectId.c_str(), "", "habits",
            10, pageToken.c_str(), "", "", false))
    {
      _serial->print("HABITS: erro: ");
      _serial->println(_fbdo.errorReason());
      return false;
    }

    String nextToken;
    // Escopo dedicado: libera JSON da listagem ANTES das consultas completions.
    {
      FirebaseJson response;
      if (!response.setJsonData(_fbdo.payload()))
      {
        _serial->println("HABITS: resposta JSON invalida.");
        return false;
      }

      FirebaseJsonData tokenData;
      if (response.get(tokenData, "nextPageToken"))
        nextToken = tokenData.to<String>();

      // Nao materializar o array completo em FirebaseJsonArray: a resposta
      // principal ja esta em 'response'. Duplicar todos os documentos
      // (incluindo icones Base64) pode causar mais um pico de RAM.
      bool hasDocuments = false;
      bool emptyDocuments = false;
      {
        FirebaseJsonData docsInfo;
        hasDocuments = response.get(docsInfo, "documents");
        if (hasDocuments)
        {
          if (docsInfo.type != "array")
          {
            _serial->printf("HABITS: documents tipo inesperado: %s.\n",
                            docsInfo.type.c_str());
            return false;
          }
          emptyDocuments = docsInfo.stringValue == "[]";
        }
      } // docsInfo deixa de segurar a representacao inteira da lista.

      if (hasDocuments)
      {
        // listDocuments foi chamado com pageSize=10. Acessamos cada item
        // diretamente, sem copiar o array todo para um segundo objeto JSON.
        for (size_t i = 0; i < 10; ++i)
        {
          FirebaseJsonData item;
          const String path = "documents/[" + String(i) + "]";
          if (!response.get(item, path))
          {
            if (i == 0 && !emptyDocuments)
            {
              _serial->println("HABITS: falha ao ler documents/[0].");
              _serial->printf("HABITS MEM: heap=%lu | maior bloco=%lu\n",
                              (unsigned long)ESP.getFreeHeap(),
                              (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
              return false;
            }
            break; // Fim dos documentos da pagina.
          }
          FirebaseJson doc;
          if (!item.getJSON(doc))
          {
            _serial->printf("HABITS: falha ao converter documento %u.\n",
                            (unsigned)i);
            return false;
          }

          FirebaseJsonData documentName;
          if (!doc.get(documentName, "name"))
          {
            _serial->printf("HABITS: documento %u sem nome.\n", (unsigned)i);
            return false;
          }

          const String fullName = documentName.to<String>();
          const int slash = fullName.lastIndexOf('/');

          Habit habit;
          habit.id = (slash >= 0) ? fullName.substring(slash + 1) : fullName;

          FirebaseJsonData nameData, positionData;
          if (doc.get(nameData, "fields/name/stringValue"))
            habit.name = nameData.to<String>();
          if (doc.get(positionData, "fields/position/integerValue"))
            habit.position = positionData.to<int>();

          FirebaseJsonData iconData;
          if (doc.get(iconData, "fields/icon/mapValue/fields/data/stringValue"))
          {
            const String base64 = iconData.to<String>();
            if (!base64.isEmpty())
            {
              FirebaseJsonData formatData, widthData, heightData;
              const bool validFormat =
                  doc.get(formatData, "fields/icon/mapValue/fields/format/stringValue") &&
                  formatData.to<String>() == "gray2bpp";
              const bool validWidth =
                  doc.get(widthData, "fields/icon/mapValue/fields/width/integerValue");
              const bool validHeight =
                  doc.get(heightData, "fields/icon/mapValue/fields/height/integerValue");

              if (validFormat && validWidth && validHeight)
              {
                habit.iconWidth = static_cast<uint16_t>(widthData.to<int>());
                habit.iconHeight = static_cast<uint16_t>(heightData.to<int>());
                if (!decodeIcon(base64, habit))
                {
                  _serial->print("HABITS: icone invalido em ");
                  _serial->println(habit.id);
                }
              }
              else
              {
                _serial->print("HABITS: formato de icone nao suportado em ");
                _serial->println(habit.id);
              }
            }
          }

          incoming.push_back(std::move(habit));
        }
      }
    }

    if (nextToken.length() > 0 && nextToken == pageToken)
    {
      _serial->println("HABITS: paginacao repetida.");
      return false;
    }
    pageToken = nextToken;
  } while (!pageToken.isEmpty());

  // Ordenacao do aplicativo; em empate usa ID para ser deterministico.
  std::sort(incoming.begin(), incoming.end(),
            [](const Habit &a, const Habit &b)
            {
              if (a.position != b.position)
                return a.position < b.position;
              return a.id.compareTo(b.id) < 0;
            });

  // Só executa novas consultas depois de liberar o JSON principal.
  for (Habit &habit : incoming)
  {
    if (!readLastCompletionDate(habit.id, habit.lastCompletedDate,
                                habit.previousCompletedDate))
    {
      _serial->println("HABITS: sincronizacao incompleta; dados anteriores preservados.");
      return false;
    }
  }

  // Transferencia dos icones para setores proprios da W25Q16.
  // A leitura do Firestore e das completions ja terminou: evitamos perder
  // a lista anterior se uma consulta de rede falhar no meio.
  // Dois bancos por slot protegem icones ainda usados por _habits.
  if (_iconFlash.begin())
  {
    for (size_t i = 0; i < incoming.size() && i < FlashManager::MAX_ICON_SLOTS; ++i)
    {
      Habit &habit = incoming[i];
      if (habit.iconWidth != 48 || habit.iconHeight != 48 ||
          habit.iconBytes.size() != FlashManager::ICON_BYTES ||
          habit.iconHash == 0)
        continue;

      int8_t protectedBank = -1;
      for (const Habit &old : _habits)
      {
        if (old.iconFlashSlot == static_cast<int8_t>(i))
        {
          protectedBank = old.iconFlashBank;
          break;
        }
      }
      int8_t bank = -1;
      if (_iconFlash.storeIcon(static_cast<uint8_t>(i), habit.iconHash,
                               habit.iconBytes.data(),
                               static_cast<uint16_t>(habit.iconBytes.size()),
                               protectedBank, bank))
      {
        habit.iconFlashSlot = static_cast<int8_t>(i);
        habit.iconFlashBank = bank;
        // swap() libera a capacidade do vector (clear() nao garante).
        std::vector<uint8_t>().swap(habit.iconBytes);
      }
      else
      {
        _serial->printf("HABITS: icone %s ficou em RAM (falha Flash).\n",
                        habit.id.c_str());
      }
    }
  }
  else
  {
    _serial->println("HABITS: Flash externa ausente; icones mantidos em RAM.");
  }

  // Troca atomica do conjunto de dados em RAM, apenas no sucesso.
  _habits.swap(incoming);

  _serial->println();
  _serial->print("HABITS: sincronizacao OK. Total: ");
  _serial->println(_habits.size());
  for (const Habit &habit : _habits)
  {
    _serial->printf("  [%d] %s (ID: %s) | ultima: %s | icone RAM: %u bytes | flash: %d/%d\n",
                    habit.position, habit.name.c_str(), habit.id.c_str(),
                    habit.lastCompletedDate.isEmpty() ? "nunca" : habit.lastCompletedDate.c_str(),
                    static_cast<unsigned>(habit.iconBytes.size()),
                    static_cast<int>(habit.iconFlashSlot),
                    static_cast<int>(habit.iconFlashBank));
  }
  _serial->printf("HABITS: heap livre: %u bytes\n", ESP.getFreeHeap());
  return true;
}

// Atualiza somente habits/{id}/completions/{YYYY-MM-DD}.
// O Firestore continua sendo a fonte de verdade; syncHabits() recarrega depois.
bool FirebaseManager::setHabitCompletedToday(const String &habitId,
                                             const String &todayISO,
                                             bool completed)
{
  if (WiFi.status() != WL_CONNECTED)
  {
    _serial->println("HABIT TOUCH: sem Wi-Fi; nada foi alterado.");
    return false;
  }
  if (habitId.isEmpty() || todayISO.length() != 10 ||
      todayISO[4] != '-' || todayISO[7] != '-')
  {
    _serial->println("HABIT TOUCH: ID ou data invalida.");
    return false;
  }
  for (int i = 0; i < 10; ++i)
  {
    if (i == 4 || i == 7)
      continue;
    if (todayISO[i] < '0' || todayISO[i] > '9')
      return false;
  }
  if (!awaitReady(5000))
    return false;

  const String path = "habits/" + habitId + "/completions/" + todayISO;
  if (!completed)
  {
    _serial->printf("HABIT TOUCH: removendo conclusao em %s\n", path.c_str());
    if (!Firebase.Firestore.deleteDocument(
            &_fbdo, _projectId.c_str(), "", path.c_str()))
    {
      _serial->print("HABIT TOUCH: erro ao remover: ");
      _serial->println(_fbdo.errorReason());
      return false;
    }
    return true;
  }

  time_t now = time(nullptr);
  if (now < 1700000000)
  {
    _serial->println("HABIT TOUCH: relogio ainda nao sincronizado.");
    return false;
  }
  struct tm utc{};
  gmtime_r(&now, &utc);
  char timestamp[25];
  if (strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &utc) == 0)
    return false;

  FirebaseJson doc;
  doc.set("fields/date/stringValue", todayISO);
  doc.set("fields/completedAt/timestampValue", timestamp);
  _serial->printf("HABIT TOUCH: criando conclusao em %s\n", path.c_str());
  if (!Firebase.Firestore.createDocument(
          &_fbdo, _projectId.c_str(), "", path.c_str(), doc.raw()))
  {
    _serial->print("HABIT TOUCH: erro ao criar: ");
    _serial->println(_fbdo.errorReason());
    return false;
  }
  return true;
}

// Muda apenas a representacao em RAM. Chamado somente pela task loop,
// nunca pela task de rede (que usa _fbdo exclusivamente).
bool FirebaseManager::setLocalHabitCompletedToday(
    const String &habitId, const String &todayISO, bool completed)
{
  for (Habit &habit : _habits)
  {
    if (habit.id != habitId)
      continue;
    if (completed)
    {
      if (habit.lastCompletedDate != todayISO)
      {
        habit.previousCompletedDate = habit.lastCompletedDate;
        habit.lastCompletedDate = todayISO;
      }
    }
    else
    {
      if (habit.lastCompletedDate == todayISO)
      {
        habit.lastCompletedDate = habit.previousCompletedDate;
        habit.previousCompletedDate = "";
      }
    }
    return true;
  }
  return false;
}

bool FirebaseManager::restoreLocalHabitDates(
    const String &habitId, const String &lastISO, const String &previousISO)
{
  for (Habit &habit : _habits)
  {
    if (habit.id != habitId)
      continue;
    habit.lastCompletedDate = lastISO;
    habit.previousCompletedDate = previousISO;
    return true;
  }
  return false;
}
