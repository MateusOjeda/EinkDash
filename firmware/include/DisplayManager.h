#ifndef EINKDASH_DISPLAY_MANAGER_H
#define EINKDASH_DISPLAY_MANAGER_H

#include <vector>
#include "Habit.h"
#include "DashboardData.h"

struct WeatherData;

class DisplayManager {
public:
  // O cabecalho exibe o icone de Wi-Fi cortado quando false.
  // Aplicado na proxima renderizacao do dashboard.
  void setWifiConnected(bool connected) { _wifiConnected = connected; }

  // Um conjunto de dados -> uma atualizacao completa do e-ink.
  void render(const DashboardData& data);

  // Mostra /image.bin (800x480, 2 bpp) do LittleFS em faixas de 8 linhas.
  // Retorna false se o arquivo estiver ausente, errado ou nao puder ser lido.
  bool showPrivacyImage();

  // Usa as mesmas coordenadas dos cards de render(), ordem do vetor _habits.
  int habitIndexAt(int16_t x, int16_t y, size_t habitCount) const;

  // Compatibilidade com o teste antigo (nao calcula dias sem todayISO).
  void show(
    const std::vector<Habit>& habits,
    const WeatherData* weather);

private:
  bool _wifiConnected = true;
};

#endif
