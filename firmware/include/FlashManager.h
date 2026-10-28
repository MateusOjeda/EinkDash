#ifndef EINKDASH_FLASH_MANAGER_H
#define EINKDASH_FLASH_MANAGER_H

#include <Arduino.h>
#include <SPI.h>

// Winbond W25Q16 EXTERNO, independente da Flash interna do ESP32.
// Reserva exclusiva: 0x100000 .. 0x118FFF (25 setores de 4 KiB).
// NUNCA apaga uma area desconhecida: cache novo exige area totalmente FF.
class FlashManager {
public:
  static constexpr uint32_t FLASH_SIZE = 2097152UL;
  static constexpr uint32_t META_ADDR = 0x100000UL;
  static constexpr uint32_t PIXELS_ADDR = 0x101000UL;
  static constexpr uint32_t IMAGE_SIZE = 800UL * 480UL / 4UL; // 96000 B
  static constexpr uint32_t SECTOR_SIZE = 4096UL;
  static constexpr uint32_t PAYLOAD_SECTORS = (IMAGE_SIZE + SECTOR_SIZE - 1) / SECTOR_SIZE;

  bool begin();
  bool isCached(uint32_t key);
  bool startWrite(uint32_t key);
  bool writeChunk(uint32_t offset, const uint8_t* data, size_t length);
  bool finishWrite();
  bool readChunk(uint32_t offset, uint8_t* out, size_t length);

private:
#pragma pack(push, 1)
  struct Header {
    char magic[4];
    uint32_t version;
    uint32_t signature;
    uint32_t imageSize;
    uint32_t crc32;
    uint8_t committed;
    uint8_t padding[11];
  };
#pragma pack(pop)
  static_assert(sizeof(Header) == 32, "Header do cache W25Q16 deve ocupar 32 B");

  bool _present = false;
  bool _writing = false;
  bool _checked = false;
  uint32_t _currentKey = 0;
  uint32_t _writingKey = 0;
  uint32_t _writeOffset = 0;
  uint32_t _crc = 0xFFFFFFFFUL;

  void select();
  void deselect();
  uint8_t status();
  bool waitReady(uint32_t timeoutMs);
  bool writeEnable();
  bool eraseSector(uint32_t address);
  bool program(uint32_t address, const uint8_t* bytes, size_t length);
  bool readAt(uint32_t address, uint8_t* data, size_t count);
  bool blank(uint32_t address, uint32_t size);
  bool readHeader(Header& h);
  bool hasOurMagic(const Header& h) const;
  uint32_t crcUpdate(uint32_t crc, const uint8_t* bytes, size_t length) const;
};

#endif
