
#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <time.h>

#include "Secrets.h"
#include "FirebaseManager.h"
#include "WiFiManager.h"
#include "TouchManager.h"
#include "DisplayManager.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <sys/time.h>
#include "HabitTouchAsync.h"

SET_LOOP_TASK_STACK_SIZE(24 * 1024);

WiFiManager wifiManager;
FirebaseManager firebaseManager;
DisplayManager displayManager;
TouchManager touchManager;
HabitTouchAsync habitTouchAsync(firebaseManager, displayManager);

// ============================================================
// SINCRONIZACAO DO RELOGIO
// ============================================================

time_t utcToTimestamp(const tm &t)
{
  const int year = t.tm_year + 1900;

  auto isLeap = [](int y)
  {
    return (y % 4 == 0 && y % 100 != 0) ||
           (y % 400 == 0);
  };

  const int daysInMonth[] = {
      31, 28, 31, 30, 31, 30,
      31, 31, 30, 31, 30, 31};

  if (year < 1970 || year > 2099 ||
      t.tm_mon < 0 || t.tm_mon > 11 ||
      t.tm_mday < 1 ||
      t.tm_hour < 0 || t.tm_hour > 23 ||
      t.tm_min < 0 || t.tm_min > 59 ||
      t.tm_sec < 0 || t.tm_sec > 59)
    return -1;

  int maxDay = daysInMonth[t.tm_mon] +
               (t.tm_mon == 1 && isLeap(year));

  if (t.tm_mday > maxDay)
    return -1;

  int64_t days = 0;

  for (int y = 1970; y < year; y++)
    days += isLeap(y) ? 366 : 365;

  for (int m = 0; m < t.tm_mon; m++)
    days += daysInMonth[m] +
            (m == 1 && isLeap(year));

  days += t.tm_mday - 1;

  return static_cast<time_t>(
      days * 86400LL +
      t.tm_hour * 3600LL +
      t.tm_min * 60LL +
      t.tm_sec);
}

bool syncClockHTTPS()
{
  Serial.println("CLOCK HTTPS: tentando sincronizar...");

  const char *endpoints[] = {
      "https://www.google.com/generate_204",
      "https://www.cloudflare.com/cdn-cgi/trace"};

  for (const char *endpoint : endpoints)
  {
    WiFiClientSecure client;

    // Somente para obter a data inicial.
    // A autenticidade TLS nao e verificada neste fallback.
    client.setInsecure();
    client.setTimeout(8);

    HTTPClient http;
    http.setConnectTimeout(8000);
    http.setTimeout(8000);

    if (!http.begin(client, endpoint))
    {
      Serial.println("CLOCK HTTPS: falha ao iniciar.");
      continue;
    }

    const char *headers[] = {"Date"};
    http.collectHeaders(headers, 1);

    int httpCode = http.GET();

    Serial.printf("CLOCK HTTPS: HTTP %d\n", httpCode);

    String dateHeader;

    if (httpCode > 0)
      dateHeader = http.header("Date");

    http.end();

    Serial.print("CLOCK HTTPS: Date = ");
    Serial.println(dateHeader);

    if (dateHeader.isEmpty())
      continue;

    dateHeader.trim();

    struct tm utcTime = {};

    char *end = strptime(
        dateHeader.c_str(),
        "%a, %d %b %Y %H:%M:%S GMT",
        &utcTime);

    if (!end || *end != '\0')
    {
      Serial.println("CLOCK HTTPS: data invalida.");
      continue;
    }

    time_t timestamp = utcToTimestamp(utcTime);

    // Rejeitar datas claramente incorretas.
    if (utcTime.tm_year + 1900 < 2024 ||
        timestamp <= 0)
    {
      Serial.println("CLOCK HTTPS: timestamp invalido.");
      continue;
    }

    struct timeval tv = {};
    tv.tv_sec = timestamp;
    tv.tv_usec = 0;

    if (settimeofday(&tv, nullptr) != 0)
    {
      Serial.println("CLOCK HTTPS: erro ao ajustar relogio.");
      continue;
    }

    Serial.println("CLOCK HTTPS: sincronizado!");

    return true;
  }

  Serial.println("CLOCK HTTPS: todas as tentativas falharam.");
  return false;
}

