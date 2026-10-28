#include "QFontRenderer.h"
#include <cstring>
#include <cstdlib>

bool QFontRenderer::begin(fs::FS& fs, const char* filename) {
  end();
  _file = fs.open(filename, FILE_READ);
  if (!_file) return false;
  if (_file.read(reinterpret_cast<uint8_t*>(&_header), sizeof(_header)) != sizeof(_header)) {
    end(); return false;
  }
  if (memcmp(_header.magic, "QFNT", 4) != 0 ||
      _header.version != 1 || _header.headerSize != 32 || _header.flags != 1 ||
      _header.glyphCount > 20000 ||
      _header.indexOffset != sizeof(QFontHeader) ||
      _header.dataOffset != 32 + _header.glyphCount * sizeof(QGlyphRecord) ||
      _header.dataOffset > static_cast<uint32_t>(_file.size())) {
    end(); return false;
  }
  _glyphs.resize(_header.glyphCount);
  size_t len = _glyphs.size() * sizeof(QGlyphRecord);
  if (!_file.seek(_header.indexOffset) ||
      _file.read(reinterpret_cast<uint8_t*>(_glyphs.data()), len) != len) {
    end(); return false;
  }
  for (size_t i = 0; i < _glyphs.size(); i++) {
    const auto& glyph = _glyphs[i];
    if ((i > 0 && _glyphs[i-1].codepoint >= glyph.codepoint) ||
        glyph.dataSize != (static_cast<uint32_t>(glyph.width) * glyph.height + 3) / 4 ||
        glyph.dataOffset < _header.dataOffset ||
        glyph.dataOffset + static_cast<uint64_t>(glyph.dataSize) > _file.size()) {
      end(); return false;
    }
  }
  return true;
}

void QFontRenderer::end() {
  if (_file) _file.close();
  _glyphs.clear();
  memset(&_header, 0, sizeof(_header));
}

const QGlyphRecord* QFontRenderer::lookup(uint32_t cp) const {
  size_t low = 0, high = _glyphs.size();
  while (low < high) {
    size_t mid = low + (high - low) / 2;
    if (_glyphs[mid].codepoint < cp) low = mid + 1;
    else high = mid;
  }
  return low < _glyphs.size() && _glyphs[low].codepoint == cp
         ? &_glyphs[low] : nullptr;
}

uint32_t QFontRenderer::nextCodepoint(const String& s, size_t& i) {
  if (i >= s.length()) return 0;
  uint8_t a = static_cast<uint8_t>(s[i++]);
  if (a < 0x80) return a;
  uint8_t count = (a >= 0xC2 && a <= 0xDF) ? 1 :
                  (a >= 0xE0 && a <= 0xEF) ? 2 :
                  (a >= 0xF0 && a <= 0xF4) ? 3 : 0;
  if (count == 0) return '?';
  uint32_t cp = a & (0x7F >> (count + 1));
  for (uint8_t n=0; n<count; n++) {
    if (i >= s.length()) return '?';
    uint8_t b = static_cast<uint8_t>(s[i]);
    if ((b & 0xC0) != 0x80) return '?';
    i++;
    cp = (cp << 6) | (b & 0x3F);
  }
  if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return '?';
  return cp;
}

void QFontRenderer::clear(uint8_t* fb, uint16_t w, uint16_t h, uint8_t gray) {
  if (!fb || w % 4 != 0) return;
  gray &= 3;
  uint8_t value = (gray << 6) | (gray << 4) | (gray << 2) | gray;
  memset(fb, value, static_cast<size_t>(w) * h / 4);
}

uint8_t QFontRenderer::pixel(const uint8_t* fb, uint16_t w, uint16_t x, uint16_t y) {
  size_t idx = (static_cast<size_t>(y) * w + x) / 4;
  return (fb[idx] >> (6 - 2*(x % 4))) & 3;
}

void QFontRenderer::setPixel(uint8_t* fb, uint16_t w, uint16_t x, uint16_t y, uint8_t gray) {
  size_t idx = (static_cast<size_t>(y) * w + x) / 4;
  uint8_t shift = 6 - 2*(x % 4);
  fb[idx] = (fb[idx] & ~(3 << shift)) | ((gray & 3) << shift);
}

int QFontRenderer::drawText(uint8_t* fb, uint16_t w, uint16_t h,
                            int startX, int baseline, const String& str, uint8_t fg) {
  if (!fb || !_file || w % 4 != 0) return startX;
  fg &= 3;
  int x = startX;
  size_t i = 0;
  while (i < str.length()) {
    uint32_t cp = nextCodepoint(str, i);
    if (cp == '\r') continue;
    if (cp == '\n') { x = startX; baseline += _header.lineHeight; continue; }
    const QGlyphRecord* g = lookup(cp);
    if (!g) g = lookup('?');
    if (!g) continue;
    if (g->dataSize) {
      uint8_t* bitmap = static_cast<uint8_t*>(malloc(g->dataSize));
      if (!bitmap) return x;
      if (!_file.seek(g->dataOffset) || _file.read(bitmap, g->dataSize) != g->dataSize) {
        free(bitmap); return x;
      }
      for (uint32_t p = 0; p < static_cast<uint32_t>(g->width)*g->height; p++) {
        uint8_t alpha = (bitmap[p/4] >> (6 - 2*(p%4))) & 3;
        if (!alpha) continue;
        int px = x + g->xOffset + (p % g->width);
        int py = baseline + g->yOffset + (p / g->width);
        if (px < 0 || py < 0 || px >= w || py >= h) continue;
        uint8_t bg = pixel(fb, w, px, py);
        uint8_t blended = (bg*(3-alpha) + fg*alpha + 1) / 3;
        setPixel(fb, w, px, py, blended);
      }
      free(bitmap);
    }
    x += g->advance;
  }
  return x;
}
