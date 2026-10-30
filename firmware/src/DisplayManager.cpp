#include "DisplayManager.h"

#include <Arduino.h>
#include <SPI.h>
#include <LittleFS.h>
#include <GxEPD2_4G_4G.h>
#include <algorithm>
#include <stdio.h>
#include "QFontGfx.h"
#include "FlashManager.h"
#include "Icons.h"
#include "WeatherIconIds.h"
#include "WeatherManager.h"
#include "AirQualityManager.h"
#include "PollenManager.h"
#include <Adafruit_GFX.h>
#include <string.h>
#include <esp_heap_caps.h>

// Driver e pinos IDÊNTICOS aos do teste que funcionou no GDEY075T7.
static GxEPD2_4G_4G<GxEPD2_750_GDEY075T7, 8> display(
    GxEPD2_750_GDEY075T7(27, 14, 12, 13));
static FlashManager flash; // compartilhado pelo renderizador rapido e de emergencia

// Use os caminhos QFNT que ja funcionaram no seu projeto.
// FONTES: todas ficam disponiveis em TODAS as funcoes deste arquivo.
// Para trocar um arquivo BIN, altere apenas o caminho aqui.
static const char *FONT_14_PATH = "/fonts/InterDisplay-SemiBold_14.bin";
static const char *FONT_16_PATH = "/fonts/InterDisplay-SemiBold_16.bin";
static const char *FONT_20_PATH = "/fonts/InterDisplay-SemiBold_20.bin";
static const char *FONT_24_PATH = "/fonts/InterDisplay-SemiBold_24.bin";
static const char *FONT_32_PATH = "/fonts/InterDisplay-SemiBold_32.bin";

namespace
{
  // Use diretamente font14, font16, font20, font24 ou font32 onde quiser.
  // Para adicionar outra, declare aqui e cadastre na tabela FONTS abaixo.
  QFontGfx font14;
  QFontGfx font16;
  QFontGfx font20;
  QFontGfx font24;
  QFontGfx font32;

  struct FontRegistration
  {
    QFontGfx *font;
    const char *path;
  };

  static const FontRegistration FONTS[] = {
      {&font14, FONT_14_PATH},
      {&font16, FONT_16_PATH},
      {&font20, FONT_20_PATH},
      {&font24, FONT_24_PATH},
      {&font32, FONT_32_PATH},
  };

  void endFonts()
  {
    for (const auto &entry : FONTS)
      entry.font->end();
  }

  bool beginFonts()
  {
    // Montagem do LittleFS e feita por DisplayManager::render().
    for (const auto &entry : FONTS)
    {
      if (!entry.font->begin(LittleFS, entry.path))
      {
        Serial.printf("DISPLAY: fonte QFNT invalida ou ausente: %s\n", entry.path);
        endFonts();
        return false;
      }
    }
    return true;
  }

  constexpr int16_t SCREEN_HEIGHT = 480;
  constexpr int16_t PAGE_HEIGHT = 8;

  // Geometria do painel 800 x 480.
  // Clima: 202px (antes 214): primeira faixa -8px, previsao -4px.
  // Cards inferiores: y=218, h=254 (antes y=230, h=242).
  constexpr int16_t HABITS_PANEL_X = 8;
  constexpr int16_t HABITS_PANEL_Y = 56;
  constexpr int16_t HABITS_PANEL_W = 262;
  constexpr int16_t HABITS_PANEL_H = 416;

  constexpr int16_t TILE_X = 16;
  constexpr int16_t TILE_Y = 62;
  constexpr int16_t TILE_W = 77;
  constexpr int16_t TILE_H = 95;
  constexpr int16_t TILE_STEP_X = 85;
  constexpr int16_t TILE_STEP_Y = 103;
  constexpr size_t MAX_VISIBLE_HABITS = 12;

  // Le um unico icone da Flash para buffer temporario de 576 B na stack.
  // Quando a Flash falha, o vetor em RAM continua como alternativa.
  const uint8_t *resolveIcon(const Habit &habit, uint8_t (&scratch)[FlashManager::ICON_BYTES])
  {
    if (habit.iconWidth != 48 || habit.iconHeight != 48)
      return nullptr;
    if (habit.iconBytes.size() == FlashManager::ICON_BYTES)
      return habit.iconBytes.data();
    if (habit.iconFlashSlot >= 0 && habit.iconFlashBank >= 0 &&
        flash.readIcon(static_cast<uint8_t>(habit.iconFlashSlot),
                       habit.iconFlashBank, habit.iconHash,
                       scratch, FlashManager::ICON_BYTES))
      return scratch;
    return nullptr;
  }

  // Cards inferiores: fundo cinza claro e 12 mini-cards BRANCOS reais.
  // Nenhum titulo. Icones 48x48 desenhados nas dimensoes do BIN.
  // Mini-cards inferiores com a MESMA largura: 94px em ambos os blocos.
  // Painel de atividades: 3 colunas; sensores: 2 colunas.
  // Margens internas de 6px, intervalos de 4px e 8px entre paineis.
  constexpr int16_t ACT_X = 278;
  constexpr int16_t ACT_Y = 218;
  constexpr int16_t ACT_W = 302;
  constexpr int16_t ACT_H = 254;
  constexpr int16_t SENSOR_X = 588;
  constexpr int16_t SENSOR_Y = 218;
  constexpr int16_t SENSOR_W = 204;
  constexpr int16_t SENSOR_H = 254;
  constexpr int16_t ACT_TILE_X = 284;
  constexpr int16_t ACT_TILE_STEP = 98;
  constexpr int16_t ACT_TILE_W = 94;
  constexpr int16_t ACT_TOP_Y = 224;
  constexpr int16_t ACT_TOP_H = 128;
  constexpr int16_t ACT_BOTTOM_Y = 359;
  constexpr int16_t ACT_BOTTOM_H = 107;
  constexpr int16_t SENSOR_TILE_X = 594;
  constexpr int16_t SENSOR_TILE_STEP = 98;
  constexpr int16_t SENSOR_TILE_W = 94;
  constexpr int16_t SENSOR_TILE_Y = 224;
  constexpr int16_t SENSOR_TILE_STEP_Y = 81;
  constexpr int16_t SENSOR_TILE_H = 77;

  void drawMetricCardBases(Adafruit_GFX &gfx)
  {
    gfx.fillRoundRect(ACT_X, ACT_Y, ACT_W, ACT_H, 7, GxEPD_LIGHTGREY);
    gfx.fillRoundRect(SENSOR_X, SENSOR_Y, SENSOR_W, SENSOR_H, 7, GxEPD_LIGHTGREY);
    for (int col = 0; col < 3; ++col)
    {
      const int16_t x = ACT_TILE_X + col * ACT_TILE_STEP;
      gfx.fillRoundRect(x, ACT_TOP_Y, ACT_TILE_W, ACT_TOP_H, 6, GxEPD_WHITE);
      gfx.fillRoundRect(x, ACT_BOTTOM_Y, ACT_TILE_W, ACT_BOTTOM_H, 6, GxEPD_WHITE);
    }
    for (int row = 0; row < 3; ++row)
      for (int col = 0; col < 2; ++col)
        gfx.fillRoundRect(SENSOR_TILE_X + col * SENSOR_TILE_STEP,
                          SENSOR_TILE_Y + row * SENSOR_TILE_STEP_Y,
                          SENSOR_TILE_W, SENSOR_TILE_H, 6, GxEPD_WHITE);
  }

