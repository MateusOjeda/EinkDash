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
#include "WeatherManager.h"
#include "AirQualityManager.h"
#include "PollenManager.h"
#include <esp_heap_caps.h>
#include <string.h>
#include <utility>

// MODO TESTE: 1 = dados ficticios APENAS na tela da agenda; 0 = dados reais.
// Nao grava eventos ou aniversarios no Firestore.
#define EINKDASH_CALENDAR_MOCK 1

SET_LOOP_TASK_STACK_SIZE(24 * 1024);

WiFiManager wifiManager;
FirebaseManager firebaseManager;
DisplayManager displayManager;
TouchManager touchManager;
HabitTouchAsync habitTouchAsync(firebaseManager, displayManager);
WeatherManager weatherManager;
AirQualityManager airQualityManager;
PollenManager pollenManager;

// Guarda a hora da ultima previsao ja desenhada na tela.
char lastWeatherDrawn[17] = {};

uint32_t totalSecondsTV = 0;
uint32_t totalSecondsPC = 0;
String screenTimeDate;

bool updateScreenTime(const char *isoDate)
{
  uint32_t tv = 0;
  uint32_t pc = 0;

  if (!firebaseManager.readScreenTime(
          "pc-principal", isoDate, tv, pc))
  {
    // Não mostrar dados de ontem após a virada do dia.
    if (screenTimeDate != isoDate)
    {
      screenTimeDate = isoDate;
      totalSecondsTV = 0;
      totalSecondsPC = 0;
    }
    return false;
  }

  screenTimeDate = isoDate;
  totalSecondsTV = tv;
  totalSecondsPC = pc;

  return true;
}

// Toque de alternancia: canto superior esquerdo, 90 x 50 pixels.
// Nao salva a preferencia na NVS: apos reiniciar, abre o dashboard.
bool privacyMode = false;
bool privacyRequested = false;
bool calendarMode = false;
bool screenTimeMode = false;
ScreenTimeDetails screenDetails;
String screenDetailsCachedDate;
bool screenDetailsCached = false;
CalendarDay calendarDay;

// Dados iniciais consultados UMA VEZ no setup() (modo real).
// No loop() nao ha polling de agenda, aniversario, nem revalidacao por tempo.
constexpr uint8_t CALENDAR_CACHE_DAYS = 7;
CalendarDay calendarCache[CALENDAR_CACHE_DAYS];
String calendarWindowStart;

// Apenas um dia de aniversarios em RAM; sem expiracao por timer.
std::vector<CalendarBirthday> birthdaysCache;
String birthdaysCacheDate;
bool birthdaysLoaded = false;

// Consultas adicionais acontecem SOMENTE ao selecionar uma data nao carregada.
std::vector<CalendarDay> calendarExtraCache;
String calendarExtraMonth;


// Teste visual: comando Serial 'iconoff' mostra Wi-Fi cortado sem desconectar.
// 'iconauto' volta a usar o estado real da conexao.
bool forceOfflineIcon = false;
bool iconNeedsRedraw = false;
bool lastIconConnected = true;

bool displayWifiConnected()
{
  return !forceOfflineIcon && WiFi.status() == WL_CONNECTED;
}

bool renderCurrentDashboard()
{
  struct tm now{};
  if (!getLocalTime(&now, 1000))
  {
    Serial.println("PRIVACIDADE: relogio indisponivel para restaurar dashboard.");
    return false;
  }
  char iso[11];
  if (!strftime(iso, sizeof(iso), "%Y-%m-%d", &now))
    return false;

  DashboardData dashboard;
  dashboard.todayISO = iso;
  dashboard.totalSecondsTV = totalSecondsTV;
  dashboard.totalSecondsPC = totalSecondsPC;
  dashboard.habits = &firebaseManager.getHabits();
  dashboard.weather = weatherManager.hasData()
                          ? &weatherManager.data()
                          : nullptr;
  dashboard.airQuality =
      airQualityManager.hasData()
          ? &airQualityManager.data()
          : nullptr;
  dashboard.pollen =
      pollenManager.hasData()
          ? &pollenManager.data()
          : nullptr;
  displayManager.setWifiConnected(displayWifiConnected());
  displayManager.render(dashboard); // sem nova consulta de rede

  // Evita redesenhar logo em seguida a mesma previsao.
  if (weatherManager.hasData())
    snprintf(lastWeatherDrawn, sizeof(lastWeatherDrawn), "%s",
             weatherManager.data().observationTime);
  return true;
}

