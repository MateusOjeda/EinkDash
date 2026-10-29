#include "FlashManager.h"
#include <string.h>
#include <stddef.h>

namespace {
constexpr uint8_t PIN_FLASH_CS = 2;
constexpr uint8_t PIN_FONT_CS = 15;
constexpr uint8_t PIN_EPD_CS = 27;
const SPISettings FLASH_SPI(8000000, MSBFIRST, SPI_MODE0);
}

void FlashManager::select() {
  SPI.beginTransaction(FLASH_SPI);
  digitalWrite(PIN_FLASH_CS, LOW);
}
void FlashManager::deselect() {
  digitalWrite(PIN_FLASH_CS, HIGH);
  SPI.endTransaction();
}
bool FlashManager::begin() {
  if (_present) return true;
  pinMode(PIN_FLASH_CS, OUTPUT);
  pinMode(PIN_FONT_CS, OUTPUT);
  pinMode(PIN_EPD_CS, OUTPUT);
  digitalWrite(PIN_FLASH_CS, HIGH);
  digitalWrite(PIN_FONT_CS, HIGH);
  digitalWrite(PIN_EPD_CS, HIGH);
  SPI.begin(18, 19, 23, PIN_EPD_CS);
  select();
  SPI.transfer(0x9F);
  const uint8_t mfr = SPI.transfer(0);
  const uint8_t type = SPI.transfer(0);
  const uint8_t cap = SPI.transfer(0);
  deselect();
  Serial.printf("FLASH: JEDEC=%02X %02X %02X\n", mfr, type, cap);
  _present = mfr == 0xEF && type == 0x40 && cap == 0x15;
  if (!_present) Serial.println("FLASH: chip inesperado; cache desativado.");
  return _present;
}
uint8_t FlashManager::status() {
  select(); SPI.transfer(0x05);
  uint8_t v = SPI.transfer(0);
  deselect(); return v;
}
bool FlashManager::waitReady(uint32_t timeoutMs) {
  uint32_t start = millis();
  while ((status() & 0x01) != 0) {
    if (millis() - start > timeoutMs) return false;
    delay(1);
  }
  return true;
}
bool FlashManager::writeEnable() {
  if (!waitReady(3000)) return false;
  select(); SPI.transfer(0x06); deselect();
  return (status() & 0x02) != 0;
}
static void sendAddress(uint32_t a) {
  SPI.transfer(uint8_t(a >> 16));
  SPI.transfer(uint8_t(a >> 8));
  SPI.transfer(uint8_t(a));
}
bool FlashManager::readAt(uint32_t a, uint8_t* dst, size_t size) {
  if (!_present || !dst || uint64_t(a) + size > FLASH_SIZE) return false;
  select(); SPI.transfer(0x03); sendAddress(a);
  for (size_t i = 0; i < size; ++i) dst[i] = SPI.transfer(0);
  deselect(); return true;
}
bool FlashManager::readChunk(uint32_t offset, uint8_t* dst, size_t size) {
  if (uint64_t(offset) + size > IMAGE_SIZE) return false;
  return readAt(PIXELS_ADDR + offset, dst, size);
}
bool FlashManager::blank(uint32_t addr, uint32_t count) {
  uint8_t b[256];
  for (uint32_t i = 0; i < count; i += sizeof(b)) {
    uint32_t n = (count - i < sizeof(b)) ? count - i : sizeof(b);
    if (!readAt(addr + i, b, n)) return false;
    for (uint32_t j = 0; j < n; ++j) if (b[j] != 0xFF) return false;
  }
  return true;
}
bool FlashManager::program(uint32_t addr, const uint8_t* bytes, size_t len) {
  if (!bytes || len == 0 || len > 256 ||
      (addr & 255U) + len > 256 || uint64_t(addr) + len > FLASH_SIZE) return false;
  if (!writeEnable()) return false;
  select(); SPI.transfer(0x02); sendAddress(addr);
  for (size_t i = 0; i < len; ++i) SPI.transfer(bytes[i]);
  deselect();
  return waitReady(3000);
}
bool FlashManager::eraseSector(uint32_t addr) {
  if ((addr % SECTOR_SIZE) != 0 || addr + SECTOR_SIZE > FLASH_SIZE) return false;
  if (!writeEnable()) return false;
  select(); SPI.transfer(0x20); sendAddress(addr); deselect();
  return waitReady(10000);
}
bool FlashManager::readHeader(Header& h) { return readAt(META_ADDR, reinterpret_cast<uint8_t*>(&h), sizeof(h)); }
bool FlashManager::hasOurMagic(const Header& h) const {
  return memcmp(h.magic, "EKD4", 4) == 0 && h.version == 1 && h.imageSize == IMAGE_SIZE;
}
uint32_t FlashManager::crcUpdate(uint32_t crc, const uint8_t* p, size_t n) const {
  while (n--) {
    crc ^= *p++;
    for (int b = 0; b < 8; ++b)
      crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320UL : 0UL);
  }
  return crc;
}
bool FlashManager::isCached(uint32_t key) {
  if (!_present) return false;
  if (_checked && _currentKey == key) return true;
  Header h{};
  if (!readHeader(h) || !hasOurMagic(h) || h.committed != 0x00 || h.signature != key) return false;
  uint8_t block[256];
  uint32_t crc = 0xFFFFFFFFUL;
  for (uint32_t i = 0; i < IMAGE_SIZE; i += sizeof(block)) {
    const size_t n = IMAGE_SIZE - i < sizeof(block) ? IMAGE_SIZE - i : sizeof(block);
    if (!readChunk(i, block, n)) return false;
    crc = crcUpdate(crc, block, n);
  }
  if ((crc ^ 0xFFFFFFFFUL) != h.crc32) {
    Serial.println("FLASH: CRC invalido; precisa reconstruir cache.");
    return false;
  }
  _checked = true;
  _currentKey = key;
  return true;
}
bool FlashManager::startWrite(uint32_t key) {
  if (!_present || _writing) return false;
  Header existing{};
  if (!readHeader(existing)) return false;
  const bool owned = hasOurMagic(existing);
  if (!owned && !blank(META_ADDR, (1 + PAYLOAD_SECTORS) * SECTOR_SIZE)) {
    Serial.println("FLASH: regiao 0x100000 tem dados desconhecidos. NAO VOU APAGAR.");
    return false;
  }
  // Se ja e nosso cache, so apagamos a reserva de nossa propriedade.
  if (owned) {
    if (!eraseSector(META_ADDR)) return false;
    for (uint32_t i = 0; i < PAYLOAD_SECTORS; ++i) {
      if (!eraseSector(PIXELS_ADDR + i * SECTOR_SIZE)) return false;
    }
  }
  Header h;
  memset(&h, 0xFF, sizeof(h));
  memcpy(h.magic, "EKD4", 4);
  h.version = 1;
  h.signature = key;
  h.imageSize = IMAGE_SIZE;
  // committed=0xFF: cache incompleto; seguro apos queda de energia.
  if (!program(META_ADDR, reinterpret_cast<const uint8_t*>(&h), sizeof(h))) return false;
  _checked = false;
  _writing = true;
  _writingKey = key;
  _writeOffset = 0;
  _crc = 0xFFFFFFFFUL;
  return true;
}
bool FlashManager::writeChunk(uint32_t offset, const uint8_t* data, size_t n) {
  if (!_writing || offset != _writeOffset || !data || uint64_t(offset) + n > IMAGE_SIZE) return false;
  size_t sent = 0;
  while (sent < n) {
    uint32_t addr = PIXELS_ADDR + offset + sent;
    size_t chunk = 256 - (addr & 255U);
    if (chunk > n - sent) chunk = n - sent;
    if (!program(addr, data + sent, chunk)) return false;
    sent += chunk;
  }
  _crc = crcUpdate(_crc, data, n);
  _writeOffset += n;
  return true;
}
bool FlashManager::finishWrite() {
  if (!_writing || _writeOffset != IMAGE_SIZE) return false;
  const uint32_t expectedCrc = _crc ^ 0xFFFFFFFFUL;
  uint8_t block[256];
  uint32_t actualCrc = 0xFFFFFFFFUL;
  for (uint32_t i = 0; i < IMAGE_SIZE; i += sizeof(block)) {
    const size_t n = IMAGE_SIZE - i < sizeof(block) ? IMAGE_SIZE - i : sizeof(block);
    if (!readChunk(i, block, n)) return false;
    actualCrc = crcUpdate(actualCrc, block, n);
  }
  if ((actualCrc ^ 0xFFFFFFFFUL) != expectedCrc) {
    Serial.println("FLASH: CRC lido nao confere; cache NAO confirmado.");
    return false;
  }
  // Programacao NOR 1->0 sem apagar: CRC e flag de commit gravados no fim.
  if (!program(META_ADDR + offsetof(Header, crc32), reinterpret_cast<const uint8_t*>(&expectedCrc), 4)) return false;
  const uint8_t committed = 0;
  if (!program(META_ADDR + offsetof(Header, committed), &committed, 1)) return false;
  _writing = false;
  _checked = true;
  _currentKey = _writingKey;
  return true;
}