bool syncClock()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("CLOCK: Wi-Fi desconectado.");
    return false;
  }

  Serial.println();
  Serial.println("CLOCK: iniciando sincronizacao NTP...");

  configTzTime(
      "BRT3",
      "pool.ntp.org",
      "time.google.com",
      "time.nist.gov");

  struct tm timeInfo = {};

  if (getLocalTime(&timeInfo, 15000))
  {
    Serial.println("CLOCK: sincronizado por NTP.");
    Serial.println(&timeInfo, "%d/%m/%Y %H:%M:%S");
    return true;
  }

  Serial.println("CLOCK: NTP falhou. Tentando HTTPS...");

  if (!syncClockHTTPS())
  {
    Serial.println("CLOCK: nao foi possivel obter horario.");
    return false;
  }

  if (!getLocalTime(&timeInfo, 1000))
  {
    Serial.println("CLOCK: horario HTTPS nao validado.");
    return false;
  }

  Serial.println("CLOCK: horario local atualizado.");
  Serial.println(&timeInfo, "%d/%m/%Y %H:%M:%S");

  return true;
}

// ============================================================
// SINCRONIZACAO DOS DADOS E ATUALIZACAO DO DISPLAY
// ============================================================

bool syncDashboard()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("EINKDASH: Wi-Fi desconectado.");
    return false;
  }

  Serial.println();
  Serial.println("=== EINKDASH: SINCRONIZACAO ===");

  // ==========================================================
  // HABITOS
  // ==========================================================

  Serial.println();
  Serial.println("EINKDASH: sincronizando habitos...");

  if (!firebaseManager.syncHabits())
  {
    Serial.println("EINKDASH: erro ao sincronizar habitos.");
    return false;
  }

  // ==========================================================
  // PREPARAR DADOS DO DASHBOARD
  // ==========================================================

  DashboardData dashboard;

  dashboard.habits = &firebaseManager.getHabits();

  // ==========================================================
  // DATA ATUAL
  // ==========================================================

  struct tm now;

  if (!getLocalTime(&now, 1000))
  {
    Serial.println("EINKDASH: nao foi possivel obter a data atual.");
    return false;
  }

  char iso[11];

  strftime(
      iso,
      sizeof(iso),
      "%Y-%m-%d",
      &now);

  dashboard.todayISO = iso;

  Serial.print("EINKDASH: data atual = ");
  Serial.println(dashboard.todayISO);

  // ==========================================================
  // FUTURAS INTEGRACOES
  // ==========================================================

  // Tempo de tela (celular e computador)
  // Previsao meteorologica
  // Google Calendar
  // Sensores ambientais

  // ==========================================================
  // ATUALIZAR DISPLAY UMA UNICA VEZ
  // ==========================================================

  Serial.println();
  Serial.println("EINKDASH: atualizando display...");

  displayManager.render(dashboard);

  Serial.println("EINKDASH: sincronizacao concluida.");

  return true;
}

// ============================================================
// AUTENTICACAO FIREBASE
// ============================================================

bool authenticateFirebase()
{
  Preferences prefs;

  prefs.begin("firebase", true);

  bool hasToken =
      prefs.getString("refreshToken", "").length() > 0;

  prefs.end();

  if (hasToken)
  {
    Serial.println("FIREBASE: restaurando sessao existente...");

    return firebaseManager.authenticate();
  }

#ifdef EINKDASH_DEV_DIRECT_WIFI

  Serial.println("FIREBASE: criando sessao anonima DEV...");

  String uid;

  if (!firebaseManager.createAnonymousSession(uid))
  {
    Serial.println("FIREBASE: erro ao criar sessao.");
    return false;
  }

  Serial.println("FIREBASE: sessao criada com sucesso.");

  Serial.print("Firebase UID: ");
  Serial.println(uid);

  return true;

#else

  Serial.println("FIREBASE: nenhuma sessao encontrada.");
  Serial.println("Configure o dispositivo via Bluetooth.");

  return false;

#endif
}

// ============================================================
// CONFIGURACAO WI-FI VIA BLUETOOTH
// ============================================================

// Futuramente chamada pelo touch.
// Durante o desenvolvimento, pode ser acionada pelo Serial.

void onTouchConfigureWiFi()
{
  Serial.println();
  Serial.println("EINKDASH: configuracao solicitada pelo usuario.");

  firebaseManager.begin(
      FIREBASE_API_KEY,
      FIREBASE_PROJECT_ID,
      Serial);

  if (!wifiManager.startConfiguration())
  {
    Serial.println("EINKDASH: configuracao nao concluida.");
    return;
  }

  // O provisioning encerra a conexao Wi-Fi para
  // iniciar o BLE de status.
  // Reconectamos antes de acessar o Firestore.

  if (!wifiManager.connectToSavedWiFi())
  {
    Serial.println("EINKDASH: Wi-Fi nao reconectou.");
    return;
  }

  // ==========================================================
  // RELOGIO
  // ==========================================================

  if (!syncClock())
  {
    Serial.println("EINKDASH: falha na sincronizacao do relogio.");
    return;
  }

  // ==========================================================
  // FIREBASE
  // ==========================================================

  if (!firebaseManager.authenticate())
  {
    Serial.println("EINKDASH: falha ao restaurar sessao Firebase.");
    return;
  }

  // ==========================================================
  // SINCRONIZAR E ATUALIZAR DISPLAY
  // ==========================================================

  syncDashboard();
}