// Retorna uma data ISO vizinha usando o relogio local ja configurado.
String calendarShiftDate(const String &iso, int delta)
{
  int year = 0, month = 0, day = 0;
  if (sscanf(iso.c_str(), "%d-%d-%d", &year, &month, &day) != 3)
    return "";
  struct tm date{};
  date.tm_year = year - 1900;
  date.tm_mon = month - 1;
  date.tm_mday = day + delta;
  date.tm_hour = 12; // Evita ambiguidades proximas a meia-noite.
  if (mktime(&date) == (time_t)-1)
    return "";
  char formatted[11];
  if (!strftime(formatted, sizeof(formatted), "%Y-%m-%d", &date))
    return "";
  return String(formatted);
}

// Avanca/retrocede meses sem permitir que o dia 31 pule meses.
String calendarMonthOffset(const String &iso, int offset)
{
  int year = 0, month = 0, day = 0;
  if (sscanf(iso.c_str(), "%d-%d-%d", &year, &month, &day) != 3)
    return "";
  struct tm date{};
  date.tm_year = year - 1900;
  date.tm_mon = month - 1 + offset;
  date.tm_mday = 1;
  date.tm_hour = 12;
  if (mktime(&date) == (time_t)-1) return "";
  char formatted[11];
  if (!strftime(formatted, sizeof(formatted), "%Y-%m-%d", &date)) return "";
  return String(formatted);
}

// Mes e ano da data selecionada (usado para limpar o cache eventual).
String calendarMonthKey(const String &iso)
{
  return iso.length() == 10 ? iso.substring(0, 7) : String();
}

// Grade real desenhada pelo DisplayManager: x=78..371; y=149..406.
String calendarTouchDate(const String &selectedISO, int16_t x, int16_t y)
{
  constexpr int16_t X = 78, Y = 149, W = 42, H = 43;
  if (x < X || x >= X + 7 * W || y < Y || y >= Y + 6 * H)
    return "";
  const int year = selectedISO.substring(0, 4).toInt();
  const int month = selectedISO.substring(5, 7).toInt();
  if (year < 2000 || month < 1 || month > 12) return "";
  struct tm first{};
  first.tm_year = year - 1900;
  first.tm_mon = month - 1;
  first.tm_mday = 1;
  first.tm_hour = 12;
  if (mktime(&first) == (time_t)-1) return "";
  const String isoFirst = calendarMonthOffset(selectedISO, 0);
  const int cell = ((y - Y) / H) * 7 + (x - X) / W;
  return calendarShiftDate(isoFirst, cell - first.tm_wday);
}

// Consulta um documento birthdays/MM-DD somente quando a data for selecionada
// e ainda nao estiver em RAM. Nenhum timer ou consulta periodica.
bool loadBirthdaysForDate(const String &iso)
{
  if (iso.length() != 10 || habitTouchAsync.hasPending())
    return false;
  if (birthdaysLoaded && birthdaysCacheDate == iso)
    return true;
  if (WiFi.status() != WL_CONNECTED)
  {
    if (birthdaysCacheDate != iso)
    {
      birthdaysCache.clear();
      birthdaysCacheDate = iso;
      birthdaysLoaded = false;
    }
    return false;
  }

  std::vector<CalendarBirthday> incoming;
  const bool ok = firebaseManager.readBirthdaysForDay(iso.c_str(), incoming);
  if (ok)
  {
    birthdaysCache.swap(incoming);
    birthdaysCacheDate = iso;
    birthdaysLoaded = true;
  }
  else if (birthdaysCacheDate != iso)
  {
    // Nunca exibir os aniversarios de uma data anterior por engano.
    birthdaysCache.clear();
    birthdaysCacheDate = iso;
    birthdaysLoaded = false;
  }
  return ok;
}