// ================================================================
// Icones individuais na W25Q16: dois bancos por posicao.
// Mantemos intacto o banco referenciado pela lista de habitos ativa.
// A gravacao nunca toca os setores da imagem-base.
// ================================================================
uint32_t FlashManager::iconAddress(uint8_t slot, uint8_t bank) const {
  return ICONS_ADDR + (uint32_t(slot) * ICON_BANKS + bank) * SECTOR_SIZE;
}

bool FlashManager::iconHeader(uint8_t slot, uint8_t bank, IconHeader& h) {
  if (slot >= MAX_ICON_SLOTS || bank >= ICON_BANKS) return false;
  return readAt(iconAddress(slot, bank), reinterpret_cast<uint8_t*>(&h), sizeof(h));
}

bool FlashManager::readIcon(uint8_t slot, int8_t bank, uint32_t key,
                            uint8_t* out, uint16_t size) {
  if (!_present || !out || slot >= MAX_ICON_SLOTS ||
      bank < 0 || bank >= ICON_BANKS || size != ICON_BYTES) return false;
  IconHeader h{};
  if (!iconHeader(slot, static_cast<uint8_t>(bank), h)) return false;
  if (memcmp(h.magic, "EKI1", 4) != 0 || h.version != 1 ||
      h.key != key || h.length != size || h.committed != 0x00) return false;
  if (!readAt(iconAddress(slot, static_cast<uint8_t>(bank)) + sizeof(IconHeader),
              out, size)) return false;
  return (crcUpdate(0xFFFFFFFFUL, out, size) ^ 0xFFFFFFFFUL) == h.crc32;
}

