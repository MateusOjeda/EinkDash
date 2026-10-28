#include "WiFiManager.h"
#include "Secrets.h"

#include <WiFi.h>
#include <WiFiProv.h>

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>

#include "esp_mac.h"


static const char *POP = WIFI_PROVISIONING_POP;
static const char *SERVICE_NAME = "EinkDash";

volatile bool provisioningFinished = false;
volatile bool eventProvisioningEnded = false;

static const String DISPLAY_TYPE =
  "einkdash-7.5";


#define DEVICE_SERVICE_UUID \
  "7b7d0001-4e69-4d91-9a31-7d5e8f000001"

#define DEVICE_MAC_CHARACTERISTIC \
  "7b7d0002-4e69-4d91-9a31-7d5e8f000001"

#define STATUS_SERVICE_UUID \
  "7b7d1001-4e69-4d91-9a31-7d5e8f000001"

#define STATUS_CHARACTERISTIC_UUID \
  "7b7d1002-4e69-4d91-9a31-7d5e8f000001"


static volatile bool statusClientConnected = false;
static volatile bool statusRead = false;


static uint8_t PROVISIONING_UUID[16] = {
  0xb5, 0xdf, 0x5a, 0x1c,
  0x3f, 0x6b,
  0xf4, 0xbf,
  0xea, 0x4a, 0x82, 0x03,
  0x04, 0x90, 0x1a, 0x02
};


static volatile bool provisioningRequested = false;
static volatile bool wifiReady = false;
static volatile bool wifiConnected = false;


static void SysProvEvent(
  arduino_event_t *event
)
{
  switch (event->event_id)
  {
    case ARDUINO_EVENT_WIFI_READY:

      Serial.println(
        "WIFI: pronto."
      );

      wifiReady = true;

      break;


    case ARDUINO_EVENT_PROV_START:

      Serial.println(
        "WIFI: provisioning iniciado."
      );

      break;


    case ARDUINO_EVENT_PROV_CRED_RECV:

      Serial.print(
        "WIFI: SSID recebido: "
      );

      Serial.println(
        (const char *)event->event_info.prov_cred_recv.ssid
      );

      break;


    case ARDUINO_EVENT_PROV_CRED_SUCCESS:

      Serial.println(
        "WIFI: credenciais recebidas com sucesso."
      );

      provisioningFinished = true;

      break;


    case ARDUINO_EVENT_PROV_CRED_FAIL:

      Serial.println(
        "Falha ao receber credenciais."
      );

      provisioningFinished = true;

      break;


    case ARDUINO_EVENT_PROV_END:

      Serial.println(
        "WIFI: provisioning encerrado."
      );

      eventProvisioningEnded = true;

      break;


    case ARDUINO_EVENT_WIFI_STA_GOT_IP:

      Serial.println(
        "WIFI: conectado."
      );

      Serial.print(
        "IP: "
      );

      Serial.println(
        WiFi.localIP()
      );

      Serial.print(
        "RSSI: "
      );

      Serial.println(
        WiFi.RSSI()
      );

      Serial.print(
        "MAC: "
      );

      Serial.println(
        WiFi.macAddress()
      );

      wifiConnected = true;

      break;


    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:

      Serial.println(
        "WIFI: desconectado."
      );

      wifiConnected = false;

      break;


    default:

      break;
  }
}


class DeviceBLECallback
  : public BLECharacteristicCallbacks
{
  void onWrite(
    BLECharacteristic *characteristic
  ) override
  {
    String value =
      characteristic->getValue();

    Serial.println();
    Serial.println(
      "=== COMANDO BLE RECEBIDO ==="
    );

    Serial.print(
      "Conteudo recebido: "
    );

    Serial.println(value);

    provisioningRequested = true;

    Serial.println(
      "Provisioning solicitado."
    );
  }
};


class StatusBLEServerCallback
  : public BLEServerCallbacks
{
  void onConnect(
    BLEServer *server
  ) override
  {
    Serial.println();
    Serial.println(
      ">>> STATUS BLE: ON_CONNECT <<<"
    );

    Serial.print(
      "statusClientConnected ANTES: "
    );

    Serial.println(
      statusClientConnected
    );

    statusClientConnected = true;

    Serial.print(
      "statusClientConnected DEPOIS: "
    );

    Serial.println(
      statusClientConnected
    );
  }


  void onDisconnect(
    BLEServer *server
  ) override
  {
    Serial.println();
    Serial.println(
      ">>> STATUS BLE: ON_DISCONNECT <<<"
    );

    Serial.print(
      "statusClientConnected ANTES: "
    );

    Serial.println(
      statusClientConnected
    );

    statusClientConnected = false;

    Serial.print(
      "statusClientConnected DEPOIS: "
    );

    Serial.println(
      statusClientConnected
    );
  }
};


class StatusBLECharacteristicCallback
  : public BLECharacteristicCallbacks
{
  void onRead(
    BLECharacteristic *characteristic
  ) override
  {
    Serial.println(
      ">>> STATUS BLE: ON_READ <<<"
    );

    statusRead = true;
  }
};