  // Layout estatico compartilhado: Flash W25Q16 e renderizador de emergencia.
  // Todas as posicoes e divisorias sao definidas apenas aqui.
  void drawBaseLayout(Adafruit_GFX &gfx)
  {
    gfx.fillScreen(GxEPD_DARKGREY);
    gfx.fillRoundRect(8, 8, 262, 40, 7, GxEPD_WHITE);
    gfx.fillRoundRect(HABITS_PANEL_X, HABITS_PANEL_Y,
                      HABITS_PANEL_W, HABITS_PANEL_H, 7, GxEPD_WHITE);
    gfx.fillRoundRect(278, 8, 514, 202, 7, GxEPD_WHITE);
    drawMetricCardBases(gfx);

    // Primeira faixa: tempo | UV | AQI
    gfx.drawFastVLine(576, 20, 58, GxEPD_LIGHTGREY);
    gfx.drawFastVLine(682, 20, 58, GxEPD_LIGHTGREY);
    gfx.drawFastHLine(286, 84, 498, GxEPD_DARKGREY);

    // Segunda faixa: chuva | nuvens | vento | Pólen
    gfx.drawFastVLine(475, 91, 39, GxEPD_LIGHTGREY);
    gfx.drawFastVLine(582, 91, 39, GxEPD_LIGHTGREY);
    gfx.drawFastVLine(692, 91, 39, GxEPD_LIGHTGREY);
    gfx.drawFastHLine(286, 137, 498, GxEPD_DARKGREY);

    // Terceira faixa: previsao de cinco dias
    for (int col = 1; col < 5; ++col)
      gfx.drawFastVLine(286 + col * 100 - 4, 146, 57, GxEPD_LIGHTGREY);
  }

  String firstUtf8Character(const String &value)
  {
    if (value.isEmpty())
      return String();
    const uint8_t first = static_cast<uint8_t>(value[0]);
    size_t bytes = 1;
    if ((first & 0xE0) == 0xC0)
      bytes = 2;
    else if ((first & 0xF0) == 0xE0)
      bytes = 3;
    else if ((first & 0xF8) == 0xF0)
      bytes = 4;
    if (bytes > value.length())
      return String();
    return value.substring(0, bytes);
  }

  bool parseISODate(const String &iso, int &year, int &month, int &day)
  {
    if (iso.length() != 10 || iso[4] != '-' || iso[7] != '-')
      return false;
    for (int i = 0; i < 10; ++i)
    {
      if (i == 4 || i == 7)
        continue;
      if (iso[i] < '0' || iso[i] > '9')
        return false;
    }
    year = iso.substring(0, 4).toInt();
    month = iso.substring(5, 7).toInt();
    day = iso.substring(8, 10).toInt();
    return year >= 2000 && month >= 1 && month <= 12 && day >= 1 && day <= 31;
  }

  // Numero de dias do calendario civil: nao depende de fuso ou horario de verao.
  int32_t daysFromCivil(int y, unsigned m, unsigned d)
  {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned yoeDays = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int32_t>(yoeDays);
  }

  // Dia da semana e mes em portugues, calculados a partir de todayISO.
  // Nao depende do timezone global nem chama NTP durante a renderizacao.
  String formatDateHeader(const String &todayISO)
  {
    int year, month, day;
    if (!parseISODate(todayISO, year, month, day))
      return "";

    static const char *DAYS[] = {
        "Domingo", "Segunda", "Terça", "Quarta",
        "Quinta", "Sexta", "Sábado"};
    static const char *MONTHS[] = {
        "Janeiro", "Fevereiro", "Março", "Abril", "Maio", "Junho",
        "Julho", "Agosto", "Setembro", "Outubro", "Novembro", "Dezembro"};

    // 01/01/1970 foi quinta-feira (indice 4).
    const int32_t delta = daysFromCivil(year, month, day) -
                          daysFromCivil(1970, 1, 1);
    const int weekday = ((delta + 4) % 7 + 7) % 7;

    return String(DAYS[weekday]) + ", " + String(day) +
           " de " + MONTHS[month - 1];
  }

  // Pequeno simbolo Wi-Fi cortado, desenhado em vetor (nao requer PNG/BIN).
  // Coordenadas relativas de 24 x 24 px. O desenho respeita o recorte da faixa.
  void drawWifiOff(Adafruit_GFX &gfx, int16_t x, int16_t y)
  {
    // Ondas Wi-Fi (tres arcos de tracos simples).
    const uint16_t ink = GxEPD_BLACK;
    gfx.drawLine(x + 1, y + 9, x + 4, y + 6, ink);
    gfx.drawLine(x + 4, y + 6, x + 8, y + 4, ink);
    gfx.drawLine(x + 8, y + 4, x + 14, y + 4, ink);
    gfx.drawLine(x + 14, y + 4, x + 19, y + 6, ink);
    gfx.drawLine(x + 19, y + 6, x + 22, y + 9, ink);

    gfx.drawLine(x + 5, y + 13, x + 8, y + 10, ink);
    gfx.drawLine(x + 8, y + 10, x + 13, y + 10, ink);
    gfx.drawLine(x + 13, y + 10, x + 18, y + 13, ink);

    gfx.drawLine(x + 9, y + 16, x + 12, y + 14, ink);
    gfx.drawLine(x + 12, y + 14, x + 15, y + 16, ink);
    gfx.fillCircle(x + 12, y + 20, 2, ink);

    // Um pequeno corredor branco separa o traco de corte das ondas.
    gfx.drawLine(x + 1, y + 2, x + 22, y + 23, GxEPD_WHITE);
    gfx.drawLine(x + 2, y + 1, x + 23, y + 22, GxEPD_WHITE);
    gfx.drawLine(x + 3, y + 1, x + 23, y + 21, GxEPD_WHITE);
    gfx.drawLine(x + 1, y + 1, x + 22, y + 22, ink);
  }

  // Data, calendario e indicador de rede sao desenhados dinamicamente.
  // A imagem-base W25Q16 continua guardando apenas o fundo do cabecalho.
  void drawNetworkDateHeader(Adafruit_GFX &gfx, Icons &icons,
                             const String &todayISO, bool wifiConnected,
                             int16_t pageTop, int16_t pageBottom)
  {
    String visible = formatDateHeader(todayISO);
    if (visible.isEmpty())
      return;

    // Icone 320: calendar_verysmall.png (24x24), no tamanho nativo.
    // O cabeçalho vai de x=8 ate x=269; texto sempre comeca apos o icone.
    constexpr int16_t CALENDAR_X = 16;
    constexpr int16_t CALENDAR_Y = 16;
    constexpr int16_t CALENDAR_SIZE = 24;
    constexpr int16_t TEXT_X = 48;
    constexpr int16_t TEXT_BASELINE = 35;
    constexpr int16_t TEXT_RIGHT = 262; // margem direita de 8 px
    constexpr int16_t WIFI_GAP = 8;
    constexpr int16_t WIFI_ICON_W = 24;

    icons.draw(gfx, 320, CALENDAR_X, CALENDAR_Y,
               CALENDAR_SIZE, CALENDAR_SIZE, pageTop, pageBottom);

    // Sem Wi-Fi: mesmo comportamento anterior (omite o dia da semana
    // e exibe o simbolo de Wi-Fi desligado apos a data).
    if (!wifiConnected)
    {
      const int comma = visible.indexOf(',');
      if (comma >= 0)
        visible = visible.substring(comma + 2);
    }

    const int16_t maxTextWidth = TEXT_RIGHT - TEXT_X -
                                 (wifiConnected ? 0 : WIFI_GAP + WIFI_ICON_W);
    if (font20.textWidth(visible) > maxTextWidth)
    {
      // Primeiro tenta abreviar somente o dia da semana, mantendo
      // o mes por extenso. Nunca deixa a data invadir a margem direita.
      const int comma = visible.indexOf(',');
      if (comma >= 0)
      {
        const String weekday = visible.substring(0, comma);
        const String shortDay = weekday == "Sábado" ? String("Sáb")
                                                    : weekday.substring(0, 3);
        visible = shortDay + visible.substring(comma);
      }
    }
    if (font20.textWidth(visible) > maxTextWidth)
    {
      // Somente se ainda nao couber, retira o dia da semana.
      const int comma = visible.indexOf(',');
      if (comma >= 0)
        visible = visible.substring(comma + 2);
    }
    if (font20.textWidth(visible) > maxTextWidth)
    {
      // Ultimo recurso: abrevia o mes (ex.: 29 de Out).
      const int de = visible.indexOf(" de ");
      if (de >= 0)
        visible = visible.substring(0, de + 4) +
                  visible.substring(de + 4, de + 7);
    }

    font20.drawText(gfx, TEXT_X, TEXT_BASELINE, visible,
                    GxEPD_WHITE, pageTop, pageBottom);
    if (!wifiConnected)
      drawWifiOff(gfx, TEXT_X + font20.textWidth(visible) + WIFI_GAP, 16);
  }

