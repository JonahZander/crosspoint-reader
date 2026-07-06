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
constexpr const char* SAVES_DIR = "/.crosspoint/tactics";
constexpr uint32_t SAVE_MAGIC = 0x54534156;  // "TSAV"
constexpr uint8_t SAVE_VERSION = 1;
constexpr int TILE = 48;
constexpr int HUD_HEIGHT = 96;
constexpr size_t MAX_LEVEL_FILE = 16 * 1024;
constexpr unsigned long AI_STEP_MS = 650;
constexpr int MAX_LEVEL_FILES = 32;

// Display names for unit types, indexed like Tactics::UNIT_TYPES.
constexpr StrId UNIT_NAME_IDS[Tactics::UNIT_TYPE_COUNT] = {
    StrId::STR_UNIT_HUMAN_SOLDIER,  StrId::STR_UNIT_HUMAN_ARCHER,
    StrId::STR_UNIT_HUMAN_KNIGHT,   StrId::STR_UNIT_ELF_FIGHTER,
    StrId::STR_UNIT_ELF_ARCHER,     StrId::STR_UNIT_ELF_MAGE,
    StrId::STR_UNIT_DWARF_WARRIOR,  StrId::STR_UNIT_DWARF_THUNDERER,
    StrId::STR_UNIT_DWARF_GUARD,    StrId::STR_UNIT_DRAKE_CLASHER,
    StrId::STR_UNIT_DRAKE_FLARE,    StrId::STR_UNIT_DRAKE_FLYER,
    StrId::STR_UNIT_GOBLIN,         StrId::STR_UNIT_ORC,
    StrId::STR_UNIT_TROLL,          StrId::STR_UNIT_WYVERN,
    StrId::STR_UNIT_GOBLIN_SLINGER,
};

// Display names for terrain, indexed like Tactics::TERRAIN_INFO.
constexpr StrId TERRAIN_NAME_IDS[Tactics::TERRAIN_COUNT] = {
    StrId::STR_TERRAIN_VOID,  StrId::STR_TERRAIN_PLAIN, StrId::STR_TERRAIN_FOREST, StrId::STR_TERRAIN_MOUNTAIN,
    StrId::STR_TERRAIN_WATER, StrId::STR_TERRAIN_ROCK,  StrId::STR_TERRAIN_CASTLE, StrId::STR_TERRAIN_ENEMY_BASE,
};

uint32_t fnv1a(const char* data, const size_t len) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < len; i++) {
    h ^= static_cast<uint8_t>(data[i]);
    h *= 16777619u;
  }
  return h;
}

}  // namespace

// ------------------------------------------------------------ save/resume

void TacticsActivity::savePath(char* out, const size_t outLen) const {
  std::snprintf(out, outLen, "%s/%s.sav", SAVES_DIR, currentLevelFile.c_str());
}

bool TacticsActivity::saveBattle() {
  if (!game || game->status() != Game::Status::Playing || currentLevelFile.empty()) return false;
  auto blob = makeUniqueNoThrow<uint8_t[]>(Game::stateSize());
  if (!blob) {
    LOG_ERR("TACTICS", "OOM: %u bytes for save", (unsigned)Game::stateSize());
    return false;
  }
  game->exportState(blob.get());
  Storage.mkdir(SAVES_DIR);
  char path[128];
  savePath(path, sizeof(path));
  HalFile file;
  if (!Storage.openFileForWrite("TACTICS", path, file)) return false;
  const uint32_t magic = SAVE_MAGIC;
  const uint8_t version = SAVE_VERSION;
  const uint32_t size = Game::stateSize();
  bool ok = file.write(&magic, sizeof(magic)) == sizeof(magic);
  ok = ok && file.write(&version, sizeof(version)) == sizeof(version);
  ok = ok && file.write(&levelCrc, sizeof(levelCrc)) == sizeof(levelCrc);
  ok = ok && file.write(&size, sizeof(size)) == sizeof(size);
  ok = ok && file.write(blob.get(), size) == size;
  if (!ok) LOG_ERR("TACTICS", "Short write on %s", path);
  return ok;
}

