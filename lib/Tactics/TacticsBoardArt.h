#pragma once
// Per-tile board art for the tactics game, shared between the firmware
// activity (GfxRenderer) and the host simulator (SimCanvas adapter). The
// functions are templated on the renderer so both sides compile the exact
// same drawing code and the simulator can never drift from the device look.
//
// The renderer must provide the GfxRenderer primitive subset:
//   drawLine(x1, y1, x2, y2, bool black)
//   fillRect(x, y, w, h, bool black)
//   drawRect(x, y, w, h, int lineWidth, bool black)

#include "TacticsSprites.h"
#include "TacticsTypes.h"

namespace Tactics {
namespace BoardArt {

constexpr int TILE = 48;  // board tile size in pixels, on device and in the sim

template <typename Renderer>
void drawTerrainTile(const Renderer& r, const int px, const int py, const Terrain t) {
  switch (t) {
    case Terrain::Forest:
      // Two layered pines: stacked triangle canopies over a stub trunk.
      for (int tree = 0; tree < 2; tree++) {
        const int cx = px + (tree == 0 ? 16 : 35);
        const int top = py + (tree == 0 ? 5 : 17);
        const int halfW = tree == 0 ? 13 : 10;
        const int layers = tree == 0 ? 3 : 2;
        int y = top;
        for (int i = 0; i < layers; i++) {
          const int w = halfW * (i + 2) / (layers + 1);
          r.drawLine(cx, y, cx - w, y + 9, true);
          r.drawLine(cx, y, cx + w, y + 9, true);
          r.drawLine(cx - w, y + 9, cx + w, y + 9, true);
          y += 6;
        }
        r.fillRect(cx - 1, top + 6 * (layers - 1) + 9, 3, 5, true);
      }
      break;
    case Terrain::Mountain:
      // Twin peaks with a snow-line zigzag and light shading on the sunlit
      // face.
      r.drawLine(px + 29, py + 7, px + 9, py + 41, true);  // main peak
      r.drawLine(px + 29, py + 7, px + 45, py + 41, true);
      r.drawLine(px + 14, py + 20, px + 3, py + 41, true);  // fore peak
      r.drawLine(px + 14, py + 20, px + 22, py + 33, true);
      r.drawLine(px + 3, py + 41, px + 45, py + 41, true);   // base
      r.drawLine(px + 24, py + 16, px + 27, py + 19, true);  // snow line
      r.drawLine(px + 27, py + 19, px + 30, py + 15, true);
      r.drawLine(px + 30, py + 15, px + 33, py + 19, true);
      r.drawLine(px + 33, py + 25, px + 36, py + 32, true);  // shading
      r.drawLine(px + 36, py + 30, px + 39, py + 37, true);
      break;
    case Terrain::Water:
      // Rolling waves: flat-crest runs with soft rises, rows offset.
      for (int row = 0; row < 3; row++) {
        const int wy = py + 12 + row * 12;
        const int off = (row % 2) * 8;
        for (int x = px + 2 + off; x + 14 <= px + TILE - 2; x += 16) {
          r.drawLine(x, wy + 2, x + 4, wy + 2, true);
          r.drawLine(x + 4, wy + 2, x + 7, wy - 2, true);
          r.drawLine(x + 7, wy - 2, x + 11, wy - 2, true);
          r.drawLine(x + 11, wy - 2, x + 14, wy + 2, true);
        }
      }
      break;
    case Terrain::Rock:
      r.fillRect(px + 2, py + 2, TILE - 4, TILE - 4, true);
      break;
    case Terrain::Castle:
      r.drawRect(px + 8, py + 16, 32, 24, 2, true);
      for (int i = 0; i < 4; i++) r.fillRect(px + 8 + i * 9, py + 10, 5, 8, true);
      r.fillRect(px + 20, py + 28, 8, 12, true);
      break;
    case Terrain::EnemyBase:
      r.drawRect(px + 6, py + 14, 36, 26, 3, true);
      r.drawLine(px + 24, py + 14, px + 24, py + 2, true);
      r.drawLine(px + 24, py + 2, px + 38, py + 6, true);
      r.drawLine(px + 38, py + 6, px + 24, py + 10, true);
      break;
    default:
      break;
  }
}

template <typename Renderer>
void drawDangerShade(const Renderer& r, const int px, const int py) {
  // Diagonal hazard hatching: "/" lines where (x + y) % 12 == 0 in tile
  // space. TILE is a multiple of 12, so the pattern runs continuously
  // across adjacent danger tiles. Drawn under the unit plate so sprites
  // and HP bars stay clean.
  for (int s = 0; s < 2 * TILE - 1; s += 12) {
    const int x0 = s < TILE ? 0 : s - (TILE - 1);
    const int x1 = s < TILE ? s : TILE - 1;
    r.drawLine(px + x0, py + s - x0, px + x1, py + s - x1, true);
  }
}

template <typename Renderer>
void drawReachBorder(const Renderer& r, const int px, const int py, const bool halo) {
  // Corner brackets marking a reachable tile — calmer than a full border,
  // still crisp on the fog dither where the old filled square drowned.
  // `halo` clears a white margin around each leg first so the brackets
  // stay readable on top of the fog dithers.
  constexpr int L = 12, T = 2, IN = 2;
  if (halo) {
    r.fillRect(px + IN - 1, py + IN - 1, L + 2, T + 2, false);
    r.fillRect(px + IN - 1, py + IN - 1, T + 2, L + 2, false);
    r.fillRect(px + TILE - IN - L - 1, py + IN - 1, L + 2, T + 2, false);
    r.fillRect(px + TILE - IN - T - 1, py + IN - 1, T + 2, L + 2, false);
    r.fillRect(px + IN - 1, py + TILE - IN - T - 1, L + 2, T + 2, false);
    r.fillRect(px + IN - 1, py + TILE - IN - L - 1, T + 2, L + 2, false);
    r.fillRect(px + TILE - IN - L - 1, py + TILE - IN - T - 1, L + 2, T + 2, false);
    r.fillRect(px + TILE - IN - T - 1, py + TILE - IN - L - 1, T + 2, L + 2, false);
  }
  r.fillRect(px + IN, py + IN, L, T, true);  // top-left
  r.fillRect(px + IN, py + IN, T, L, true);
  r.fillRect(px + TILE - IN - L, py + IN, L, T, true);  // top-right
  r.fillRect(px + TILE - IN - T, py + IN, T, L, true);
  r.fillRect(px + IN, py + TILE - IN - T, L, T, true);  // bottom-left
  r.fillRect(px + IN, py + TILE - IN - L, T, L, true);
  r.fillRect(px + TILE - IN - L, py + TILE - IN - T, L, T, true);  // bottom-right
  r.fillRect(px + TILE - IN - T, py + TILE - IN - L, T, L, true);
}

template <typename Renderer>
void drawUnitTile(const Renderer& r, const int px, const int py, const Unit& u) {
  const bool player = u.side == Side::Player;
  // 16x16 sprite drawn 2x (32x32) on a plate: white plate with black
  // pixels for player units, inverted for enemies. Sprites live in flash
  // (TacticsSprites.h) and are shared with the host simulator.
  const int ox = px + (TILE - 2 * SPRITE_SIZE) / 2;
  const int oy = py + 3;
  r.fillRect(ox - 2, oy - 2, 2 * SPRITE_SIZE + 4, 2 * SPRITE_SIZE + 4, !player);
  for (int sy = 0; sy < SPRITE_SIZE; sy++) {
    for (int sx = 0; sx < SPRITE_SIZE; sx++) {
      if (spritePixel(u.type, sx, sy)) {
        r.fillRect(ox + sx * 2, oy + sy * 2, 2, 2, player);
      }
    }
  }
  // HP bar
  const int barW = TILE - 12;
  const int fill = barW * u.hp / u.info().maxHp;
  r.drawRect(px + 6, py + TILE - 9, barW, 5, 1, true);
  r.fillRect(px + 6, py + TILE - 9, fill, 5, true);
  if (player && u.acted) {
    r.fillRect(px + TILE - 12, py + 2, 8, 8, true);  // done marker
  }
}

template <typename Renderer>
void drawCursor(const Renderer& r, const int px, const int py, const bool targetRing) {
  r.drawRect(px, py, TILE, TILE, 3, true);
  if (targetRing) {
    r.drawRect(px + 4, py + 4, TILE - 8, TILE - 8, 2, true);
  }
}

}  // namespace BoardArt
}  // namespace Tactics
