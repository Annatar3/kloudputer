// Kloudputer — palette and the drawing pieces shared by the splash, chat and menus.
// Everything draws on lgfx::LovyanGFX, so the same code renders on the device and in the host preview.
#pragma once
#ifdef ARDUINO
#include <M5GFX.h>
#else
// Host preview: the harness includes its Arduino String shim before this header.
#include <lgfx/v1/platforms/device.hpp>
#include <lgfx/v1/platforms/common.hpp>
#include <lgfx/v1/LGFXBase.hpp>
#include <lgfx/v1/LGFX_Sprite.hpp>
#endif
#include <algorithm>
#include <cmath>
#include <vector>
#include "brand.h"

namespace ui {
using Gfx = lgfx::LovyanGFX;

constexpr uint16_t rgb(uint32_t c) { return ((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F); }
constexpr uint16_t BG = rgb(0x1A1918);
constexpr uint16_t BAR = rgb(0x262422);
constexpr uint16_t LINE = rgb(0x3A3633);
constexpr uint16_t SEL = rgb(0x3A2A22);
constexpr uint16_t ORANGE = rgb(0xD97757);
constexpr uint16_t TEXT = rgb(0xF0EEE6);
constexpr uint16_t SOFT = rgb(0xBFBAAE);
constexpr uint16_t DIM = rgb(0x8A857C);
constexpr uint16_t SYS = rgb(0xD4A15A);
constexpr uint16_t ERR = rgb(0xE06C5A);
constexpr uint16_t GREEN = rgb(0x8DB377);

constexpr int W = 240, H = 135;
constexpr int CHAR_W = 6;  // Font0

// The cloud mark: three puffs on a flat base, `size` wide and about 0.64 x `size` tall, centered in the
// w x h cell at (x, y). Anti-aliased shapes blend with pixels read back from the target, and this
// panel's readback isn't dependable, so the mark is drawn into a small sprite over the known background
// and pushed as an opaque block.
//   outline: an outlined cloud with a ">_" prompt inside (splash).
//   filled:  a solid cloud, with a background ">" cut in when `prompt` (header, busy screen).
enum class Cloud : uint8_t { Outline, Filled };

// The silhouette, grown (inset < 0) or shrunk (inset > 0) by `inset` pixels.
inline void cloudShape(Gfx& s, float cx, float cy, float size, float inset, uint16_t col) {
  s.fillSmoothCircle(cx - 0.42f * size, cy + 0.08f * size, 0.30f * size - inset, col);
  s.fillSmoothCircle(cx + 0.02f * size, cy - 0.12f * size, 0.42f * size - inset, col);
  s.fillSmoothCircle(cx + 0.45f * size, cy + 0.10f * size, 0.28f * size - inset, col);
  s.fillSmoothRoundRect(cx - 0.72f * size + inset, cy + 0.02f * size + inset, 1.45f * size - 2 * inset,
                        0.36f * size - 2 * inset, std::max(0.0f, 0.18f * size - inset), col);
}

inline uint16_t mix565(uint16_t a, uint16_t b, float f) {  // f = 0 gives a, 1 gives b
  const int r = (a >> 11) + ((b >> 11) - (a >> 11)) * f + 0.5f;
  const int g = ((a >> 5) & 63) + (((b >> 5) & 63) - ((a >> 5) & 63)) * f + 0.5f;
  const int bl = (a & 31) + ((b & 31) - (a & 31)) * f + 0.5f;
  return r << 11 | g << 5 | bl;
}

// Hollows the anti-aliased silhouette in `s` to a `stroke`-wide outline by each pixel's true distance
// to the outside. Shrinking each puff on its own leaves seams inside, where the puffs overlap.
inline void hollow(lgfx::LGFX_Sprite& s, int w, int h, float stroke, uint16_t col, uint16_t bg) {
  int shift = 11, mask = 31;  // judge coverage by the channel where col and bg differ most
  if (std::abs(((col >> 5) & 63) - ((bg >> 5) & 63)) > 2 * std::abs((col >> 11) - (bg >> 11))) shift = 5, mask = 63;
  const float span = static_cast<float>(((col >> shift) & mask) - ((bg >> shift) & mask));
  std::vector<float> cover(w * h);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const float c = (((s.readPixel(x, y) >> shift) & mask) - ((bg >> shift) & mask)) / span;
      cover[y * w + x] = std::min(1.0f, std::max(0.0f, c));
    }
  const int reach = static_cast<int>(stroke) + 2;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      if (cover[y * w + x] < 0.5f) continue;
      float nearest = reach + 1.0f;
      for (int dy = -reach; dy <= reach; ++dy)
        for (int dx = -reach; dx <= reach; ++dx) {
          const int nx = x + dx, ny = y + dy;
          const bool outside = nx < 0 || ny < 0 || nx >= w || ny >= h || cover[ny * w + nx] < 0.5f;
          if (outside) nearest = std::min(nearest, std::sqrt(static_cast<float>(dx * dx + dy * dy)) - 0.5f);
        }
      const float inner = std::min(1.0f, std::max(0.0f, nearest - stroke));
      if (inner > 0) s.drawPixel(x, y, mix565(bg, col, cover[y * w + x] * (1 - inner)));
    }
}

