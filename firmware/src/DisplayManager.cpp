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
#include <Adafruit_GFX.h>
#include <string.h>
#include <esp_heap_caps.h>

// Driver e pinos IDÊNTICOS aos do teste que funcionou no GDEY075T7.
static GxEPD2_4G_4G<GxEPD2_750_GDEY075T7, 8> display(
    GxEPD2_750_GDEY075T7(27, 14, 12, 13));
static FlashManager flash; // compartilhado pelo renderizador rapido e de emergencia

// Use os caminhos QFNT que ja funcionaram no seu projeto.
static const char *FONT_14_PATH = "/fonts/InterDisplay-SemiBold_14.bin";
static const char *FONT_DATE_PATH = "/fonts/InterDisplay-SemiBold_20.bin";
static const char *FONT_TITLE_PATH = "/fonts/InterDisplay-SemiBold_24.bin";
static const char *FONT_BODY_PATH = "/fonts/InterDisplay-SemiBold_20.bin";
static const char *FONT_HABIT_16_PATH = "/fonts/InterDisplay-SemiBold_16.bin";
static const char *FONT_32_PATH = "/fonts/InterDisplay-SemiBold_32.bin";

namespace
{
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

  void drawEmptyPanels()
  {
    // Renderizador legado: mesma geometria usada pelo cache da Flash.
    display.fillScreen(GxEPD_WHITE);
    display.fillRect(0, 0, 800, 480, GxEPD_DARKGREY);

    display.fillRoundRect(8, 8, 262, 40, 7, GxEPD_WHITE);
    display.fillRoundRect(HABITS_PANEL_X, HABITS_PANEL_Y,
                          HABITS_PANEL_W, HABITS_PANEL_H, 7, GxEPD_WHITE);
    display.fillRoundRect(278, 8, 514, 202, 7, GxEPD_WHITE);
    display.fillRoundRect(278, 218, 264, 254, 7, GxEPD_WHITE);
    display.fillRoundRect(550, 218, 242, 254, 7, GxEPD_WHITE);

    // Clima em tres faixas: condicao atual; indicadores; previsao de cinco dias.
    // Nenhuma moldura interna nem preenchimento cinza.
    // Separadores conforme mockup: clima / sensacao / max-min / UV.
    // Condicao atual e Sens. dividem uma area, sem linha entre elas.
    // Divisorias apenas antes de max/min e UV.
    display.drawFastVLine(528, 20, 58, GxEPD_LIGHTGREY);
    display.drawFastVLine(638, 20, 58, GxEPD_LIGHTGREY);
    display.drawFastHLine(286, 84, 498, GxEPD_DARKGREY);
    // Segunda faixa: chuva + intensidade | nuvens | vento.
    display.drawFastVLine(502, 91, 39, GxEPD_LIGHTGREY);
    display.drawFastVLine(630, 91, 39, GxEPD_LIGHTGREY);
    display.drawFastHLine(286, 137, 498, GxEPD_DARKGREY);
    for (int col = 1; col < 5; ++col)
      display.drawFastVLine(286 + col * 100 - 4, 146, 57, GxEPD_LIGHTGREY);
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

  // Data e indicador de rede ficam na camada dinamica; a imagem-base W25Q16
  // guarda somente o fundo branco do cabecalho. Assim, desconectar/reconectar
  // nao obriga a reconstruir os 96 KB do cache.
  void drawNetworkDateHeader(Adafruit_GFX &gfx, QFontGfx &dateFont,
                             const String &todayISO, bool wifiConnected,
                             int16_t pageTop, int16_t pageBottom)
  {
    String visible = formatDateHeader(todayISO);
    if (visible.isEmpty())
      return;

    if (!wifiConnected)
    {
      // Remove apenas o dia da semana: "Quinta, 29 de Outubro" -> "29 de Outubro".
      const int comma = visible.indexOf(',');
      if (comma >= 0)
        visible = visible.substring(comma + 2);

      constexpr int16_t ICON_W = 24;
      constexpr int16_t GAP = 8;
      const int16_t textW = dateFont.textWidth(visible);
      const int16_t contentW = textW + GAP + ICON_W;
      const int16_t x = 8 + (262 - contentW) / 2;
      dateFont.drawText(gfx, x, 35, visible, GxEPD_WHITE,
                        pageTop, pageBottom);
      drawWifiOff(gfx, x + textW + GAP, 16);
    }
    else
    {
      if (dateFont.textWidth(visible) > 245)
      {
        const int comma = visible.indexOf(',');
        if (comma > 3)
          visible = visible.substring(0, 3) + visible.substring(comma);
      }
      const int16_t x = 8 + (262 - dateFont.textWidth(visible)) / 2;
      dateFont.drawText(gfx, x, 35, visible, GxEPD_WHITE,
                        pageTop, pageBottom);
    }
  }

  String compactDateHeader(QFontGfx &font, const String &date)
  {
    String label = formatDateHeader(date);
    if (font.textWidth(label) > 245)
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
                     QFontGfx &initialsFont, QFontGfx &daysFont,
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
      drawCenteredText(initialsFont, firstUtf8Character(habit.name),
                       x, TILE_W, y + 49, GxEPD_LIGHTGREY,
                       pageTop, pageBottom);
    }

    const String age = formatLastCompletion(habit.lastCompletedDate, todayISO);
    drawCenteredText(daysFont, age, x, TILE_W, y + 82,
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
    return String(WeatherManager::conditionName(w.currentCode));
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
      return "Sem chuva";
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

  void drawCompactWeather(Adafruit_GFX &gfx, const WeatherData *weather,
                          Icons &icons, QFontGfx &f14,
                          QFontGfx &f20, QFontGfx &f24, QFontGfx &f32,
                          int16_t top, int16_t bottom)
  {
    if (!weather)
    {
      f20.drawText(gfx, 298, 65, "Aguardando previsão do tempo",
                   GxEPD_WHITE, top, bottom);
      return;
    }

    // PRIMEIRA FAIXA (mesma composicao do Photoshop):
    //   [icone] [21°] [Sens. 24°] | [Max. 29° / Min. 19°] | [UV]
    //           [Pancadas de chuva]
    // A condicao e uma unica linha ABAIXO dos dois conjuntos (temperatura + sens.).
    // Nao existe divisoria entre temperatura atual e sensacao termica.
    icons.draw(gfx, weatherIconId(weather->currentCode, weather->isDay, false),
               287, 5, 70, 70, top, bottom);

    // V14: conteudo da faixa superior sobe alguns pixels para equilibrar
    // margens. Temp. principal e grupo Sens./23° tem topo visual alinhado;
    // a descricao e o valor de Min. compartilham a baseline y=73.
    // A temperatura e a descricao comecam EXATAMENTE no mesmo X.
    // Mantemos a temperatura centralizada na faixa original de 73 px,
    // mas reutilizamos seu X real para o texto inferior.
    const String currentText = weatherDegrees(weather->currentTempC);
    constexpr int16_t TEMP_AREA_X = 360;
    constexpr int16_t TEMP_AREA_W = 73;
    const int16_t currentX = TEMP_AREA_X +
                             (TEMP_AREA_W - f32.textWidth(currentText)) / 2;
    f32.drawText(gfx, currentX, 43, currentText,
                 GxEPD_WHITE, top, bottom);

    // 'Sens.' e seu numero usam o MESMO espaco rotulo->valor que Max./Min.
    // Rotulos na mesma linha (baseline=38) e valores na mesma (baseline=44).
    constexpr int16_t LABEL_VALUE_GAP = 8;
    constexpr int16_t SENS_X = 435;
    const String sensLabel = "Sens.";
    const String apparentText = weatherDegrees(weather->apparentTempC);
    f14.drawText(gfx, SENS_X, 31, sensLabel,
                 GxEPD_WHITE, top, bottom);
    f24.drawText(gfx, SENS_X + f14.textWidth(sensLabel) + LABEL_VALUE_GAP,
                 37, apparentText, GxEPD_WHITE, top, bottom);

    // Condicao abaixo de temperatura + sensacao, ALINHADA A ESQUERDA
    // com o inicio do numero da temperatura, nunca centralizada.
    const int16_t conditionWidth = 528 - currentX - 6; // ate a divisoria
    String condition = compactCondition(*weather);
    if (f14.textWidth(condition) > conditionWidth)
    {
      if (condition == "Pancadas de chuva")
        condition = "Pancadas";
      else if (condition == "Pancadas de neve")
        condition = "Neve";
      else if (condition == "Garoa congelante")
        condition = "Garoa";
      else if (condition == "Chuva congelante")
        condition = "Chuva";
      else if (condition == "Parcialmente nublado")
        condition = "Parc. nublado";
    }
    if (f14.textWidth(condition) > conditionWidth)
    {
      const String ellipsis = "...";
      while (!condition.isEmpty() &&
             f14.textWidth(condition + ellipsis) > conditionWidth)
        condition.remove(condition.length() - 1);
      condition += ellipsis;
    }
    f14.drawText(gfx, currentX, 75, condition,
                 GxEPD_WHITE, top, bottom);

    // Maxima e minima: rotulo e valor separados pelos MESMOS 8 px.
    constexpr int16_t MAX_MIN_X = 539;
    const String maxLabel = "Máx.";
    const String minLabel = "Mín.";
    const String maxText = weather->dayCount
                               ? weatherDegrees(weather->days[0].maxC)
                               : String("--");
    const String minText = weather->dayCount
                               ? weatherDegrees(weather->days[0].minC)
                               : String("--");
    f14.drawText(gfx, MAX_MIN_X, 31, maxLabel, GxEPD_WHITE, top, bottom);
    f24.drawText(gfx, MAX_MIN_X + f14.textWidth(maxLabel) + LABEL_VALUE_GAP,
                 37, maxText, GxEPD_WHITE, top, bottom);
    f14.drawText(gfx, MAX_MIN_X, 69, minLabel, GxEPD_WHITE, top, bottom);
    f24.drawText(gfx, MAX_MIN_X + f14.textWidth(minLabel) + LABEL_VALUE_GAP,
                 75, minText, GxEPD_WHITE, top, bottom);

    // UV: desenhado nos 48x48 px nativos do PNG/BIN (sem reducao para 30),
    // com rotulo e numero a direita. Assim os tracos e letras 'UV' nao somem.
    icons.draw(gfx, 303, 645, 18, 48, 48, top, bottom);
    f14.drawText(gfx, 705, 36, "UV máx.", GxEPD_WHITE, top, bottom);
    const String uvNumber = weather->dayCount && isfinite(weather->days[0].uvMax)
                                ? String(static_cast<int>(lroundf(weather->days[0].uvMax)))
                                : String("--");
    f24.drawText(gfx, 705, 73, uvNumber, GxEPD_WHITE, top, bottom);

    // Segunda faixa: proxima chuva (horario + mm/h + prob.) -> nuvens -> vento.
    const String nextRain = nextRainLabel(*weather);
    icons.draw(gfx, 318, 287, 94, 34, 34, top, bottom);
    f14.drawText(gfx, 329, 106, "Próx. chuva", GxEPD_WHITE, top, bottom);
    f20.drawText(gfx, 329, 132, nextRain, GxEPD_WHITE, top, bottom);
    if (weather->hasNextRain)
    {
      const String intensity = isfinite(weather->nextRainMm)
                                   ? String(weather->nextRainMm, 1) + " mm/h"
                                   : String("-- mm/h");
      const String chance = weather->nextRainProbabilityPct >= 0
                                ? String("prob. ") + weather->nextRainProbabilityPct + "%"
                                : String("prob. --");
      f14.drawText(gfx, 417, 106, intensity, GxEPD_WHITE, top, bottom);
      f14.drawText(gfx, 417, 127, chance, GxEPD_WHITE, top, bottom);
    }

    // Reutiliza o icone de nublado pequeno (ID 202) do BIN existente.
    icons.draw(gfx, 202, 506, 93, 37, 37, top, bottom);
    f14.drawText(gfx, 548, 107, "Nuvens", GxEPD_WHITE, top, bottom);
    const String clouds = isfinite(weather->cloudCoverPct)
                              ? String(static_cast<int>(lroundf(weather->cloudCoverPct))) + "%"
                              : String("--");
    f20.drawText(gfx, 548, 132, clouds, GxEPD_WHITE, top, bottom);

    icons.draw(gfx, 302, 637, 96, 30, 30, top, bottom);
    f14.drawText(gfx, 677, 106, "Vento máx.", GxEPD_WHITE, top, bottom);
    const String windNumber = weather->dayCount && isfinite(weather->days[0].windMaxKmh)
                                  ? String(static_cast<int>(lroundf(weather->days[0].windMaxKmh)))
                                  : String("--");
    f24.drawText(gfx, 677, 132, windNumber, GxEPD_WHITE, top, bottom);
    // Mantem o numero grande; somente a unidade fica menor.
    f14.drawText(gfx, 682 + f24.textWidth(windNumber), 132,
                 "km/h", GxEPD_WHITE, top, bottom);

    // Terceira faixa: proximos cinco dias, sem incluir hoje.
    for (uint8_t i = 1; i < weather->dayCount && i <= 5; ++i)
    {
      const int16_t x = 286 + (i - 1) * 100;
      const WeatherDay &day = weather->days[i];
      drawWeatherCentered(gfx, f14, compactForecastDay(day), x, 96, 157,
                          GxEPD_WHITE, top, bottom);
      icons.draw(gfx, weatherIconId(day.weatherCode, true, true),
                 x, 152, 48, 48, top, bottom);
      f20.drawText(gfx, x + 54, 183, weatherDegrees(day.maxC),
                   GxEPD_WHITE, top, bottom);
      f14.drawText(gfx, x + 54, 203, weatherDegrees(day.minC),
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

  QFontGfx dateFont;
  QFontGfx initialsFont;
  QFontGfx habitDays;
  QFontGfx weatherSmall;
  QFontGfx weatherBig;
  if (!dateFont.begin(LittleFS, FONT_DATE_PATH))
  {
    Serial.printf("DISPLAY: fonte QFNT invalida ou ausente: %s\n", FONT_DATE_PATH);
    return;
  }
  if (!initialsFont.begin(LittleFS, FONT_TITLE_PATH))
  {
    Serial.printf("DISPLAY: fonte QFNT invalida ou ausente: %s\n", FONT_TITLE_PATH);
    dateFont.end();
    return;
  }
  if (!habitDays.begin(LittleFS, FONT_HABIT_16_PATH) ||
      !weatherSmall.begin(LittleFS, FONT_14_PATH) ||
      !weatherBig.begin(LittleFS, FONT_32_PATH))
  {
    Serial.println("DISPLAY: fonte climatica 14/32 ausente.");
    habitDays.end();
    weatherSmall.end();
    weatherBig.end();
    initialsFont.end();
    dateFont.end();
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
  if (!dateText.isEmpty() && dateFont.textWidth(dateText) > 245)
  {
    Serial.printf("DISPLAY: aviso - cabecalho com %d pixels (limite 245).\n",
                  dateFont.textWidth(dateText));
  }

  int16_t pageTop = 0;
  display.firstPage();
  do
  {
    const int16_t pageBottom = std::min<int16_t>(pageTop + PAGE_HEIGHT,
                                                 SCREEN_HEIGHT);
    drawEmptyPanels();
    drawNetworkDateHeader(display, dateFont, data.todayISO, wifiConnected, pageTop, pageBottom);
    drawCompactWeather(display, weather, icons, weatherSmall,
                       dateFont, initialsFont, weatherBig, pageTop, pageBottom);
    if (data.habits)
    {
      const size_t count = std::min<size_t>(data.habits->size(), MAX_VISIBLE_HABITS);
      for (size_t i = 0; i < count; ++i)
      {
        drawHabitTile((*data.habits)[i], i, data.todayISO,
                      initialsFont, habitDays, pageTop, pageBottom);
      }
    }
    pageTop += PAGE_HEIGHT;
  } while (display.nextPage());

  display.epd2.hibernate();
  icons.end();
  weatherBig.end();
  weatherSmall.end();
  habitDays.end();
  initialsFont.end();
  dateFont.end();
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
    h = addString(h, "EinkDash-base-v17-wifi-dynamic-header");
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

  void drawBaseLayout(PageCanvas &gfx)
  {
    gfx.fillScreen(GxEPD_DARKGREY);
    gfx.fillRoundRect(8, 8, 262, 40, 7, GxEPD_WHITE);
    gfx.fillRoundRect(HABITS_PANEL_X, HABITS_PANEL_Y,
                      HABITS_PANEL_W, HABITS_PANEL_H, 7, GxEPD_WHITE);
    gfx.fillRoundRect(278, 8, 514, 202, 7, GxEPD_WHITE);
    gfx.fillRoundRect(278, 218, 264, 254, 7, GxEPD_WHITE);
    gfx.fillRoundRect(550, 218, 242, 254, 7, GxEPD_WHITE);

    // Mesmas tres faixas do renderizador legado; sem bordas internas.
    // Separadores conforme mockup: clima / sensacao / max-min / UV.
    // Condicao atual e Sens. dividem uma area, sem linha entre elas.
    // Divisorias apenas antes de max/min e UV.
    gfx.drawFastVLine(528, 20, 58, GxEPD_LIGHTGREY);
    gfx.drawFastVLine(638, 20, 58, GxEPD_LIGHTGREY);
    gfx.drawFastHLine(286, 84, 498, GxEPD_DARKGREY);
    // Segunda faixa: chuva + intensidade | nuvens | vento.
    gfx.drawFastVLine(502, 91, 39, GxEPD_LIGHTGREY);
    gfx.drawFastVLine(630, 91, 39, GxEPD_LIGHTGREY);
    gfx.drawFastHLine(286, 137, 498, GxEPD_DARKGREY);
    for (int col = 1; col < 5; ++col)
      gfx.drawFastVLine(286 + col * 100 - 4, 146, 57, GxEPD_LIGHTGREY);
  }

  void drawBaseHabit(PageCanvas &gfx, const Habit &habit, size_t i,
                     QFontGfx &initials, int16_t top, int16_t bottom)
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
      const int16_t posX = x + (TILE_W - initials.textWidth(initial)) / 2;
      initials.drawText(gfx, posX, y + 49, initial, GxEPD_LIGHTGREY, top, bottom);
    }
    // A linha de "Hoje" / "Xd" fica VAZIA na memoria Flash.
  }

  bool createBase(const DashboardData &data, uint32_t key)
  {
    QFontGfx initials;
    if (!initials.begin(LittleFS, FONT_TITLE_PATH))
    {
      Serial.println("FLASH: fonte dos icones ausente. Cache cancelado.");
      return false;
    }
    if (!flash.startWrite(key))
    {
      initials.end();
      return false;
    }
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
          drawBaseHabit(canvas, (*data.habits)[i], i, initials, y, y + rows);
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
    initials.end();
    if (ok)
      Serial.println("FLASH: imagem-base de 96.000 bytes salva/verificada.");
    return ok;
  }

  void drawDynamicAges(const DashboardData &data, QFontGfx &font,
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
      const int16_t textX = x + (TILE_W - font.textWidth(value)) / 2;
      font.drawText(canvas, textX, y + 82, value,
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

    QFontGfx dateFont;
    QFontGfx daysFont;
    QFontGfx weatherSmall;
    QFontGfx weatherBody;
    QFontGfx weatherValue;
    QFontGfx weatherBig;
    if (!dateFont.begin(LittleFS, FONT_DATE_PATH) ||
        !daysFont.begin(LittleFS, FONT_HABIT_16_PATH) ||
        !weatherSmall.begin(LittleFS, FONT_14_PATH) ||
        !weatherBody.begin(LittleFS, FONT_BODY_PATH) ||
        !weatherValue.begin(LittleFS, FONT_TITLE_PATH) ||
        !weatherBig.begin(LittleFS, FONT_32_PATH))
    {
      Serial.println("FLASH: QFNT InterDisplay 14/16/20/24/32 ausente.");
      dateFont.end();
      daysFont.end();
      weatherSmall.end();
      weatherBody.end();
      weatherValue.end();
      weatherBig.end();
      return false;
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
      drawNetworkDateHeader(canvas, dateFont, data.todayISO, wifiConnected, y, y + rows);
      drawDynamicAges(data, daysFont, y, y + rows);
      drawCompactWeather(canvas, weather, icons,
                         weatherSmall, weatherBody, weatherValue, weatherBig,
                         y, y + rows);
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
    weatherBig.end();
    weatherValue.end();
    weatherBody.end();
    weatherSmall.end();
    daysFont.end();
    dateFont.end();
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
  // O WeatherManager so modifica seus dados no loop(). O ponteiro e estavel,
  // inclusive quando o touch dispara um redraw local de habitos.
  const WeatherData *weather = _weather && _weather->hasData() ? &_weather->data() : nullptr;
  if (renderFlash(data, weather, _wifiConnected))
    return;
  Serial.println("DISPLAY: W25Q16 indisponivel. Usando renderizador anterior, sem gravar na Flash.");
  renderLegacy(data, weather, _wifiConnected);
}

void DisplayManager::showHabits(const std::vector<Habit> &habits)
{
  DashboardData data;
  data.habits = &habits;
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
