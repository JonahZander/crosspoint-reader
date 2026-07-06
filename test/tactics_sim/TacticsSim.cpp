// Host-side simulator for the tactics mini-game.
//
// Runs the exact engine the firmware uses (lib/Tactics) with a mock of the
// device's portrait 480x800 e-ink layout, so gameplay and visuals can be
// exercised without hardware. Commands come from stdin (or piped), state
// prints as ASCII, and every state change can be dumped as a BMP frame.
//
//   TacticsSim <level.yaml> [--frames <dir>]
//
// Commands:
//   p              print board + unit roster
//   s <u>          show movement range of player unit u
//   m <u> <x> <y>  move unit u
//   a <u> <t>      attack enemy t with unit u (also prints the forecast)
//   f <u>          finish unit u without attacking
//   e              end player turn (runs the whole enemy phase)
//   q              quit
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "SimCanvas.h"
#include "TacticsGame.h"
#include "TacticsLevel.h"
#include "TacticsSprites.h"

using namespace Tactics;

namespace {

// Mocked device geometry (portrait X4).
constexpr int SCREEN_W = 480;
constexpr int SCREEN_H = 800;
constexpr int TILE = 48;
constexpr int VIEW_COLS = SCREEN_W / TILE;          // 10
constexpr int VIEW_ROWS = (SCREEN_H - 128) / TILE;  // 14
constexpr int HUD_Y = VIEW_ROWS * TILE;             // 672

struct Camera {
  int x = 0;
  int y = 0;

  void clampTo(const Level& level) {
    const int maxX = level.width - VIEW_COLS;
    const int maxY = level.height - VIEW_ROWS;
    if (x > maxX) x = maxX;
    if (y > maxY) y = maxY;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
  }

