#include "QFontGfx.h"
#include <GxEPD2_4G.h>
#include <string.h>
#include <algorithm>

bool QFontGfx::begin(fs::FS& fs, const char* path) {
  end();
  _file = fs.open(path, FILE_READ);
  if (!_file) return false;

  const uint32_t fileSize = _file.size();
  FontHeader header{};
  if (fileSize < sizeof(header) ||
      _file.read(reinterpret_cast<uint8_t*>(&header), sizeof(header)) != sizeof(header) ||
      memcmp(header.magic, "QFNT", 4) != 0 ||
      header.version != 1 || header.headerSize != sizeof(header) ||
      header.glyphCount == 0 || header.glyphCount > 8192 ||
      header.indexOffset < sizeof(header) ||
      static_cast<uint64_t>(header.indexOffset) +
          static_cast<uint64_t>(header.glyphCount) * sizeof(Glyph) > fileSize ||
      header.dataOffset > fileSize) {
    end();
    return false;
  }

  if (!_file.seek(header.indexOffset)) { end(); return false; }
  _glyphs.resize(header.glyphCount);
  if (_file.read(reinterpret_cast<uint8_t*>(_glyphs.data()),
                 _glyphs.size() * sizeof(Glyph)) != _glyphs.size() * sizeof(Glyph)) {
    end();
    return false;
  }

  uint32_t previous = 0;
  for (size_t i = 0; i < _glyphs.size(); ++i) {
    const Glyph& g = _glyphs[i];
    const uint32_t bytes = (static_cast<uint32_t>(g.width) * g.height + 3) / 4;
    if ((i > 0 && g.codepoint <= previous) ||
        bytes != g.dataSize ||
        g.dataOffset < header.dataOffset ||
        static_cast<uint64_t>(g.dataOffset) + g.dataSize > fileSize) {
      end();
      return false;
    }
    previous = g.codepoint;
  }

  _pixelSize = header.pixelSize;
  _lineHeight = header.lineHeight;
  return true;
}

void QFontGfx::end() {
  if (_file) _file.close();
  _glyphs.clear();
  _pixelSize = 0;
  _lineHeight = 0;
}

const QFontGfx::Glyph* QFontGfx::findGlyph(uint32_t codepoint) const {
  size_t lo = 0, hi = _glyphs.size();
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (_glyphs[mid].codepoint < codepoint) lo = mid + 1;
    else hi = mid;
  }
  return lo < _glyphs.size() && _glyphs[lo].codepoint == codepoint
             ? &_glyphs[lo] : nullptr;
}

uint32_t QFontGfx::nextCodepoint(const String& s, size_t& pos) {
  if (pos >= s.length()) return 0;
  const uint8_t first = static_cast<uint8_t>(s[pos++]);
  if (first < 0x80) return first;

  unsigned continuation = 0;
  uint32_t codepoint = 0;
  uint32_t minCodepoint = 0;
  if ((first & 0xE0) == 0xC0) {
    continuation = 1; codepoint = first & 0x1F; minCodepoint = 0x80;
  } else if ((first & 0xF0) == 0xE0) {
    continuation = 2; codepoint = first & 0x0F; minCodepoint = 0x800;
  } else if ((first & 0xF8) == 0xF0) {
    continuation = 3; codepoint = first & 0x07; minCodepoint = 0x10000;
  } else return '?';

  for (unsigned i = 0; i < continuation; ++i) {
    if (pos >= s.length()) return '?';
    uint8_t next = static_cast<uint8_t>(s[pos]);
    if ((next & 0xC0) != 0x80) return '?';
    ++pos;
    codepoint = (codepoint << 6) | (next & 0x3F);
  }
  if (codepoint < minCodepoint || codepoint > 0x10FFFF ||
      (codepoint >= 0xD800 && codepoint <= 0xDFFF)) return '?';
  return codepoint;
}

int16_t QFontGfx::textWidth(const String& text) const {
  if (!isOpen()) return 0;

  int32_t width = 0;
  size_t pos = 0;
  while (pos < text.length()) {
    const uint32_t codepoint = nextCodepoint(text, pos);
    if (codepoint == '\n' || codepoint == '\r') break;
    const Glyph* glyph = findGlyph(codepoint);
    if (!glyph) glyph = findGlyph('?');
    if (glyph) width += glyph->advance;
  }
  return static_cast<int16_t>(std::max<int32_t>(0, std::min<int32_t>(width, 32767)));
}

uint16_t QFontGfx::shade(uint8_t coverage, uint16_t background) {
  // QFNT: 0=transparente, 1=33%, 2=67%, 3=preto opaco.
  // Para branco, preserva os quatro niveis da propria fonte.
  if (background == GxEPD_WHITE) {
    switch (coverage) {
      case 1: return GxEPD_LIGHTGREY;
      case 2: return GxEPD_DARKGREY;
      default: return GxEPD_BLACK;
    }
  }
  // Antialiasing sobre o cinza claro do cabecalho/cards.
  if (background == GxEPD_LIGHTGREY) {
    return coverage == 3 ? GxEPD_BLACK : GxEPD_DARKGREY;
  }
  return GxEPD_BLACK;
}

void QFontGfx::drawText(Adafruit_GFX& gfx, int16_t x, int16_t baseline,
                        const String& text, uint16_t background,
                        int16_t clipTop, int16_t clipBottom) {
  if (!isOpen()) return;
  int32_t penX = x;
  int32_t penY = baseline;
  size_t pos = 0;

  while (pos < text.length()) {
    uint32_t codepoint = nextCodepoint(text, pos);
    if (codepoint == '\r') continue;
    if (codepoint == '\n') {
      penX = x;
      penY += _lineHeight;
      continue;
    }
    const Glyph* glyph = findGlyph(codepoint);
    if (!glyph) glyph = findGlyph('?');
    if (!glyph) continue;

    const int32_t gx = penX + glyph->xOffset;
    const int32_t gy = penY + glyph->yOffset;
    const int32_t gxEnd = gx + glyph->width;
    const int32_t gyEnd = gy + glyph->height;

    if (glyph->dataSize && gxEnd > 0 && gx < gfx.width() &&
        gyEnd > clipTop && gy < clipBottom) {
      // Um glifo por vez: nao copia fontes completas nem framebuffer para RAM.
      _scratch.resize(glyph->dataSize);
      if (_file.seek(glyph->dataOffset) &&
          _file.read(_scratch.data(), _scratch.size()) == _scratch.size()) {
        const int32_t y0 = std::max<int32_t>(gy, clipTop);
        const int32_t y1 = std::min<int32_t>(gyEnd, clipBottom);
        const int32_t x0 = std::max<int32_t>(gx, 0);
        const int32_t x1 = std::min<int32_t>(gxEnd, gfx.width());
        for (int32_t py = y0; py < y1; ++py) {
          for (int32_t px = x0; px < x1; ++px) {
            const uint32_t index = (py - gy) * glyph->width + (px - gx);
            const uint8_t coverage =
                (_scratch[index >> 2] >> (6 - 2 * (index & 3))) & 3;
            if (coverage) gfx.drawPixel(px, py, shade(coverage, background));
          }
        }
      }
    }
    penX += glyph->advance;
  }
}
