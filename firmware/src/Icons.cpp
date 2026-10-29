#include "Icons.h"
#include <string.h>
#include <algorithm>
#include <GxEPD2_4G.h>

bool Icons::begin(const char* path) {
  end();
  _file = LittleFS.open(path, "r");
  if (!_file) return false;
  if (_file.size() < sizeof(Header) ||
      _file.read(reinterpret_cast<uint8_t*>(&_header), sizeof(_header)) != sizeof(_header) ||
      memcmp(_header.magic, "WIC2", 4) != 0 ||
      _header.version != 1 ||
      _header.count == 0 || _header.count > 128 ||
      _header.indexOffset < sizeof(Header) ||
      uint64_t(_header.indexOffset) + uint64_t(_header.count) * sizeof(Entry) > _file.size() ||
      _header.dataOffset < _header.indexOffset + uint32_t(_header.count) * sizeof(Entry) ||
      _header.dataOffset > _file.size()) {
    end();
    return false;
  }
  return true;
}

void Icons::end() {
  if (_file) _file.close();
  memset(&_header, 0, sizeof(_header));
}

bool Icons::find(uint16_t id, Entry& out) {
  if (!isOpen()) return false;
  // Busca no indice na Flash, sem aloca-lo na RAM (ate 128 registros).
  for (uint16_t i = 0; i < _header.count; ++i) {
    const uint32_t at = _header.indexOffset + uint32_t(i) * sizeof(Entry);
    if (!_file.seek(at) ||
        _file.read(reinterpret_cast<uint8_t*>(&out), sizeof(out)) != sizeof(out)) return false;
    if (out.id != id) continue;
    const uint32_t stride = (uint32_t(out.width) + 3) / 4;
    return out.width > 0 && out.width <= 128 && out.height > 0 && out.height <= 112 &&
           out.size == stride * out.height &&
           out.offset >= _header.dataOffset &&
           uint64_t(out.offset) + out.size <= _file.size();
  }
  return false;
}

bool Icons::draw(Adafruit_GFX& gfx, uint16_t id, int16_t x, int16_t y,
                        int16_t width, int16_t height,
                        int16_t clipTop, int16_t clipBottom) {
  if (width <= 0 || height <= 0 || y >= clipBottom || y + height <= clipTop) return true;
  Entry e{};
  if (!find(id, e)) return false;
  const uint32_t stride = (uint32_t(e.width) + 3) / 4;
  uint8_t row[32]; // 128 pixels / 4 pixels por byte
  const uint16_t colors[4] = {GxEPD_BLACK, GxEPD_DARKGREY, GxEPD_LIGHTGREY, GxEPD_WHITE};
  const int16_t firstY = std::max<int16_t>(0, clipTop - y);
  const int16_t lastY  = std::min<int16_t>(height, clipBottom - y);
  for (int16_t dy = firstY; dy < lastY; ++dy) {
    const uint32_t sy = uint32_t(dy) * e.height / height;
    if (!_file.seek(e.offset + sy * stride) || _file.read(row, stride) != stride) return false;
    for (int16_t dx = 0; dx < width; ++dx) {
      const int16_t px = x + dx;
      if (px < 0 || px >= gfx.width()) continue;
      const uint32_t sx = uint32_t(dx) * e.width / width;
      const uint8_t v = (row[sx >> 2] >> (6 - 2 * (sx & 3))) & 3;
      if (v != 3) gfx.drawPixel(px, y + dy, colors[v]); // branco = fundo branco
    }
  }
  return true;
}