bool TacticsActivity::tryResumeBattle() {
  char path[128];
  savePath(path, sizeof(path));
  if (!Storage.exists(path)) return false;
  HalFile file;
  if (!Storage.openFileForRead("TACTICS", path, file)) return false;
  uint32_t magic = 0;
  uint8_t version = 0;
  uint32_t crc = 0;
  uint32_t size = 0;
  bool ok = file.read(&magic, sizeof(magic)) == sizeof(magic) && magic == SAVE_MAGIC;
  ok = ok && file.read(&version, sizeof(version)) == sizeof(version) && version == SAVE_VERSION;
  ok = ok && file.read(&crc, sizeof(crc)) == sizeof(crc) && crc == levelCrc;
  ok = ok && file.read(&size, sizeof(size)) == sizeof(size) && size == Game::stateSize();
  if (!ok) {
    LOG_INF("TACTICS", "Stale/invalid save ignored: %s", path);
    return false;
  }
  auto blob = makeUniqueNoThrow<uint8_t[]>(Game::stateSize());
  if (!blob) return false;
  if (file.read(blob.get(), size) != static_cast<int>(size)) return false;
  return game->importState(blob.get());
}

// ------------------------------------------------------------ lifecycle

void TacticsActivity::onEnter() {
  Activity::onEnter();
  mode = Mode::LevelSelect;
  scanLevels();
  requestUpdate();
}

void TacticsActivity::onExit() {
  Activity::onExit();
  // Persist a battle in progress so the reader can be put down mid-fight.
  if (game && mode != Mode::LevelSelect && mode != Mode::GameOver) saveBattle();
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
  // Mark levels with a battle in progress.
  levelHasSave.assign(levelFiles.size(), false);
  for (size_t i = 0; i < levelFiles.size(); i++) {
    char path[128];
    std::snprintf(path, sizeof(path), "%s/%s.sav", SAVES_DIR, levelFiles[i].c_str());
    levelHasSave[i] = Storage.exists(path);
  }
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
  currentLevelFile = fileName;
  levelCrc = fnv1a(buffer.get(), size);
  // start() first, unconditionally: a rejected save must leave a playable
  // fresh board.
  game->start(*level);
  if (tryResumeBattle()) LOG_INF("TACTICS", "Resumed battle for %s", fileName.c_str());

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
  nextRefreshFull = true;  // fresh board on a clean panel
  turnsSinceFullRefresh = 0;
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

int TacticsActivity::nearestReadyUnit(const int fromX, const int fromY) const {
  int best = -1;
  int bestDist = 1 << 20;
  for (int i = 0; i < game->unitCount(); i++) {
    const Unit& u = game->unit(i);
    if (!u.alive || u.side != Side::Player || u.acted) continue;
    const int dx = u.x > fromX ? u.x - fromX : fromX - u.x;
    const int dy = u.y > fromY ? u.y - fromY : fromY - u.y;
    if (dx + dy < bestDist) {
      bestDist = dx + dy;
      best = i;
    }
  }
  return best;
}

void TacticsActivity::jumpToNextReadyUnit() {
  // Cycle: first ready unit strictly after the one under the cursor, else wrap.
  const int here = game->unitIndexAt(cursorX, cursorY);
  int pick = -1;
  for (int off = 1; off <= game->unitCount(); off++) {
    const int i = (here >= 0 ? here + off : off - 1) % game->unitCount();
    const Unit& u = game->unit(i);
    if (u.alive && u.side == Side::Player && !u.acted) {
      pick = i;
      break;
    }
  }
  if (pick < 0) return;
  cursorX = game->unit(pick).x;
  cursorY = game->unit(pick).y;
  centerCamera(cursorX, cursorY);
  requestUpdate();
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
    case Mode::ActionSelect:
      loopActionSelect();
      break;
    case Mode::TargetSelect:
      loopTargetSelect();
      break;
    case Mode::PauseMenu:
      loopPauseMenu();
      break;
    case Mode::Inspect:
      loopInspect();
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
      std::snprintf(statusLine, sizeof(statusLine), "%s", I18N.get(UNIT_NAME_IDS[game->unit(idx).type]));
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
      undoX = game->unit(selectedUnit).x;
      undoY = game->unit(selectedUnit).y;
      game->moveUnit(selectedUnit, cursorX, cursorY);
      if (game->status() != Game::Status::Playing) {
        deleteSave();
        mode = Mode::GameOver;  // stepped onto the enemy base
        requestUpdate();
        return;
      }
      targetCount = game->targetsFrom(selectedUnit, cursorX, cursorY, targets, 8);
      actionSel = 0;
      mode = Mode::ActionSelect;
      requestUpdate();
    }
  }
}