  String compactDateHeader(const String &date)
  {
    String label = formatDateHeader(date);
    if (font20.textWidth(label) > 245)
    {
      const int comma = label.indexOf(',');
      if (comma > 3)
        label = label.substring(0, 3) + label.substring(comma);
    }
    return label;
  }

  String formatLastCompletion(const String &lastISO, const String &todayISO)
  {
    if (lastISO.isEmpty())
      return "-"; // Nunca realizado

    int ly, lm, ld, ty, tm, td;
    if (!parseISODate(lastISO, ly, lm, ld))
      return "-";
    if (!parseISODate(todayISO, ty, tm, td))
    {
      // Sem relogio valido: mostra data REAL, nunca uma idade inventada.
      return lastISO.substring(8, 10) + "/" + lastISO.substring(5, 7);
    }

    const int32_t days = daysFromCivil(ty, tm, td) - daysFromCivil(ly, lm, ld);
    if (days < 0)
      return "-";
    if (days == 0)
      return "Hoje";
    // Ate 30 dias: 1d ... 30d. Depois: meses completos do calendario.
    if (days <= 30)
      return String(days) + "d";

    int months = (ty - ly) * 12 + (tm - lm) - (td < ld ? 1 : 0);
    // Meses civis tem de 28 a 31 dias. Para >30d, nunca exibir 0m.
    if (months < 1)
      months = 1;
    if (months < 12)
      return String(months) + "m";
    const int years = months / 12;
    const int remainingMonths = months % 12;
    return String(years) + "a" +
           (remainingMonths ? String(remainingMonths) + "m" : String());
  }

  void drawCenteredText(QFontGfx &font, const String &text,
                        int16_t x, int16_t width, int16_t baseline,
                        uint16_t background,
                        int16_t pageTop, int16_t pageBottom)
  {
    const int16_t textWidth = font.textWidth(text);
    const int16_t centeredX = x + (width - textWidth) / 2;
    font.drawText(display, centeredX, baseline, text, background,
                  pageTop, pageBottom);
  }

  void drawHabitTile(const Habit &habit, size_t index, const String &todayISO,
                     int16_t pageTop, int16_t pageBottom)
  {
    const int16_t x = TILE_X + (index % 3) * TILE_STEP_X;
    const int16_t y = TILE_Y + (index / 3) * TILE_STEP_Y;

    display.fillRoundRect(x, y, TILE_W, TILE_H, 7, GxEPD_LIGHTGREY);

    // O formato gray2bpp ja foi decodificado pelo FirebaseManager.
    uint8_t scratch[FlashManager::ICON_BYTES];
    const uint8_t *icon = resolveIcon(habit, scratch);
    if (icon)
    {
      // Desenha os quatro tons como recebidos do aplicativo, incluindo branco.
      display.drawGreyPixmap(icon, 2, x + 14, y + 7, 48, 48);
    }
    else
    {
      // Enquanto nao houver icone enviado pelo app, usa a inicial com nossa QFNT.
      // Nao usa fonte de emergencia nem elimina acentos.
      drawCenteredText(font24, firstUtf8Character(habit.name),
                       x, TILE_W, y + 49, GxEPD_LIGHTGREY,
                       pageTop, pageBottom);
    }

    const String age = formatLastCompletion(habit.lastCompletedDate, todayISO);
    drawCenteredText(font16, age, x, TILE_W, y + 82,
                     GxEPD_LIGHTGREY, pageTop, pageBottom);
  }

  // ============================================================================
  // CARD UNICO DO CLIMA (800x480): desenhado sobre a base do W25Q16.
  // Somente o header WIC2 + uma linha de icone (max. 32 B) na RAM.
  // ============================================================================
  String weatherDegrees(float value)
  {
    if (!isfinite(value))
      return "--";
    return String(static_cast<int>(lroundf(value))) + "°";
  }

  String compactCondition(const WeatherData &w)
  {
    return w.currentDescription[0] ? String(w.currentDescription)
                                   : String(WeatherManager::conditionName(w.currentCode));
  }

  String compactForecastDay(const WeatherDay &day)
  {
    int year, month, date;
    if (!parseISODate(String(day.date), year, month, date))
      return "--";
    const int32_t delta = daysFromCivil(year, month, date) - daysFromCivil(1970, 1, 1);
    static const char *NAMES[] = {"Dom", "Seg", "Ter", "Qua", "Qui", "Sex", "Sáb"};
    return NAMES[((delta + 4) % 7 + 7) % 7];
  }

  String nextRainLabel(const WeatherData &w)
  {
    if (!w.hasNextRain || strlen(w.nextRainTime) < 16)
      return "-";
    if (w.rainNow)
      return "";
    const String iso(w.nextRainTime);
    // A data da primeira previsao diaria e hoje.
    if (w.dayCount && iso.substring(0, 10) == String(w.days[0].date))
      return iso.substring(11, 16);
    return iso.substring(8, 10) + "/" + iso.substring(5, 7);
  }

  // Alinha textos meteorologicos ao centro sem criar um novo framebuffer.
  void drawWeatherCentered(Adafruit_GFX &gfx, QFontGfx &font,
                           const String &value, int16_t x, int16_t width,
                           int16_t baseline, uint16_t background,
                           int16_t top, int16_t bottom)
  {
    font.drawText(gfx, x + (width - font.textWidth(value)) / 2,
                  baseline, value, background, top, bottom);
  }

  // --------------------------------------------------------------------------
  // INDICADORES: VALORES DEMONSTRATIVOS para testar o layout.
  // Os sensores, passos, agua e tempo de tela AINDA NAO sao lidos de uma API.
  // Quando integrar os dados, substituir apenas os valores deste bloco.
  // Nao altera os habitos, a previsao ou a imagem-base dos outros cards.
  // --------------------------------------------------------------------------
  // struct MetricDemoData
  // {
  //   const char *steps = "6.432";
  //   const char *bike = "12,5";
  //   const char *water = "1,8";
  //   uint8_t progress[3] = {64, 63, 60};
  //   uint8_t phoneHours = 2, phoneMinutes = 15;
  //   uint8_t pcHours = 3, pcMinutes = 40;
  //   uint8_t tvHours = 0, tvMinutes = 55;
  //   const char *temperature = "24,3°";
  //   const char *humidity = "52";
  //   const char *co2 = "620";
  //   const char *lux = "320";
  //   const char *pm25 = "8";
  //   const char *noise = "42";
  // };

