#include "TacticsActivity.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>
#include <TacticsSprites.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "components/UITheme.h"
#include "fontIds.h"

using Tactics::Game;
using Tactics::Level;
using Tactics::Side;
using Tactics::Terrain;
using Tactics::Unit;

namespace {

constexpr const char* LEVELS_DIR = "/tactics";
constexpr int TILE = 48;
constexpr int HUD_HEIGHT = 96;
constexpr size_t MAX_LEVEL_FILE = 16 * 1024;
constexpr unsigned long AI_STEP_MS = 650;
constexpr int MAX_LEVEL_FILES = 32;

// Display names for unit types, indexed like Tactics::UNIT_TYPES.
constexpr StrId UNIT_NAME_IDS[Tactics::UNIT_TYPE_COUNT] = {
    STR_UNIT_HUMAN_SOLDIER, STR_UNIT_HUMAN_ARCHER,  STR_UNIT_HUMAN_KNIGHT,  STR_UNIT_ELF_FIGHTER,
    STR_UNIT_ELF_ARCHER,    STR_UNIT_ELF_MAGE,      STR_UNIT_DWARF_WARRIOR, STR_UNIT_DWARF_THUNDERER,
    STR_UNIT_DWARF_GUARD,   STR_UNIT_DRAKE_CLASHER, STR_UNIT_DRAKE_FLARE,   STR_UNIT_DRAKE_FLYER,
    STR_UNIT_GOBLIN,        STR_UNIT_ORC,           STR_UNIT_TROLL,         STR_UNIT_WYVERN,
};

}  // namespace

// ------------------------------------------------------------ lifecycle

void TacticsActivity::onEnter() {
  Activity::onEnter();
  mode = Mode::LevelSelect;
  scanLevels();
  requestUpdate();
}

void TacticsActivity::onExit() {
  Activity::onExit();
  game.reset();
  level.reset();
  levelFiles.clear();
  levelFiles.shrink_to_fit();
}

// ------------------------------------------------------------ level loading

void TacticsActivity::scanLevels() {
  levelFiles.clear();
  levelFiles.reserve(8);
  if (!Storage.exists(LEVELS_DIR)) return;
  auto dir = Storage.open(LEVELS_DIR);
  if (!dir) return;
  char name[64];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (file.isDirectory()) continue;
    file.getName(name, sizeof(name));
    const size_t len = std::strlen(name);
    if (name[0] == '.') continue;
    if (len < 6 || std::strcmp(name + len - 5, ".yaml") != 0) continue;
    if (levelFiles.size() >= MAX_LEVEL_FILES) break;
    levelFiles.emplace_back(name);
  }
  std::sort(levelFiles.begin(), levelFiles.end());
  if (levelSelIndex >= static_cast<int>(levelFiles.size())) levelSelIndex = 0;
}

bool TacticsActivity::loadLevel(const std::string& fileName) {
  levelError[0] = '\0';
  const std::string path = std::string(LEVELS_DIR) + "/" + fileName;
  HalFile file;
  if (!Storage.openFileForRead("TACTICS", path, file)) {
    std::snprintf(levelError, sizeof(levelError), "%s", tr(STR_TACTICS_LEVEL_ERROR));
    return false;
  }
  const size_t size = file.fileSize();
  if (size == 0 || size > MAX_LEVEL_FILE) {
    LOG_ERR("TACTICS", "Level file size out of range: %u", (unsigned)size);
    std::snprintf(levelError, sizeof(levelError), "%s", tr(STR_TACTICS_LEVEL_ERROR));
    return false;
  }
  const auto buffer = makeUniqueNoThrow<char[]>(size);
  if (!buffer) {
    LOG_ERR("TACTICS", "OOM: %u bytes for level text", (unsigned)size);
    return false;
  }
  if (file.read(buffer.get(), size) != static_cast<int>(size)) {
    LOG_ERR("TACTICS", "Short read on %s", path.c_str());
    return false;
  }

  if (!level) level = makeUniqueNoThrow<Level>();
  if (!game) game = makeUniqueNoThrow<Game>();
  if (!level || !game) {
    LOG_ERR("TACTICS", "OOM: game state");
    return false;
  }
  char err[64];
  if (!Tactics::parseLevel(buffer.get(), size, *level, err, sizeof(err))) {
    LOG_ERR("TACTICS", "Parse failed for %s: %s", path.c_str(), err);
    std::snprintf(levelError, sizeof(levelError), "%s: %s", tr(STR_TACTICS_LEVEL_ERROR), err);
    return false;
  }
  game->start(*level);

  // Open on the first player unit.
  for (int i = 0; i < game->unitCount(); i++) {
    if (game->unit(i).side == Side::Player) {
      cursorX = game->unit(i).x;
      cursorY = game->unit(i).y;
      break;
    }
  }
  selectedUnit = -1;
  centerCamera(cursorX, cursorY);
  std::snprintf(statusLine, sizeof(statusLine), "%s", level->name);
  mode = Mode::Cursor;
  return true;
}

