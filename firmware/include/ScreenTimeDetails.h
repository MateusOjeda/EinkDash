#ifndef EINKDASH_SCREEN_TIME_DETAILS_H
#define EINKDASH_SCREEN_TIME_DETAILS_H

#include <Arduino.h>
#include <vector>

// Categorias armazenadas pelo monitor Windows em screenTime/{deviceId}/days/{data}.
// Nao representa videos especificos: YouTube/Netflix sao categorias de uso.
struct ScreenTimeEntry
{
  String name;
  uint32_t seconds = 0;
};

struct ScreenTimeDetails
{
  String date;
  // Reservado para a futura coleta Android; permanece indisponivel hoje.
  uint32_t totalPhone = 0;
  bool phoneDataAvailable = false;
  uint32_t totalPC = 0;
  uint32_t totalTV = 0;
  std::vector<ScreenTimeEntry> pc;
  std::vector<ScreenTimeEntry> tv;
  bool dataAvailable = false; // consulta Firestore concluida, mesmo que sem registros
};

#endif
