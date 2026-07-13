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
#include "TacticsBoardArt.h"
#include "TacticsGame.h"
#include "TacticsLevel.h"
#include "TacticsSprites.h"

using namespace Tactics;

namespace {

// Mocked device geometry (portrait X4). TILE and HUD_HEIGHT mirror
// TacticsActivity.cpp so the tile grid and HUD strip land on the same rows.
constexpr int SCREEN_W = 480;
constexpr int SCREEN_H = 800;
constexpr int TILE = BoardArt::TILE;
constexpr int HUD_HEIGHT = 96;
constexpr int VIEW_COLS = SCREEN_W / TILE;                 // 10
constexpr int VIEW_ROWS = (SCREEN_H - HUD_HEIGHT) / TILE;  // 14
constexpr int HUD_Y = VIEW_ROWS * TILE;                    // 672

// Display names matching lib/I18n/translations/english.yaml, indexed like
// Tactics::UNIT_TYPES / TERRAIN_INFO (the sim has no I18N).
constexpr const char* UNIT_NAMES[UNIT_TYPE_COUNT] = {
    "Soldier",       "Archer",    "Knight",      "Elf Fighter",   "Elf Archer",  "Elf Mage",
    "Dwarf Warrior", "Thunderer", "Dwarf Guard", "Drake Clasher", "Drake Flare", "Drake Flyer",
    "Goblin",        "Orc",       "Troll",       "Wyvern",        "Slinger",
};
constexpr const char* TERRAIN_NAMES[TERRAIN_COUNT] = {
    "Void", "Plain", "Forest", "Mountain", "Water", "Rock", "Castle", "Enemy Base",
};

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

// GfxRenderer-shaped adapter over SimCanvas so the shared board art in
// TacticsBoardArt.h — the code the device compiles — draws on the host
// canvas unchanged.
struct GfxAdapter {
  SimCanvas& c;
  void drawLine(const int x1, const int y1, const int x2, const int y2, const bool black) const {
    c.drawLine(x1, y1, x2, y2, black ? 0 : 255);
  }
  void fillRect(const int x, const int y, const int w, const int h, const bool black) const {
    c.fillRect(x, y, w, h, black ? 0 : 255);
  }
  void drawRect(const int x, const int y, const int w, const int h, const int lineWidth, const bool black) const {
    c.drawRect(x, y, w, h, lineWidth, black ? 0 : 255);
  }
};

// Fog fills mirroring GfxRenderer::fillRectImpl<DarkGray/LightGray>: the
// device dither writes BOTH inks, so fog fully replaces whatever was drawn
// underneath. DarkGray = 50% checkerboard, LightGray = 25% stipple.
void fillRectDitherDark(SimCanvas& c, const int x, const int y, const int w, const int h) {
  for (int yy = y; yy < y + h; yy++) {
    for (int xx = x; xx < x + w; xx++) c.set(xx, yy, ((xx + yy) & 1) == 0 ? 0 : 255);
  }
}

void fillRectDitherLight(SimCanvas& c, const int x, const int y, const int w, const int h) {
  for (int yy = y; yy < y + h; yy++) {
    for (int xx = x; xx < x + w; xx++) c.set(xx, yy, (xx & 1) == 0 && (yy & 1) == 0 ? 0 : 255);
  }
}

// Which TacticsActivity::Mode a frame corresponds to; picks the cursor
// style and HUD contents exactly like the device does.
enum class FrameMode { Cursor, MoveSelect, Target, Enemy, GameOver };

// Mirrors TacticsActivity::renderHud() with the sim's block font standing in
// for the device fonts (same layout, sizes approximated).
void renderHud(const Game& game, const Camera& cam, SimCanvas& c, const char* status, const FrameMode mode,
               const int cursorX, const int cursorY, const int attacker, const int target) {
  const Level& level = game.level();
  const int top = HUD_Y;
  c.fillRect(0, top, SCREEN_W, 2, 0);
  char line[96];
  char turnBuf[32];
  std::snprintf(turnBuf, sizeof(turnBuf), "TURN %d", game.turn());

  if (mode == FrameMode::GameOver) {
    const bool won = game.status() == Game::Status::Victory;
    c.drawText(16, top + 12, won ? "VICTORY!" : "DEFEAT", 3, 0);
    c.drawText(16, top + 52, turnBuf, 2, 0);
    if (won) {
      const int tw = SimCanvas::textWidth("CONTINUE", 2);
      c.drawText(SCREEN_W - tw - 16, top + 30, "CONTINUE", 2, 0);
    }
    return;  // like the device: no minimap on the game-over strip
  }

  // Left: turn + context line.
  c.drawText(16, top + 10, turnBuf, 3, 0);
  c.drawText(16, top + 50, status, 2, 0);

  // Right: attack forecast during target selection, else unit + terrain
  // under the cursor.
  if (mode == FrameMode::Target && attacker >= 0 && target >= 0) {
    const Game::Forecast f = game.forecast(attacker, target);
    std::snprintf(line, sizeof(line), "%s %d  >%d  <%d%s", UNIT_NAMES[game.unit(target).type], game.unit(target).hp,
                  f.damageToTarget, f.counterDamage, f.flank ? "  FLANK +50%" : "");
    c.drawText(SCREEN_W - SimCanvas::textWidth(line, 1) - 16, top + 14, line, 1, 0);
  } else {
    const int ui = game.unitIndexAt(cursorX, cursorY);
    if (ui >= 0 && game.isVisible(cursorX, cursorY)) {
      const Unit& u = game.unit(ui);
      char stats[48];
      std::snprintf(stats, sizeof(stats), "A%d R%d D%d%% M%d", u.info().attack, u.info().range,
                    game.defenseAt(u, u.x, u.y), u.info().move);
      std::snprintf(line, sizeof(line), "%s %d/%d  %s", UNIT_NAMES[u.type], u.hp, u.info().maxHp, stats);
      c.drawText(SCREEN_W - SimCanvas::textWidth(line, 1) - 16, top + 14, line, 1, 0);
    }
    if (game.isExplored(cursorX, cursorY)) {
      const Terrain t = level.at(cursorX, cursorY);
      const auto& ti = TERRAIN_INFO[static_cast<int>(t)];
      char defBuf[20] = {0};
      char movBuf[20] = {0};
      char healBuf[20] = {0};
      if (ti.defense > 0) std::snprintf(defBuf, sizeof(defBuf), "DEF+%d%%", (int)ti.defense);
      if (ti.moveCost > 0) std::snprintf(movBuf, sizeof(movBuf), "MOV %d", (int)ti.moveCost);
      if (ti.heal > 0) std::snprintf(healBuf, sizeof(healBuf), "HEALS %d", (int)ti.heal);
      const char* racial = "";
      if (ui >= 0 && game.isVisible(cursorX, cursorY)) {
        const Unit& u = game.unit(ui);
        if (!u.info().flies && ((t == Terrain::Forest && u.info().race == Race::Elf) ||
                                (t == Terrain::Mountain && u.info().race == Race::Dwarf))) {
          racial = "+15 RACIAL";
        }
      }
      std::snprintf(line, sizeof(line), "%s  %s %s %s %s", TERRAIN_NAMES[static_cast<int>(t)], defBuf, movBuf, healBuf,
                    racial);
      c.drawText(SCREEN_W - SimCanvas::textWidth(line, 1) - 16, top + 54, line, 1, 0);
    }
  }

  // Bottom scanline minimap, only when the map is bigger than the view.
  if (level.width > VIEW_COLS || level.height > VIEW_ROWS) {
    constexpr int mmW = 60;
    constexpr int mmH = 40;
    const int mmX = SCREEN_W - mmW - 10;
    const int mmY = SCREEN_H - mmH - 6;
    c.drawRect(mmX, mmY, mmW, mmH, 1, 0);
    const int vx = mmX + cam.x * mmW / level.width;
    const int vy = mmY + cam.y * mmH / level.height;
    const int vw = VIEW_COLS * mmW / level.width;
    const int vh = VIEW_ROWS * mmH / level.height;
    c.drawRect(vx, vy, vw > 2 ? vw : 2, vh > 2 ? vh : 2, 1, 0);
    // Landmarks: explored castles and the enemy base as 3x3 dots.
    for (int y = 0; y < level.height; y++) {
      for (int x = 0; x < level.width; x++) {
        const Terrain t = level.at(x, y);
        if (t != Terrain::Castle && t != Terrain::EnemyBase) continue;
        if (!game.isExplored(x, y)) continue;
        c.fillRect(mmX + x * mmW / level.width - 1, mmY + y * mmH / level.height - 1, 3, 3, 0);
      }
    }
  }
}

// Mirrors TacticsActivity::renderBoard(); the per-tile art is the shared
// device code in TacticsBoardArt.h. For MoveSelect frames the caller must
// have run computeDangerZones() then computeReach(unit) — the order the
// device uses on every repaint.
void renderFrame(const Game& game, const Camera& cam, SimCanvas& c, const char* status, const FrameMode mode,
                 const int cursorX, const int cursorY, const int attacker = -1, const int target = -1) {
  const Level& level = game.level();
  const GfxAdapter r{c};
  c.clear();
  for (int vy = 0; vy < VIEW_ROWS; vy++) {
    for (int vx = 0; vx < VIEW_COLS; vx++) {
      const int mx = cam.x + vx;
      const int my = cam.y + vy;
      const int px = vx * TILE;
      const int py = vy * TILE;
      if (mx >= level.width || my >= level.height) continue;
      const Terrain t = level.at(mx, my);
      const bool reachHere = mode == FrameMode::MoveSelect && game.isReachable(mx, my);
      if (!game.isExplored(mx, my)) {
        fillRectDitherDark(c, px, py, TILE, TILE);
        // Units can walk into the unknown; mark it (haloed for contrast).
        if (reachHere) BoardArt::drawReachBorder(r, px, py, true);
        continue;
      }
      if (t == Terrain::Void) continue;
      BoardArt::drawTerrainTile(r, px, py, t);
      const bool vis = game.isVisible(mx, my);
      // Danger tiles get hazard hatching under the unit plate while a
      // destination is being chosen (device: MoveSelect mode).
      const bool danger = mode == FrameMode::MoveSelect && vis && game.isDangerous(mx, my);
      if (danger) BoardArt::drawDangerShade(r, px, py);
      if (vis) {
        const int ui = game.unitIndexAt(mx, my);
        if (ui >= 0) BoardArt::drawUnitTile(r, px, py, game.unit(ui));
      } else {
        fillRectDitherLight(c, px, py, TILE, TILE);
      }
      if (reachHere) BoardArt::drawReachBorder(r, px, py, !vis);
    }
  }
  // Cursor / target highlight, in the modes the device shows one.
  if (mode == FrameMode::Cursor || mode == FrameMode::MoveSelect || mode == FrameMode::Target) {
    const int px = (cursorX - cam.x) * TILE;
    const int py = (cursorY - cam.y) * TILE;
    if (px >= 0 && py >= 0 && px < SCREEN_W && py < HUD_Y) {
      BoardArt::drawCursor(r, px, py, mode == FrameMode::Target);
    }
  }
  renderHud(game, cam, c, status, mode, cursorX, cursorY, attacker, target);
}

class FrameDumper {
 public:
  explicit FrameDumper(std::string dir) : dir_(std::move(dir)) {}