bool FlashManager::storeIcon(uint8_t slot, uint32_t key,
                             const uint8_t* bytes, uint16_t size,
                             int8_t avoidBank, int8_t& storedBank) {
  storedBank = -1;
  if (slot >= MAX_ICON_SLOTS || !bytes || size != ICON_BYTES ||
      !begin()) return false;

  // Se o mesmo icone ja esta gravado, nao regrava nem desgasta a Flash.
  uint8_t scratch[ICON_BYTES];
  for (uint8_t bank = 0; bank < ICON_BANKS; ++bank) {
    if (readIcon(slot, bank, key, scratch, size) &&
        memcmp(scratch, bytes, size) == 0) {
      storedBank = static_cast<int8_t>(bank);
      return true;
    }
  }

  // Faz a substituicao SEM apagar o banco referenciado por _habits.
  const uint8_t bank = (avoidBank == 0) ? 1 : 0;
  const uint32_t addr = iconAddress(slot, bank);
  IconHeader old{};
  if (!iconHeader(slot, bank, old)) return false;
  const bool owned = memcmp(old.magic, "EKI1", 4) == 0 && old.version == 1;
  if (owned) {
    if (!eraseSector(addr)) return false;
  } else if (!blank(addr, SECTOR_SIZE)) {
    Serial.printf("FLASH ICON: setor %08lX desconhecido; preservado.\n",
                  static_cast<unsigned long>(addr));
    return false;
  }

  IconHeader h;
  memset(&h, 0xFF, sizeof(h));
  memcpy(h.magic, "EKI1", 4);
  h.version = 1;
  h.key = key;
  h.length = size;
  h.crc32 = crcUpdate(0xFFFFFFFFUL, bytes, size) ^ 0xFFFFFFFFUL;
  // committed=0xFF durante a gravacao. So marca 0 ao validar.
  if (!program(addr, reinterpret_cast<const uint8_t*>(&h), sizeof(h))) return false;

  size_t sent = 0;
  while (sent < size) {
    const uint32_t at = addr + sizeof(h) + sent;
    size_t n = 256 - (at & 255U);
    if (n > size - sent) n = size - sent;
    if (!program(at, bytes + sent, n)) return false;
    sent += n;
  }

  uint8_t verify[ICON_BYTES];
  if (!readAt(addr + sizeof(h), verify, size) ||
      memcmp(verify, bytes, size) != 0) return false;
  const uint8_t committed = 0;
  if (!program(addr + offsetof(IconHeader, committed), &committed, 1)) return false;
  storedBank = static_cast<int8_t>(bank);
  return true;
}
