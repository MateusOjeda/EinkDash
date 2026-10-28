#include "TouchManager.h"
#include <initializer_list>

// GT911: touch status em 0x814E, primeiro ponto em 0x8150.
// Enderecos I2C possiveis: 0x5D e 0x14.
// No exemplo original Good Display o endereco e 0x5D (0xBA/0xBB em 8 bits).

bool TouchManager::readReg(uint16_t reg, uint8_t *data, size_t size)
{
  if (!_address || !size)
    return false;
  Wire.beginTransmission(_address);
  Wire.write(static_cast<uint8_t>(reg >> 8));
  Wire.write(static_cast<uint8_t>(reg & 0xFF));
  // O codigo original Good Display envia STOP apos o endereco do registrador
  // e inicia uma nova transferencia para a leitura.
  if (Wire.endTransmission(true) != 0)
    return false;
  if (Wire.requestFrom(static_cast<int>(_address), static_cast<int>(size), true) != static_cast<int>(size))
    return false;
  for (size_t i = 0; i < size; ++i)
    data[i] = static_cast<uint8_t>(Wire.read());
  return true;
}

bool TouchManager::writeReg8(uint16_t reg, uint8_t value)
{
  if (!_address)
    return false;
  Wire.beginTransmission(_address);
  Wire.write(static_cast<uint8_t>(reg >> 8));
  Wire.write(static_cast<uint8_t>(reg & 0xFF));
  Wire.write(value);
  return Wire.endTransmission(true) == 0;
}

bool TouchManager::begin(int sda, int scl)
{
  _address = 0;
  _pressed = false;
  _lastTapMs = 0;
  _lastTouchReportMs = 0;
  // O exemplo da Good Display usa A0 (GPIO36) como IRQ do touch.
  // GPIO36 e somente entrada e nao possui pull-up interno.
  pinMode(36, INPUT);
  Wire.begin(sda, scl);
  Wire.setClock(100000);
  for (uint8_t candidate : {static_cast<uint8_t>(0x5D), static_cast<uint8_t>(0x14)})
  {
    Wire.beginTransmission(candidate);
    if (Wire.endTransmission() == 0)
    {
      _address = candidate;
      break;
    }
  }
  if (!_address)
  {
    Serial.printf("TOUCH: GT911 nao encontrado em SDA=%d, SCL=%d (0x5D/0x14).\n", sda, scl);
    return false;
  }
  uint8_t id[4] = {};
  if (!readReg(0x8140, id, 4))
  {
    Serial.println("TOUCH: endereco I2C respondeu, mas leitura do ID falhou.");
    _address = 0;
    return false;
  }
  Serial.printf("TOUCH: chip id='%c%c%c%c', endereco=0x%02X\n",
                id[0], id[1], id[2], id[3], _address);
  uint8_t resolution[4] = {};
  if (readReg(0x8048, resolution, 4))
  {
    uint16_t rx = resolution[0] | (resolution[1] << 8);
    uint16_t ry = resolution[2] | (resolution[3] << 8);
    if (rx >= 200 && rx <= 2000)
      _maxX = rx;
    if (ry >= 200 && ry <= 2000)
      _maxY = ry;
  }
  Serial.printf("TOUCH: resolucao do controlador %u x %u\n", _maxX, _maxY);
  return true;
}

void TouchManager::setTransform(bool swapXY, bool mirrorX, bool mirrorY)
{
  _swapXY = swapXY;
  _mirrorX = mirrorX;
  _mirrorY = mirrorY;
}

bool TouchManager::readTap(TouchPointEink &point)
{
  if (!_address)
    return false;

  uint8_t status = 0;

  if (!readReg(0x814E, &status, 1))
    return false;

  const bool ready = (status & 0x80) != 0;

  if (!ready)
  {
    // Caso o controlador nao emita um pacote de liberacao, nao ficar
    // bloqueado para sempre apos o primeiro toque.
    if (_pressed && millis() - _lastTouchReportMs > 250)
      _pressed = false;
    return false; // dado ainda nao pronto
  }

  uint8_t count = status & 0x0F;
  if (count > 5)
  {
    writeReg8(0x814E, 0);
    return false;
  }
  if (count == 0)
  {
    writeReg8(0x814E, 0);
    _pressed = false; // liberou o dedo
    return false;
  }

  uint8_t raw[8] = {};
  bool ok = readReg(0x8150, raw, sizeof(raw));
  bool ackOk = writeReg8(0x814E, 0); // obrigatorio para receber o proximo evento
  if (!ok || !ackOk)
  {
    Serial.printf("TOUCH DEBUG: leitura ponto=%s | limpar status=%s\n",
                  ok ? "OK" : "FALHOU", ackOk ? "OK" : "FALHOU");
  }
  if (!ok)
    return false;
  _lastTouchReportMs = millis();
  if (_pressed)
    return false; // nao repete enquanto dedo permanece pressionado
  _pressed = true;

  uint32_t ms = millis();
  if (_lastTapMs != 0 && ms - _lastTapMs < 500)
    return false;
  _lastTapMs = ms;

  // O bloco lido a partir de 0x8150 comeca no X_L, nao no Track ID.
  // No exemplo da Good Display, a leitura parte de 0x814E:
  // status=[0], trackId=[1], X_L=[2], X_H=[3], Y_L=[4], Y_H=[5].
  // Portanto, aqui (partindo de 0x8150): X_L=[0], X_H=[1], Y_L=[2], Y_H=[3].
  uint16_t x = static_cast<uint16_t>(raw[0]) | (static_cast<uint16_t>(raw[1]) << 8);
  uint16_t y = static_cast<uint16_t>(raw[2]) | (static_cast<uint16_t>(raw[3]) << 8);
  point.rawX = x;
  point.rawY = y;

  int32_t mx, my;
  if (_swapXY)
  {
    mx = static_cast<int32_t>(y) * 800 / _maxY;
    my = static_cast<int32_t>(x) * 480 / _maxX;
  }
  else
  {
    mx = static_cast<int32_t>(x) * 800 / _maxX;
    my = static_cast<int32_t>(y) * 480 / _maxY;
  }
  if (_mirrorX)
    mx = 799 - mx;
  if (_mirrorY)
    my = 479 - my;
  if (mx < 0 || mx >= 800 || my < 0 || my >= 480)
    return false;
  point.x = static_cast<int16_t>(mx);
  point.y = static_cast<int16_t>(my);
  return true;
}
