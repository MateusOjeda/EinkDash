#ifndef EINKDASH_ICONS_H
#define EINKDASH_ICONS_H

#include <Arduino.h>
#include <LittleFS.h>
#include <Adafruit_GFX.h>

// Le qualquer icone no formato WIC2 de /icons.bin, por LINHAS (ate 32 B/linha).
// Nao carrega imagens completas, indice completo nem framebuffer na RAM.
class Icons {
public:
  bool begin(const char* path = "/icons.bin");
  void end();
  bool isOpen() const { return static_cast<bool>(_file); }
  // Desenha com nearest-neighbor para a dimensao alvo, sem buffer grande.
  bool draw(Adafruit_GFX& gfx, uint16_t id, int16_t x, int16_t y,
            int16_t width, int16_t height, int16_t clipTop, int16_t clipBottom);
private:
#pragma pack(push, 1)
  struct Header {
    char magic[4];
    uint16_t version;
    uint16_t count;
    uint32_t indexOffset;
    uint32_t dataOffset;
  };
  struct Entry {
    uint32_t id;
    uint16_t width;
    uint16_t height;
    uint32_t offset;
    uint32_t size;
    uint32_t crc32;
  };
#pragma pack(pop)
  static_assert(sizeof(Header) == 16, "WIC2 Header deve ter 16 bytes");
  static_assert(sizeof(Entry) == 20, "WIC2 Entry deve ter 20 bytes");
  bool find(uint16_t id, Entry& out);
  File _file;
  Header _header{};
};

#endif
