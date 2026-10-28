#include "DisplayManager.h"

#include <Arduino.h>
#include <SPI.h>
#include <LittleFS.h>
#include <GxEPD2_4G_4G.h>
#include <algorithm>
#include <stdio.h>
#include "QFontGfx.h"
#include "FlashManager.h"
#include <Adafruit_GFX.h>
#include <string.h>

// Driver e pinos IDÊNTICOS aos do teste que funcionou no GDEY075T7.
static GxEPD2_4G_4G<GxEPD2_750_GDEY075T7, 64> display(
    GxEPD2_750_GDEY075T7(27, 14, 12, 13));

// Use os caminhos QFNT que ja funcionaram no seu projeto.
static const char* FONT_DATE_PATH = "/fonts/LatoBold_16.bin";
static const char* FONT_TITLE_PATH = "/fonts/LatoBold_32.bin";
static const char* FONT_BODY_PATH = "/fonts/LatoBold_24.bin";

namespace {
constexpr int16_t SCREEN_HEIGHT = 480;
constexpr int16_t PAGE_HEIGHT = 64;

// Geometria do mockup reduzido proporcionalmente para 800 x 480.
constexpr int16_t HABITS_PANEL_X = 8;
constexpr int16_t HABITS_PANEL_Y = 56;
constexpr int16_t HABITS_PANEL_W = 262;
constexpr int16_t HABITS_PANEL_H = 416;

constexpr int16_t TILE_X = 16;
constexpr int16_t TILE_Y = 62;
constexpr int16_t TILE_W = 77;
constexpr int16_t TILE_H = 93;
constexpr int16_t TILE_STEP_X = 85;
constexpr int16_t TILE_STEP_Y = 101;
constexpr size_t MAX_VISIBLE_HABITS = 12;

void drawEmptyPanels() {
  // Fundo em cinza-escuro: moldura externa e divisorias dos paineis.
  // Mantemos fillScreen(WHITE) e fillRect para evitar o problema
  // de listras observado ao usar fillScreen(BLACK).
  display.fillScreen(GxEPD_WHITE);
  display.fillRect(0, 0, 800, 480, GxEPD_DARKGREY);

  // Margem externa: 8 px em todos os lados.
  // Espacamento entre os paineis: 8 px.
  display.fillRoundRect(8, 8, 262, 40, 7, GxEPD_WHITE); // Data

  display.fillRoundRect(HABITS_PANEL_X, HABITS_PANEL_Y,
                        HABITS_PANEL_W, HABITS_PANEL_H, 7, GxEPD_WHITE); // Habitos

  display.fillRoundRect(278, 8, 514, 282, 7, GxEPD_WHITE);   // Clima
  display.fillRoundRect(278, 298, 264, 174, 7, GxEPD_WHITE); // Agenda
  display.fillRoundRect(550, 298, 242, 174, 7, GxEPD_WHITE); // Sensores
}

String firstUtf8Character(const String& value) {
  if (value.isEmpty()) return String();
  const uint8_t first = static_cast<uint8_t>(value[0]);
  size_t bytes = 1;
  if ((first & 0xE0) == 0xC0) bytes = 2;
  else if ((first & 0xF0) == 0xE0) bytes = 3;
  else if ((first & 0xF8) == 0xF0) bytes = 4;
  if (bytes > value.length()) return String();
  return value.substring(0, bytes);
}

bool parseISODate(const String& iso, int& year, int& month, int& day) {
  if (iso.length() != 10 || iso[4] != '-' || iso[7] != '-') return false;
  for (int i = 0; i < 10; ++i) {
    if (i == 4 || i == 7) continue;
    if (iso[i] < '0' || iso[i] > '9') return false;
  }
  year = iso.substring(0, 4).toInt();
  month = iso.substring(5, 7).toInt();
  day = iso.substring(8, 10).toInt();
  return year >= 2000 && month >= 1 && month <= 12 && day >= 1 && day <= 31;
}

// Numero de dias do calendario civil: nao depende de fuso ou horario de verao.
int32_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned yoeDays = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int32_t>(yoeDays);
}