// ------------------------------------------------------------ camera

int TacticsActivity::viewCols() const { return renderer.getScreenWidth() / TILE; }
int TacticsActivity::viewRows() const { return (renderer.getScreenHeight() - HUD_HEIGHT) / TILE; }
int TacticsActivity::hudTop() const { return viewRows() * TILE; }

void TacticsActivity::clampCamera() {
  const int maxX = level->width - viewCols();
  const int maxY = level->height - viewRows();
  if (camX > maxX) camX = maxX;
  if (camY > maxY) camY = maxY;
  if (camX < 0) camX = 0;
  if (camY < 0) camY = 0;
}

void TacticsActivity::centerCamera(const int x, const int y) {
  camX = x - viewCols() / 2;
  camY = y - viewRows() / 2;
  clampCamera();
}

void TacticsActivity::ensureCursorVisible() {
  if (cursorX < camX + 1) camX = cursorX - 1;
  if (cursorX > camX + viewCols() - 2) camX = cursorX - viewCols() + 2;
  if (cursorY < camY + 1) camY = cursorY - 1;
  if (cursorY > camY + viewRows() - 2) camY = cursorY - viewRows() + 2;
  clampCamera();
}

// ------------------------------------------------------------ input

void TacticsActivity::loop() {
  switch (mode) {
    case Mode::LevelSelect:
      loopLevelSelect();
      break;
    case Mode::Cursor:
      loopCursor();
      break;
    case Mode::MoveSelect:
      loopMoveSelect();
      break;
    case Mode::TargetSelect:
      loopTargetSelect();
      break;
    case Mode::PauseMenu:
      loopPauseMenu();
      break;
    case Mode::EnemyTurn:
      loopEnemyTurn();
      break;
    case Mode::GameOver:
      loopGameOver();
      break;
  }
}

void TacticsActivity::loopLevelSelect() {
  const int count = static_cast<int>(levelFiles.size());
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome();
    return;
  }
  if (count == 0) return;
  if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
      mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    levelSelIndex = (levelSelIndex + count - 1) % count;
    requestUpdate();
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
      mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    levelSelIndex = (levelSelIndex + 1) % count;
    requestUpdate();
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (loadLevel(levelFiles[levelSelIndex])) {
      mode = Mode::Cursor;
    }
    requestUpdate();
  }
}

bool TacticsActivity::moveCursor() {
  int dx = 0, dy = 0;
  if (mappedInput.wasPressed(MappedInputManager::Button::Left)) dx = -1;
  if (mappedInput.wasPressed(MappedInputManager::Button::Right)) dx = 1;
  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) dy = -1;
  if (mappedInput.wasPressed(MappedInputManager::Button::Down)) dy = 1;
  if (dx == 0 && dy == 0) return false;
  const int nx = cursorX + dx;
  const int ny = cursorY + dy;
  if (nx < 0 || ny < 0 || nx >= level->width || ny >= level->height) return false;
  cursorX = nx;
  cursorY = ny;
  ensureCursorVisible();
  requestUpdate();
  return true;
}