  struct MetricDemoData
  {
    const char *steps = "-";
    const char *bike = "-";
    const char *water = "-";
    uint8_t progress[3] = {0, 0, 0};
    uint8_t phoneHours = 0, phoneMinutes = 0;
    uint8_t pcHours = 0, pcMinutes = 0;
    uint8_t tvHours = 0, tvMinutes = 0;
    const char *temperature = "-°";
    const char *humidity = "-";
    const char *co2 = "-";
    const char *lux = "-";
    const char *pm25 = "-";
    const char *noise = "-";
  };

  // Unidades fisicas usam fonte 14, numeros usam 20; apenas '%' pode ser 20.
  void drawValueUnit(Adafruit_GFX &gfx,
                     const String &value, const String &unit,
                     int16_t x, int16_t w, int16_t baseline,
                     int16_t top, int16_t bottom, bool fullPercent = false)
  {
    QFontGfx &uf = fullPercent ? font20 : font14;
    const int16_t gap = unit.isEmpty() ? 0 : (fullPercent ? 0 : 2);
    const int16_t wholeW = font20.textWidth(value) + gap + uf.textWidth(unit);
    int16_t pos = x + (w - wholeW) / 2;
    font20.drawText(gfx, pos, baseline, value, GxEPD_WHITE, top, bottom);
    pos += font20.textWidth(value) + gap;
    if (!unit.isEmpty())
      uf.drawText(gfx, pos, baseline, unit, GxEPD_WHITE, top, bottom);
  }

  // Formato 2h15m: h/m sempre menores que os digitos, na MESMA linha.
  void drawDuration(Adafruit_GFX &gfx,
                    uint8_t hours, uint8_t minutes, int16_t x, int16_t w,
                    int16_t baseline, int16_t top, int16_t bottom)
  {
    const String first = hours ? String(hours) : String(minutes);
    const String firstUnit = hours ? "h" : "min";
    const String last = hours ? String(minutes) : String();
    const String lastUnit = hours ? String("m") : String();
    const int16_t wholeW = font20.textWidth(first) + font14.textWidth(firstUnit) +
                           font20.textWidth(last) + font14.textWidth(lastUnit);
    int16_t px = x + (w - wholeW) / 2;
    font20.drawText(gfx, px, baseline, first, GxEPD_WHITE, top, bottom);
    px += font20.textWidth(first);
    font14.drawText(gfx, px, baseline, firstUnit, GxEPD_WHITE, top, bottom);
    px += font14.textWidth(firstUnit);
    if (hours)
    {
      font20.drawText(gfx, px, baseline, last, GxEPD_WHITE, top, bottom);
      px += font20.textWidth(last);
      font14.drawText(gfx, px, baseline, lastUnit, GxEPD_WHITE, top, bottom);
    }
  }

  void drawDemoIndicators(Adafruit_GFX &gfx, Icons &icons,
                          int16_t top, int16_t bottom)
  {
    const MetricDemoData m;
    const uint16_t actIds[3] = {313, 312, 311};
    const char *actNames[3] = {"Passos", "Bicicleta", "Água"};
    const char *actValues[3] = {m.steps, m.bike, m.water};
    const char *actUnits[3] = {"", "km", "L"};

    // Atividades: 3 colunas com barras de progresso, sem textos de metas.
    for (int c = 0; c < 3; ++c)
    {
      const int16_t x = ACT_TILE_X + c * ACT_TILE_STEP;
      const int16_t y = ACT_TOP_Y;
      if (y >= bottom || y + ACT_TOP_H <= top)
        continue;
      icons.draw(gfx, actIds[c], x + (ACT_TILE_W - 48) / 2, y + 5,
                 48, 48, top, bottom);
      const String label(actNames[c]);
      font14.drawText(gfx, x + (ACT_TILE_W - font14.textWidth(label)) / 2,
                      y + 68, label, GxEPD_WHITE, top, bottom);
      drawValueUnit(gfx, actValues[c], actUnits[c],
                    x + 2, ACT_TILE_W - 4, y + 90, top, bottom);
      // Fundo do progresso = cinza claro; preenchimento = cinza escuro.
      gfx.fillRoundRect(x + (ACT_TILE_W - 66) / 2, y + 99, 66, 7, 3, GxEPD_LIGHTGREY);
      const int16_t fill = (66 * m.progress[c]) / 100;
      if (fill > 0)
        gfx.fillRoundRect(x + (ACT_TILE_W - 66) / 2, y + 99, fill, 7, 3, GxEPD_DARKGREY);
      const String percent = String(m.progress[c]) + "%";
      font14.drawText(gfx, x + (ACT_TILE_W - font14.textWidth(percent)) / 2,
                      y + 120, percent, GxEPD_WHITE, top, bottom);
    }

    // Tempo de tela: 3 mini-cards inferiores, sem titulos nos cards principais.
    const uint16_t screenIds[3] = {314, 315, 317};
    const char *screenNames[3] = {"Celular", "PC", "TV"};
    const uint8_t hours[3] = {m.phoneHours, m.pcHours, m.tvHours};
    const uint8_t minutes[3] = {m.phoneMinutes, m.pcMinutes, m.tvMinutes};
    for (int c = 0; c < 3; ++c)
    {
      const int16_t x = ACT_TILE_X + c * ACT_TILE_STEP;
      const int16_t y = ACT_BOTTOM_Y;
      if (y >= bottom || y + ACT_BOTTOM_H <= top)
        continue;
      icons.draw(gfx, screenIds[c], x + (ACT_TILE_W - 48) / 2, y + 7,
                 48, 48, top, bottom);
      const String label(screenNames[c]);
      font14.drawText(gfx, x + (ACT_TILE_W - font14.textWidth(label)) / 2,
                      y + 73, label, GxEPD_WHITE, top, bottom);
      drawDuration(gfx, hours[c], minutes[c],
                   x + 2, ACT_TILE_W - 4, y + 96, top, bottom);
    }

    // Sensores: 2 colunas x 3 linhas. Sem rotulos; apenas icone centralizado + valor.
    const uint16_t sensorIds[6] = {304, 305, 306, 307, 308, 309};
    const char *sensorValues[6] = {m.temperature, m.humidity, m.co2,
                                   m.lux, m.pm25, m.noise};
    const char *sensorUnits[6] = {"", "%", "ppm", "lx", "µg/m³", "dB"};
    for (int i = 0; i < 6; ++i)
    {
      const int col = i % 2, row = i / 2;
      const int16_t x = SENSOR_TILE_X + col * SENSOR_TILE_STEP;
      const int16_t y = SENSOR_TILE_Y + row * SENSOR_TILE_STEP_Y;
      if (y >= bottom || y + SENSOR_TILE_H <= top)
        continue;
      icons.draw(gfx, sensorIds[i], x + (SENSOR_TILE_W - 48) / 2, y + 4,
                 48, 48, top, bottom);
      drawValueUnit(gfx, sensorValues[i], sensorUnits[i],
                    x + 3, SENSOR_TILE_W - 6, y + 69, top, bottom,
                    i == 1);
    }
  }