void TacticsActivity::deleteSave() {
  if (currentLevelFile.empty()) return;
  char path[128];
  savePath(path, sizeof(path));
  if (Storage.exists(path)) Storage.remove(path);
}

void TacticsActivity::undoSelectedMove() {
  game->undoMove(selectedUnit, undoX, undoY);
  cursorX = undoX;
  cursorY = undoY;
  if (dangerOverlay) game->computeDangerZones();  // fog changed with the undo
  game->computeReach(selectedUnit);
  ensureCursorVisible();
  mode = Mode::MoveSelect;
}

void TacticsActivity::loopActionSelect() {
  // Options: [Attack] Wait Undo — Attack present only with targets.
  const int optionCount = targetCount > 0 ? 3 : 2;
  if (mappedInput.wasPressed(MappedInputManager::Button::Left) ||
      mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    actionSel = (actionSel + optionCount - 1) % optionCount;
    requestUpdate();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Right) ||
      mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    actionSel = (actionSel + 1) % optionCount;
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    // Back is a shortcut for Undo.
    undoSelectedMove();
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const int action = targetCount > 0 ? actionSel : actionSel + 1;  // 0=Attack 1=Wait 2=Undo
    switch (action) {
      case 0:
        beginTargetSelect();
        break;
      case 1:
        finishSelectedUnit();
        break;
      case 2:
        undoSelectedMove();
        break;
      default:
        break;
    }
    requestUpdate();
  }
}

void TacticsActivity::beginTargetSelect() {
  const Unit& u = game->unit(selectedUnit);
  targetCount = game->targetsFrom(selectedUnit, u.x, u.y, targets, 8);
  if (targetCount == 0) return;  // shouldn't happen: Attack is hidden without targets
  targetSel = 0;
  mode = Mode::TargetSelect;
}

void TacticsActivity::finishSelectedUnit() {
  if (selectedUnit >= 0) game->finishUnit(selectedUnit);
  selectedUnit = -1;
  if (dangerOverlay) game->computeDangerZones();
  if (game->allPlayerUnitsActed()) {
    game->endPlayerTurn();
    lastAiStepMs = millis();
    std::snprintf(statusLine, sizeof(statusLine), "%s", tr(STR_TACTICS_ENEMY_TURN));
    mode = Mode::EnemyTurn;
  } else {
    const int next = nearestReadyUnit(cursorX, cursorY);
    if (next >= 0) {
      cursorX = game->unit(next).x;
      cursorY = game->unit(next).y;
      ensureCursorVisible();
    }
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
    actionSel = 0;
    mode = Mode::ActionSelect;  // back out to attack/wait/undo
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    game->attack(selectedUnit, targets[targetSel]);
    selectedUnit = -1;
    if (dangerOverlay) game->computeDangerZones();
    if (game->status() != Game::Status::Playing) {
      deleteSave();
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
      const int next = nearestReadyUnit(cursorX, cursorY);
      if (next >= 0) {
        cursorX = game->unit(next).x;
        cursorY = game->unit(next).y;
        ensureCursorVisible();
      }
      mode = Mode::Cursor;
    }
    requestUpdate();
  }
}

