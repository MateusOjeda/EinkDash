#include "FirebaseManager.h"

#include <WiFi.h>
#include <algorithm>
#include <utility>
#include <mbedtls/base64.h>

void FirebaseManager::begin(const char* apiKey,
                            const char* projectId,
                            Stream& serial) {
  _projectId = projectId;
  _serial = &serial;
  _config.api_key = apiKey;
}

bool FirebaseManager::awaitReady(uint32_t timeoutMs) {
  const uint32_t startedAt = millis();
  while (!Firebase.ready()) {
    if (millis() - startedAt >= timeoutMs) {
      _serial->println("FIREBASE: timeout aguardando Firebase.ready().");
      return false;
    }
    delay(100);
  }
  return true;
}

bool FirebaseManager::createAnonymousSession(String& uid) {
  uid = "";
  _serial->println("FIREBASE: criando sessao anonima...");
  Firebase.reconnectNetwork(true);

  if (!Firebase.signUp(&_config, &_auth, "", "")) {
    _serial->print("FIREBASE: falha no cadastro anonimo: ");
    _serial->println(_config.signer.signupError.message.c_str());
    return false;
  }

  uid = _auth.token.uid.c_str();
  Firebase.begin(&_config, &_auth);
  if (!awaitReady()) return false;

  const char* refreshToken = Firebase.getRefreshToken();
  if (!refreshToken || !strlen(refreshToken)) {
    _serial->println("FIREBASE: refresh token vazio.");
    return false;
  }

  if (!_preferences.begin("firebase", false)) {
    _serial->println("FIREBASE: nao foi possivel abrir NVS.");
    return false;
  }
  const size_t saved = _preferences.putString("refreshToken", refreshToken);
  _preferences.end();
  if (!saved) {
    _serial->println("FIREBASE: falha ao salvar refresh token na NVS.");
    return false;
  }
  _serial->println("FIREBASE: token persistido na NVS (conteudo omitido).");
  return true;
}

bool FirebaseManager::authenticate() {
  if (!_preferences.begin("firebase", true)) {
    _serial->println("FIREBASE: nao foi possivel abrir NVS.");
    return false;
  }
  const String refreshToken = _preferences.getString("refreshToken", "");
  _preferences.end();

  if (refreshToken.isEmpty()) {
    _serial->println("FIREBASE: sessao ausente. Configure pelo app uma vez.");
    return false;
  }

  Firebase.reconnectNetwork(true);
  Firebase.setIdToken(&_config, "", 3600, refreshToken.c_str());
  Firebase.begin(&_config, &_auth);
  if (!awaitReady()) return false;

  _serial->println("FIREBASE: autenticado com refresh token da NVS.");
  return true;
}

// Converte o Base64 do aplicativo diretamente para bytes gray2bpp.
// 48 x 48 -> 576 bytes; nao guarda a string Base64 depois de ler o documento.
bool FirebaseManager::decodeIcon(const String& base64, Habit& habit) {
  if (habit.iconWidth == 0 || habit.iconHeight == 0 ||
      habit.iconWidth > 128 || habit.iconHeight > 128) {
    return false;
  }

  const size_t pixelCount = static_cast<size_t>(habit.iconWidth) * habit.iconHeight;
  const size_t expectedBytes = (pixelCount + 3) / 4;

  habit.iconBytes.resize(expectedBytes);
  size_t decodedBytes = 0;

  const int result = mbedtls_base64_decode(
      habit.iconBytes.data(), habit.iconBytes.size(), &decodedBytes,
      reinterpret_cast<const unsigned char*>(base64.c_str()), base64.length());

  if (result != 0 || decodedBytes != expectedBytes) {
    habit.iconBytes.clear();
    habit.iconWidth = 0;
    habit.iconHeight = 0;
    return false;
  }
  return true;
}