WiFiManager::WiFiManager()
  : _firebase(nullptr)
{
}


void WiFiManager::setFirebaseManager(
  FirebaseManager& firebase
)
{
  _firebase = &firebase;
}


void WiFiManager::begin()
{
  WiFi.mode(WIFI_STA);

  WiFi.onEvent(
    SysProvEvent
  );

  _deviceMac =
    WiFi.macAddress();

  Serial.print(
    "MAC Wi-Fi: "
  );

  Serial.println(
    _deviceMac
  );
}


String WiFiManager::getDeviceMac()
{
  return _deviceMac;
}


bool WiFiManager::isConnected()
{
  return WiFi.status() == WL_CONNECTED;
}


bool WiFiManager::connectToSavedWiFi()
{
  Serial.println();

  Serial.println(
    "Conectando ao Wi-Fi salvo..."
  );

  wifiReady = false;

  WiFi.begin();

  unsigned long startTime =
    millis();

  while (
    millis() - startTime < 20000
  )
  {
    if (
      WiFi.status() == WL_CONNECTED
    )
    {
      Serial.println();

      Serial.println(
        "Wi-Fi conectado."
      );

      Serial.print(
        "IP: "
      );

      Serial.println(
        WiFi.localIP()
      );

      Serial.print(
        "RSSI: "
      );

      Serial.println(
        WiFi.RSSI()
      );

      return true;
    }

    delay(500);

    Serial.print(".");
  }

  Serial.println();

  Serial.println(
    "ERRO: nao foi possivel conectar ao Wi-Fi."
  );

  return false;
}