// Ajusta somente os identificadores de datas do cache para o dia local.
// Nao faz consultas; pode ser chamado ao abrir a agenda depois da meia-noite.
void ensureCalendarWindow()
{
  struct tm now{};
  if (!getLocalTime(&now, 50))
    return;
  char iso[11];
  if (!strftime(iso, sizeof(iso), "%Y-%m-%d", &now))
    return;
  if (calendarWindowStart == iso)
    return;

  CalendarDay shifted[CALENDAR_CACHE_DAYS];
  for (uint8_t i = 0; i < CALENDAR_CACHE_DAYS; ++i)
  {
    const String date = calendarShiftDate(iso, i);
    shifted[i].date = date;
    shifted[i].loadError = true; // Ainda nao carregado nesta inicializacao.
    for (uint8_t j = 0; j < CALENDAR_CACHE_DAYS; ++j)
    {
      if (calendarCache[j].date == date)
      {
        shifted[i] = std::move(calendarCache[j]);
        break;
      }
    }
  }
  for (uint8_t i = 0; i < CALENDAR_CACHE_DAYS; ++i)
    calendarCache[i] = std::move(shifted[i]);

  calendarWindowStart = iso;
  Serial.printf("CALENDAR CACHE: janela iniciada em %s (%u dias).\n",
                iso, (unsigned)CALENDAR_CACHE_DAYS);
}

// Uma unica carga inicial, executada somente no setup() ou apos
// reconfiguracao explicita de Wi-Fi. Nao e chamada pelo loop().
void preloadCalendarOnce()
{
  ensureCalendarWindow();
#if !EINKDASH_CALENDAR_MOCK
  if (calendarWindowStart.isEmpty() || WiFi.status() != WL_CONNECTED)
    return;
  for (uint8_t i = 0; i < CALENDAR_CACHE_DAYS; ++i)
  {
    CalendarDay fresh;
    if (firebaseManager.readCalendarDay(calendarCache[i].date.c_str(), fresh))
      calendarCache[i] = std::move(fresh);
    else if (!calendarCache[i].found)
      calendarCache[i].loadError = true;
  }
  // Aniversarios somente para hoje; outras datas sao lidas ao seleciona-las.
  loadBirthdaysForDate(calendarWindowStart);
  Serial.printf("CALENDAR: carga inicial concluida; heap=%u.\n", ESP.getFreeHeap());
#else
  Serial.println("CALENDAR MOCK: nenhuma leitura de agenda ou aniversarios no Firebase.");
#endif
}

