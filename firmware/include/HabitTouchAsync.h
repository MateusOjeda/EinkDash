#ifndef EINKDASH_HABIT_TOUCH_ASYNC_H
#define EINKDASH_HABIT_TOUCH_ASYNC_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "FirebaseManager.h"
#include "DisplayManager.h"
#include "TouchManager.h"

// Uma tarefa para a tela/touch (loop), outra exclusivamente para escritas Firestore.
// Nao executa syncHabits() na task de rede: _habits e acessado apenas no loop.
class HabitTouchAsync {
 public:
  HabitTouchAsync(FirebaseManager& firebase, DisplayManager& display)
      : _firebase(firebase), _display(display) {}

  bool begin();
  void handleTap(const TouchPointEink& touch); // loop task
  void poll(); // loop task: confirma ou desfaz as alteracoes
  bool hasPending() const;

 private:
  static constexpr size_t MAX_PENDING = 12;
  static constexpr size_t ID_CAPACITY = 128;

  struct Job {
    uint32_t seq;
    char id[ID_CAPACITY];
    char todayISO[11];
    bool completed;
  };
  struct Result {
    uint32_t seq;
    bool saved;
  };
  struct Pending {
    bool active = false;
    uint32_t seq = 0;
    String id;
    String lastBefore;
    String previousBefore;
  };

  FirebaseManager& _firebase;
  DisplayManager& _display;
  QueueHandle_t _jobs = nullptr;
  QueueHandle_t _results = nullptr;
  TaskHandle_t _workerHandle = nullptr;
  Pending _pending[MAX_PENDING];
  uint32_t _nextSeq = 0;
  bool _ready = false;

  static void workerEntry(void* context);
  void workerLoop();
  void redrawLocal(const String& todayISO);
};

#endif