  // Qualidade do ar externo (AQI brasileiro), usando dados ja lidos em RAM.
  // Mesmo estilo vertical da faixa UV: icone, categoria e numero.
  void drawAirQuality(Adafruit_GFX &gfx, const AirQualityData *airQuality,
                      Icons &icons, int16_t top, int16_t bottom)
  {
    if (!airQuality || airQuality->brazilAqi < 0)
      return;

    // ID 322: icone escolhido para qualidade do ar (30x30).
    icons.draw(gfx, 322, 692, 26, 30, 30, top, bottom);

    // Ex.: "N1 – Boa" -> "Boa". Estados maiores usam abreviacao curta.
    String category = "AQI";
    const String raw(airQuality->category);
    if (raw.startsWith("N1"))
      category = "Boa";
    else if (raw.startsWith("N2"))
      category = "Mod.";
    else if (raw.startsWith("N3"))
      category = "Ruim";
    else if (raw.startsWith("N4"))
      category = "M. ruim";
    else if (raw.startsWith("N5"))
      category = "Péss.";

    font14.drawText(gfx, 736, 36, category, GxEPD_WHITE, top, bottom);
    font20.drawText(gfx, 736, 71, String(airQuality->brazilAqi),
                    GxEPD_WHITE, top, bottom);
  }

  // Quarta celula da segunda faixa: x=688..792, y=91..130.
  // UPI de gramineas do Google Pollen: indice de 0 a 5.
  void drawPollen(Adafruit_GFX &gfx, const PollenData *pollen,
                  Icons &icons, int16_t top, int16_t bottom)
  {
    if (!pollen || pollen->grassIndex < 0)
      return;

    icons.draw(gfx, 321, 698, 95, 30, 30, top, bottom);
    font14.drawText(gfx, 733, 106, "Pólen", GxEPD_WHITE, top, bottom);
    font20.drawText(gfx, 733, 132, String(pollen->grassIndex) + "/5",
                    GxEPD_WHITE, top, bottom);
  }

  void drawCompactWeather(Adafruit_GFX &gfx, const WeatherData *weather,
                          Icons &icons, int16_t top, int16_t bottom)
  {
    if (!weather)
    {
      return;
    }

    // PRIMEIRA FAIXA (mesma composicao do Photoshop):
    //   [icone] [21°] [Sens. 24°] [Max. 29° / Min. 19°] | [UV] | [AQI]
    //           [Pancadas de chuva]
    // A condicao e uma unica linha ABAIXO dos dois conjuntos (temperatura + sens.).
    // Nao existe divisoria entre temperatura atual e sensacao termica.
    icons.draw(gfx, weatherIconId(weather->currentCode, weather->isDay, false),
               281, 9, 70, 70, top, bottom);

    // V14: conteudo da faixa superior sobe alguns pixels para equilibrar
    // margens. Temp. principal e grupo Sens./23° tem topo visual alinhado;
    // a descricao e o valor de Min. compartilham a baseline y=73.
    // A temperatura e a descricao comecam EXATAMENTE no mesmo X.
    // Mantemos a temperatura centralizada na faixa original de 73 px,
    // mas reutilizamos seu X real para o texto inferior.
    const String currentText = weatherDegrees(weather->currentTempC);
    constexpr int16_t TEMP_AREA_X = 342;
    constexpr int16_t TEMP_AREA_W = 73;
    const int16_t currentX = TEMP_AREA_X +
                             (TEMP_AREA_W - font32.textWidth(currentText)) / 2;
    font32.drawText(gfx, currentX, 43, currentText,
                    GxEPD_WHITE, top, bottom);

    // 'Sens.' e seu numero usam o MESMO espaco rotulo->valor que Max./Min.
    // Rotulos na mesma linha (baseline=38) e valores na mesma (baseline=44).
    constexpr int16_t LABEL_VALUE_GAP = 4;
    constexpr int16_t SENS_X = 414;
    const String sensLabel = "Sens.";
    const String apparentText = weatherDegrees(weather->apparentTempC);
    font14.drawText(gfx, SENS_X, 36, sensLabel,
                    GxEPD_WHITE, top, bottom);
    font20.drawText(gfx, SENS_X + font14.textWidth(sensLabel) + LABEL_VALUE_GAP,
                    36, apparentText, GxEPD_WHITE, top, bottom);

    // Condicao abaixo de temperatura + sensacao, ALINHADA A ESQUERDA
    // com o inicio do numero da temperatura, nunca centralizada.
    const int16_t conditionWidth = 498 - currentX - 8; // ate o grupo Max./Min.
    String condition = compactCondition(*weather);
    if (font14.textWidth(condition) > conditionWidth)
    {
      struct ShortCondition
      {
        const char *type;
        const char *text;
      };

      static const ShortCondition names[] = {
          {"CLEAR", "Céu limpo"},
          {"MOSTLY_CLEAR", "Poucas nuvens"},
          {"PARTLY_CLOUDY", "Parc. nublado"},
          {"MOSTLY_CLOUDY", "Muito nublado"},
          {"CLOUDY", "Nublado"},
          {"WINDY", "Ventando"},
          {"WIND_AND_RAIN", "Chuva e vento"},
          {"LIGHT_RAIN", "Chuva fraca"},
          {"RAIN", "Chuva"},
          {"HEAVY_RAIN", "Chuva forte"},
          {"LIGHT_RAIN_SHOWERS", "Pancadas leves"},
          {"RAIN_SHOWERS", "Pancadas"},
          {"HEAVY_RAIN_SHOWERS", "Pancadas fortes"},
          {"CHANCE_OF_SHOWERS", "Poss. de chuva"},
          {"SCATTERED_SHOWERS", "Chuva isolada"},
          {"LIGHT_TO_MODERATE_RAIN", "Chuva leve/mod."},
          {"MODERATE_TO_HEAVY_RAIN", "Chuva mod./forte"},
          {"RAIN_PERIODICALLY_HEAVY", "Chuva variável"},
          {"THUNDERSTORM", "Trovoada"},
          {"THUNDERSHOWER", "Chuva e trovões"},
          {"LIGHT_THUNDERSTORM_RAIN", "Trovoadas leves"},
          {"SCATTERED_THUNDERSTORMS", "Trovoadas isoladas"},
          {"HEAVY_THUNDERSTORM", "Tempestade forte"},
          {"HAIL", "Granizo"},
          {"SNOW", "Neve"},
          {"HEAVY_SNOW", "Neve forte"}};

      for (const auto &entry : names)
      {
        if (strcmp(weather->currentType, entry.type) == 0)
        {
          condition = entry.text;
          break;
        }
      }
    }
    if (font14.textWidth(condition) > conditionWidth)
    {
      const String ellipsis = "...";
      while (!condition.isEmpty() &&
             font14.textWidth(condition + ellipsis) > conditionWidth)
        condition.remove(condition.length() - 1);
      condition += ellipsis;
    }
    font14.drawText(gfx, currentX, 70, condition,
                    GxEPD_WHITE, top, bottom);

    // Maxima e minima: mantem o espacamento atual entre rotulo e valor.
    constexpr int16_t MAX_MIN_X = 498;
    const String maxLabel = "Máx.";
    const String minLabel = "Mín.";
    const String maxText = weather->dayCount
                               ? weatherDegrees(weather->days[0].maxC)
                               : String("--");
    const String minText = weather->dayCount
                               ? weatherDegrees(weather->days[0].minC)
                               : String("--");
    font14.drawText(gfx, MAX_MIN_X, 36, maxLabel, GxEPD_WHITE, top, bottom);
    font20.drawText(gfx, MAX_MIN_X + font14.textWidth(maxLabel) + LABEL_VALUE_GAP,
                    36, maxText, GxEPD_WHITE, top, bottom);
    font14.drawText(gfx, MAX_MIN_X, 71, minLabel, GxEPD_WHITE, top, bottom);
    font20.drawText(gfx, MAX_MIN_X + font14.textWidth(minLabel) + LABEL_VALUE_GAP + 4,
                    71, minText, GxEPD_WHITE, top, bottom);

    // UV: desenhado nos 48x48 px nativos do PNG/BIN (sem reducao para 30),
    // com rotulo e numero a direita. Assim os tracos e letras 'UV' nao somem.
    icons.draw(gfx, 303, 575, 18, 48, 48, top, bottom);
    // Atribuicao exigida pelo Google para exibir dados da Weather API.
    // Linha superior no bloco UV, sem alterar a geometria dos cards.
    font14.drawText(gfx, 624, 36, "UV máx.", GxEPD_WHITE, top, bottom);
    const String uvNumber = weather->dayCount && isfinite(weather->days[0].uvMax)
                                ? String(static_cast<int>(lroundf(weather->days[0].uvMax)))
                                : String("--");
    font20.drawText(gfx, 624, 71, uvNumber, GxEPD_WHITE, top, bottom);

    // Segunda faixa: proxima chuva, nuvens e vento; polen e desenhado separadamente.
    const String nextRain = nextRainLabel(*weather);
    icons.draw(gfx, 318, 284, 94, 34, 34, top, bottom);
    font14.drawText(gfx, 319, 106, weather->rainNow ? "Chuva agora" : "Próx. chuva",
                    GxEPD_WHITE, top, bottom);
    font20.drawText(gfx, 319, 132, nextRain, GxEPD_WHITE, top, bottom);
    if (weather->hasNextRain)
    {
      const String intensity = isfinite(weather->nextRainMm)
                                   ? String(weather->nextRainMm, 1) + " mm/h"
                                   : String("-- mm/h");
      const String chance = weather->nextRainProbabilityPct >= 0
                                ? String("prob. ") + weather->nextRainProbabilityPct + "%"
                                : String("prob. --");
      font14.drawText(gfx, 404, 106, intensity, GxEPD_WHITE, top, bottom);
      font14.drawText(gfx, 404, 127, chance, GxEPD_WHITE, top, bottom);
    }

    // Reutiliza o icone de nublado pequeno (ID 202) do BIN existente.
    icons.draw(gfx, 202, 480, 93, 37, 37, top, bottom);
    font14.drawText(gfx, 523, 107, "Nuvens", GxEPD_WHITE, top, bottom);
    const String clouds = isfinite(weather->cloudCoverPct)
                              ? String(static_cast<int>(lroundf(weather->cloudCoverPct))) + "%"
                              : String("--");
    font20.drawText(gfx, 523, 132, clouds, GxEPD_WHITE, top, bottom);

    icons.draw(gfx, 302, 586, 98, 26, 26, top, bottom);
    font14.drawText(gfx, 614, 106, "Vento máx.", GxEPD_WHITE, top, bottom);
    const String windNumber = weather->dayCount && isfinite(weather->days[0].windMaxKmh)
                                  ? String(static_cast<int>(lroundf(weather->days[0].windMaxKmh)))
                                  : String("--");
    font20.drawText(gfx, 614, 132, windNumber, GxEPD_WHITE, top, bottom);
    // Mantem o numero grande; somente a unidade fica menor.
    font14.drawText(gfx, 620 + font20.textWidth(windNumber), 132,
                    "km/h", GxEPD_WHITE, top, bottom);

    // Terceira faixa: proximos cinco dias, sem incluir hoje.
    for (uint8_t i = 1; i < weather->dayCount && i <= 5; ++i)
    {
      const int16_t x = 286 + (i - 1) * 100;
      const WeatherDay &day = weather->days[i];
      drawWeatherCentered(gfx, font14, compactForecastDay(day), x, 96, 157,
                          GxEPD_WHITE, top, bottom);
      icons.draw(gfx, weatherIconId(day.weatherCode, true, true),
                 x, 152, 48, 48, top, bottom);
      font20.drawText(gfx, x + 54, 183, weatherDegrees(day.maxC),
                      GxEPD_WHITE, top, bottom);
      font14.drawText(gfx, x + 54, 203, weatherDegrees(day.minC),
                      GxEPD_WHITE, top, bottom);
    }
  }

} // namespace

