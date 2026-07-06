#pragma once
// Tiny host-side monochrome canvas that mimics the device framebuffer, plus
// a BMP writer so playthroughs can be inspected as images. Host-only code —
// this never ships in the firmware, so std library use is fine here.
#include <cstdint>
#include <string>
#include <vector>

class SimCanvas {
 public:
  SimCanvas(int w, int h) : width_(w), height_(h), pixels_(static_cast<size_t>(w) * h, 255) {}

  int width() const { return width_; }
  int height() const { return height_; }

  void clear(uint8_t v = 255) { std::fill(pixels_.begin(), pixels_.end(), v); }

  void set(const int x, const int y, const uint8_t v) {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) return;
    pixels_[static_cast<size_t>(y) * width_ + x] = v;
  }

  void fillRect(int x, int y, int w, int h, uint8_t v);
  void drawRect(int x, int y, int w, int h, int thickness, uint8_t v);
  void drawLine(int x1, int y1, int x2, int y2, uint8_t v);
  void fillCircle(int cx, int cy, int r, uint8_t v);
  void drawCircle(int cx, int cy, int r, int thickness, uint8_t v);
  // 50% checkerboard dither over a rect (the fog look).
  void ditherRect(int x, int y, int w, int h, uint8_t v, int step = 2);

  // 5x7 bitmap font, uppercase/digits/punctuation only; `scale` integer-zooms.
  void drawText(int x, int y, const char* text, int scale, uint8_t v);
  static int textWidth(const char* text, int scale);

  bool writeBmp(const std::string& path) const;

 private:
  int width_;
  int height_;
  std::vector<uint8_t> pixels_;  // 8-bit grayscale, 255 = white
};
