#pragma once

#include <Arduino.h>
#include <FS.h>
#include <SD.h>
#include <vector>

// QFNT v1, 2 bits/pixel de COBERTURA:
// 0 = transparente, 1 = 33%, 2 = 67%, 3 = 100%.
// Framebuffer de DESTINO: 0 = preto, 1 = cinza-escuro,
// 2 = cinza-claro, 3 = branco. Quatro pixels por byte.

#pragma pack(push, 1)
struct QFontHeader {
  char magic[4];
  uint16_t version, headerSize, pixelSize, ascent, descent, lineHeight;
  uint32_t glyphCount, indexOffset, dataOffset, flags;
};

struct QGlyphRecord {
  uint32_t codepoint;
  int16_t xOffset, yOffset;
  uint16_t width, height;
  int16_t advance;
  uint16_t reserved;
  uint32_t dataOffset, dataSize;
};
#pragma pack(pop)

static_assert(sizeof(QFontHeader) == 32, "QFontHeader deve ter 32 bytes");
static_assert(sizeof(QGlyphRecord) == 24, "QGlyphRecord deve ter 24 bytes");

class QFontRenderer {
public:
  bool begin(fs::FS& fs, const char* filename);
  void end();
  bool isOpen() const { return static_cast<bool>(_file); }
  uint16_t lineHeight() const { return _header.lineHeight; }

  // 'baseline' é a LINHA DE BASE da primeira linha de texto.
  // framebuffer 2bpp já deve estar preenchido; retorna x final.
  int drawText(uint8_t* framebuffer, uint16_t fbWidth, uint16_t fbHeight,
               int x, int baseline, const String& utf8, uint8_t foreground = 0);

  // Utilitários para um framebuffer 2-bpp de largura múltipla de 4.
  static void clear(uint8_t* framebuffer, uint16_t width,
                    uint16_t height, uint8_t gray = 3);
  static uint8_t pixel(const uint8_t* framebuffer, uint16_t width,
                       uint16_t x, uint16_t y);
  static void setPixel(uint8_t* framebuffer, uint16_t width,
                       uint16_t x, uint16_t y, uint8_t gray);

private:
  const QGlyphRecord* lookup(uint32_t codepoint) const;
  static uint32_t nextCodepoint(const String& str, size_t& cursor);

  File _file;
  QFontHeader _header{};
  std::vector<QGlyphRecord> _glyphs;
};