  void center(const Level& level, const int tx, const int ty) {
    x = tx - VIEW_COLS / 2;
    y = ty - VIEW_ROWS / 2;
    clampTo(level);
  }
};

void drawTerrain(SimCanvas& c, const int px, const int py, const Terrain t) {
  switch (t) {
    case Terrain::Forest:
      for (int i = 0; i < 3; i++) {
        const int bx = px + 8 + i * 14;
        const int by = py + (i % 2 ? 14 : 26);
        c.drawLine(bx, by + 12, bx + 5, by, 0);
        c.drawLine(bx + 5, by, bx + 10, by + 12, 0);
        c.drawLine(bx, by + 12, bx + 10, by + 12, 0);
        c.drawLine(bx + 5, by + 12, bx + 5, by + 16, 0);
      }
      break;
    case Terrain::Mountain:
      c.drawLine(px + 4, py + 40, px + 22, py + 8, 0);
      c.drawLine(px + 22, py + 8, px + 42, py + 40, 0);
      c.drawLine(px + 4, py + 40, px + 42, py + 40, 0);
      c.drawLine(px + 16, py + 24, px + 22, py + 30, 0);
      c.drawLine(px + 22, py + 30, px + 28, py + 20, 0);
      break;
    case Terrain::Water:
      for (int i = 0; i < 3; i++) {
        const int wy = py + 14 + i * 10;
        for (int s = 0; s < 4; s++) {
          c.drawLine(px + 4 + s * 10, wy + (s % 2 ? 2 : 0), px + 14 + s * 10 - 6, wy + (s % 2 ? 0 : 2), 0);
        }
      }
      break;
    case Terrain::Rock:
      c.fillRect(px + 2, py + 2, TILE - 4, TILE - 4, 0);
      break;
    case Terrain::Castle:
      c.drawRect(px + 8, py + 16, 32, 24, 2, 0);
      for (int i = 0; i < 4; i++) c.fillRect(px + 8 + i * 9, py + 10, 5, 8, 0);
      c.fillRect(px + 20, py + 28, 8, 12, 0);
      break;
    case Terrain::EnemyBase:
      c.drawRect(px + 6, py + 14, 36, 26, 3, 0);
      c.drawLine(px + 24, py + 14, px + 24, py + 2, 0);
      c.drawLine(px + 24, py + 2, px + 38, py + 6, 0);
      c.drawLine(px + 38, py + 6, px + 24, py + 10, 0);
      for (int i = 0; i < 3; i++) c.fillRect(px + 10 + i * 11, py + 8, 6, 7, 0);
      break;
    default:
      break;
  }
}

void drawUnit(SimCanvas& c, const int px, const int py, const Unit& u) {
  const bool player = u.side == Side::Player;
  // 16x16 sprite drawn 2x (32x32) on a plate: white plate + black pixels
  // for the player, inverted for enemies.
  const int ox = px + (TILE - 2 * SPRITE_SIZE) / 2;
  const int oy = py + 3;
  c.fillRect(ox - 2, oy - 2, 2 * SPRITE_SIZE + 4, 2 * SPRITE_SIZE + 4, player ? 255 : 0);
  for (int sy = 0; sy < SPRITE_SIZE; sy++) {
    for (int sx = 0; sx < SPRITE_SIZE; sx++) {
      if (spritePixel(u.type, sx, sy)) {
        c.fillRect(ox + sx * 2, oy + sy * 2, 2, 2, player ? 0 : 255);
      }
    }
  }
  // HP bar
  const int barW = 36;
  const int fill = barW * u.hp / u.info().maxHp;
  c.drawRect(px + 6, py + 40, barW, 6, 1, 0);
  c.fillRect(px + 6, py + 40, fill, 6, 0);
  if (u.acted && player) {
    c.drawText(px + 36, py + 2, ">", 1, 0);  // done marker
  }
}

void renderFrame(const Game& game, const Camera& cam, SimCanvas& c, const char* status, const bool showReach = false) {
  const Level& level = game.level();
  c.clear();
  for (int vy = 0; vy < VIEW_ROWS; vy++) {
    for (int vx = 0; vx < VIEW_COLS; vx++) {
      const int mx = cam.x + vx;
      const int my = cam.y + vy;
      const int px = vx * TILE;
      const int py = vy * TILE;
      if (mx >= level.width || my >= level.height) continue;
      const Terrain t = level.at(mx, my);
      if (!game.isExplored(mx, my)) {
        c.ditherRect(px, py, TILE, TILE, 0, 2);  // heavy fog
        continue;
      }
      if (t == Terrain::Void) continue;
      drawTerrain(c, px, py, t);
      const bool vis = game.isVisible(mx, my);
      if (vis) {
        const int ui = game.unitIndexAt(mx, my);
        if (ui >= 0) drawUnit(c, px, py, game.unit(ui));
      } else {
        c.ditherRect(px, py, TILE, TILE, 0, 4);  // thin "remembered" fog
      }
      if (showReach && game.isReachable(mx, my)) {
        c.fillRect(px + 2, py + 2, 6, 6, 0);
      }
      c.drawRect(px, py, TILE + 1, TILE + 1, 1, 190);
    }
  }
  // HUD
  c.fillRect(0, HUD_Y, SCREEN_W, 2, 0);
  char line[64];
  std::snprintf(line, sizeof(line), "TURN %d", game.turn());
  c.drawText(12, HUD_Y + 14, line, 3, 0);
  c.drawText(12, HUD_Y + 46, status, 2, 0);
  // Minimap: 2px per tile, explored area white, viewport outlined.
  const int mmX = SCREEN_W - level.width * 2 - 12;
  const int mmY = HUD_Y + 14;
  c.drawRect(mmX - 2, mmY - 2, level.width * 2 + 4, level.height * 2 + 4, 1, 0);
  for (int y = 0; y < level.height; y++) {
    for (int x = 0; x < level.width; x++) {
      if (!game.isExplored(x, y)) {
        c.fillRect(mmX + x * 2, mmY + y * 2, 2, 2, 120);
      }
    }
  }
  for (int i = 0; i < game.unitCount(); i++) {
    const Unit& u = game.unit(i);
    if (!u.alive) continue;
    if (u.side == Side::Player) {
      c.fillRect(mmX + u.x * 2 - 1, mmY + u.y * 2 - 1, 4, 4, 0);
    } else if (game.isVisible(u.x, u.y)) {
      c.drawRect(mmX + u.x * 2 - 1, mmY + u.y * 2 - 1, 4, 4, 1, 0);
    }
  }
  c.drawRect(mmX + cam.x * 2, mmY + cam.y * 2, VIEW_COLS * 2, VIEW_ROWS * 2, 1, 0);
}

class FrameDumper {
 public:
  explicit FrameDumper(std::string dir) : dir_(std::move(dir)) {}

