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
constexpr const char* COMPLETED_FILE = "/.crosspoint/tactics/completed.txt";
constexpr size_t MAX_LEVEL_FILE = 16 * 1024;
constexpr unsigned long AI_STEP_MS = 650;
constexpr int MAX_LEVEL_FILES = 64;  // battles + story interludes

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
  if (!ok) {
    LOG_ERR("TACTICS", "Short write on %s", path);
  } else {
    LOG_INF("TACTICS", "Battle saved: %s", path);
  }
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
    const bool isLevel = len >= 6 && std::strcmp(name + len - 5, ".yaml") == 0;
    const bool isStory = len >= 7 && std::strcmp(name + len - 6, ".story") == 0;
    if (!isLevel && !isStory) continue;
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
  loadCompleted();
}

bool TacticsActivity::isStoryFile(const std::string& name) {
  return name.size() >= 7 && name.compare(name.size() - 6, 6, ".story") == 0;
}

bool TacticsActivity::openEntry(const int index) {
  if (index < 0 || index >= static_cast<int>(levelFiles.size())) return false;
  const std::string& name = levelFiles[index];
  if (isStoryFile(name)) return loadStory(name);
  return loadLevel(name);
}

void TacticsActivity::advanceAfter(const std::string& file) {
  // Files sort by name; the next one in order is the next campaign beat.
  int idx = -1;
  for (size_t i = 0; i < levelFiles.size(); i++) {
    if (levelFiles[i] == file) {
      idx = static_cast<int>(i);
      break;
    }
  }
  if (idx >= 0 && idx + 1 < static_cast<int>(levelFiles.size())) {
    levelSelIndex = idx + 1;
    if (openEntry(idx + 1)) {
      requestUpdate();
      return;
    }
  }
  scanLevels();
  mode = Mode::LevelSelect;
  requestUpdate();
}

// ------------------------------------------------------------ story viewer

bool TacticsActivity::loadStory(const std::string& fileName) {
  levelError[0] = '\0';
  const std::string path = std::string(LEVELS_DIR) + "/" + fileName;
  HalFile file;
  if (!Storage.openFileForRead("TACTICS", path, file)) {
    std::snprintf(levelError, sizeof(levelError), "%s", tr(STR_TACTICS_LEVEL_ERROR));
    return false;
  }
  const size_t size = file.fileSize();
  if (size == 0 || size > MAX_LEVEL_FILE) {
    LOG_ERR("TACTICS", "Story file size out of range: %u", (unsigned)size);
    return false;
  }
  storyText = makeUniqueNoThrow<char[]>(size + 1);
  if (!storyText) {
    LOG_ERR("TACTICS", "OOM: %u bytes for story text", (unsigned)size);
    return false;
  }
  if (file.read(storyText.get(), size) != static_cast<int>(size)) {
    LOG_ERR("TACTICS", "Short read on %s", path.c_str());
    storyText.reset();
    return false;
  }
  // Normalise line endings in place.
  size_t w = 0;
  for (size_t r = 0; r < size; r++) {
    if (storyText[r] != '\r') storyText[w++] = storyText[r];
  }
  storySize = w;
  storyText[w] = '\0';
  currentStoryFile = fileName;

  storyPageStarts.clear();
  storyPageStarts.reserve(8);
  storyPageStarts.push_back(0);
  for (size_t off = layoutStoryPage(0, false); off < storySize;) {
    storyPageStarts.push_back(static_cast<uint32_t>(off));
    const size_t next = layoutStoryPage(off, false);
    if (next <= off) break;  // safety: layout must always advance
    off = next;
  }
  storyPage = 0;
  mode = Mode::StoryView;
  return true;
}

