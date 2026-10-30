#ifndef EINKDASH_DISPLAY_MANAGER_H
#define EINKDASH_DISPLAY_MANAGER_H

#include <vector>
#include "Habit.h"
#include "DashboardData.h"
#include "CalendarData.h"
#include "ScreenTimeDetails.h"

struct WeatherData;

class DisplayManager
{
public:
  // O cabecalho exibe o icone de Wi-Fi cortado quando false.
  // Aplicado na proxima renderizacao do dashboard.
  void setWifiConnected(bool connected) { _wifiConnected = connected; }

  // Um conjunto de dados -> uma atualizacao completa do e-ink.
  void render(const DashboardData &data);

  // Mostra /image.bin (800x480, 2 bpp) do LittleFS em faixas de 8 linhas.
  // Retorna false se o arquivo estiver ausente, errado ou nao puder ser lido.
  bool showPrivacyImage();

  // Desenha a agenda de dois cards diretamente no canvas 4 tons.
  // Sem BIN de imagem, sem fundo fotografico e sem Google Tasks.
  bool renderCalendar(const CalendarDay &selected,
                      const std::vector<CalendarBirthday> &birthdays);

  // Nova tela de detalhes, renderizada em 4 tons sem BIN novo.
  bool renderScreenTimeDetails(const ScreenTimeDetails &details);

  // Usa as mesmas coordenadas dos cards de render(), ordem do vetor _habits.
  int habitIndexAt(int16_t x, int16_t y, size_t habitCount) const;

private:
  bool _wifiConnected = true;
};

#endif