bool showCalendarDate(const String &iso)
{
  if (iso.length() != 10 || habitTouchAsync.hasPending())
    return false;
  // Atualiza a referencia local do dia, sem fazer sincronizacao de rede.
  ensureCalendarWindow();

#if EINKDASH_CALENDAR_MOCK
  // MOCK EXCLUSIVAMENTE VISUAL, sem consultas nem gravacoes no Firestore.
  // O restante do firmware (habitos, clima, tempo de tela) continua real.
  CalendarDay demo;
  demo.date = iso;
  demo.found = true;
  demo.loadError = false;
  std::vector<CalendarBirthday> demoBirthdays;

  const uint8_t mm = static_cast<uint8_t>(iso.substring(5, 7).toInt());
  const uint8_t dd = static_cast<uint8_t>(iso.substring(8, 10).toInt());

  auto addEvent = [&](const char *title, const char *start,
                      const char *end, bool allDay)
  {
    CalendarEvent event;
    event.title = title;
    event.start = start;
    event.end = end;
    event.allDay = allDay;
    demo.events.push_back(std::move(event));
  };
  auto addBirthday = [&](const char *name)
  {
    CalendarBirthday birthday;
    birthday.name = name;
    birthday.month = mm;
    birthday.day = dd;
    demoBirthdays.push_back(std::move(birthday));
  };

  // Cenarios vinculados a hoje, amanha e depois de amanha.
  // Os outros dias sao deixados vazios intencionalmente.
  if (iso == calendarWindowStart)
  {
    addBirthday("Sofia");
    addBirthday("Danilo");
    addBirthday("Leo");
    // TESTE: 3 eventos que iniciam juntos, terminam em horarios distintos.
    // A coluna liberada as 10:00 e reutilizada pelo evento "Retorno".
    // Nenhum instante tem mais de 3 eventos simultaneos.
    addEvent("Reunião", "09:00", "11:30", false);
    addEvent("Estudo", "09:00", "10:30", false);
    addEvent("Aula", "09:00", "10:00", false);
    addEvent("Retorno", "10:00", "10:45", false);
    // Evento isolado: deve voltar a ocupar a largura inteira.
    addEvent("Consulta médica", "13:00", "14:00", false);
    addEvent("Yoga", "16:30", "18:00", false);
  }
  else if (iso == calendarShiftDate(calendarWindowStart, 1))
  {
    addBirthday("Mariana");
    // TESTE: sobreposicao parcial e 3 eventos em parte do intervalo.
    addEvent("Café", "08:30", "09:30", false);
    addEvent("Compras", "09:00", "10:00", false);
    addEvent("Reunião", "09:15", "09:45", false);
    addEvent("Cinema", "17:00", "18:30", false);
  }
  else if (iso == calendarShiftDate(calendarWindowStart, 2))
  {
    addEvent("Dia de folga", "", "", true);
    addEvent("Caminhada", "10:00", "11:30", false);
  }

  Serial.printf("CALENDAR MOCK: %s | %u evento(s), %u aniversario(s) [NAO GRAVADOS].\n",
                iso.c_str(), (unsigned)demo.events.size(),
                (unsigned)demoBirthdays.size());

  if (!displayManager.renderCalendar(demo, demoBirthdays))
    return false;

  // Mantem os dados visiveis para restauracao da tela em caso de falha
  // ao entrar no modo visitas. Nao modifica o cache real de sete dias.
  calendarDay = std::move(demo);
  birthdaysCache = std::move(demoBirthdays);
  birthdaysCacheDate = iso;
  birthdaysLoaded = false;
  return true;
#else
  const String monthKey = calendarMonthKey(iso);
  if (calendarExtraMonth != monthKey)
  {
    calendarExtraCache.clear();
    calendarExtraMonth = monthKey;
  }

  CalendarDay next;
  bool cached = false;
  for (const CalendarDay &day : calendarCache)
    if (day.date == iso && !day.loadError)
    {
      next = day;
      cached = true;
      break;
    }
  if (!cached)
    for (const CalendarDay &day : calendarExtraCache)
      if (day.date == iso && !day.loadError)
      {
        next = day;
        cached = true;
        break;
      }

  if (!cached)
  {
    next.date = iso;
    if (WiFi.status() != WL_CONNECTED ||
        !firebaseManager.readCalendarDay(iso.c_str(), next))
      next.loadError = true;
    if (!next.loadError)
    {
      // Cache local de datas consultadas sob demanda durante este mes.
      if (calendarExtraCache.size() >= 31) calendarExtraCache.erase(calendarExtraCache.begin());
      calendarExtraCache.push_back(next);
    }
  }

  // Busca apenas os aniversarios do dia escolhido, nao de todos os contatos.
  // A falha nao impede a exibicao dos eventos do Calendar.
  if (!loadBirthdaysForDate(iso))
    Serial.println("BIRTHDAYS: leitura indisponivel; agenda exibida sem novos aniversarios.");
  Serial.printf("CALENDAR: abrindo %s (%s).\n", iso.c_str(),
                cached ? "RAM" : "Firestore / tentativa de rede");
  if (!displayManager.renderCalendar(next, birthdaysCache))
    return false;
  calendarDay = std::move(next);
  return true;
#endif // EINKDASH_CALENDAR_MOCK
}