static void renderLegacy(const DashboardData &data, const WeatherData *weather, bool wifiConnected)
{
  const size_t total = data.habits ? data.habits->size() : 0;
  Serial.printf("DISPLAY: montando dashboard (%u habitos).\n", (unsigned)total);
  Serial.printf("DISPLAY: heap antes: %u bytes\n", ESP.getFreeHeap());

  if (!LittleFS.begin(false))
  {
    Serial.println("DISPLAY: LittleFS indisponivel; renderizacao cancelada.");
    return;
  }

  Icons icons;
  if (!icons.begin())
    Serial.println("ICONS: /icons.bin ausente/invalido (formato WIC2); exibindo so textos.");

  SPI.begin(18, 19, 23, 27);
  flash.begin(); // permite recuperar icones externos no renderizador de emergencia
  display.init(115200);
  display.setRotation(0);
  display.setFullWindow();
  Serial.printf("DISPLAY: area enderecavel %d x %d pixels.\n",
                display.width(), display.height());
  const String dateText = formatDateHeader(data.todayISO);
  Serial.print("DISPLAY: cabecalho = ");
  Serial.println(dateText.isEmpty() ? "DATA INDISPONIVEL" : dateText);
  if (!dateText.isEmpty() && font20.textWidth(dateText) > 245)
  {
    Serial.printf("DISPLAY: aviso - cabecalho com %d pixels (limite 245).\n",
                  font20.textWidth(dateText));
  }

  int16_t pageTop = 0;
  display.firstPage();
  do
  {
    const int16_t pageBottom = std::min<int16_t>(pageTop + PAGE_HEIGHT,
                                                 SCREEN_HEIGHT);
    drawBaseLayout(display);
    drawNetworkDateHeader(display, icons, data.todayISO, wifiConnected, pageTop, pageBottom);
    drawCompactWeather(display, weather, icons, pageTop, pageBottom);
    drawAirQuality(display, data.airQuality, icons, pageTop, pageBottom);
    drawPollen(display, data.pollen, icons, pageTop, pageBottom);
    drawDemoIndicators(display, icons, pageTop, pageBottom);
    if (data.habits)
    {
      const size_t count = std::min<size_t>(data.habits->size(), MAX_VISIBLE_HABITS);
      for (size_t i = 0; i < count; ++i)
      {
        drawHabitTile((*data.habits)[i], i, data.todayISO,
                      pageTop, pageBottom);
      }
    }
    pageTop += PAGE_HEIGHT;
  } while (display.nextPage());

  display.epd2.hibernate();
  icons.end();
  Serial.printf("DISPLAY: concluido. Heap livre: %u bytes\n", ESP.getFreeHeap());
}

// ============================================================================
// CACHE PERSISTENTE: imagem-base 800x480 / 2 bpp no W25Q16.
// Datas dos habitos NAO sao incluidas na base; apenas sao compostas em RAM.
// Nenhum framebuffer de 96 KB e alocado no ESP32.
// ============================================================================

namespace
{
  constexpr int16_t WIDTH = 800;
  constexpr int16_t HEIGHT = 480;
  constexpr int16_t STRIPE = 32;
  constexpr size_t STRIPE_BYTES = WIDTH * STRIPE / 4;