// Consulta estruturada testada com sucesso: 1 documento por habito,
// ordenado pelo campo "date" DESCENDING.
bool FirebaseManager::readLastCompletionDate(const String& habitId,
                                             String& outDate, String& outPrevious) {
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
          &_fbdo, _projectId.c_str(), "", parentPath.c_str(), &query)) {
    _serial->print("COMPLETIONS: erro no habito ");
    _serial->println(habitId);
    _serial->println(_fbdo.errorReason());
    return false;
  }

  FirebaseJsonArray results;
  if (!results.setJsonArrayData(_fbdo.payload())) {
    _serial->println("COMPLETIONS: resposta inesperada.");
    return false;
  }

  if (results.size() == 0) return true;

  // Ultimas duas datas: se hoje for desmarcado, a tela recupera
  // imediatamente a conclusao anterior, mesmo sem acesso a rede.
  for (int i = 0; i < 2 && i < static_cast<int>(results.size()); ++i) {
    FirebaseJsonData rowData;
    if (!results.get(rowData, i)) return false;
    FirebaseJson row;
    if (!rowData.getJSON(row)) return false;
    FirebaseJsonData dateData;
    if (!row.get(dateData, "document/fields/date/stringValue")) continue;
    const String dateValue = dateData.to<String>();
    if (i == 0) outDate = dateValue;
    else outPrevious = dateValue;
  }

  return true;
}

bool FirebaseManager::syncHabits() {
  if (WiFi.status() != WL_CONNECTED) {
    _serial->println("HABITS: sem conexao Wi-Fi.");
    return false;
  }
  _serial->println("HABITS: buscando habits no Firestore...");

  std::vector<Habit> incoming;
  String pageToken;

  do {
    if (!Firebase.Firestore.listDocuments(
            &_fbdo, _projectId.c_str(), "", "habits",
            10, pageToken.c_str(), "", "", false)) {
      _serial->print("HABITS: erro: ");
      _serial->println(_fbdo.errorReason());
      return false;
    }

    String nextToken;
    // Escopo dedicado: libera JSON da listagem ANTES das consultas completions.
    {
      FirebaseJson response;
      if (!response.setJsonData(_fbdo.payload())) {
        _serial->println("HABITS: resposta JSON invalida.");
        return false;
      }

      FirebaseJsonData tokenData;
      if (response.get(tokenData, "nextPageToken"))
        nextToken = tokenData.to<String>();

      FirebaseJsonData docsData;
      if (response.get(docsData, "documents")) {
        FirebaseJsonArray docs;
        if (!docsData.getArray(docs)) {
          _serial->println("HABITS: documents invalido.");
          return false;
        }

        for (size_t i = 0; i < docs.size(); ++i) {
          FirebaseJsonData item;
          if (!docs.get(item, i)) continue;
          FirebaseJson doc;
          if (!item.getJSON(doc)) continue;

          FirebaseJsonData documentName;
          if (!doc.get(documentName, "name")) continue;

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
          if (doc.get(iconData, "fields/icon/mapValue/fields/data/stringValue")) {
            const String base64 = iconData.to<String>();
            if (!base64.isEmpty()) {
              FirebaseJsonData formatData, widthData, heightData;
              const bool validFormat =
                  doc.get(formatData, "fields/icon/mapValue/fields/format/stringValue") &&
                  formatData.to<String>() == "gray2bpp";
              const bool validWidth =
                  doc.get(widthData, "fields/icon/mapValue/fields/width/integerValue");
              const bool validHeight =
                  doc.get(heightData, "fields/icon/mapValue/fields/height/integerValue");

              if (validFormat && validWidth && validHeight) {
                habit.iconWidth = static_cast<uint16_t>(widthData.to<int>());
                habit.iconHeight = static_cast<uint16_t>(heightData.to<int>());
                if (!decodeIcon(base64, habit)) {
                  _serial->print("HABITS: icone invalido em ");
                  _serial->println(habit.id);
                }
              } else {
                _serial->print("HABITS: formato de icone nao suportado em ");
                _serial->println(habit.id);
              }
            }
          }

          incoming.push_back(std::move(habit));
        }
      }
    }

    if (nextToken.length() > 0 && nextToken == pageToken) {
      _serial->println("HABITS: paginacao repetida.");
      return false;
    }
    pageToken = nextToken;
  } while (!pageToken.isEmpty());

  // Ordenacao do aplicativo; em empate usa ID para ser deterministico.
  std::sort(incoming.begin(), incoming.end(),
            [](const Habit& a, const Habit& b) {
              if (a.position != b.position) return a.position < b.position;
              return a.id.compareTo(b.id) < 0;
            });

  // Só executa novas consultas depois de liberar o JSON principal.
  for (Habit& habit : incoming) {
    if (!readLastCompletionDate(habit.id, habit.lastCompletedDate,
                                habit.previousCompletedDate)) {
      _serial->println("HABITS: sincronizacao incompleta; dados anteriores preservados.");
      return false;
    }
  }

  // Troca atomica do conjunto de dados em RAM, apenas no sucesso.
  _habits.swap(incoming);

  _serial->println();
  _serial->print("HABITS: sincronizacao OK. Total: ");
  _serial->println(_habits.size());
  for (const Habit& habit : _habits) {
    _serial->printf("  [%d] %s (ID: %s) | ultima: %s | icone: %u bytes\n",
                    habit.position, habit.name.c_str(), habit.id.c_str(),
                    habit.lastCompletedDate.isEmpty() ? "nunca" : habit.lastCompletedDate.c_str(),
                    static_cast<unsigned>(habit.iconBytes.size()));
  }
  _serial->printf("HABITS: heap livre: %u bytes\n", ESP.getFreeHeap());
  return true;
}