// Abrir em resposta a toque, consultando no maximo uma vez por dia.
// Nenhum acesso a Firestore e iniciado automaticamente pelo loop().
bool showScreenTimeDetails()
{
  if (habitTouchAsync.hasPending()) return false;
  struct tm now{};
  char iso[11];
  if (!getLocalTime(&now, 1000) || !strftime(iso, sizeof(iso), "%Y-%m-%d", &now))
    return false;

  if (!screenDetailsCached || screenDetailsCachedDate != iso)
  {
    ScreenTimeDetails fresh;
    fresh.date = iso;
    // Os totais ja carregados no setup permanecem disponiveis offline.
    fresh.totalPC = totalSecondsPC;
    fresh.totalTV = totalSecondsTV;
    if (WiFi.status() == WL_CONNECTED &&
        firebaseManager.readScreenTimeDetails("pc-principal", iso, fresh))
    {
      screenDetails = std::move(fresh);
      screenDetailsCached = true;
      screenDetailsCachedDate = iso;
    }
    else
    {
      // Sem rede: nao inventa lista de apps. Usa totais ja existentes.
      screenDetails = ScreenTimeDetails();
      screenDetails.date = iso;
      screenDetails.totalPC = totalSecondsPC;
      screenDetails.totalTV = totalSecondsTV;
      screenDetails.dataAvailable = false;
      screenDetailsCached = false; // permite tentar novamente em outro toque
      Serial.println("SCREEN DETAILS: rede indisponivel, sem categorias.");
    }
  }

  if (!displayManager.renderScreenTimeDetails(screenDetails)) return false;
  screenTimeMode = true;
  Serial.println("SCREEN DETAILS: tela aberta por toque.");
  return true;
}

void handlePrivacyCorner()
{
  if (privacyMode)
  {
    Serial.println("PRIVACIDADE: restaurando dashboard...");
    if (renderCurrentDashboard())
    {
      privacyMode = false;
      Serial.println("PRIVACIDADE: dashboard restaurado.");
    }
    return;
  }

  // Se houve uma marcacao de habito agora, aguardar o resultado do
  // Firebase. Um rollback assincrono pode redesenhar o dashboard.
  privacyRequested = !privacyRequested;
  Serial.println(privacyRequested
                     ? "PRIVACIDADE: ocultacao solicitada."
                     : "PRIVACIDADE: ocultacao cancelada.");
}