void TacticsActivity::loopCursor() {
  if (moveCursor()) return;
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    pauseSel = 0;
    mode = Mode::PauseMenu;
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const int idx = game->unitIndexAt(cursorX, cursorY);
    if (idx >= 0 && game->unit(idx).side == Side::Player && !game->unit(idx).acted) {
      selectedUnit = idx;
      game->computeReach(idx);
      mode = Mode::MoveSelect;
      std::snprintf(statusLine, sizeof(statusLine), "%s", tr(UNIT_NAME_IDS[game->unit(idx).type]));
      requestUpdate();
    }
  }
}

void TacticsActivity::loopMoveSelect() {
  if (moveCursor()) return;
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    selectedUnit = -1;
    mode = Mode::Cursor;
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (game->isReachable(cursorX, cursorY)) {
      game->moveUnit(selectedUnit, cursorX, cursorY);
      if (game->status() != Game::Status::Playing) {
        mode = Mode::GameOver;  // stepped onto the enemy base
        requestUpdate();
        return;
      }
      beginTargetSelect();
      requestUpdate();
    }
  }
}

void TacticsActivity::beginTargetSelect() {
  const Unit& u = game->unit(selectedUnit);
  targetCount = game->targetsFrom(selectedUnit, u.x, u.y, targets, 8);
  if (targetCount == 0) {
    finishSelectedUnit();
    return;
  }
  targetSel = 0;
  mode = Mode::TargetSelect;
}

void TacticsActivity::finishSelectedUnit() {
  if (selectedUnit >= 0) game->finishUnit(selectedUnit);
  selectedUnit = -1;
  if (game->allPlayerUnitsActed()) {
    game->endPlayerTurn();
    lastAiStepMs = millis();
    std::snprintf(statusLine, sizeof(statusLine), "%s", tr(STR_TACTICS_ENEMY_TURN));
    mode = Mode::EnemyTurn;
  } else {
    mode = Mode::Cursor;
  }
  requestUpdate();
}

void TacticsActivity::loopTargetSelect() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Left) ||
      mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    targetSel = (targetSel + targetCount - 1) % targetCount;
    requestUpdate();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Right) ||
      mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    targetSel = (targetSel + 1) % targetCount;
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finishSelectedUnit();  // skip attacking
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    game->attack(selectedUnit, targets[targetSel]);
    selectedUnit = -1;
    if (game->status() != Game::Status::Playing) {
      mode = Mode::GameOver;
      requestUpdate();
      return;
    }
    if (game->allPlayerUnitsActed()) {
      game->endPlayerTurn();
      lastAiStepMs = millis();
      std::snprintf(statusLine, sizeof(statusLine), "%s", tr(STR_TACTICS_ENEMY_TURN));
      mode = Mode::EnemyTurn;
    } else {
      mode = Mode::Cursor;
    }
    requestUpdate();
  }
}

void TacticsActivity::loopPauseMenu() {
  constexpr int OPTION_COUNT = 4;  // resume, end turn, restart, quit
  if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
      mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    pauseSel = (pauseSel + OPTION_COUNT - 1) % OPTION_COUNT;
    requestUpdate();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
      mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    pauseSel = (pauseSel + 1) % OPTION_COUNT;
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    mode = Mode::Cursor;
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    switch (pauseSel) {
      case 0:  // resume
        mode = Mode::Cursor;
        break;
      case 1:  // end turn
        for (int i = 0; i < game->unitCount(); i++) {
          if (game->unit(i).alive && game->unit(i).side == Side::Player) game->finishUnit(i);
        }
        game->endPlayerTurn();
        lastAiStepMs = millis();
        std::snprintf(statusLine, sizeof(statusLine), "%s", tr(STR_TACTICS_ENEMY_TURN));
        mode = Mode::EnemyTurn;
        break;
      case 2:  // restart
        game->start(*level);
        selectedUnit = -1;
        mode = Mode::Cursor;
        break;
      case 3:  // quit to level list
        mode = Mode::LevelSelect;
        break;
      default:
        break;
    }
    requestUpdate();
  }
}