  void dump(const Game& game, Camera cam, const char* status, const bool showReach = false) {
    if (dir_.empty()) return;
    SimCanvas canvas(SCREEN_W, SCREEN_H);
    renderFrame(game, cam, canvas, status, showReach);
    char path[512];
    std::snprintf(path, sizeof(path), "%s/frame_%03d.bmp", dir_.c_str(), counter_++);
    canvas.writeBmp(path);
    std::printf("[frame] %s\n", path);
  }

 private:
  std::string dir_;
  int counter_ = 0;
};

void printAscii(const Game& game) {
  const Level& level = game.level();
  std::printf("     ");
  for (int x = 0; x < level.width; x++) std::printf("%d", x % 10);
  std::printf("\n");
  for (int y = 0; y < level.height; y++) {
    std::printf("%4d ", y);
    for (int x = 0; x < level.width; x++) {
      char ch;
      if (!game.isExplored(x, y)) {
        ch = '?';
      } else {
        const Terrain t = level.at(x, y);
        ch = TERRAIN_INFO[static_cast<int>(t)].mapChar;
        if (game.isVisible(x, y)) {
          const int ui = game.unitIndexAt(x, y);
          if (ui >= 0) {
            const Unit& u = game.unit(ui);
            if (u.side == Side::Player) {
              ch = static_cast<char>('0' + ui % 10);
            } else {
              ch = u.info().glyph[0];  // monsters use lowercase glyphs
            }
          }
        }
      }
      std::printf("%c", ch);
    }
    std::printf("\n");
  }
  std::printf("units:\n");
  for (int i = 0; i < game.unitCount(); i++) {
    const Unit& u = game.unit(i);
    if (!u.alive) continue;
    if (u.side == Side::Player) {
      std::printf("  [%d] %-16s hp %2d/%2d at (%d,%d)%s\n", i, u.info().id, u.hp, u.info().maxHp, u.x, u.y,
                  u.acted ? " done" : "");
    } else if (game.isVisible(u.x, u.y)) {
      std::printf("  (%d) %-16s hp %2d/%2d at (%d,%d)  ENEMY\n", i, u.info().id, u.hp, u.info().maxHp, u.x, u.y);
    }
  }
}

const char* statusName(const Game::Status s) {
  switch (s) {
    case Game::Status::Victory:
      return "VICTORY!";
    case Game::Status::Defeat:
      return "DEFEAT";
    default:
      return "playing";
  }
}

}  // namespace