void printMemory(const char *etapa)
{
  Serial.printf(
      "\nMEM [%s]\n"
      "Heap livre: %lu bytes\n"
      "Maior bloco: %lu bytes\n"
      "Menor heap registrado: %lu bytes\n"
      "Stack livre minima do loop: %lu bytes\n",
      etapa,
      (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT),
      (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
      (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
      (unsigned long)uxTaskGetStackHighWaterMark(NULL));
}

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
  dashboard.weather = weatherManager.hasData()
                          ? &weatherManager.data()
                          : nullptr;
  dashboard.airQuality =
      airQualityManager.hasData()
          ? &airQualityManager.data()
          : nullptr;
  dashboard.pollen =
      pollenManager.hasData()
          ? &pollenManager.data()
          : nullptr;

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

  // Primeiro busca os valores no Firestore
  updateScreenTime(iso);

  // Depois passa os valores atualizados ao dashboard
  dashboard.totalSecondsTV = totalSecondsTV;
  dashboard.totalSecondsPC = totalSecondsPC;

  // ==========================================================
  // FUTURAS INTEGRACOES
  // ==========================================================

  // Google Calendar
  // Sensores ambientais

  // ==========================================================
  // ATUALIZAR DISPLAY UMA UNICA VEZ
  // ==========================================================

  Serial.println();
  Serial.println("EINKDASH: atualizando display...");

  if (!privacyMode && !privacyRequested)
  {
    displayManager.setWifiConnected(displayWifiConnected());
    displayManager.render(dashboard);
  }
  else
    Serial.println("PRIVACIDADE: dados sincronizados, sem revelar o dashboard.");

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
  // Carga unicamente aqui, apos configuracao explicita, nunca no loop().
  preloadCalendarOnce();
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
      "Stack livre minima: %lu bytes\n",
      (unsigned long)uxTaskGetStackHighWaterMark(NULL));

  Serial.printf(
      "Heap livre: %lu bytes\n",
      (unsigned long)ESP.getFreeHeap());

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

  // ============================================================
  // CONFIGURACOES DO FIRESTORE
  // ============================================================

  const bool settingsLoaded =
      firebaseManager.syncSettings("settings/dashboard");

  const DeviceSettings &settings = firebaseManager.getSettings();

  if (settingsLoaded && settings.hasLocation)
  {
    Serial.printf(
        "LOCALIZACAO: %.6f, %.6f\n",
        settings.latitude,
        settings.longitude);

    if (airQualityManager.begin())
      airQualityManager.fetchAndLog(
          settings.latitude, settings.longitude);

    if (pollenManager.begin())
      pollenManager.fetchAndLog(
          settings.latitude, settings.longitude);

    if (weatherManager.begin())
      weatherManager.fetchAndLog(
          settings.latitude, settings.longitude);
  }
  else
  {
    Serial.println(
        "ENV: sem coordenadas validas no Firestore. "
        "Consultas ambientais ignoradas.");
  }

  Serial.println("EINKDASH: Firebase autenticado.");

  Serial.printf(
      "Stack livre minima apos Firebase: %lu bytes\n",
      (unsigned long)uxTaskGetStackHighWaterMark(NULL));

  Serial.printf(
      "Heap livre apos Firebase: %lu bytes\n",
      (unsigned long)ESP.getFreeHeap());

  // ==========================================================
  // INICIALIZAR TOUCH, PREVISAO E DASHBOARD
  // ==========================================================

  // Pinos corretos do GT911 nesta placa: SDA=32, SCL=33.
  if (touchManager.begin(32, 33))
  {
    touchManager.setTransform(false, false, false);
    Serial.println("TOUCH: pronto para testes de coordenadas.");
  }

  // A primeira renderizacao ja inclui os dados meteorologicos
  // caso a consulta inicial tenha sido bem-sucedida.
  const bool dashboardDrawn = syncDashboard();

  // Carga unica das datas iniciais. Nenhuma leitura periodica no loop().
  // Em modo mock, nenhuma consulta ao Calendar/Contacts e feita.
  preloadCalendarOnce();

  // Marca a previsao inicial como desenhada para evitar
  // uma segunda renderizacao identica no primeiro loop().
  if (dashboardDrawn && weatherManager.hasData())
  {
    snprintf(lastWeatherDrawn, sizeof(lastWeatherDrawn),
             "%s", weatherManager.data().observationTime);
  }

  // Iniciar a task permanente de gravacao de habitos por ultimo.
  if (!habitTouchAsync.begin())
  {
    Serial.println("TOUCH ASYNC: erro ao iniciar.");
  }
}

// ============================================================
// LOOP
// ============================================================