size_t TacticsActivity::layoutStoryPage(size_t offset, const bool draw) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int textW = pageWidth - 48;
  int y = metrics.topPadding + metrics.headerHeight + 12;
  const int limit = pageHeight - 56;
  const int lineH = renderer.getLineHeight(UI_12_FONT_ID) + 8;
  char lineBuf[128];

  size_t i = offset;
  while (i < storySize && y + lineH <= limit) {
    if (storyText[i] == '\n') {  // blank line = paragraph spacing
      i++;
      y += lineH / 2;
      continue;
    }
    const size_t lineStart = i;
    int n = 0;
    int lastSpace = -1;
    while (i < storySize && storyText[i] != '\n' && n < static_cast<int>(sizeof(lineBuf)) - 1) {
      lineBuf[n] = storyText[i];
      if (storyText[i] == ' ') lastSpace = n;
      lineBuf[n + 1] = '\0';
      if (renderer.getTextWidth(UI_12_FONT_ID, lineBuf) > textW) {
        if (lastSpace > 0) {
          n = lastSpace;
          i = lineStart + lastSpace + 1;  // resume after the space
        } else if (n == 0) {
          n = 1;  // single oversized glyph: force progress
          i++;
        }
        break;
      }
      n++;
      i++;
    }
    lineBuf[n] = '\0';
    if (draw && n > 0) renderer.drawText(UI_12_FONT_ID, 24, y, lineBuf, true);
    y += lineH;
    if (i < storySize && storyText[i] == '\n') i++;  // line consumed its newline
  }
  return i;
}

void TacticsActivity::loopStoryView() {
  const int pageCount = static_cast<int>(storyPageStarts.size());
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    mode = Mode::LevelSelect;
    requestUpdate();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Left) ||
      mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    if (storyPage > 0) {
      storyPage--;
      requestUpdate();
    }
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Right) ||
      mappedInput.wasPressed(MappedInputManager::Button::Down) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (storyPage + 1 < pageCount) {
      storyPage++;
      requestUpdate();
    } else {
      advanceAfter(currentStoryFile);  // story finished: next campaign beat
    }
  }
}

void TacticsActivity::renderStoryView() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_TACTICS));
  if (storyPage < static_cast<int>(storyPageStarts.size())) {
    layoutStoryPage(storyPageStarts[storyPage], true);
  }
  char pageBuf[16];
  std::snprintf(pageBuf, sizeof(pageBuf), tr(STR_TACTICS_PAGE), storyPage + 1,
                static_cast<int>(storyPageStarts.size()));
  const int tw = renderer.getTextWidth(SMALL_FONT_ID, pageBuf);
  renderer.drawText(SMALL_FONT_ID, pageWidth - tw - 16, pageHeight - 52, pageBuf, true);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_TACTICS_CONTINUE), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

// ------------------------------------------------------------ completion

void TacticsActivity::loadCompleted() {
  completedFiles.clear();
  if (!Storage.exists(COMPLETED_FILE)) return;
  HalFile file;
  if (!Storage.openFileForRead("TACTICS", COMPLETED_FILE, file)) return;
  const size_t size = file.fileSize();
  if (size == 0 || size > 4096) return;
  const auto buf = makeUniqueNoThrow<char[]>(size + 1);
  if (!buf) return;
  if (file.read(buf.get(), size) != static_cast<int>(size)) return;
  buf[size] = '\0';
  completedFiles.reserve(8);
  size_t start = 0;
  for (size_t i = 0; i <= size; i++) {
    if (i == size || buf[i] == '\n') {
      size_t len = i - start;
      while (len > 0 && buf[start + len - 1] == '\r') len--;
      if (len > 0) completedFiles.emplace_back(buf.get() + start, len);
      start = i + 1;
    }
  }
}

bool TacticsActivity::isCompleted(const std::string& fileName) const {
  for (const auto& f : completedFiles) {
    if (f == fileName) return true;
  }
  return false;
}