void TacticsActivity::loopPauseMenu() {
  constexpr int OPTION_COUNT = 8;  // resume, next unit, inspect, danger, refresh, end turn, restart, quit
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
    nextRefreshFull = true;  // leaving the full-screen menu
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    switch (pauseSel) {
      case 0:  // resume
        mode = Mode::Cursor;
        nextRefreshFull = true;
        break;
      case 1:  // next unit
        mode = Mode::Cursor;
        nextRefreshFull = true;
        jumpToNextReadyUnit();
        break;
      case 2:  // inspect unit/terrain under cursor
        inspectUnit = game->isVisible(cursorX, cursorY) ? game->unitIndexAt(cursorX, cursorY) : -1;
        mode = Mode::Inspect;
        nextRefreshFull = true;
        break;
      case 3:  // toggle danger zones
        dangerOverlay = !dangerOverlay;
        if (dangerOverlay) game->computeDangerZones();
        break;
      case 4:  // manual full refresh
        mode = Mode::Cursor;
        nextRefreshFull = true;
        break;
      case 5:  // end turn
        for (int i = 0; i < game->unitCount(); i++) {
          if (game->unit(i).alive && game->unit(i).side == Side::Player) game->finishUnit(i);
        }
        game->endPlayerTurn();
        lastAiStepMs = millis();
        std::snprintf(statusLine, sizeof(statusLine), "%s", tr(STR_TACTICS_ENEMY_TURN));
        mode = Mode::EnemyTurn;
        nextRefreshFull = true;
        break;
      case 6:  // restart
        game->start(*level);
        selectedUnit = -1;
        dangerOverlay = false;
        deleteSave();
        mode = Mode::Cursor;
        nextRefreshFull = true;
        break;
      case 7:  // quit to level list
        deleteSave();
        scanLevels();  // refresh save markers
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
        deleteSave();
        mode = Mode::GameOver;
      } else {
        mode = Mode::Cursor;
        if (++turnsSinceFullRefresh >= 5) {
          turnsSinceFullRefresh = 0;
          nextRefreshFull = true;  // wipe accumulated e-ink ghosting
        }
        char turnBuf[48];
        std::snprintf(turnBuf, sizeof(turnBuf), tr(STR_TACTICS_TURN), game->turn());
        std::snprintf(statusLine, sizeof(statusLine), "%s", turnBuf);
        const int next = nearestReadyUnit(cursorX, cursorY);
        if (next >= 0) {
          cursorX = game->unit(next).x;
          cursorY = game->unit(next).y;
        }
        ensureCursorVisible();
        if (dangerOverlay) game->computeDangerZones();
        saveBattle();  // checkpoint each completed enemy phase
      }
      requestUpdate();
      return;
    }
    if (act.visible) {
      centerCamera(act.toX, act.toY);
      if (act.target >= 0) {
        std::snprintf(statusLine, sizeof(statusLine), "%s -%d", I18N.get(UNIT_NAME_IDS[game->unit(act.unit).type]),
                      act.damage);
      } else {
        std::snprintf(statusLine, sizeof(statusLine), "%s", tr(STR_TACTICS_ENEMY_TURN));
      }
      if (game->status() != Game::Status::Playing) {
        deleteSave();
        mode = Mode::GameOver;
      }
      requestUpdate();
      return;
    }
  }
}

void TacticsActivity::loopInspect() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    mode = Mode::Cursor;
    nextRefreshFull = true;  // wipe the full-screen overlay cleanly
    requestUpdate();
  }
}

void TacticsActivity::loopGameOver() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
      mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    scanLevels();  // refresh save markers
    mode = Mode::LevelSelect;
    requestUpdate();
  }
}

// ------------------------------------------------------------ rendering

void TacticsActivity::render(RenderLock&&) {
  renderer.clearScreen();
  if (mode == Mode::LevelSelect) {
    renderLevelSelect();
  } else if (mode == Mode::Inspect) {
    renderInspect();
  } else if (mode == Mode::PauseMenu) {
    renderPauseMenu();
  } else {
    renderBoard();
    renderHud();
  }
  renderer.displayBuffer(nextRefreshFull ? HalDisplay::FULL_REFRESH : HalDisplay::FAST_REFRESH);
  nextRefreshFull = false;
}