void TacticsActivity::loopEnemyTurn() {
  if (millis() - lastAiStepMs < AI_STEP_MS) return;
  lastAiStepMs = millis();

  // Run AI steps until one is player-visible (worth a repaint), or the
  // enemy phase ends. Hidden monster shuffling should not slow the turn.
  for (int i = 0; i < Tactics::MAX_UNITS + 1; i++) {
    const Game::AiAction act = game->aiStep();
    if (!act.acted) {
      if (game->status() != Game::Status::Playing) {
        mode = Mode::GameOver;
      } else {
        mode = Mode::Cursor;
        char turnBuf[48];
        std::snprintf(turnBuf, sizeof(turnBuf), tr(STR_TACTICS_TURN), game->turn());
        std::snprintf(statusLine, sizeof(statusLine), "%s", turnBuf);
        for (int u = 0; u < game->unitCount(); u++) {
          if (game->unit(u).alive && game->unit(u).side == Side::Player) {
            cursorX = game->unit(u).x;
            cursorY = game->unit(u).y;
            break;
          }
        }
        ensureCursorVisible();
      }
      requestUpdate();
      return;
    }
    if (act.visible) {
      centerCamera(act.toX, act.toY);
      if (act.target >= 0) {
        std::snprintf(statusLine, sizeof(statusLine), "%s -%d", tr(UNIT_NAME_IDS[game->unit(act.unit).type]),
                      act.damage);
      } else {
        std::snprintf(statusLine, sizeof(statusLine), "%s", tr(STR_TACTICS_ENEMY_TURN));
      }
      if (game->status() != Game::Status::Playing) mode = Mode::GameOver;
      requestUpdate();
      return;
    }
  }
}

void TacticsActivity::loopGameOver() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
      mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    mode = Mode::LevelSelect;
    requestUpdate();
  }
}

// ------------------------------------------------------------ rendering

void TacticsActivity::render(RenderLock&&) {
  renderer.clearScreen();
  if (mode == Mode::LevelSelect) {
    renderLevelSelect();
  } else {
    renderBoard();
    renderHud();
  }
  renderer.displayBuffer();
}