void loop()
{
  habitTouchAsync.poll(); // processar confirmacoes/falhas do Firebase

  // A data muda sem reconstruir a imagem-base. Um refresh so e pedido
  // quando o indicador realmente muda, e nunca revela o modo visitas.
  const bool iconConnected = displayWifiConnected();
  displayManager.setWifiConnected(iconConnected);
  if (iconConnected != lastIconConnected)
  {
    lastIconConnected = iconConnected;
    iconNeedsRedraw = true;
  }

  // Nenhuma consulta programada aqui; rede somente apos toque explicito.
  TouchPointEink touch;
  if (touchManager.readTap(touch))
  {
    // O canto superior direito funciona no dashboard e na foto.
    if (touch.x >= 710 && touch.x < 800 && touch.y >= 0 && touch.y < 50)
      handlePrivacyCorner();
    else if (!privacyMode && !privacyRequested && screenTimeMode)
    {
      // Mesmo gesto de voltar da agenda, sem desenhar icone adicional.
      if (touch.x < 80 && touch.y < 80 && !habitTouchAsync.hasPending())
      {
        if (renderCurrentDashboard())
        {
          screenTimeMode = false;
          iconNeedsRedraw = false;
        }
      }
    }
    else if (!privacyMode && !privacyRequested && calendarMode)
    {
      // Voltar pelo canto superior esquerdo; sem seta/botao.
      // Area de voltar invisivel, livre do titulo deslocado para a direita.
      if (touch.x < 50 && touch.y < 80)
      {
        if (!habitTouchAsync.hasPending() && renderCurrentDashboard())
        {
          calendarMode = false;
          iconNeedsRedraw = false;
        }
      }
      // Navegacao do mes por toque nos cantos INFERIORES da TELA.
      // Esquerda = mes anterior; direita = proximo mes.
      // Somente o toque solicita novos dados, nunca um timer do loop().
      else if (touch.y >= 430 &&
               (touch.x < 75 || touch.x >= 725))
      {
        const int delta = touch.x < 75 ? -1 : +1;
        const String target = calendarMonthOffset(calendarDay.date, delta);
        if (!target.isEmpty()) showCalendarDate(target);
      }
      // Mantem a navegacao anterior pelo titulo como alternativa.
      else if (touch.y < 90 && touch.x >= 70 && touch.x < 380)
      {
        const int delta = touch.x < 225 ? -1 : +1;
        const String target = calendarMonthOffset(calendarDay.date, delta);
        if (!target.isEmpty()) showCalendarDate(target);
      }
      // Dia do calendario: o card direito mostra seus compromissos.
      else
      {
        const String target = calendarTouchDate(calendarDay.date, touch.x, touch.y);
        if (!target.isEmpty() && target != calendarDay.date)
          showCalendarDate(target);
      }
    }
    else if (!privacyMode && !privacyRequested)
    {
      // Icone calendario 24x24 em (16,16), toque ampliado para 60x52.
      // Somente altera a area tocavel; o desenho permanece 24x24.
      if (touch.x >= 8 && touch.x < 68 && touch.y >= 4 && touch.y < 56)
      {
        if (!habitTouchAsync.hasPending())
        {
          struct tm now{};
          char iso[11];
          if (getLocalTime(&now, 1000) &&
              strftime(iso, sizeof(iso), "%Y-%m-%d", &now) &&
              showCalendarDate(iso))
          {
            calendarMode = true;
          }
        }
      }
      // Qualquer mini-card inferior de tempo de tela abre os detalhes.
      else if (touch.x >= 278 && touch.x < 580 &&
               touch.y >= 359 && touch.y < 466)
      {
        showScreenTimeDetails();
      }
      else
        habitTouchAsync.handleTap(touch);
    }
    // Em modo visitas, qualquer outro toque e ignorado.
  }

  // So trocar de imagem depois que todas as gravacoes pendentes
  // terminarem, evitando que um rollback do Firebase revele habitos.
  if (privacyRequested && !privacyMode && !habitTouchAsync.hasPending())
  {
    privacyRequested = false;
    if (displayManager.showPrivacyImage())
    {
      calendarMode = false;
      screenTimeMode = false;
      privacyMode = true;
      Serial.println("PRIVACIDADE: modo visitas ativo.");
    }
    else
    {
      Serial.println("PRIVACIDADE: foto indisponivel; dashboard mantido.");
      // Se a leitura falhou a meio da transferencia, restaurar a tela anterior.
      if (calendarMode)
        displayManager.renderCalendar(calendarDay, birthdaysCache);
      else if (screenTimeMode)
        displayManager.renderScreenTimeDetails(screenDetails);
      else
        renderCurrentDashboard();
    }
  }

  if (iconNeedsRedraw && !privacyMode && !privacyRequested &&
      !calendarMode && !screenTimeMode && !habitTouchAsync.hasPending())
  {
    Serial.println("DISPLAY: estado da rede mudou; atualizando cabecalho.");
    if (renderCurrentDashboard())
      iconNeedsRedraw = false;
  }

  // Sem consultas ou sincronizacoes automaticas no loop().
  delay(50);
}
