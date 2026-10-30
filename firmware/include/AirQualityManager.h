#ifndef EINKDASH_AIR_QUALITY_MANAGER_H
#define EINKDASH_AIR_QUALITY_MANAGER_H

#include <Arduino.h>

// Dados mantidos em RAM para o DashboardData.
struct AirQualityData
{
  int brazilAqi = -1;
  char category[32] = {};
  float pm25 = -1.0f;
  float pm10 = -1.0f;
};

class AirQualityManager
{
public:
  bool begin();
  bool fetchAndLog(double latitude, double longitude);

  bool hasData() const { return _hasData; }
  const AirQualityData& data() const { return _data; }

private:
  AirQualityData _data;
  bool _hasData = false;
};

#endif