void TacticsActivity::markCompleted(const std::string& fileName) {
  if (isCompleted(fileName)) return;
  completedFiles.push_back(fileName);
  Storage.mkdir(SAVES_DIR);
  HalFile file;
  if (!Storage.openFileForWrite("TACTICS", COMPLETED_FILE, file)) return;
  for (const auto& f : completedFiles) {
    file.write(f.c_str(), f.size());
    file.write("\n", 1);
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
    case Mode::StoryView:
      loopStoryView();
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
    openEntry(levelSelIndex);
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
      game->computeDangerZones();  // sword markers during move selection
      game->computeReach(idx);     // after: danger computation trashes the reach cache
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
      const int8_t fromX = game->unit(selectedUnit).x;
      const int8_t fromY = game->unit(selectedUnit).y;
      // Record which enemies are visible before the move — a move that
      // reveals a new enemy has bought information and cannot be undone.
      bool visBefore[Tactics::MAX_UNITS] = {false};
      for (int i = 0; i < game->unitCount(); i++) {
        const Unit& u = game->unit(i);
        if (u.alive && u.side == Side::Enemy) visBefore[i] = game->isVisible(u.x, u.y);
      }
      game->moveUnit(selectedUnit, cursorX, cursorY);
      if (game->status() != Game::Status::Playing) {
        deleteSave();
        mode = Mode::GameOver;  // stepped onto the enemy base
        requestUpdate();
        return;
      }
      bool revealed = false;
      for (int i = 0; i < game->unitCount() && !revealed; i++) {
        const Unit& u = game->unit(i);
        if (u.alive && u.side == Side::Enemy && !visBefore[i] && game->isVisible(u.x, u.y)) revealed = true;
      }
      undoUnit = revealed ? -1 : selectedUnit;
      undoX = fromX;
      undoY = fromY;
      // Moves auto-commit: straight to target selection when there is
      // something to fight, otherwise the unit is done.
      targetCount = game->targetsFrom(selectedUnit, cursorX, cursorY, targets, 8);
      if (targetCount > 0) {
        targetSel = 0;
        mode = Mode::TargetSelect;
      } else {
        finishSelectedUnit();
      }
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
    undoUnit = -1;  // the enemy phase makes every move final
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
    finishSelectedUnit();  // skip attacking; the move itself stays undoable
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    game->attack(selectedUnit, targets[targetSel]);
    undoUnit = -1;  // fights are never undone
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

int TacticsActivity::buildPauseActions(PauseAction* out) const {
  int n = 0;
  out[n++] = PauseAction::Resume;
  if (undoUnit >= 0) out[n++] = PauseAction::UndoMove;
  out[n++] = PauseAction::NextUnit;
  out[n++] = PauseAction::Inspect;
  out[n++] = PauseAction::Danger;
  out[n++] = PauseAction::Refresh;
  out[n++] = PauseAction::EndTurn;
  out[n++] = PauseAction::Restart;
  out[n++] = PauseAction::Quit;
  return n;
}

void TacticsActivity::loopPauseMenu() {
  PauseAction actions[9];
  const int optionCount = buildPauseActions(actions);
  if (pauseSel >= optionCount) pauseSel = optionCount - 1;
  if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
      mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    pauseSel = (pauseSel + optionCount - 1) % optionCount;
    requestUpdate();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
      mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    pauseSel = (pauseSel + 1) % optionCount;
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    mode = Mode::Cursor;
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    switch (actions[pauseSel]) {
      case PauseAction::Resume:
        mode = Mode::Cursor;
        break;
      case PauseAction::UndoMove:
        if (undoUnit >= 0) {
          game->undoMove(undoUnit, undoX, undoY);
          cursorX = undoX;
          cursorY = undoY;
          selectedUnit = -1;
          if (dangerOverlay) game->computeDangerZones();
          centerCamera(cursorX, cursorY);
          undoUnit = -1;
        }
        mode = Mode::Cursor;
        break;
      case PauseAction::NextUnit:
        mode = Mode::Cursor;
        jumpToNextReadyUnit();
        break;
      case PauseAction::Inspect:
        inspectUnit = game->isVisible(cursorX, cursorY) ? game->unitIndexAt(cursorX, cursorY) : -1;
        mode = Mode::Inspect;
        break;
      case PauseAction::Danger:
        dangerOverlay = !dangerOverlay;
        if (dangerOverlay) game->computeDangerZones();
        break;
      case PauseAction::Refresh:
        mode = Mode::Cursor;
        nextRefreshFull = true;
        break;
      case PauseAction::EndTurn:
        for (int i = 0; i < game->unitCount(); i++) {
          if (game->unit(i).alive && game->unit(i).side == Side::Player) game->finishUnit(i);
        }
        undoUnit = -1;  // the enemy phase makes every move final
        game->endPlayerTurn();
        lastAiStepMs = millis();
        std::snprintf(statusLine, sizeof(statusLine), "%s", tr(STR_TACTICS_ENEMY_TURN));
        mode = Mode::EnemyTurn;
        break;
      case PauseAction::Restart:
        game->start(*level);
        selectedUnit = -1;
        undoUnit = -1;
        dangerOverlay = false;
        deleteSave();
        mode = Mode::Cursor;
        break;
      case PauseAction::Quit:  // keep the battle resumable
        saveBattle();
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
    requestUpdate();
  }
}

void TacticsActivity::loopGameOver() {
  const bool won = game->status() == Game::Status::Victory;
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (won) {
      markCompleted(currentLevelFile);
      advanceAfter(currentLevelFile);  // next campaign beat (story or battle)
      return;
    }
    scanLevels();  // refresh save markers
    mode = Mode::LevelSelect;
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (won) markCompleted(currentLevelFile);
    scanLevels();
    mode = Mode::LevelSelect;
    requestUpdate();
  }
}

// ------------------------------------------------------------ rendering

void TacticsActivity::render(RenderLock&&) {
  renderer.clearScreen();
  if (mode == Mode::LevelSelect) {
    renderLevelSelect();
  } else if (mode == Mode::StoryView) {
    renderStoryView();
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
      const int fileIdx = first + i;
      const bool hasSave = fileIdx < static_cast<int>(levelHasSave.size()) && levelHasSave[fileIdx];
      const char* marker = hasSave ? "> " : (isCompleted(levelFiles[fileIdx]) ? "* " : "");
      std::snprintf(nameBuf, sizeof(nameBuf), "%s%s", marker, levelFiles[fileIdx].c_str());
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

  PauseAction actions[9];
  const int optionCount = buildPauseActions(actions);
  const int lineH = renderer.getLineHeight(UI_12_FONT_ID) + 12;
  for (int i = 0; i < optionCount; i++) {
    const char* label = "";
    switch (actions[i]) {
      case PauseAction::Resume:
        label = tr(STR_TACTICS_RESUME);
        break;
      case PauseAction::UndoMove:
        label = tr(STR_TACTICS_UNDO);
        break;
      case PauseAction::NextUnit:
        label = tr(STR_TACTICS_NEXT_UNIT);
        break;
      case PauseAction::Inspect:
        label = tr(STR_TACTICS_INSPECT);
        break;
      case PauseAction::Danger:
        label = dangerOverlay ? tr(STR_TACTICS_DANGER_ON) : tr(STR_TACTICS_DANGER_OFF);
        break;
      case PauseAction::Refresh:
        label = tr(STR_TACTICS_REFRESH);
        break;
      case PauseAction::EndTurn:
        label = tr(STR_TACTICS_END_TURN);
        break;
      case PauseAction::Restart:
        label = tr(STR_TACTICS_RESTART);
        break;
      case PauseAction::Quit:
        label = tr(STR_TACTICS_QUIT);
        break;
      default:
        break;
    }
    const bool sel = i == pauseSel;
    if (sel) renderer.fillRect(12, y - 4, pageWidth - 24, lineH - 4, true);
    renderer.drawText(UI_12_FONT_ID, 24, y, label, !sel);
    y += lineH;
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void TacticsActivity::renderInspect() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_TACTICS_INSPECT));
  int y = metrics.topPadding + metrics.headerHeight + 12;
  // Everything must stay above the button hints; skip lines that don't fit
  // (landscape is much shorter than portrait).
  const int limit = pageHeight - 56;
  const int lineH = renderer.getLineHeight(SMALL_FONT_ID) + 6;
  char line[96];

  const Terrain t = level->at(cursorX, cursorY);
  const auto& ti = Tactics::TERRAIN_INFO[static_cast<int>(t)];

  if (inspectUnit >= 0) {
    const Unit& u = game->unit(inspectUnit);
    const auto& info = u.info();
    // Sprite at 2x next to the name + HP.
    const int sx = 24;
    for (int sy = 0; sy < Tactics::SPRITE_SIZE; sy++) {
      for (int sxp = 0; sxp < Tactics::SPRITE_SIZE; sxp++) {
        if (Tactics::spritePixel(u.type, sxp, sy)) {
          renderer.fillRect(sx + sxp * 2, y + sy * 2, 2, 2, true);
        }
      }
    }
    const int textX = sx + Tactics::SPRITE_SIZE * 2 + 16;
    renderer.drawText(UI_12_FONT_ID, textX, y, I18N.get(UNIT_NAME_IDS[u.type]), true, EpdFontFamily::BOLD);
    std::snprintf(line, sizeof(line), tr(STR_TACTICS_HP), u.hp, info.maxHp);
    renderer.drawText(SMALL_FONT_ID, textX, y + renderer.getLineHeight(UI_12_FONT_ID) + 4, line, true);
    y += Tactics::SPRITE_SIZE * 2 + 12;

    char extras[40] = {0};
    if (info.regen > 0) std::snprintf(extras, sizeof(extras), tr(STR_TACTICS_REGEN), info.regen);
    char vision[24];
    std::snprintf(vision, sizeof(vision), tr(STR_TACTICS_VISION), info.vision);
    char stats[48];
    std::snprintf(stats, sizeof(stats), tr(STR_TACTICS_STATLINE), info.attack, info.range, (int)info.defense,
                  info.move);
    std::snprintf(line, sizeof(line), "%s  %s  %s %s", stats, vision, extras, info.flies ? tr(STR_TACTICS_FLIES) : "");
    if (y + lineH <= limit) {
      renderer.drawText(SMALL_FONT_ID, 24, y, line, true);
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
    if (y + lineH <= limit) {
      renderer.drawText(SMALL_FONT_ID, 24, y, line, true);
      y += lineH;
    }
    y += 6;
  }

  // Terrain block (always shown).
  if (y + lineH <= limit) {
    renderer.drawText(UI_12_FONT_ID, 24, y, I18N.get(TERRAIN_NAME_IDS[static_cast<int>(t)]), true,
                      EpdFontFamily::BOLD);
    y += renderer.getLineHeight(UI_12_FONT_ID) + 6;
  }
  char defBuf[24] = {0};
  char movBuf[24] = {0};
  char healBuf[24] = {0};
  if (ti.defense > 0) std::snprintf(defBuf, sizeof(defBuf), tr(STR_TACTICS_TERRAIN_DEF), (int)ti.defense);
  if (ti.moveCost > 0) std::snprintf(movBuf, sizeof(movBuf), tr(STR_TACTICS_TERRAIN_MOVE), (int)ti.moveCost);
  if (ti.heal > 0) std::snprintf(healBuf, sizeof(healBuf), tr(STR_TACTICS_TERRAIN_HEAL), (int)ti.heal);
  std::snprintf(line, sizeof(line), "%s %s %s", defBuf, movBuf, healBuf);
  if (y + lineH <= limit) {
    renderer.drawText(SMALL_FONT_ID, 24, y, line, true);
    y += lineH + 6;
  }

  if (y + lineH <= limit) {
    renderer.drawText(SMALL_FONT_ID, 24, y, tr(STR_TACTICS_DMG_RULE), true);
  }

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

void TacticsActivity::drawDangerSword(const int px, const int py) const {
  // 12x12 sword, blade pointing up-right, in the tile's top-left corner
  // (the done-marker owns top-right, the HP bar owns the bottom).
  const int x = px + 3;
  const int y = py + 3;
  renderer.drawLine(x + 3, y + 8, x + 10, y + 1, true);  // blade
  renderer.drawLine(x + 4, y + 9, x + 11, y + 2, true);  // blade, 2px wide
  renderer.drawLine(x + 2, y + 6, x + 6, y + 10, true);  // crossguard
  renderer.fillRect(x + 0, y + 9, 3, 3, true);           // pommel
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
      // Danger marker: a small sword in the tile's top-left corner. Always
      // shown while choosing a destination; elsewhere only when toggled on.
      if ((dangerOverlay || mode == Mode::MoveSelect) && vis && game->isDangerous(mx, my)) {
        drawDangerSword(px, py);
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
    if (won) {  // Confirm advances to the next campaign beat
      const int tw = renderer.getTextWidth(SMALL_FONT_ID, tr(STR_TACTICS_CONTINUE));
      renderer.drawText(SMALL_FONT_ID, pageWidth - tw - 16, top + 30, tr(STR_TACTICS_CONTINUE), true);
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