// Atualiza somente habits/{id}/completions/{YYYY-MM-DD}.
// O Firestore continua sendo a fonte de verdade; syncHabits() recarrega depois.
bool FirebaseManager::setHabitCompletedToday(const String& habitId,
                                              const String& todayISO,
                                              bool completed) {
  if (WiFi.status() != WL_CONNECTED) {
    _serial->println("HABIT TOUCH: sem Wi-Fi; nada foi alterado.");
    return false;
  }
  if (habitId.isEmpty() || todayISO.length() != 10 ||
      todayISO[4] != '-' || todayISO[7] != '-') {
    _serial->println("HABIT TOUCH: ID ou data invalida.");
    return false;
  }
  for (int i = 0; i < 10; ++i) {
    if (i == 4 || i == 7) continue;
    if (todayISO[i] < '0' || todayISO[i] > '9') return false;
  }
  if (!awaitReady(5000)) return false;

  const String path = "habits/" + habitId + "/completions/" + todayISO;
  if (!completed) {
    _serial->printf("HABIT TOUCH: removendo conclusao em %s\n", path.c_str());
    if (!Firebase.Firestore.deleteDocument(
            &_fbdo, _projectId.c_str(), "", path.c_str())) {
      _serial->print("HABIT TOUCH: erro ao remover: ");
      _serial->println(_fbdo.errorReason());
      return false;
    }
    return true;
  }

  time_t now = time(nullptr);
  if (now < 1700000000) {
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
          &_fbdo, _projectId.c_str(), "", path.c_str(), doc.raw())) {
    _serial->print("HABIT TOUCH: erro ao criar: ");
    _serial->println(_fbdo.errorReason());
    return false;
  }
  return true;
}

// Muda apenas a representacao em RAM. Chamado somente pela task loop,
// nunca pela task de rede (que usa _fbdo exclusivamente).
bool FirebaseManager::setLocalHabitCompletedToday(
    const String& habitId, const String& todayISO, bool completed) {
  for (Habit& habit : _habits) {
    if (habit.id != habitId) continue;
    if (completed) {
      if (habit.lastCompletedDate != todayISO) {
        habit.previousCompletedDate = habit.lastCompletedDate;
        habit.lastCompletedDate = todayISO;
      }
    } else {
      if (habit.lastCompletedDate == todayISO) {
        habit.lastCompletedDate = habit.previousCompletedDate;
        habit.previousCompletedDate = "";
      }
    }
    return true;
  }
  return false;
}

bool FirebaseManager::restoreLocalHabitDates(
    const String& habitId, const String& lastISO, const String& previousISO) {
  for (Habit& habit : _habits) {
    if (habit.id != habitId) continue;
    habit.lastCompletedDate = lastISO;
    habit.previousCompletedDate = previousISO;
    return true;
  }
  return false;
}
