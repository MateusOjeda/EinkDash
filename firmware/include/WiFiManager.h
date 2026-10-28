#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "FirebaseManager.h"
#include <Arduino.h>

class WiFiManager
{
public:

  WiFiManager();

  void setFirebaseManager(
    FirebaseManager& firebase
  );

  void begin();

  bool connectToSavedWiFi();

  bool startConfiguration();

  String getDeviceMac();

  bool isConnected();

private:
  FirebaseManager* _firebase;

  String _deviceMac;
};

#endif