int main(const int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <level.yaml> [--frames <dir>]\n", argv[0]);
    return 2;
  }
  std::string framesDir;
  for (int i = 2; i < argc - 1; i++) {
    if (std::strcmp(argv[i], "--frames") == 0) framesDir = argv[i + 1];
  }

  std::ifstream in(argv[1]);
  if (!in) {
    std::fprintf(stderr, "cannot open %s\n", argv[1]);
    return 2;
  }
  std::stringstream ss;
  ss << in.rdbuf();
  const std::string text = ss.str();

  Level level;
  char err[96];
  if (!parseLevel(text.c_str(), text.size(), level, err, sizeof(err))) {
    std::fprintf(stderr, "level parse error: %s\n", err);
    return 1;
  }

  Game game;
  game.start(level);
  Camera cam;
  FrameDumper frames(framesDir);
  std::printf("loaded '%s'  %dx%d  win=%d  units=%d\n", level.name, level.width, level.height,
              static_cast<int>(level.win), level.unitCount);
  // Center the opening camera on the first player unit.
  for (int i = 0; i < game.unitCount(); i++) {
    if (game.unit(i).side == Side::Player) {
      cam.center(level, game.unit(i).x, game.unit(i).y);
      break;
    }
  }
  printAscii(game);
  frames.dump(game, cam, "DEPLOY");

  std::string cmdLine;
  while (game.status() == Game::Status::Playing && std::getline(std::cin, cmdLine)) {
    std::istringstream cmd(cmdLine);
    std::string op;
    cmd >> op;
    if (op.empty() || op[0] == '#') continue;
    if (op == "q") break;
    if (op == "p") {
      printAscii(game);
      continue;
    }
    if (op == "s") {
      int u;
      cmd >> u;
      game.computeReach(u);
      cam.center(level, game.unit(u).x, game.unit(u).y);
      std::printf("reach for [%d]:\n", u);
      printAscii(game);
      frames.dump(game, cam, "RANGE", true);
      continue;
    }
    if (op == "m") {
      int u, x, y;
      cmd >> u >> x >> y;
      if (game.moveUnit(u, x, y)) {
        cam.center(level, x, y);
        std::printf("moved [%d] to (%d,%d)\n", u, x, y);
        char st[64];
        std::snprintf(st, sizeof(st), "%s MOVES", game.unit(u).info().glyph);
        frames.dump(game, cam, st);
        // Report anything newly in attack range.
        int8_t targets[8];
        const int n = game.targetsFrom(u, x, y, targets, 8);
        for (int i = 0; i < n; i++) {
          const Game::Forecast f = game.forecast(u, targets[i]);
          std::printf("  can attack (%d) %s: %d dmg, counter %d\n", targets[i], game.unit(targets[i]).info().id,
                      f.damageToTarget, f.counterDamage);
        }
      } else {
        std::printf("ILLEGAL move [%d] to (%d,%d)\n", u, x, y);
      }
      continue;
    }
    if (op == "a") {
      int u, t;
      cmd >> u >> t;
      const Game::Forecast f = game.forecast(u, t);
      if (game.attack(u, t)) {
        std::printf("[%d] hits (%d) for %d%s; counter %d%s\n", u, t, f.damageToTarget, f.targetDies ? " KILL" : "",
                    f.counterDamage, f.attackerDies ? " DIED" : "");
        cam.center(level, game.unit(t).x, game.unit(t).y);
        frames.dump(game, cam, f.targetDies ? "ENEMY SLAIN" : "ATTACK!");
      } else {
        std::printf("ILLEGAL attack %d -> %d\n", u, t);
      }
      continue;
    }
    if (op == "f") {
      int u;
      cmd >> u;
      game.finishUnit(u);
      continue;
    }
    if (op == "e") {
      game.endPlayerTurn();
      Game::AiAction act;
      while ((act = game.aiStep()).acted) {
        if (!act.visible) continue;  // hidden moves stay hidden, like on device
        const Unit& u = game.unit(act.unit);
        if (act.target >= 0) {
          std::printf("enemy %s attacks %s: %d dmg%s, counter %d%s\n", u.info().id, game.unit(act.target).info().id,
                      act.damage, act.targetDied ? " KILL" : "", act.counter, act.attackerDied ? " (enemy died)" : "");
          cam.center(level, act.toX, act.toY);
          frames.dump(game, cam, "ENEMY ATTACK");
        } else if (act.fromX != act.toX || act.fromY != act.toY) {
          std::printf("enemy %s moves to (%d,%d)\n", u.info().id, act.toX, act.toY);
          cam.center(level, act.toX, act.toY);
          frames.dump(game, cam, "ENEMY MOVES");
        }
      }
      std::printf("--- turn %d, your move ---\n", game.turn());
      printAscii(game);
      continue;
    }
    std::printf("unknown command: %s\n", op.c_str());
  }

  std::printf("=== %s (turn %d) ===\n", statusName(game.status()), game.turn());
  if (game.status() != Game::Status::Playing) {
    for (int i = 0; i < game.unitCount(); i++) {
      if (game.unit(i).side == Side::Player) {
        cam.center(level, game.unit(i).x, game.unit(i).y);
        break;
      }
    }
    frames.dump(game, cam, statusName(game.status()));
  }
  return game.status() == Game::Status::Victory ? 0 : 1;
}