inline void cloud(Gfx& d, int x, int y, int w, int h, float size, Cloud style, uint16_t col, uint16_t bg,
                  bool prompt = true) {
  lgfx::LGFX_Sprite s(&d);
  s.setColorDepth(16);
  if (!s.createSprite(w, h)) {
    d.fillRect(x, y, w, h, bg);
    return;
  }
  s.fillScreen(bg);
  const float cx = w / 2.0f, cy = h / 2.0f + 0.08f * size;  // centers the silhouette's box, not its puffs
  cloudShape(s, cx, cy, size, 0, col);
  const float stroke = std::max(1.5f, 0.09f * size);
  if (style == Cloud::Outline) hollow(s, w, h, stroke, col, bg);
  const uint16_t ink = style == Cloud::Outline ? col : bg;
  if (prompt) {
    const float half = stroke / 2;
    s.drawWedgeLine(cx - 0.30f * size, cy - 0.08f * size, cx - 0.12f * size, cy + 0.06f * size, half, half, ink);
    s.drawWedgeLine(cx - 0.12f * size, cy + 0.06f * size, cx - 0.30f * size, cy + 0.20f * size, half, half, ink);
  }
  if (prompt && style == Cloud::Outline) {
    s.fillRect(cx - 0.02f * size, cy + 0.12f * size, 0.30f * size, std::max(2.0f, stroke * 0.9f), ink);
  }
  s.pushSprite(x, y);
}

inline void wifiIcon(Gfx& d, int x, int y, int bars) {
  for (int i = 0; i < 3; ++i) d.fillRect(x + i * 3, y + 6 - (i + 1) * 2, 2, (i + 1) * 2, i < bars ? SOFT : LINE);
}

inline void speakerIcon(Gfx& d, int x, int y) {
  d.fillRect(x, y + 2, 2, 3, SOFT);
  d.fillTriangle(x + 2, y + 3, x + 5, y, x + 5, y + 6, SOFT);
  d.drawFastVLine(x + 7, y + 1, 5, SOFT);
}

inline void batteryIcon(Gfx& d, int x, int y, int pct) {
  d.drawRect(x, y, 12, 7, SOFT);
  d.fillRect(x + 12, y + 2, 1, 3, SOFT);
  const int fill = pct > 0 ? std::max(1, 8 * std::min(pct, 100) / 100) : 0;
  d.fillRect(x + 2, y + 2, fill, 3, pct < 20 ? ERR : SOFT);
}

// Font0 text padded with spaces (or cut) to exactly `cols` cells, so an opaque draw covers what was there.
inline String fit(const String& s, int cols) {
  String out = s.substring(0, std::max(cols, 0));
  while (static_cast<int>(out.length()) < cols) out += ' ';
  return out;
}

// Before the first line of each Claude reply: a small dot in the 11 x 7 cell at (x, y).
inline void replyMark(Gfx& d, int x, int y, uint16_t col, uint16_t bg) {
  lgfx::LGFX_Sprite s(&d);
  s.setColorDepth(16);
  if (!s.createSprite(11, 7)) {
    d.fillRect(x, y, 11, 7, bg);
    return;
  }
  s.fillScreen(bg);
  s.fillSmoothCircle(5, 3, 2, col);
  s.pushSprite(x, y);
}

// ---------- splash ----------
constexpr int kSplashX = 92;

inline void splash(Gfx& d, const char* version) {
  d.fillScreen(BG);
  cloud(d, 4, 40, 84, 56, 56, Cloud::Outline, ORANGE, BG);
  d.setTextDatum(lgfx::textdatum_t::top_left);
  d.setFont(&fonts::FreeSansBold12pt7b);
  d.setTextColor(TEXT);
  d.drawString(brand::kProduct, kSplashX - 2, 36);
  d.setFont(&fonts::Font0);
  d.setTextColor(SOFT);
  d.drawString(brand::kTagline, kSplashX, 64);
  d.setTextColor(DIM);
  d.drawString(version, kSplashX, 76);
}

// One boot phase: the status line under the title and a bar `done` of `total` full.
inline void splashStep(Gfx& d, const String& text, int done, int total) {
  d.setTextDatum(lgfx::textdatum_t::top_left);
  d.setFont(&fonts::Font0);
  d.setTextColor(ORANGE, BG);
  d.drawString(fit(text, (W - 12 - kSplashX) / CHAR_W).c_str(), kSplashX, 100);
  constexpr int kBarW = 120;
  d.fillRoundRect(kSplashX, 114, kBarW, 3, 1, LINE);
  if (done > 0) d.fillRoundRect(kSplashX, 114, kBarW * std::min(done, total) / total, 3, 1, ORANGE);
}
}  // namespace ui