bool WiFiManager::startConfiguration()
{
  Serial.println();

  Serial.println(
    "================================"
  );

  Serial.println(
    "     MODO CONFIGURACAO WI-FI"
  );

  Serial.println(
    "================================"
  );


  Serial.println();

  Serial.println(
    "Obtendo MAC Wi-Fi..."
  );

  WiFi.mode(WIFI_STA);

  WiFi.begin();

  unsigned long startTime =
    millis();

  while (
    millis() - startTime < 20000
  )
  {
    _deviceMac =
      WiFi.macAddress();

    if (
      _deviceMac.length() > 0 &&
      _deviceMac !=
        "00:00:00:00:00:00"
    )
    {
      break;
    }

    delay(100);
  }


  if (
    _deviceMac.length() == 0 ||
    _deviceMac ==
      "00:00:00:00:00:00"
  )
  {
    Serial.println(
      "ERRO: nao foi possivel obter o MAC."
    );

    WiFi.disconnect();

    return false;
  }


  WiFi.disconnect();

  Serial.print(
    "MAC Wi-Fi: "
  );

  Serial.println(
    _deviceMac
  );


  Serial.println();

  Serial.println(
    "Apagando configuracao Wi-Fi atual..."
  );


  if (
    !WiFi.disconnect(false, true)
  )
  {
    Serial.println(
      "AVISO: nao foi possivel apagar a configuracao Wi-Fi."
    );
  }
  else
  {
    Serial.println(
      "Configuracao Wi-Fi apagada."
    );
  }


  Serial.println();

  Serial.println(
    "Iniciando BLE de identificacao..."
  );


  provisioningRequested = false;


  BLEDevice::init(
    SERVICE_NAME
  );


  BLEServer *server =
    BLEDevice::createServer();


  BLEService *service =
    server->createService(
      DEVICE_SERVICE_UUID
    );


  BLECharacteristic *characteristic =
    service->createCharacteristic(
      DEVICE_MAC_CHARACTERISTIC,
      BLECharacteristic::PROPERTY_READ |
      BLECharacteristic::PROPERTY_WRITE
    );


  characteristic->setCallbacks(
    new DeviceBLECallback()
  );


  String deviceInfo =
    _deviceMac +
    "|" +
    DISPLAY_TYPE;


  characteristic->setValue(
    deviceInfo.c_str()
  );


  service->start();


  BLEAdvertising *advertising =
    BLEDevice::getAdvertising();


  advertising->addServiceUUID(
    DEVICE_SERVICE_UUID
  );

  advertising->setScanResponse(
    true
  );


  BLEDevice::startAdvertising();


  Serial.println(
    "BLE de identificacao ativo."
  );

  Serial.println(
    "Aguardando comando do aplicativo..."
  );


  unsigned long provisionStartTime =
    millis();


  while (!provisioningRequested)
  {
    if (
      millis() - provisionStartTime >= 30000
    )
    {
      Serial.println(
        "Timeout aguardando comando do aplicativo."
      );

      BLEDevice::stopAdvertising();
      BLEDevice::deinit(false);

      return false;
    }

    delay(10);
  }


  Serial.println();

  Serial.println(
    "Comando recebido."
  );


  BLEDevice::stopAdvertising();

  BLEDevice::deinit(false);


  delay(1000);


  Serial.println();

  Serial.println(
    "Iniciando provisioning BLE..."
  );


  provisioningFinished = false;
  wifiConnected = false;
  eventProvisioningEnded = false;


  WiFi.onEvent(
    SysProvEvent
  );


  Serial.println(
    "BLE beginProvision starting."
  );


  WiFiProv.beginProvision(
    NETWORK_PROV_SCHEME_BLE,
    NETWORK_PROV_SCHEME_HANDLER_NONE,
    NETWORK_PROV_SECURITY_1,
    POP,
    "PROV_EinkDash",
    NULL,
    PROVISIONING_UUID,
    true
  );


  Serial.println();

  Serial.println(
    "BLE de provisioning ativo."
  );


  unsigned long provisioningStartTime =
    millis();


  while (!wifiConnected)
  {
    if (
      provisioningFinished &&
      !wifiConnected
    )
    {
      Serial.println(
        "ERRO: provisioning terminou sem conexao Wi-Fi."
      );

      return false;
    }


    if (
      millis() - provisioningStartTime >= 60000
    )
    {
      Serial.println(
        "Timeout aguardando conexao Wi-Fi."
      );

      return false;
    }


    delay(10);
  }


  Serial.println();

  Serial.println(
    "Wi-Fi configurado e conectado com sucesso."
  );


  delay(2000);


  WiFiProv.endProvision();


  unsigned long waitProvisionEnd =
    millis();


  while (!eventProvisioningEnded)
  {
    if (
      millis() - waitProvisionEnd >= 10000
    )
    {
      Serial.println(
        "Timeout aguardando encerramento do provisioning."
      );

      return false;
    }

    delay(10);
  }


  delay(2000);

  BLEDevice::stopAdvertising();


  // ============================================================
  // FIREBASE
  // ============================================================

  if (_firebase == nullptr)
  {
    Serial.println(
      "ERRO: FirebaseManager nao configurado."
    );

    return false;
  }


  Serial.println();

  Serial.println(
    "Criando nova sessao Firebase Anonymous..."
  );


  String firebaseUid;


  if (
    !_firebase->createAnonymousSession(
      firebaseUid
    )
  )
  {
    Serial.println(
      "ERRO: nao foi possivel criar sessao Firebase."
    );

    return false;
  }


  Serial.println();

  Serial.println(
    "Sessao Firebase criada com sucesso."
  );

  Serial.print(
    "Firebase UID: "
  );

  Serial.println(
    firebaseUid
  );

  WiFi.disconnect();

  // ============================================================
  // BLE DE STATUS
  // ============================================================

  Serial.println();

  Serial.println(
    "Iniciando BLE de status..."
  );


  statusClientConnected = false;
  statusRead = false;

  Serial.println(
    "STATUS BLE: antes do BLEDevice::init"
  );

  BLEDevice::init(
    "EinkDashStatus"
  );

  Serial.println(
    "STATUS BLE: depois do BLEDevice::init"
  );

  BLEServer *statusServer =
    BLEDevice::createServer();

  Serial.println(
    "STATUS BLE: depois do createServer"
  );

  statusServer->setCallbacks(
    new StatusBLEServerCallback()
  );

  Serial.println(
    "STATUS BLE: depois do setCallbacks"
  );

  BLEService *statusService =
    statusServer->createService(
      STATUS_SERVICE_UUID
    );

  Serial.println(
    "STATUS BLE: depois do createService"
  );

  BLECharacteristic *statusCharacteristic =
    statusService->createCharacteristic(
      STATUS_CHARACTERISTIC_UUID,
      BLECharacteristic::PROPERTY_READ
    );

  Serial.println(
    "STATUS BLE: depois do createCharacteristic"
  );


  statusCharacteristic->setCallbacks(
    new StatusBLECharacteristicCallback()
  );

  String status =
    "WIFI_CONNECTED|" +
    firebaseUid;


  statusCharacteristic->setValue(
    status.c_str()
  );


  statusService->start();


  BLEAdvertising *statusAdvertising =
    BLEDevice::getAdvertising();


  statusAdvertising->addServiceUUID(
    STATUS_SERVICE_UUID
  );

  statusAdvertising->setScanResponse(
    true
  );


  BLEDevice::startAdvertising();


  Serial.println(
    "BLE de status ativo."
  );

  Serial.println(
    "Aguardando aplicativo..."
  );


  unsigned long waitConnectionOnStatus =
    millis();


  while (!statusClientConnected)
  {
    if (
      millis() - waitConnectionOnStatus >= 30000
    )
    {
      Serial.println(
        "Timeout aguardando conexao BLE de status."
      );

      BLEDevice::stopAdvertising();
      BLEDevice::deinit(false);

      return false;
    }

    delay(50);
  }


  unsigned long waitStatusRead =
    millis();


  while (!statusRead)
  {
    if (
      millis() - waitStatusRead >= 10000
    )
    {
      Serial.println(
        "Timeout aguardando leitura do status."
      );

      BLEDevice::stopAdvertising();
      BLEDevice::deinit(false);

      return false;
    }

    delay(50);
  }

  delay(2000);

  BLEDevice::stopAdvertising();

  BLEDevice::deinit(false);


  Serial.println(
    "BLE de status encerrado."
  );


  return true;
}