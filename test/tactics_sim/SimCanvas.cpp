#include "SimCanvas.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

// Each glyph is 7 rows of 5 columns, written as readable strings.
struct Glyph {
  char c;
  const char* rows[7];
};

constexpr Glyph FONT[] = {
    {' ', {".....", ".....", ".....", ".....", ".....", ".....", "....."}},
    {'0', {".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###."}},
    {'1', {"..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###."}},
    {'2', {".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####"}},
    {'3', {".###.", "#...#", "....#", "..##.", "....#", "#...#", ".###."}},
    {'4', {"...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#."}},
    {'5', {"#####", "#....", "####.", "....#", "....#", "#...#", ".###."}},
    {'6', {".###.", "#....", "####.", "#...#", "#...#", "#...#", ".###."}},
    {'7', {"#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..."}},
    {'8', {".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###."}},
    {'9', {".###.", "#...#", "#...#", ".####", "....#", "....#", ".###."}},
    {'A', {".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}},
    {'B', {"####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."}},
    {'C', {".###.", "#...#", "#....", "#....", "#....", "#...#", ".###."}},
    {'D', {"####.", "#...#", "#...#", "#...#", "#...#", "#...#", "####."}},
    {'E', {"#####", "#....", "#....", "####.", "#....", "#....", "#####"}},
    {'F', {"#####", "#....", "#....", "####.", "#....", "#....", "#...."}},
    {'G', {".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".###."}},
    {'H', {"#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}},
    {'I', {".###.", "..#..", "..#..", "..#..", "..#..", "..#..", ".###."}},
    {'J', {"..###", "...#.", "...#.", "...#.", "...#.", "#..#.", ".##.."}},
    {'K', {"#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#"}},
    {'L', {"#....", "#....", "#....", "#....", "#....", "#....", "#####"}},
    {'M', {"#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#"}},
    {'N', {"#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#", "#...#"}},
    {'O', {".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}},
    {'P', {"####.", "#...#", "#...#", "####.", "#....", "#....", "#...."}},
    {'Q', {".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#"}},
    {'R', {"####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"}},
    {'S', {".####", "#....", "#....", ".###.", "....#", "....#", "####."}},
    {'T', {"#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."}},
    {'U', {"#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}},
    {'V', {"#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.."}},
    {'W', {"#...#", "#...#", "#...#", "#.#.#", "#.#.#", "##.##", "#...#"}},
    {'X', {"#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#"}},
    {'Y', {"#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.."}},
    {'Z', {"#####", "....#", "...#.", "..#..", ".#...", "#....", "#####"}},
    {':', {".....", "..#..", "..#..", ".....", "..#..", "..#..", "....."}},
    {'.', {".....", ".....", ".....", ".....", ".....", ".##..", ".##.."}},
    {',', {".....", ".....", ".....", ".....", ".##..", "..#..", ".#..."}},
    {'!', {"..#..", "..#..", "..#..", "..#..", "..#..", ".....", "..#.."}},
    {'?', {".###.", "#...#", "....#", "...#.", "..#..", ".....", "..#.."}},
    {'-', {".....", ".....", ".....", "#####", ".....", ".....", "....."}},
    {'/', {"....#", "...#.", "...#.", "..#..", ".#...", ".#...", "#...."}},
    {'>', {".#...", "..#..", "...#.", "....#", "...#.", "..#..", ".#..."}},
    {'*', {".....", ".#.#.", "..#..", "#####", "..#..", ".#.#.", "....."}},
    {'(', {"...#.", "..#..", ".#...", ".#...", ".#...", "..#..", "...#."}},
    {')', {".#...", "..#..", "...#.", "...#.", "...#.", "..#..", ".#..."}},
};

const Glyph* findGlyph(char c) {
  if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  for (const Glyph& g : FONT) {
    if (g.c == c) return &g;
  }
  return nullptr;
}

}  // namespace

void SimCanvas::fillRect(const int x, const int y, const int w, const int h, const uint8_t v) {
  for (int yy = y; yy < y + h; yy++) {
    for (int xx = x; xx < x + w; xx++) set(xx, yy, v);
  }
}

void SimCanvas::drawRect(const int x, const int y, const int w, const int h, const int thickness, const uint8_t v) {
  fillRect(x, y, w, thickness, v);
  fillRect(x, y + h - thickness, w, thickness, v);
  fillRect(x, y, thickness, h, v);
  fillRect(x + w - thickness, y, thickness, h, v);
}

void SimCanvas::drawLine(int x1, int y1, const int x2, const int y2, const uint8_t v) {
  const int dx = std::abs(x2 - x1), sx = x1 < x2 ? 1 : -1;
  const int dy = -std::abs(y2 - y1), sy = y1 < y2 ? 1 : -1;
  int err = dx + dy;
  while (true) {
    set(x1, y1, v);
    if (x1 == x2 && y1 == y2) break;
    const int e2 = 2 * err;
    if (e2 >= dy) {
      err += dy;
      x1 += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y1 += sy;
    }
  }
}

void SimCanvas::fillCircle(const int cx, const int cy, const int r, const uint8_t v) {
  for (int y = -r; y <= r; y++) {
    for (int x = -r; x <= r; x++) {
      if (x * x + y * y <= r * r) set(cx + x, cy + y, v);
    }
  }
}

void SimCanvas::drawCircle(const int cx, const int cy, const int r, const int thickness, const uint8_t v) {
  const int inner = (r - thickness) * (r - thickness);
  for (int y = -r; y <= r; y++) {
    for (int x = -r; x <= r; x++) {
      const int d = x * x + y * y;
      if (d <= r * r && d >= inner) set(cx + x, cy + y, v);
    }
  }
}

void SimCanvas::ditherRect(const int x, const int y, const int w, const int h, const uint8_t v, const int step) {
  for (int yy = y; yy < y + h; yy++) {
    for (int xx = x; xx < x + w; xx++) {
      if ((xx / 1 + yy / 1) % step == 0) set(xx, yy, v);
    }
  }
}

void SimCanvas::drawText(int x, const int y, const char* text, const int scale, const uint8_t v) {
  for (const char* p = text; *p; p++) {
    const Glyph* g = findGlyph(*p);
    if (g) {
      for (int row = 0; row < 7; row++) {
        for (int col = 0; col < 5; col++) {
          if (g->rows[row][col] == '#') {
            fillRect(x + col * scale, y + row * scale, scale, scale, v);
          }
        }
      }
    }
    x += 6 * scale;
  }
}

int SimCanvas::textWidth(const char* text, const int scale) {
  return static_cast<int>(std::strlen(text)) * 6 * scale - scale;
}

bool SimCanvas::writeBmp(const std::string& path) const {
  // 8-bit grayscale BMP with a 256-entry palette.
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  const int rowSize = (width_ + 3) & ~3;  // rows padded to 4 bytes
  const uint32_t dataSize = static_cast<uint32_t>(rowSize) * height_;
  const uint32_t paletteSize = 256 * 4;
  const uint32_t dataOffset = 14 + 40 + paletteSize;
  const uint32_t fileSize = dataOffset + dataSize;

  auto put16 = [&](uint16_t val) { std::fputc(val & 0xFF, f), std::fputc(val >> 8, f); };
  auto put32 = [&](uint32_t val) {
    for (int i = 0; i < 4; i++) std::fputc((val >> (8 * i)) & 0xFF, f);
  };

  std::fputc('B', f);
  std::fputc('M', f);
  put32(fileSize);
  put32(0);
  put32(dataOffset);
  put32(40);  // BITMAPINFOHEADER
  put32(static_cast<uint32_t>(width_));
  put32(static_cast<uint32_t>(height_));
  put16(1);
  put16(8);
  put32(0);  // BI_RGB
  put32(dataSize);
  put32(2835);
  put32(2835);
  put32(256);
  put32(0);
  for (int i = 0; i < 256; i++) {
    std::fputc(i, f);
    std::fputc(i, f);
    std::fputc(i, f);
    std::fputc(0, f);
  }
  std::vector<uint8_t> row(static_cast<size_t>(rowSize), 0);
  for (int y = height_ - 1; y >= 0; y--) {  // BMP is bottom-up
    std::memcpy(row.data(), &pixels_[static_cast<size_t>(y) * width_], static_cast<size_t>(width_));
    std::fwrite(row.data(), 1, static_cast<size_t>(rowSize), f);
  }
  std::fclose(f);
  return true;
}