// ============================================================
// SETUP
// ============================================================

void setup()
{
  Serial.begin(115200);
  delay(1000);

  Serial.printf(
      "Stack configurada: %u bytes\n",
      (unsigned)getArduinoLoopTaskStackSize());

  Serial.printf(
      "Stack livre minima: %u bytes\n",
      (unsigned)uxTaskGetStackHighWaterMark(NULL));

  Serial.printf(
      "Heap livre: %u bytes\n",
      ESP.getFreeHeap());

  Serial.println();
  Serial.println("=== EINKDASH ===");

  wifiManager.setFirebaseManager(firebaseManager);

  firebaseManager.begin(
      FIREBASE_API_KEY,
      FIREBASE_PROJECT_ID,
      Serial);

  // ==========================================================
  // CONEXAO WI-FI
  // ==========================================================

  bool wifiConnected = false;

#ifdef EINKDASH_DEV_DIRECT_WIFI

  Serial.println("DEV: conectando diretamente ao Wi-Fi...");

  WiFi.mode(WIFI_STA);

  WiFi.begin(
      DEV_WIFI_SSID,
      DEV_WIFI_PASSWORD);

  unsigned long start = millis();

  while (
      WiFi.status() != WL_CONNECTED &&
      millis() - start < 20000)
  {
    delay(500);
    Serial.print(".");
  }

  wifiConnected = WiFi.status() == WL_CONNECTED;

#else

  wifiConnected = wifiManager.connectToSavedWiFi();

#endif

  if (!wifiConnected)
  {
    Serial.println();
    Serial.println("ERRO: Wi-Fi indisponivel.");
    return;
  }

  Serial.println();
  Serial.println("Wi-Fi conectado!");

  Serial.print("IP: ");
  Serial.println(WiFi.localIP());

  Serial.print("DEVICE ID (MAC): ");
  Serial.println(WiFi.macAddress());

  // ==========================================================
  // SINCRONIZAR RELOGIO
  // ==========================================================

  if (!syncClock())
  {
    Serial.println("EINKDASH: falha na sincronizacao do relogio.");
    return;
  }

  // ==========================================================
  // AUTENTICACAO FIREBASE
  // ==========================================================

  if (!authenticateFirebase())
  {
    Serial.println("EINKDASH: falha na autenticacao Firebase.");
    return;
  }

  Serial.println("EINKDASH: Firebase autenticado.");

  Serial.printf(
      "Stack livre minima apos Firebase: %u bytes\n",
      (unsigned)uxTaskGetStackHighWaterMark(NULL));

  Serial.printf(
      "Heap livre apos Firebase: %u bytes\n",
      ESP.getFreeHeap());

  // ==========================================================
  // SINCRONIZAR DADOS E ATUALIZAR DISPLAY
  // ==========================================================

  syncDashboard();

  // Usar 21/22 somente se corresponderem ao touch da sua placa.
  if (touchManager.begin(32, 33))
  {
    touchManager.setTransform(false, false, false);
    Serial.println("TOUCH: pronto para testes de coordenadas.");
  }

  if (!habitTouchAsync.begin())
  {
    Serial.println("TOUCH ASYNC: erro! Gravacao por toque desativada.");
  }
}

// ============================================================
// LOOP
// ============================================================

void loop()
{
#ifdef EINKDASH_DEV_SERIAL_PROVISIONING

  if (Serial.available())
  {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();

    // ========================================================
    // CONFIGURAR WI-FI
    // ========================================================

    if (cmd == "wifi")
    {
      if (habitTouchAsync.hasPending())
      {
        Serial.println("WIFI: aguarde a conclusao das gravacoes de habitos.");
      }
      else
      {
        onTouchConfigureWiFi();
      }
    }

    // ========================================================
    // SINCRONIZACAO MANUAL
    // ========================================================

    else if (cmd == "sync")
    {
      if (habitTouchAsync.hasPending())
      {
        Serial.println("SYNC: aguarde a conclusao das gravacoes de habitos.");
      }
      else if (WiFi.status() != WL_CONNECTED)
      {
        Serial.println("EINKDASH: Wi-Fi desconectado.");
      }
      else
      {
        Serial.println("EINKDASH: sincronizacao manual...");
        if (syncClock())
          syncDashboard();
      }
    }
  }

#endif

  habitTouchAsync.poll(); // processar confirmacoes/falhas do Firebase
  TouchPointEink touch;
  if (touchManager.readTap(touch))
  {
    habitTouchAsync.handleTap(touch);
  }

  delay(50);
}