  // Adafruit_GFX real, com um framebuffer de apenas 32 linhas.
  // Permite usar QFontGfx sem copiar nem alterar a biblioteca de fontes.
  class PageCanvas final : public Adafruit_GFX
  {
  public:
    PageCanvas() : Adafruit_GFX(800, 480) { memset(_buf, 0xFF, sizeof(_buf)); }

    void beginPage(int16_t y, int16_t h)
    {
      _pageY = y;
      _pageH = h;
      memset(_buf, 0xFF, sizeof(_buf));
    }
    uint8_t *bytes() { return _buf; }
    const uint8_t *bytes() const { return _buf; }

    void drawPixel(int16_t x, int16_t y, uint16_t color) override
    {
      if (x < 0 || x >= WIDTH || y < _pageY || y >= _pageY + _pageH)
        return;
      uint8_t v;
      if (color == GxEPD_WHITE)
        v = 3;
      else if (color == GxEPD_LIGHTGREY)
        v = 2;
      else if (color == GxEPD_DARKGREY)
        v = 1;
      else
        v = 0;
      const size_t at = size_t(y - _pageY) * (WIDTH / 4) + (x >> 2);
      const uint8_t shift = 2 * (3 - (x & 3));
      _buf[at] = (_buf[at] & ~(uint8_t(3) << shift)) | (v << shift);
    }
    void fillScreen(uint16_t color) override
    {
      uint8_t v = color == GxEPD_WHITE ? 3 : color == GxEPD_LIGHTGREY ? 2
                                         : color == GxEPD_DARKGREY    ? 1
                                                                      : 0;
      memset(_buf, v * 0x55, sizeof(_buf));
    }
    // Mesma ordem de pixels de GxEPD2_4G_4G::drawGreyPixmap(depth=2):
    // MSB primeiro, 4 pixels por byte. Preserva branco opaco do icone.
    void drawGreyPixmap(const uint8_t *src, int16_t x, int16_t y,
                        int16_t w, int16_t h)
    {
      const uint16_t shades[] = {GxEPD_BLACK, GxEPD_DARKGREY,
                                 GxEPD_LIGHTGREY, GxEPD_WHITE};
      const int16_t stride = (w + 3) / 4;
      for (int16_t j = 0; j < h; ++j)
      {
        if (y + j < _pageY || y + j >= _pageY + _pageH)
          continue;
        for (int16_t i = 0; i < w; ++i)
        {
          const uint8_t packed = src[j * stride + (i / 4)];
          const uint8_t shade = (packed >> (6 - 2 * (i & 3))) & 3;
          drawPixel(x + i, y + j, shades[shade]);
        }
      }
    }

  private:
    int16_t _pageY = 0;
    int16_t _pageH = STRIPE;
    uint8_t _buf[STRIPE_BYTES];
  };

  PageCanvas canvas;

  // FNV-1a: somente a parte ESTATICA do layout. Quando ordem, icone, nome,
  // quantidade ou data do cabecalho mudarem, a imagem-base e reconstruida.
  uint32_t addBytes(uint32_t h, const uint8_t *bytes, size_t n)
  {
    for (size_t i = 0; i < n; ++i)
    {
      h ^= bytes[i];
      h *= 16777619UL;
    }
    return h;
  }
  uint32_t addString(uint32_t h, const String &s)
  {
    h = addBytes(h, reinterpret_cast<const uint8_t *>(s.c_str()), s.length());
    const uint8_t delim = 0xFF;
    return addBytes(h, &delim, 1);
  }
  uint32_t staticSignature(const DashboardData &data)
  {
    uint32_t h = 2166136261UL;
    h = addString(h, "EinkDash-base-v33");
    h = addString(h, data.todayISO);
    const size_t count = data.habits ? std::min<size_t>(data.habits->size(), MAX_VISIBLE_HABITS) : 0;
    uint8_t n = static_cast<uint8_t>(count);
    h = addBytes(h, &n, 1);
    for (size_t i = 0; i < count; ++i)
    {
      const Habit &habit = (*data.habits)[i];
      h = addString(h, habit.id);
      h = addString(h, habit.name);
      h = addBytes(h, reinterpret_cast<const uint8_t *>(&habit.iconWidth), sizeof(habit.iconWidth));
      h = addBytes(h, reinterpret_cast<const uint8_t *>(&habit.iconHeight), sizeof(habit.iconHeight));
      // Hash calculado ANTES de liberar a imagem da RAM.
      h = addBytes(h, reinterpret_cast<const uint8_t *>(&habit.iconHash),
                   sizeof(habit.iconHash));
    }
    return h;
  }

  void drawBaseHabit(PageCanvas &gfx, const Habit &habit, size_t i,
                     int16_t top, int16_t bottom)
  {
    const int16_t x = TILE_X + (i % 3) * TILE_STEP_X;
    const int16_t y = TILE_Y + (i / 3) * TILE_STEP_Y;
    if (y >= bottom || y + TILE_H <= top)
      return;
    gfx.fillRoundRect(x, y, TILE_W, TILE_H, 7, GxEPD_LIGHTGREY);
    uint8_t scratch[FlashManager::ICON_BYTES];
    const uint8_t *icon = resolveIcon(habit, scratch);
    if (icon)
    {
      gfx.drawGreyPixmap(icon, x + 14, y + 7, 48, 48);
    }
    else
    {
      const String initial = firstUtf8Character(habit.name);
      const int16_t posX = x + (TILE_W - font24.textWidth(initial)) / 2;
      font24.drawText(gfx, posX, y + 49, initial, GxEPD_LIGHTGREY, top, bottom);
    }
    // A linha de "Hoje" / "Xd" fica VAZIA na memoria Flash.
  }

  bool createBase(const DashboardData &data, uint32_t key)
  {
    if (!flash.startWrite(key))
      return false;
    bool ok = true;
    for (int16_t y = 0; y < HEIGHT; y += STRIPE)
    {
      const int16_t rows = std::min<int16_t>(STRIPE, HEIGHT - y);
      canvas.beginPage(y, rows);
      drawBaseLayout(canvas);
      if (data.habits)
      {
        const size_t count = std::min<size_t>(data.habits->size(), MAX_VISIBLE_HABITS);
        for (size_t i = 0; i < count; ++i)
          drawBaseHabit(canvas, (*data.habits)[i], i, y, y + rows);
      }
      const uint32_t offset = uint32_t(y) * (WIDTH / 4);
      if (!flash.writeChunk(offset, canvas.bytes(), size_t(rows) * WIDTH / 4))
      {
        Serial.printf("FLASH: falha ao gravar faixa y=%d.\n", y);
        ok = false;
        break;
      }
    }
    if (ok)
      ok = flash.finishWrite();
    if (ok)
      Serial.println("FLASH: imagem-base de 96.000 bytes salva/verificada.");
    return ok;
  }

  void drawDynamicAges(const DashboardData &data,
                       int16_t top, int16_t bottom)
  {
    if (!data.habits)
      return;
    const size_t count = std::min<size_t>(data.habits->size(), MAX_VISIBLE_HABITS);
    for (size_t i = 0; i < count; ++i)
    {
      const int16_t x = TILE_X + (i % 3) * TILE_STEP_X;
      const int16_t y = TILE_Y + (i / 3) * TILE_STEP_Y;
      // Fonte de 16px: tinta proxima a linha de base y+82; faixas sem
      // intersecao nao precisam abrir glifos.
      if (y + 88 < top || y + 52 >= bottom)
        continue;
      const String value = formatLastCompletion((*data.habits)[i].lastCompletedDate, data.todayISO);
      const int16_t textX = x + (TILE_W - font16.textWidth(value)) / 2;
      font16.drawText(canvas, textX, y + 82, value,
                      GxEPD_LIGHTGREY, top, bottom);
    }
  }

