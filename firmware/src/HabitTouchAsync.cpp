#include "HabitTouchAsync.h"

#include <WiFi.h>
#include <time.h>
#include <stdio.h>
#include <string.h>

bool HabitTouchAsync::begin() {
  if (_ready) return true;

  _jobs = xQueueCreate(MAX_PENDING, sizeof(Job));
  _results = xQueueCreate(MAX_PENDING, sizeof(Result));
  if (!_jobs || !_results) {
    Serial.println("TOUCH ASYNC: sem memoria para as filas.");
    return false;
  }

  // 12 KB em bytes para ESP32 Arduino/FreeRTOS.
  const BaseType_t created = xTaskCreatePinnedToCore(
      workerEntry, "habit-firestore", 12 * 1024,
      this, 1, &_workerHandle, 0);
  if (created != pdPASS) {
    Serial.println("TOUCH ASYNC: sem memoria para task Firestore.");
    return false;
  }
  _ready = true;
  Serial.println("TOUCH ASYNC: iniciado (Firestore em outra task).");
  return true;
}

void HabitTouchAsync::workerEntry(void* context) {
  static_cast<HabitTouchAsync*>(context)->workerLoop();
}

void HabitTouchAsync::workerLoop() {
  Job job{};
  for (;;) {
    if (xQueueReceive(_jobs, &job, portMAX_DELAY) != pdTRUE) continue;

    // A task de rede NUNCA altera _habits ou chama o DisplayManager.
    // FirebaseManager::_fbdo e usado somente aqui enquanto ha pendencias;
    // comandos manuais de sync/wifi devem ser bloqueados durante as pendencias.
    const bool ok = _firebase.setHabitCompletedToday(
        String(job.id), String(job.todayISO), job.completed);
    const Result result{job.seq, ok};
    xQueueSend(_results, &result, portMAX_DELAY);
    Serial.printf("MEM TASK Firebase: stack minima livre=%lu bytes\n",
                  static_cast<unsigned long>(uxTaskGetStackHighWaterMark(nullptr)));
  }
}

bool HabitTouchAsync::hasPending() const {
  for (const Pending& p : _pending)
    if (p.active) return true;
  return false;
}

void HabitTouchAsync::redrawLocal(const String& todayISO) {
  DashboardData dashboard;
  dashboard.todayISO = todayISO;
  dashboard.habits = &_firebase.getHabits();
  // No GDEY075T7 a janela parcial em 4 cinzas pode acinzentar
  // os demais paineis. Redraw 4G completo e o modo seguro atual.
  _display.render(dashboard);
}

void HabitTouchAsync::handleTap(const TouchPointEink& touch) {
  if (!_ready) {
    Serial.println("TOUCH: task Firebase indisponivel; toque ignorado.");
    return;
  }

  const auto& habits = _firebase.getHabits();
  const int index = _display.habitIndexAt(touch.x, touch.y, habits.size());
  if (index < 0) return;
  const Habit& habit = habits[static_cast<size_t>(index)];
  const String habitId = habit.id;
  Serial.printf("TOUCH: %s\n", habit.name.c_str());

  for (const Pending& p : _pending) {
    if (p.active && p.id == habitId) {
      Serial.println("TOUCH: esta gravacao ainda esta pendente; toque ignorado.");
      return;
    }
  }

  Pending* slot = nullptr;
  for (Pending& p : _pending) {
    if (!p.active) { slot = &p; break; }
  }
  if (!slot) {
    Serial.println("TOUCH: fila cheia; toque ignorado.");
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("TOUCH: sem Wi-Fi; nao altera a imagem.");
    return;
  }
  if (habitId.isEmpty() || habitId.length() >= ID_CAPACITY) {
    Serial.println("TOUCH: identificador invalido/maior que 127 bytes.");
    return;
  }

  struct tm now{};
  if (!getLocalTime(&now, 1000)) {
    Serial.println("TOUCH: horario indisponivel; nao altera a imagem.");
    return;
  }
  char date[11];
  if (!strftime(date, sizeof(date), "%Y-%m-%d", &now)) return;
  const String todayISO(date);

  const bool completeToday = (habit.lastCompletedDate == todayISO);
  const bool newComplete = !completeToday;
  // Guarda os DOIS valores para rollback exato se a escrita falhar.
  slot->active = true;
  slot->seq = ++_nextSeq;
  slot->id = habitId;
  slot->lastBefore = habit.lastCompletedDate;
  slot->previousBefore = habit.previousCompletedDate;

  if (!_firebase.setLocalHabitCompletedToday(habitId, todayISO, newComplete)) {
    slot->active = false;
    return;
  }

  Serial.printf("TOUCH: atualizando localmente %s -> %s\n",
                habitId.c_str(), newComplete ? "Hoje" : "data anterior");
  redrawLocal(todayISO);  // primeiro mostra a mudanca, sem aguardar Firestore

  Job job{};
  job.seq = slot->seq;
  snprintf(job.id, sizeof(job.id), "%s", habitId.c_str());
  snprintf(job.todayISO, sizeof(job.todayISO), "%s", date);
  job.completed = newComplete;
  if (xQueueSend(_jobs, &job, 0) != pdTRUE) {
    Serial.println("TOUCH: falha na fila; restaurando estado local.");
    _firebase.restoreLocalHabitDates(habitId,
                                     slot->lastBefore, slot->previousBefore);
    slot->active = false;
    redrawLocal(todayISO);
    return;
  }
  Serial.println("TOUCH: escrita Firebase enfileirada (assincrona).");
}

void HabitTouchAsync::poll() {
  if (!_ready) return;
  Result result{};
  while (xQueueReceive(_results, &result, 0) == pdTRUE) {
    Pending* slot = nullptr;
    for (Pending& p : _pending) {
      if (p.active && p.seq == result.seq) { slot = &p; break; }
    }
    if (!slot) {
      Serial.printf("TOUCH: resposta desconhecida seq=%lu.\n",
                    static_cast<unsigned long>(result.seq));
      continue;
    }
    const String habitId = slot->id;
    if (result.saved) {
      Serial.printf("TOUCH: Firebase confirmou %s.\n", habitId.c_str());
      slot->active = false;
      slot->id = "";
      slot->lastBefore = "";
      slot->previousBefore = "";
      continue;
    }

    Serial.printf("TOUCH: Firebase FALHOU para %s; desfazendo imagem.\n",
                  habitId.c_str());
    _firebase.restoreLocalHabitDates(habitId,
                                     slot->lastBefore, slot->previousBefore);
    slot->active = false;
    slot->id = "";
    slot->lastBefore = "";
    slot->previousBefore = "";

    struct tm now{};
    if (getLocalTime(&now, 1000)) {
      char date[11];
      if (strftime(date, sizeof(date), "%Y-%m-%d", &now))
        redrawLocal(date);
    }
    Serial.println("TOUCH: se houver duvida sobre o estado remoto, execute sync.");
  }
}
