#ifndef EINKDASH_DISPLAY_MANAGER_H
#define EINKDASH_DISPLAY_MANAGER_H

#include <vector>
#include "Habit.h"
#include "DashboardData.h"

class DisplayManager {
public:
  // Um conjunto de dados -> uma atualizacao completa do e-ink.
  void render(const DashboardData& data);

  // Usa as mesmas coordenadas dos cards de render(), ordem do vetor _habits.
  int habitIndexAt(int16_t x, int16_t y, size_t habitCount) const;

  // Compatibilidade com o teste antigo (nao calcula dias sem todayISO).
  void showHabits(const std::vector<Habit>& habits);
};

#endif