// Dia da semana e mes em portugues, calculados a partir de todayISO.
// Nao depende do timezone global nem chama NTP durante a renderizacao.
String formatDateHeader(const String& todayISO) {
  int year, month, day;
  if (!parseISODate(todayISO, year, month, day)) return "";

  static const char* DAYS[] = {
      "Domingo", "Segunda", "Terça", "Quarta",
      "Quinta", "Sexta", "Sábado"};
  static const char* MONTHS[] = {
      "Janeiro", "Fevereiro", "Março", "Abril", "Maio", "Junho",
      "Julho", "Agosto", "Setembro", "Outubro", "Novembro", "Dezembro"};

  // 01/01/1970 foi quinta-feira (indice 4).
  const int32_t delta = daysFromCivil(year, month, day) -
                        daysFromCivil(1970, 1, 1);
  const int weekday = ((delta + 4) % 7 + 7) % 7;

  return String(DAYS[weekday]) + ", " + String(day) +
         " de " + MONTHS[month - 1];
}

void drawDateHeader(QFontGfx& dateFont, const String& todayISO,
                    int16_t pageTop, int16_t pageBottom) {
  const String text = formatDateHeader(todayISO);
  if (text.isEmpty()) return;

  // O cabeçalho fica entre x=8..269 e y=8..47.
  // Fonte QFNT real, com UTF-8, sem fallback para fonte padrão.
  dateFont.drawText(display, 20, 36, text, GxEPD_WHITE,
                    pageTop, pageBottom);
}

String formatLastCompletion(const String& lastISO, const String& todayISO) {
  if (lastISO.isEmpty()) return "-"; // Nunca realizado

  int ly, lm, ld, ty, tm, td;
  if (!parseISODate(lastISO, ly, lm, ld)) return "-";
  if (!parseISODate(todayISO, ty, tm, td)) {
    // Sem relogio valido: mostra data REAL, nunca uma idade inventada.
    return lastISO.substring(8, 10) + "/" + lastISO.substring(5, 7);
  }

  const int32_t days = daysFromCivil(ty, tm, td) - daysFromCivil(ly, lm, ld);
  if (days < 0) return "-";
  if (days == 0) return "Hoje";

  const int months = (ty - ly) * 12 + (tm - lm) - (td < ld ? 1 : 0);
  if (months < 1) return String(days) + "d";
  if (months < 12) return String(months) + "m";
  return String(months / 12) + "a";
}

void drawCenteredText(QFontGfx& font, const String& text,
                      int16_t x, int16_t width, int16_t baseline,
                      uint16_t background,
                      int16_t pageTop, int16_t pageBottom) {
  const int16_t textWidth = font.textWidth(text);
  const int16_t centeredX = x + (width - textWidth) / 2;
  font.drawText(display, centeredX, baseline, text, background,
                pageTop, pageBottom);
}