  // Reenvia os 96 KB prontos por faixas, sem redesenhar layout nem icones.
  // A tela permanece em 4 tons e a atualizacao continua sendo total.
  bool renderFlash(const DashboardData &data, const WeatherData *weather, bool wifiConnected)
  {
    const uint32_t totalStart = millis();
    if (!LittleFS.begin(false))
      return false;
    if (!flash.begin())
      return false;
    const uint32_t signature = staticSignature(data);
    if (!flash.isCached(signature))
    {
      Serial.println("FLASH: cache ausente ou layout alterado. Gerando imagem-base uma vez...");
      const uint32_t started = millis();
      if (!createBase(data, signature))
        return false;
      Serial.printf("FLASH PERF: geracao inicial=%lu ms\n",
                    (unsigned long)(millis() - started));
    }
    else
    {
      Serial.println("FLASH: cache valido, reaproveitando imagem-base.");
    }

    Icons icons;
    if (!icons.begin())
      Serial.println("ICONS: /icons.bin ausente/invalido (formato WIC2). Exibindo textos.");

    // Inicializa o UC8179 uma unica vez por chamada, nao 8x.
    display.init(115200);
    display.setRotation(0);
    uint32_t readMs = 0;
    uint32_t composeMs = 0;
    uint32_t transferMs = 0;
    bool ok = true;
    for (int16_t y = 0; y < HEIGHT; y += STRIPE)
    {
      const int16_t rows = std::min<int16_t>(STRIPE, HEIGHT - y);
      canvas.beginPage(y, rows);
      uint32_t t = millis();
      if (!flash.readChunk(uint32_t(y) * (WIDTH / 4),
                           canvas.bytes(), size_t(rows) * WIDTH / 4))
      {
        ok = false;
        break;
      }
      readMs += millis() - t;
      t = millis();
      drawNetworkDateHeader(canvas, icons, data.todayISO, wifiConnected, y, y + rows);
      drawDynamicAges(data, y, y + rows);
      drawCompactWeather(canvas, weather, icons, y, y + rows);
      drawAirQuality(canvas, data.airQuality, icons, y, y + rows);
      drawPollen(canvas, data.pollen, icons, y, y + rows);
      drawDemoIndicators(canvas, icons, y, y + rows);
      composeMs += millis() - t;
      t = millis();
      display.writeImage_4G(canvas.bytes(), 2, 0, y, WIDTH, rows);
      transferMs += millis() - t;
    }
    uint32_t refreshMs = 0;
    if (ok)
    {
      const uint32_t t = millis();
      display.refresh(false);
      refreshMs = millis() - t;
    }
    display.epd2.hibernate();
    icons.end();
    Serial.printf("DISPLAY FLASH PERF: leitura=%lu ms; textos=%lu ms; transferencia=%lu ms; refresh=%lu ms; TOTAL=%lu ms; heap=%u\n",
                  (unsigned long)readMs, (unsigned long)composeMs,
                  (unsigned long)transferMs, (unsigned long)refreshMs,
                  (unsigned long)(millis() - totalStart), ESP.getFreeHeap());
    Serial.printf("DISPLAY MEM: heap8=%u; maior bloco8=%u; buffer GxEPD2=1600 B; PageCanvas=6400 B\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    return ok;
  }
} // namespace

void DisplayManager::render(const DashboardData &data)
{
  // Todas as fontes sao abertas uma unica vez e ficam disponiveis
  // tanto na geracao do cache quanto nos dois caminhos de renderizacao.
  if (!LittleFS.begin(false))
  {
    Serial.println("DISPLAY: LittleFS indisponivel; renderizacao cancelada.");
    return;
  }
  if (!beginFonts())
    return;

  const WeatherData *weather = data.weather;
  if (!renderFlash(data, weather, _wifiConnected))
  {
    Serial.println("DISPLAY: W25Q16 indisponivel. Usando renderizador anterior, sem gravar na Flash.");
    renderLegacy(data, weather, _wifiConnected);
  }
  endFonts();
}

void DisplayManager::show(
    const std::vector<Habit> &habits,
    const WeatherData *weather)
{
  DashboardData data;
  data.habits = &habits;
  data.weather = weather;
  render(data);
}

int DisplayManager::habitIndexAt(int16_t x, int16_t y, size_t habitCount) const
{
  const size_t count = std::min<size_t>(habitCount, MAX_VISIBLE_HABITS);
  for (size_t i = 0; i < count; ++i)
  {
    const int16_t tileX = TILE_X + (i % 3) * TILE_STEP_X;
    const int16_t tileY = TILE_Y + (i / 3) * TILE_STEP_Y;
    if (x >= tileX && x < tileX + TILE_W &&
        y >= tileY && y < tileY + TILE_H)
      return static_cast<int>(i);
  }
  return -1;
}

// ============================================================================
// MODO VISITAS: mostra /image.bin diretamente do LittleFS.
// Mesmo display e mesmo SPI utilizados pelo dashboard. Nao aloca 96 KB.
// O arquivo deve ser RAW: 800 x 480, 4 tons (2 bits/pixel), 96.000 bytes.
// ============================================================================
bool DisplayManager::showPrivacyImage()
{
  constexpr int16_t IMAGE_W = 800;
  constexpr int16_t IMAGE_H = 480;
  constexpr int16_t ROWS_PER_CHUNK = 8;
  constexpr size_t CHUNK_BYTES = IMAGE_W * ROWS_PER_CHUNK / 4;
  constexpr size_t IMAGE_BYTES = IMAGE_W * IMAGE_H / 4;

  if (!LittleFS.begin(false))
  {
    Serial.println("PRIVACIDADE: nao foi possivel montar LittleFS.");
    return false;
  }

  File file = LittleFS.open("/image.bin", "r");
  if (!file)
  {
    Serial.println("PRIVACIDADE: /image.bin nao encontrado.");
    return false;
  }
  if (file.size() != IMAGE_BYTES)
  {
    Serial.printf("PRIVACIDADE: arquivo com %lu bytes; esperado %lu.\n",
                  (unsigned long)file.size(), (unsigned long)IMAGE_BYTES);
    file.close();
    return false;
  }

  // Apenas 1.600 bytes na stack da task loop, nao 96.000 na RAM.
  uint8_t buffer[CHUNK_BYTES];

  // Pinos SPI e modo de atualizacao iguais aos de renderFlash().
  SPI.begin(18, 19, 23, 27);
  display.init(115200);
  display.setRotation(0);

  bool ok = true;
  const uint32_t started = millis();
  for (int16_t y = 0; y < IMAGE_H; y += ROWS_PER_CHUNK)
  {
    const size_t n = file.read(buffer, CHUNK_BYTES);
    if (n != CHUNK_BYTES)
    {
      Serial.printf("PRIVACIDADE: falha ao ler a faixa y=%d (%lu/%lu).\n",
                    y, (unsigned long)n, (unsigned long)CHUNK_BYTES);
      ok = false;
      break;
    }
    // Mesma ordem gray2bpp usada por image.bin e pelo framebuffer W25Q16.
    display.writeImage_4G(buffer, 2, 0, y, IMAGE_W, ROWS_PER_CHUNK);
  }
  file.close();

  if (ok)
  {
    display.refresh(false);
    Serial.printf("PRIVACIDADE: imagem exibida (%lu ms).\n",
                  (unsigned long)(millis() - started));
  }
  display.epd2.hibernate();
  return ok;
}