void TacticsActivity::renderLevelSelect() {
  const int pageWidth = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, 60}, tr(STR_TACTICS));

  const int top = metrics.topPadding + 70;
  const int lineH = renderer.getLineHeight(UI_12_FONT_ID) + 14;
  if (levelFiles.empty()) {
    renderer.drawText(UI_12_FONT_ID, 20, top, tr(STR_TACTICS_NO_LEVELS), true);
  } else {
    const int visible = (renderer.getScreenHeight() - top - 80) / lineH;
    int first = levelSelIndex - visible / 2;
    if (first > static_cast<int>(levelFiles.size()) - visible) first = static_cast<int>(levelFiles.size()) - visible;
    if (first < 0) first = 0;
    for (int i = 0; i < visible && first + i < static_cast<int>(levelFiles.size()); i++) {
      const int y = top + i * lineH;
      const bool sel = first + i == levelSelIndex;
      if (sel) renderer.fillRect(8, y - 4, pageWidth - 16, lineH - 4, true);
      renderer.drawText(UI_12_FONT_ID, 20, y, levelFiles[first + i].c_str(), !sel);
    }
  }
  if (levelError[0] != '\0') {
    renderer.drawText(SMALL_FONT_ID, 20, renderer.getScreenHeight() - 100, levelError, true);
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void TacticsActivity::drawTerrainTile(const int px, const int py, const Terrain t) const {
  switch (t) {
    case Terrain::Forest:
      for (int i = 0; i < 3; i++) {
        const int bx = px + 8 + i * 14;
        const int by = py + (i % 2 ? 14 : 26);
        renderer.drawLine(bx, by + 12, bx + 5, by, true);
        renderer.drawLine(bx + 5, by, bx + 10, by + 12, true);
        renderer.drawLine(bx, by + 12, bx + 10, by + 12, true);
        renderer.drawLine(bx + 5, by + 12, bx + 5, by + 16, true);
      }
      break;
    case Terrain::Mountain:
      renderer.drawLine(px + 4, py + 40, px + 22, py + 8, true);
      renderer.drawLine(px + 22, py + 8, px + 42, py + 40, true);
      renderer.drawLine(px + 4, py + 40, px + 42, py + 40, true);
      break;
    case Terrain::Water:
      for (int i = 0; i < 3; i++) {
        const int wy = py + 14 + i * 10;
        for (int s = 0; s < 4; s++) {
          renderer.drawLine(px + 4 + s * 10, wy + (s % 2 ? 2 : 0), px + 8 + s * 10, wy + (s % 2 ? 0 : 2), true);
        }
      }
      break;
    case Terrain::Rock:
      renderer.fillRect(px + 2, py + 2, TILE - 4, TILE - 4, true);
      break;
    case Terrain::Castle:
      renderer.drawRect(px + 8, py + 16, 32, 24, 2, true);
      for (int i = 0; i < 4; i++) renderer.fillRect(px + 8 + i * 9, py + 10, 5, 8, true);
      renderer.fillRect(px + 20, py + 28, 8, 12, true);
      break;
    case Terrain::EnemyBase:
      renderer.drawRect(px + 6, py + 14, 36, 26, 3, true);
      renderer.drawLine(px + 24, py + 14, px + 24, py + 2, true);
      renderer.drawLine(px + 24, py + 2, px + 38, py + 6, true);
      renderer.drawLine(px + 38, py + 6, px + 24, py + 10, true);
      break;
    default:
      break;
  }
}

void TacticsActivity::drawUnitTile(const int px, const int py, const Unit& u) const {
  const bool player = u.side == Side::Player;
  // 16x16 sprite drawn 2x (32x32) on a plate: white plate with black
  // pixels for player units, inverted for enemies. Sprites live in flash
  // (TacticsSprites.h) and are shared with the host simulator.
  const int ox = px + (TILE - 2 * Tactics::SPRITE_SIZE) / 2;
  const int oy = py + 3;
  renderer.fillRect(ox - 2, oy - 2, 2 * Tactics::SPRITE_SIZE + 4, 2 * Tactics::SPRITE_SIZE + 4, !player);
  for (int sy = 0; sy < Tactics::SPRITE_SIZE; sy++) {
    for (int sx = 0; sx < Tactics::SPRITE_SIZE; sx++) {
      if (Tactics::spritePixel(u.type, sx, sy)) {
        renderer.fillRect(ox + sx * 2, oy + sy * 2, 2, 2, player);
      }
    }
  }
  // HP bar
  const int barW = TILE - 12;
  const int fill = barW * u.hp / u.info().maxHp;
  renderer.drawRect(px + 6, py + TILE - 9, barW, 5, 1, true);
  renderer.fillRect(px + 6, py + TILE - 9, fill, 5, true);
  if (player && u.acted) {
    renderer.fillRect(px + TILE - 12, py + 2, 8, 8, true);  // done marker
  }
}

void TacticsActivity::renderBoard() {
  const int cols = viewCols();
  const int rows = viewRows();
  for (int vy = 0; vy < rows; vy++) {
    for (int vx = 0; vx < cols; vx++) {
      const int mx = camX + vx;
      const int my = camY + vy;
      const int px = vx * TILE;
      const int py = vy * TILE;
      if (mx >= level->width || my >= level->height) continue;
      const Terrain t = level->at(mx, my);
      if (!game->isExplored(mx, my)) {
        renderer.fillRectDither(px, py, TILE, TILE, Color::DarkGray);
        continue;
      }
      if (t == Terrain::Void) continue;
      drawTerrainTile(px, py, t);
      const bool vis = game->isVisible(mx, my);
      if (vis) {
        const int ui = game->unitIndexAt(mx, my);
        if (ui >= 0) drawUnitTile(px, py, game->unit(ui));
      } else {
        renderer.fillRectDither(px, py, TILE, TILE, Color::LightGray);
      }
      // Movement overlay for the selected unit.
      if (mode == Mode::MoveSelect && game->isReachable(mx, my)) {
        renderer.fillRect(px + 2, py + 2, 7, 7, true);
      }
    }
  }
  // Cursor / target highlight
  int cx = cursorX, cy = cursorY;
  if (mode == Mode::TargetSelect && targetCount > 0) {
    cx = game->unit(targets[targetSel]).x;
    cy = game->unit(targets[targetSel]).y;
  }
  if (mode == Mode::Cursor || mode == Mode::MoveSelect || mode == Mode::TargetSelect) {
    const int px = (cx - camX) * TILE;
    const int py = (cy - camY) * TILE;
    renderer.drawRect(px, py, TILE, TILE, 3, true);
    if (mode == Mode::TargetSelect) {
      renderer.drawRect(px + 4, py + 4, TILE - 8, TILE - 8, 2, true);
    }
  }
}

void TacticsActivity::renderHud() {
  const int top = hudTop();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  renderer.fillRect(0, top, pageWidth, 2, true);

  char line[96];
  char turnBuf[32];
  std::snprintf(turnBuf, sizeof(turnBuf), tr(STR_TACTICS_TURN), game->turn());

  if (mode == Mode::GameOver) {
    const bool won = game->status() == Game::Status::Victory;
    std::snprintf(line, sizeof(line), "%s", won ? tr(STR_TACTICS_VICTORY) : tr(STR_TACTICS_DEFEAT));
    renderer.drawText(UI_12_FONT_ID, 16, top + 12, line, true);
    renderer.drawText(SMALL_FONT_ID, 16, top + 52, turnBuf, true);
    return;
  }

  if (mode == Mode::PauseMenu) {
    const char* options[4] = {tr(STR_TACTICS_RESUME), tr(STR_TACTICS_END_TURN), tr(STR_TACTICS_RESTART),
                              tr(STR_TACTICS_QUIT)};
    const int w = pageWidth / 4;
    for (int i = 0; i < 4; i++) {
      const bool sel = i == pauseSel;
      if (sel) renderer.fillRect(i * w, top + 6, w, HUD_HEIGHT - 12, true);
      const int tw = renderer.getTextWidth(SMALL_FONT_ID, options[i]);
      renderer.drawText(SMALL_FONT_ID, i * w + (w - tw) / 2, top + HUD_HEIGHT / 2 - 8, options[i], !sel);
    }
    return;
  }

  // Left: turn + context line
  renderer.drawText(UI_12_FONT_ID, 16, top + 10, turnBuf, true);
  renderer.drawText(SMALL_FONT_ID, 16, top + 50, statusLine, true);

  // Right: unit under cursor (if visible), or attack forecast
  if (mode == Mode::TargetSelect && targetCount > 0) {
    const Game::Forecast f = game->forecast(selectedUnit, targets[targetSel]);
    std::snprintf(line, sizeof(line), "%s %d  >%d  <%d", tr(UNIT_NAME_IDS[game->unit(targets[targetSel]).type]),
                  game->unit(targets[targetSel]).hp, f.damageToTarget, f.counterDamage);
    const int tw = renderer.getTextWidth(SMALL_FONT_ID, line);
    renderer.drawText(SMALL_FONT_ID, pageWidth - tw - 16, top + 30, line, true);
  } else {
    const int ui = game->unitIndexAt(cursorX, cursorY);
    if (ui >= 0 && game->isVisible(cursorX, cursorY)) {
      const Unit& u = game->unit(ui);
      std::snprintf(line, sizeof(line), "%s %d/%d", tr(UNIT_NAME_IDS[u.type]), u.hp, u.info().maxHp);
      const int tw = renderer.getTextWidth(SMALL_FONT_ID, line);
      renderer.drawText(SMALL_FONT_ID, pageWidth - tw - 16, top + 30, line, true);
    }
  }

  // Bottom scanline: minimap-style position indicator for big maps.
  if (level->width > viewCols() || level->height > viewRows()) {
    const int mmW = 60;
    const int mmH = 40;
    const int mmX = pageWidth - mmW - 10;
    const int mmY = pageHeight - mmH - 6;
    renderer.drawRect(mmX, mmY, mmW, mmH, 1, true);
    const int vx = mmX + camX * mmW / level->width;
    const int vy = mmY + camY * mmH / level->height;
    const int vw = viewCols() * mmW / level->width;
    const int vh = viewRows() * mmH / level->height;
    renderer.drawRect(vx, vy, vw > 2 ? vw : 2, vh > 2 ? vh : 2, 1, true);
  }
}