void TacticsActivity::renderLevelSelect() {
  const int pageWidth = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_TACTICS));

  const int top = metrics.topPadding + metrics.headerHeight + 10;
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
      char nameBuf[80];
      const bool hasSave = first + i < static_cast<int>(levelHasSave.size()) && levelHasSave[first + i];
      std::snprintf(nameBuf, sizeof(nameBuf), "%s%s", hasSave ? "> " : "", levelFiles[first + i].c_str());
      renderer.drawText(UI_12_FONT_ID, 20, y, nameBuf, !sel);
    }
  }
  if (levelError[0] != '\0') {
    renderer.drawText(SMALL_FONT_ID, 20, renderer.getScreenHeight() - 100, levelError, true);
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void TacticsActivity::renderPauseMenu() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_TACTICS_PAUSED));

  int y = metrics.topPadding + metrics.headerHeight + 12;
  char line[96];
  // Objective + turn.
  switch (level->win) {
    case Tactics::WinCondition::CaptureBase:
      std::snprintf(line, sizeof(line), "%s", tr(STR_TACTICS_OBJ_CAPTURE));
      break;
    case Tactics::WinCondition::Survive:
      std::snprintf(line, sizeof(line), tr(STR_TACTICS_OBJ_SURVIVE), (int)level->turnLimit);
      break;
    default:
      std::snprintf(line, sizeof(line), "%s", tr(STR_TACTICS_OBJ_DEFEAT_ALL));
      break;
  }
  renderer.drawText(SMALL_FONT_ID, 24, y, line, true);
  y += renderer.getLineHeight(SMALL_FONT_ID) + 6;
  if (level->turnLimit > 0) {
    std::snprintf(line, sizeof(line), tr(STR_TACTICS_TURN_OF), game->turn(), (int)level->turnLimit);
  } else {
    std::snprintf(line, sizeof(line), tr(STR_TACTICS_TURN), game->turn());
  }
  renderer.drawText(SMALL_FONT_ID, 24, y, line, true);
  y += renderer.getLineHeight(SMALL_FONT_ID) + 14;

  const char* options[8] = {
      tr(STR_TACTICS_RESUME),  tr(STR_TACTICS_NEXT_UNIT),
      tr(STR_TACTICS_INSPECT), dangerOverlay ? tr(STR_TACTICS_DANGER_ON) : tr(STR_TACTICS_DANGER_OFF),
      tr(STR_TACTICS_REFRESH), tr(STR_TACTICS_END_TURN),
      tr(STR_TACTICS_RESTART), tr(STR_TACTICS_QUIT),
  };
  const int lineH = renderer.getLineHeight(UI_12_FONT_ID) + 12;
  for (int i = 0; i < 8; i++) {
    const bool sel = i == pauseSel;
    if (sel) renderer.fillRect(12, y - 4, pageWidth - 24, lineH - 4, true);
    renderer.drawText(UI_12_FONT_ID, 24, y, options[i], !sel);
    y += lineH;
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void TacticsActivity::renderInspect() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_TACTICS_INSPECT));
  int y = metrics.topPadding + metrics.headerHeight + 16;
  const int lineH = renderer.getLineHeight(UI_12_FONT_ID) + 10;
  char line[96];

  const Terrain t = level->at(cursorX, cursorY);
  const auto& ti = Tactics::TERRAIN_INFO[static_cast<int>(t)];

  if (inspectUnit >= 0) {
    const Unit& u = game->unit(inspectUnit);
    const auto& info = u.info();
    // Sprite at 3x next to the name.
    const int sx = 24;
    for (int sy = 0; sy < Tactics::SPRITE_SIZE; sy++) {
      for (int sxp = 0; sxp < Tactics::SPRITE_SIZE; sxp++) {
        if (Tactics::spritePixel(u.type, sxp, sy)) {
          renderer.fillRect(sx + sxp * 3, y + sy * 3, 3, 3, true);
        }
      }
    }
    renderer.drawText(UI_12_FONT_ID, sx + Tactics::SPRITE_SIZE * 3 + 16, y + 8, I18N.get(UNIT_NAME_IDS[u.type]), true,
                      EpdFontFamily::BOLD);
    std::snprintf(line, sizeof(line), tr(STR_TACTICS_HP), u.hp, info.maxHp);
    renderer.drawText(UI_12_FONT_ID, sx + Tactics::SPRITE_SIZE * 3 + 16, y + 8 + lineH, line, true);
    y += Tactics::SPRITE_SIZE * 3 + 24;

    std::snprintf(line, sizeof(line), tr(STR_TACTICS_STATLINE), info.attack, info.range, (int)info.defense, info.move);
    renderer.drawText(UI_12_FONT_ID, 24, y, line, true);
    y += lineH;
    std::snprintf(line, sizeof(line), tr(STR_TACTICS_VISION), info.vision);
    renderer.drawText(UI_12_FONT_ID, 24, y, line, true);
    y += lineH;
    if (info.regen > 0) {
      std::snprintf(line, sizeof(line), tr(STR_TACTICS_REGEN), info.regen);
      renderer.drawText(UI_12_FONT_ID, 24, y, line, true);
      y += lineH;
    }
    if (info.flies) {
      renderer.drawText(UI_12_FONT_ID, 24, y, tr(STR_TACTICS_FLIES), true);
      y += lineH;
    }
    // Defense breakdown on the tile it stands on.
    int terrainDef = 0;
    int racialDef = 0;
    if (!info.flies) {
      terrainDef = ti.defense;
      if (t == Terrain::Forest && info.race == Tactics::Race::Elf) racialDef = 15;
      if (t == Terrain::Mountain && info.race == Tactics::Race::Dwarf) racialDef = 15;
    }
    std::snprintf(line, sizeof(line), tr(STR_TACTICS_DEF_BREAKDOWN), game->defenseAt(u, u.x, u.y), (int)info.defense,
                  terrainDef, racialDef);
    renderer.drawText(UI_12_FONT_ID, 24, y, line, true);
    y += lineH;
  }

  // Terrain block (always shown).
  y += 8;
  renderer.drawText(UI_12_FONT_ID, 24, y, I18N.get(TERRAIN_NAME_IDS[static_cast<int>(t)]), true, EpdFontFamily::BOLD);
  y += lineH;
  char defBuf[24] = {0};
  char movBuf[24] = {0};
  char healBuf[24] = {0};
  if (ti.defense > 0) std::snprintf(defBuf, sizeof(defBuf), tr(STR_TACTICS_TERRAIN_DEF), (int)ti.defense);
  if (ti.moveCost > 0) std::snprintf(movBuf, sizeof(movBuf), tr(STR_TACTICS_TERRAIN_MOVE), (int)ti.moveCost);
  if (ti.heal > 0) std::snprintf(healBuf, sizeof(healBuf), tr(STR_TACTICS_TERRAIN_HEAL), (int)ti.heal);
  std::snprintf(line, sizeof(line), "%s %s %s", defBuf, movBuf, healBuf);
  renderer.drawText(UI_12_FONT_ID, 24, y, line, true);
  y += lineH + 8;

  renderer.drawText(SMALL_FONT_ID, 24, y, tr(STR_TACTICS_DMG_RULE), true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "", "");
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
        renderer.fillRect(px + (TILE - 7) / 2, py + (TILE - 7) / 2, 7, 7, true);
      }
      // Danger overlay: dithered strip along the tile bottom.
      if (dangerOverlay && vis && game->isDangerous(mx, my)) {
        renderer.fillRectDither(px + 2, py + TILE - 8, TILE - 4, 6, Color::DarkGray);
      }
    }
  }
  // Cursor / target highlight
  int cx = cursorX, cy = cursorY;
  if (mode == Mode::TargetSelect && targetCount > 0) {
    cx = game->unit(targets[targetSel]).x;
    cy = game->unit(targets[targetSel]).y;
  }
  if (mode == Mode::Cursor || mode == Mode::MoveSelect || mode == Mode::ActionSelect || mode == Mode::TargetSelect) {
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

  if (mode == Mode::ActionSelect) {
    const char* options[3];
    int n = 0;
    if (targetCount > 0) options[n++] = tr(STR_TACTICS_ATTACK);
    options[n++] = tr(STR_TACTICS_WAIT);
    options[n++] = tr(STR_TACTICS_UNDO);
    const int w = pageWidth / n;
    for (int i = 0; i < n; i++) {
      const bool sel = i == actionSel;
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
    std::snprintf(line, sizeof(line), "%s %d  >%d  <%d", I18N.get(UNIT_NAME_IDS[game->unit(targets[targetSel]).type]),
                  game->unit(targets[targetSel]).hp, f.damageToTarget, f.counterDamage);
    const int tw = renderer.getTextWidth(SMALL_FONT_ID, line);
    renderer.drawText(SMALL_FONT_ID, pageWidth - tw - 16, top + 10, line, true);
  } else {
    // Line 1: unit under cursor — name, HP, compact stat line.
    const int ui = game->unitIndexAt(cursorX, cursorY);
    if (ui >= 0 && game->isVisible(cursorX, cursorY)) {
      const Unit& u = game->unit(ui);
      char stats[48];
      std::snprintf(stats, sizeof(stats), tr(STR_TACTICS_STATLINE), u.info().attack, u.info().range,
                    game->defenseAt(u, u.x, u.y), u.info().move);
      std::snprintf(line, sizeof(line), "%s %d/%d  %s", I18N.get(UNIT_NAME_IDS[u.type]), u.hp, u.info().maxHp, stats);
      const int tw = renderer.getTextWidth(SMALL_FONT_ID, line);
      renderer.drawText(SMALL_FONT_ID, pageWidth - tw - 16, top + 10, line, true);
    }
    // Line 2: terrain under cursor (explored tiles only).
    if (game->isExplored(cursorX, cursorY)) {
      const Terrain t = level->at(cursorX, cursorY);
      const auto& ti = Tactics::TERRAIN_INFO[static_cast<int>(t)];
      char defBuf[20] = {0};
      char movBuf[20] = {0};
      char healBuf[20] = {0};
      if (ti.defense > 0) std::snprintf(defBuf, sizeof(defBuf), tr(STR_TACTICS_TERRAIN_DEF), (int)ti.defense);
      if (ti.moveCost > 0) std::snprintf(movBuf, sizeof(movBuf), tr(STR_TACTICS_TERRAIN_MOVE), (int)ti.moveCost);
      if (ti.heal > 0) std::snprintf(healBuf, sizeof(healBuf), tr(STR_TACTICS_TERRAIN_HEAL), (int)ti.heal);
      // Racial bonus hint when the unit standing here benefits.
      const char* racial = "";
      if (ui >= 0 && game->isVisible(cursorX, cursorY)) {
        const Unit& u = game->unit(ui);
        if (!u.info().flies && ((t == Terrain::Forest && u.info().race == Tactics::Race::Elf) ||
                                (t == Terrain::Mountain && u.info().race == Tactics::Race::Dwarf))) {
          racial = tr(STR_TACTICS_RACIAL_BONUS);
        }
      }
      std::snprintf(line, sizeof(line), "%s  %s %s %s %s", I18N.get(TERRAIN_NAME_IDS[static_cast<int>(t)]), defBuf,
                    movBuf, healBuf, racial);
      const int tw = renderer.getTextWidth(SMALL_FONT_ID, line);
      renderer.drawText(SMALL_FONT_ID, pageWidth - tw - 16, top + 50, line, true);
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
    // Landmarks: explored castles and the enemy base as 3x3 dots.
    for (int y = 0; y < level->height; y++) {
      for (int x = 0; x < level->width; x++) {
        const Terrain t = level->at(x, y);
        if (t != Terrain::Castle && t != Terrain::EnemyBase) continue;
        if (!game->isExplored(x, y)) continue;
        const int lx = mmX + x * mmW / level->width;
        const int ly = mmY + y * mmH / level->height;
        renderer.fillRect(lx - 1, ly - 1, 3, 3, true);
      }
    }
  }
}