  void dump(const Game& game, const Camera& cam, const char* status, const FrameMode mode, const int cursorX,
            const int cursorY, const int attacker = -1, const int target = -1) {
    if (dir_.empty()) return;
    SimCanvas canvas(SCREEN_W, SCREEN_H);
    renderFrame(game, cam, canvas, status, mode, cursorX, cursorY, attacker, target);
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
  // Open with the cursor on the first player unit, like the device does.
  int cursorX = 0, cursorY = 0;
  for (int i = 0; i < game.unitCount(); i++) {
    if (game.unit(i).side == Side::Player) {
      cursorX = game.unit(i).x;
      cursorY = game.unit(i).y;
      cam.center(level, cursorX, cursorY);
      break;
    }
  }
  printAscii(game);
  frames.dump(game, cam, level.name, FrameMode::Cursor, cursorX, cursorY);

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
      // Device order on every MoveSelect repaint: danger zones first, then
      // reach (the danger computation trashes the reach cache).
      game.computeDangerZones();
      game.computeReach(u);
      cursorX = game.unit(u).x;
      cursorY = game.unit(u).y;
      cam.center(level, cursorX, cursorY);
      std::printf("reach for [%d]:\n", u);
      printAscii(game);
      frames.dump(game, cam, UNIT_NAMES[game.unit(u).type], FrameMode::MoveSelect, cursorX, cursorY);
      continue;
    }
    if (op == "m") {
      int u, x, y;
      cmd >> u >> x >> y;
      const int hpBefore = game.unit(u).hp;
      if (game.moveUnit(u, x, y)) {
        cursorX = x;
        cursorY = y;
        cam.center(level, x, y);
        std::printf("moved [%d] to (%d,%d)\n", u, x, y);
        char st[64];
        if (game.unit(u).hp != hpBefore) {  // parting blow on the way out
          std::snprintf(st, sizeof(st), "%s -%d", UNIT_NAMES[game.unit(u).type], hpBefore - game.unit(u).hp);
        } else {
          std::snprintf(st, sizeof(st), "%s", UNIT_NAMES[game.unit(u).type]);
        }
        frames.dump(game, cam, st, FrameMode::Cursor, x, y);
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
      // Validate before dumping so an illegal attack leaves no stray frame.
      int8_t tg[8];
      const int n = game.targetsFrom(u, game.unit(u).x, game.unit(u).y, tg, 8);
      bool legal = false;
      for (int i = 0; i < n; i++) legal = legal || tg[i] == t;
      if (legal) {
        // Frame 1: the device's target-select look — cursor ring on the
        // victim, forecast line on the HUD — before the blow lands.
        cam.center(level, game.unit(t).x, game.unit(t).y);
        frames.dump(game, cam, UNIT_NAMES[game.unit(u).type], FrameMode::Target, game.unit(t).x, game.unit(t).y, u, t);
      }
      if (legal && game.attack(u, t)) {
        std::printf("[%d] hits (%d) for %d%s; counter %d%s\n", u, t, f.damageToTarget, f.targetDies ? " KILL" : "",
                    f.counterDamage, f.attackerDies ? " DIED" : "");
        // Frame 2: the board after the exchange (the device repaints in
        // Cursor mode once the attack resolves).
        const Unit& atk = game.unit(u);
        cursorX = atk.alive ? atk.x : game.unit(t).x;
        cursorY = atk.alive ? atk.y : game.unit(t).y;
        cam.center(level, cursorX, cursorY);
        frames.dump(game, cam, UNIT_NAMES[atk.type], FrameMode::Cursor, cursorX, cursorY);
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
          char st[64];
          std::snprintf(st, sizeof(st), "%s -%d", UNIT_NAMES[u.type], act.damage);
          frames.dump(game, cam, st, FrameMode::Enemy, act.toX, act.toY);
        } else if (act.fromX != act.toX || act.fromY != act.toY) {
          std::printf("enemy %s moves to (%d,%d)\n", u.info().id, act.toX, act.toY);
          cam.center(level, act.toX, act.toY);
          frames.dump(game, cam, "ENEMY TURN...", FrameMode::Enemy, act.toX, act.toY);
        }
      }
      std::printf("--- turn %d, your move ---\n", game.turn());
      // End-of-phase beat, like the device: cursor jumps to a ready unit
      // and the status line shows the new turn number.
      if (game.status() == Game::Status::Playing) {
        for (int i = 0; i < game.unitCount(); i++) {
          const Unit& pu = game.unit(i);
          if (pu.alive && pu.side == Side::Player && !pu.acted) {
            cursorX = pu.x;
            cursorY = pu.y;
            cam.center(level, cursorX, cursorY);
            break;
          }
        }
        char st[32];
        std::snprintf(st, sizeof(st), "TURN %d", game.turn());
        frames.dump(game, cam, st, FrameMode::Cursor, cursorX, cursorY);
      }
      printAscii(game);
      continue;
    }
    std::printf("unknown command: %s\n", op.c_str());
  }

  std::printf("=== %s (turn %d) ===\n", statusName(game.status()), game.turn());
  if (game.status() != Game::Status::Playing) {
    for (int i = 0; i < game.unitCount(); i++) {
      if (game.unit(i).side == Side::Player && game.unit(i).alive) {
        cam.center(level, game.unit(i).x, game.unit(i).y);
        break;
      }
    }
    frames.dump(game, cam, statusName(game.status()), FrameMode::GameOver, cursorX, cursorY);
  }
  return game.status() == Game::Status::Victory ? 0 : 1;
}