void drawHabitTile(const Habit& habit, size_t index, const String& todayISO,
                   QFontGfx& initialsFont, QFontGfx& daysFont,
                   int16_t pageTop, int16_t pageBottom) {
  const int16_t x = TILE_X + (index % 3) * TILE_STEP_X;
  const int16_t y = TILE_Y + (index / 3) * TILE_STEP_Y;

  display.fillRoundRect(x, y, TILE_W, TILE_H, 7, GxEPD_LIGHTGREY);

  // O formato gray2bpp ja foi decodificado pelo FirebaseManager.
  const size_t expectedIconBytes = 48 * 48 / 4;
  if (habit.hasIcon() && habit.iconWidth == 48 && habit.iconHeight == 48 &&
      habit.iconBytes.size() == expectedIconBytes) {
    // Desenha os quatro tons como recebidos do aplicativo, incluindo branco.
    display.drawGreyPixmap(habit.iconBytes.data(), 2,
                           x + 14, y + 7, 48, 48);
  } else {
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

} // namespace

static void renderLegacy(const DashboardData& data) {
  const size_t total = data.habits ? data.habits->size() : 0;
  Serial.printf("DISPLAY: montando dashboard (%u habitos).\n", (unsigned)total);
  Serial.printf("DISPLAY: heap antes: %u bytes\n", ESP.getFreeHeap());

  if (!LittleFS.begin(false)) {
    Serial.println("DISPLAY: LittleFS indisponivel; renderizacao cancelada.");
    return;
  }

  QFontGfx dateFont;
  QFontGfx initialsFont;
  QFontGfx daysFont;
  if (!dateFont.begin(LittleFS, FONT_DATE_PATH)) {
    Serial.printf("DISPLAY: fonte QFNT invalida ou ausente: %s\n", FONT_DATE_PATH);
    return;
  }
  if (!initialsFont.begin(LittleFS, FONT_TITLE_PATH)) {
    Serial.printf("DISPLAY: fonte QFNT invalida ou ausente: %s\n", FONT_TITLE_PATH);
    dateFont.end();
    return;
  }
  if (!daysFont.begin(LittleFS, FONT_BODY_PATH)) {
    Serial.printf("DISPLAY: fonte QFNT invalida ou ausente: %s\n", FONT_BODY_PATH);
    initialsFont.end();
    dateFont.end();
    return;
  }

  SPI.begin(18, 19, 23, 27);
  display.init(115200);
  display.setRotation(0);
  display.setFullWindow();
  Serial.printf("DISPLAY: area enderecavel %d x %d pixels.\n",
                display.width(), display.height());
  const String dateText = formatDateHeader(data.todayISO);
  Serial.print("DISPLAY: cabecalho = ");
  Serial.println(dateText.isEmpty() ? "DATA INDISPONIVEL" : dateText);
  if (!dateText.isEmpty() && dateFont.textWidth(dateText) > 245) {
    Serial.printf("DISPLAY: aviso - cabecalho com %d pixels (limite 245).\n",
                  dateFont.textWidth(dateText));
  }

  int16_t pageTop = 0;
  display.firstPage();
  do {
    const int16_t pageBottom = std::min<int16_t>(pageTop + PAGE_HEIGHT,
                                                  SCREEN_HEIGHT);
    drawEmptyPanels();
    drawDateHeader(dateFont, data.todayISO, pageTop, pageBottom);
    if (data.habits) {
      const size_t count = std::min<size_t>(data.habits->size(), MAX_VISIBLE_HABITS);
      for (size_t i = 0; i < count; ++i) {
        drawHabitTile((*data.habits)[i], i, data.todayISO,
                      initialsFont, daysFont, pageTop, pageBottom);
      }
    }
    pageTop += PAGE_HEIGHT;
  } while (display.nextPage());

  display.epd2.hibernate();
  daysFont.end();
  initialsFont.end();
  dateFont.end();
  Serial.printf("DISPLAY: concluido. Heap livre: %u bytes\n", ESP.getFreeHeap());
}


// ============================================================================
// CACHE PERSISTENTE: imagem-base 800x480 / 2 bpp no W25Q16.
// Datas dos habitos NAO sao incluidas na base; apenas sao compostas em RAM.
// Nenhum framebuffer de 96 KB e alocado no ESP32.
// ============================================================================

namespace {
constexpr int16_t WIDTH = 800;
constexpr int16_t HEIGHT = 480;
constexpr int16_t STRIPE = 64;
constexpr size_t STRIPE_BYTES = WIDTH * STRIPE / 4;

FlashManager flash;

// Adafruit_GFX real, com um framebuffer de apenas 64 linhas.
// Permite usar QFontGfx sem copiar nem alterar a biblioteca de fontes.
class PageCanvas final : public Adafruit_GFX {
public:
  PageCanvas() : Adafruit_GFX(800, 480) { memset(_buf, 0xFF, sizeof(_buf)); }

  void beginPage(int16_t y, int16_t h) {
    _pageY = y;
    _pageH = h;
    memset(_buf, 0xFF, sizeof(_buf));
  }
  uint8_t* bytes() { return _buf; }
  const uint8_t* bytes() const { return _buf; }

  void drawPixel(int16_t x, int16_t y, uint16_t color) override {
    if (x < 0 || x >= WIDTH || y < _pageY || y >= _pageY + _pageH) return;
    uint8_t v;
    if (color == GxEPD_WHITE) v = 3;
    else if (color == GxEPD_LIGHTGREY) v = 2;
    else if (color == GxEPD_DARKGREY) v = 1;
    else v = 0;
    const size_t at = size_t(y - _pageY) * (WIDTH / 4) + (x >> 2);
    const uint8_t shift = 2 * (3 - (x & 3));
    _buf[at] = (_buf[at] & ~(uint8_t(3) << shift)) | (v << shift);
  }
  void fillScreen(uint16_t color) override {
    uint8_t v = color == GxEPD_WHITE ? 3 :
                color == GxEPD_LIGHTGREY ? 2 :
                color == GxEPD_DARKGREY ? 1 : 0;
    memset(_buf, v * 0x55, sizeof(_buf));
  }
  // Mesma ordem de pixels de GxEPD2_4G_4G::drawGreyPixmap(depth=2):
  // MSB primeiro, 4 pixels por byte. Preserva branco opaco do icone.
  void drawGreyPixmap(const uint8_t* src, int16_t x, int16_t y,
                      int16_t w, int16_t h) {
    const uint16_t shades[] = {GxEPD_BLACK, GxEPD_DARKGREY,
                               GxEPD_LIGHTGREY, GxEPD_WHITE};
    const int16_t stride = (w + 3) / 4;
    for (int16_t j = 0; j < h; ++j) {
      if (y + j < _pageY || y + j >= _pageY + _pageH) continue;
      for (int16_t i = 0; i < w; ++i) {
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
uint32_t addBytes(uint32_t h, const uint8_t* bytes, size_t n) {
  for (size_t i = 0; i < n; ++i) { h ^= bytes[i]; h *= 16777619UL; }
  return h;
}
uint32_t addString(uint32_t h, const String& s) {
  h = addBytes(h, reinterpret_cast<const uint8_t*>(s.c_str()), s.length());
  const uint8_t delim = 0xFF;
  return addBytes(h, &delim, 1);
}
uint32_t staticSignature(const DashboardData& data) {
  uint32_t h = 2166136261UL;
  h = addString(h, "EinkDash-base-v1-borda-cinza-8-fonte-QFNT");
  h = addString(h, data.todayISO);
  const size_t count = data.habits ? std::min<size_t>(data.habits->size(), MAX_VISIBLE_HABITS) : 0;
  uint8_t n = static_cast<uint8_t>(count);
  h = addBytes(h, &n, 1);
  for (size_t i = 0; i < count; ++i) {
    const Habit& habit = (*data.habits)[i];
    h = addString(h, habit.id);
    h = addString(h, habit.name);
    h = addBytes(h, reinterpret_cast<const uint8_t*>(&habit.iconWidth), sizeof(habit.iconWidth));
    h = addBytes(h, reinterpret_cast<const uint8_t*>(&habit.iconHeight), sizeof(habit.iconHeight));
    const uint32_t iconSize = habit.iconBytes.size();
    h = addBytes(h, reinterpret_cast<const uint8_t*>(&iconSize), sizeof(iconSize));
    if (iconSize) h = addBytes(h, habit.iconBytes.data(), iconSize);
  }
  return h;
}

void drawBaseLayout(PageCanvas& gfx) {
  gfx.fillScreen(GxEPD_DARKGREY);
  gfx.fillRoundRect(8, 8, 262, 40, 7, GxEPD_WHITE);
  gfx.fillRoundRect(HABITS_PANEL_X, HABITS_PANEL_Y,
                    HABITS_PANEL_W, HABITS_PANEL_H, 7, GxEPD_WHITE);
  gfx.fillRoundRect(278, 8, 514, 282, 7, GxEPD_WHITE);
  gfx.fillRoundRect(278, 298, 264, 174, 7, GxEPD_WHITE);
  gfx.fillRoundRect(550, 298, 242, 174, 7, GxEPD_WHITE);
}

void drawBaseHabit(PageCanvas& gfx, const Habit& habit, size_t i,
                   QFontGfx& initials, int16_t top, int16_t bottom) {
  const int16_t x = TILE_X + (i % 3) * TILE_STEP_X;
  const int16_t y = TILE_Y + (i / 3) * TILE_STEP_Y;
  if (y >= bottom || y + TILE_H <= top) return;
  gfx.fillRoundRect(x, y, TILE_W, TILE_H, 7, GxEPD_LIGHTGREY);
  if (habit.hasIcon() && habit.iconWidth == 48 && habit.iconHeight == 48 &&
      habit.iconBytes.size() == 48 * 48 / 4) {
    gfx.drawGreyPixmap(habit.iconBytes.data(), x + 14, y + 7, 48, 48);
  } else {
    const String initial = firstUtf8Character(habit.name);
    const int16_t posX = x + (TILE_W - initials.textWidth(initial)) / 2;
    initials.drawText(gfx, posX, y + 49, initial, GxEPD_LIGHTGREY, top, bottom);
  }
  // A linha de "Hoje" / "Xd" fica VAZIA na memoria Flash.
}

bool createBase(const DashboardData& data, uint32_t key) {
  QFontGfx dateFont;
  QFontGfx initials;
  if (!dateFont.begin(LittleFS, FONT_DATE_PATH)) {
    Serial.println("FLASH: fonte de data ausente. Cache cancelado.");
    return false;
  }
  if (!initials.begin(LittleFS, FONT_TITLE_PATH)) {
    dateFont.end();
    Serial.println("FLASH: fonte dos icones ausente. Cache cancelado.");
    return false;
  }
  if (!flash.startWrite(key)) {
    initials.end(); dateFont.end();
    return false;
  }
  const String dateText = formatDateHeader(data.todayISO);
  bool ok = true;
  for (int16_t y = 0; y < HEIGHT; y += STRIPE) {
    const int16_t rows = std::min<int16_t>(STRIPE, HEIGHT - y);
    canvas.beginPage(y, rows);
    drawBaseLayout(canvas);
    if (!dateText.isEmpty())
      dateFont.drawText(canvas, 20, 36, dateText, GxEPD_WHITE, y, y + rows);
    if (data.habits) {
      const size_t count = std::min<size_t>(data.habits->size(), MAX_VISIBLE_HABITS);
      for (size_t i = 0; i < count; ++i)
        drawBaseHabit(canvas, (*data.habits)[i], i, initials, y, y + rows);
    }
    const uint32_t offset = uint32_t(y) * (WIDTH / 4);
    if (!flash.writeChunk(offset, canvas.bytes(), size_t(rows) * WIDTH / 4)) {
      Serial.printf("FLASH: falha ao gravar faixa y=%d.\n", y);
      ok = false; break;
    }
  }
  if (ok) ok = flash.finishWrite();
  initials.end(); dateFont.end();
  if (ok) Serial.println("FLASH: imagem-base de 96.000 bytes salva/verificada.");
  return ok;
}

void drawDynamicAges(const DashboardData& data, QFontGfx& font,
                     int16_t top, int16_t bottom) {
  if (!data.habits) return;
  const size_t count = std::min<size_t>(data.habits->size(), MAX_VISIBLE_HABITS);
  for (size_t i = 0; i < count; ++i) {
    const int16_t x = TILE_X + (i % 3) * TILE_STEP_X;
    const int16_t y = TILE_Y + (i / 3) * TILE_STEP_Y;
    // Fonte 24px: tinta proxima a linha de base y+82; faixas sem
    // intersecao nao precisam abrir glifos.
    if (y + 88 < top || y + 52 >= bottom) continue;
    const String value = formatLastCompletion((*data.habits)[i].lastCompletedDate, data.todayISO);
    const int16_t textX = x + (TILE_W - font.textWidth(value)) / 2;
    font.drawText(canvas, textX, y + 82, value,
                  GxEPD_LIGHTGREY, top, bottom);
  }
}

// Reenvia os 96 KB prontos por faixas, sem redesenhar layout nem icones.
// A tela permanece em 4 tons e a atualizacao continua sendo total.
bool renderFlash(const DashboardData& data) {
  const uint32_t totalStart = millis();
  if (!LittleFS.begin(false)) return false;
  if (!flash.begin()) return false;
  const uint32_t signature = staticSignature(data);
  if (!flash.isCached(signature)) {
    Serial.println("FLASH: cache ausente ou layout alterado. Gerando imagem-base uma vez...");
    const uint32_t started = millis();
    if (!createBase(data, signature)) return false;
    Serial.printf("FLASH PERF: geracao inicial=%lu ms\n",
                  (unsigned long)(millis() - started));
  } else {
    Serial.println("FLASH: cache valido, reaproveitando imagem-base.");
  }

  QFontGfx daysFont;
  if (!daysFont.begin(LittleFS, FONT_BODY_PATH)) {
    Serial.println("FLASH: fonte QFNT de dias indisponivel.");
    return false;
  }

  // Inicializa o UC8179 uma unica vez por chamada, nao 8x.
  display.init(115200);
  display.setRotation(0);
  uint32_t readMs = 0;
  uint32_t composeMs = 0;
  uint32_t transferMs = 0;
  bool ok = true;
  for (int16_t y = 0; y < HEIGHT; y += STRIPE) {
    const int16_t rows = std::min<int16_t>(STRIPE, HEIGHT - y);
    canvas.beginPage(y, rows);
    uint32_t t = millis();
    if (!flash.readChunk(uint32_t(y) * (WIDTH / 4),
                         canvas.bytes(), size_t(rows) * WIDTH / 4)) {
      ok = false; break;
    }
    readMs += millis() - t;
    t = millis();
    drawDynamicAges(data, daysFont, y, y + rows);
    composeMs += millis() - t;
    t = millis();
    display.writeImage_4G(canvas.bytes(), 2, 0, y, WIDTH, rows);
    transferMs += millis() - t;
  }
  uint32_t refreshMs = 0;
  if (ok) {
    const uint32_t t = millis();
    display.refresh(false);
    refreshMs = millis() - t;
  }
  display.epd2.hibernate();
  daysFont.end();
  Serial.printf("DISPLAY FLASH PERF: leitura=%lu ms; textos=%lu ms; transferencia=%lu ms; refresh=%lu ms; TOTAL=%lu ms; heap=%u\n",
                (unsigned long)readMs, (unsigned long)composeMs,
                (unsigned long)transferMs, (unsigned long)refreshMs,
                (unsigned long)(millis() - totalStart), ESP.getFreeHeap());
  return ok;
}
} // namespace

void DisplayManager::render(const DashboardData& data) {
  if (renderFlash(data)) return;
  Serial.println("DISPLAY: W25Q16 indisponivel. Usando renderizador anterior, sem gravar na Flash.");
  renderLegacy(data);
}

void DisplayManager::showHabits(const std::vector<Habit>& habits) {
  DashboardData data;
  data.habits = &habits;
  render(data);
}

int DisplayManager::habitIndexAt(int16_t x, int16_t y, size_t habitCount) const {
  const size_t count = std::min<size_t>(habitCount, MAX_VISIBLE_HABITS);
  for (size_t i = 0; i < count; ++i) {
    const int16_t tileX = TILE_X + (i % 3) * TILE_STEP_X;
    const int16_t tileY = TILE_Y + (i / 3) * TILE_STEP_Y;
    if (x >= tileX && x < tileX + TILE_W &&
        y >= tileY && y < tileY + TILE_H)
      return static_cast<int>(i);
  }
  return -1;
}
