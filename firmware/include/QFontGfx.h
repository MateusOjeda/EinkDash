#ifndef EINKDASH_QFONT_GFX_H
#define EINKDASH_QFONT_GFX_H

#include <Arduino.h>
#include <FS.h>
#include <Adafruit_GFX.h>
#include <stdint.h>
#include <vector>

// Le as fontes QFNT v1 criadas para o EinkDash diretamente do LittleFS.
// Desenha em um Adafruit_GFX (GxEPD2_4G) paginado, sem framebuffer de 96 KB.
class QFontGfx {
 public:
  bool begin(fs::FS& fs, const char* path);
  void end();
  bool isOpen() const { return static_cast<bool>(_file); }
  uint16_t pixelSize() const { return _pixelSize; }
  uint16_t lineHeight() const { return _lineHeight; }
  int16_t textWidth(const String& text) const;

  // baseline: linha de base do texto; clipTop/clipBottom: pagina visivel (fim exclusivo).
  // backgroundColor informa a cor que ja foi desenhada abaixo do texto.
  void drawText(Adafruit_GFX& gfx, int16_t x, int16_t baseline,
                const String& text, uint16_t backgroundColor,
                int16_t clipTop, int16_t clipBottom);

 private:
#pragma pack(push, 1)
  struct FontHeader {
    char magic[4];
    uint16_t version;
    uint16_t headerSize;
    uint16_t pixelSize;
    uint16_t ascent;
    uint16_t descent;
    uint16_t lineHeight;
    uint32_t glyphCount;
    uint32_t indexOffset;
    uint32_t dataOffset;
    uint32_t flags;
  };
  struct Glyph {
    uint32_t codepoint;
    int16_t xOffset;
    int16_t yOffset;
    uint16_t width;
    uint16_t height;
    int16_t advance;
    uint16_t reserved;
    uint32_t dataOffset;
    uint32_t dataSize;
  };
#pragma pack(pop)

  static_assert(sizeof(FontHeader) == 32, "Cabecalho QFNT precisa ter 32 bytes");
  static_assert(sizeof(Glyph) == 24, "Registro QFNT precisa ter 24 bytes");

  File _file;
  std::vector<Glyph> _glyphs;
  std::vector<uint8_t> _scratch; // reutilizado, evita alocacoes por caractere
  uint16_t _pixelSize = 0;
  uint16_t _lineHeight = 0;

  const Glyph* findGlyph(uint32_t codepoint) const;
  static uint32_t nextCodepoint(const String& s, size_t& position);
  static uint16_t shade(uint8_t coverage, uint16_t background);
};

#endif
