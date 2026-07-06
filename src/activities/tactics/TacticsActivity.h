#pragma once
// Turn-based tactics mini-game activity. Gameplay rules live in
// lib/Tactics (hardware-free, unit-tested on the host); this activity owns
// input, the scrolling camera over maps larger than the screen, fog/terrain
// rendering and the paced enemy-turn playback.
//
// Levels are plain-text files users drop into /tactics on the SD card (they
// can be uploaded via the web file transfer). See docs/tactics-levels.md.
#include <TacticsGame.h>
#include <TacticsLevel.h>

#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"

class TacticsActivity final : public Activity {
 public:
  explicit TacticsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Tactics", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return mode == Mode::EnemyTurn; }

 private:
  enum class Mode : uint8_t {
    LevelSelect,
    Cursor,        // free cursor, nothing selected
    MoveSelect,    // own unit selected, reach overlay shown
    ActionSelect,  // unit has moved: attack / wait / undo
    TargetSelect,  // choosing an attack target
    PauseMenu,     // full-screen menu: resume / next / inspect / ...
    Inspect,       // full-screen unit + terrain panel
    EnemyTurn,
    GameOver,
  };

  // Level selection
  void scanLevels();
  bool loadLevel(const std::string& fileName);
  std::vector<std::string> levelFiles;
  int levelSelIndex = 0;
  char levelError[96] = {0};

  // Gameplay state (heap-allocated in onEnter, freed in onExit)
  std::unique_ptr<Tactics::Level> level;
  std::unique_ptr<Tactics::Game> game;
  Mode mode = Mode::LevelSelect;
  int cursorX = 0, cursorY = 0;
  int camX = 0, camY = 0;
  int selectedUnit = -1;
  int8_t targets[8] = {0};
  int targetCount = 0;
  int targetSel = 0;
  int pauseSel = 0;
  int actionSel = 0;
  int8_t undoX = 0, undoY = 0;  // selected unit's pre-move position
  bool dangerOverlay = false;
  int inspectUnit = -1;  // -1 = terrain-only inspect
  unsigned long lastAiStepMs = 0;
  char statusLine[64] = {0};
  bool nextRefreshFull = false;
  uint8_t turnsSinceFullRefresh = 0;

  // Input handling per mode
  void loopLevelSelect();
  void loopCursor();
  void loopMoveSelect();
  void loopActionSelect();
  void loopTargetSelect();
  void loopPauseMenu();
  void loopInspect();
  void loopEnemyTurn();
  void loopGameOver();
  bool moveCursor();  // shared cursor movement, returns true if moved
  void undoSelectedMove();
  int nearestReadyUnit(int fromX, int fromY) const;  // -1 if none
  void jumpToNextReadyUnit();                        // cycles from cursor

  // Camera
  int viewCols() const;
  int viewRows() const;
  int hudTop() const;
  void clampCamera();
  void centerCamera(int x, int y);
  void ensureCursorVisible();

  // Rendering
  void renderLevelSelect();
  void renderBoard();
  void renderHud();
  void renderPauseMenu();
  void renderInspect();
  void drawTerrainTile(int px, int py, Tactics::Terrain t) const;
  void drawUnitTile(int px, int py, const Tactics::Unit& u) const;
  void beginTargetSelect();  // collects targets, transitions mode
  void finishSelectedUnit();
  void deleteSave();  // battle save management (see save/resume section)
};
