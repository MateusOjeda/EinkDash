#ifndef EINKDASH_HABIT_H
#define EINKDASH_HABIT_H

#include <Arduino.h>
#include <stdint.h>
#include <vector>

// Dados que o display precisara futuramente.
// Nenhum historico completo e mantido; apenas a ultima realizacao.
struct Habit {
  String id;
  String name;
  int position = -1;
  String lastCompletedDate;  // YYYY-MM-DD; vazio = nunca
  String previousCompletedDate; // ultima anterior, usada para desmarcar HOJE sem nova consulta

  uint16_t iconWidth = 0;
  uint16_t iconHeight = 0;
  std::vector<uint8_t> iconBytes;  // RAM somente se Flash externa indisponivel
  uint32_t iconHash = 0;          // assinatura dos bytes + ID + dimensoes
  int8_t iconFlashSlot = -1;      // 0..11; -1 = nao armazenado
  int8_t iconFlashBank = -1;      // 0 ou 1; -1 = nao armazenado

  bool hasIcon() const {
    return !iconBytes.empty() || (iconFlashSlot >= 0 && iconFlashBank >= 0);
  }
};

#endif
