#ifndef EINKDASH_POLLEN_MANAGER_H
#define EINKDASH_POLLEN_MANAGER_H

#include <Arduino.h>

// Dados mantidos em RAM para o DashboardData.
struct PollenData
{
  int grassIndex = -1;
  char category[32] = {};
};

class PollenManager
{
public:
  bool begin();
  bool fetchAndLog(double latitude, double longitude);

  bool hasData() const { return _hasData; }
  const PollenData& data() const { return _data; }

private:
  PollenData _data;
  bool _hasData = false;
};

#endif
