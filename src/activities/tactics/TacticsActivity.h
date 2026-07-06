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
    StoryView,     // paged text from a .story file (campaign interludes)
    Cursor,        // free cursor, nothing selected
    MoveSelect,    // own unit selected, reach overlay shown
    TargetSelect,  // unit has moved, choosing an attack target
    PauseMenu,     // full-screen menu: resume / undo / next / inspect / ...
    Inspect,       // full-screen unit + terrain panel
    EnemyTurn,
    GameOver,
  };

  // Pause menu entries; the visible list is built per-state (Undo only
  // appears while the last pure move is still revertible).
  enum class PauseAction : uint8_t {
    Resume,
    UndoMove,
    NextUnit,
    Inspect,
    Danger,
    Refresh,
    EndTurn,
    Restart,
    Quit,
  };

  // Level selection. The list holds both .yaml battles and .story text
  // interludes, sorted by filename — that order *is* the campaign.
  void scanLevels();
  bool loadLevel(const std::string& fileName);
  bool openEntry(int index);                  // dispatches story vs battle
  void advanceAfter(const std::string& file); // campaign chain: open next entry
  static bool isStoryFile(const std::string& name);
  std::vector<std::string> levelFiles;
  int levelSelIndex = 0;
  char levelError[96] = {0};

  // Story viewer
  bool loadStory(const std::string& fileName);
  void loopStoryView();
  void renderStoryView();
  // Lays out one page starting at `offset`; draws when `draw`, returns the
  // offset of the next page (== storySize when this was the last page).
  size_t layoutStoryPage(size_t offset, bool draw);
  std::unique_ptr<char[]> storyText;
  size_t storySize = 0;
  std::vector<uint32_t> storyPageStarts;
  int storyPage = 0;
  std::string currentStoryFile;

  // Campaign completion markers (/.crosspoint/tactics/completed.txt)
  void loadCompleted();
  void markCompleted(const std::string& fileName);
  bool isCompleted(const std::string& fileName) const;
  std::vector<std::string> completedFiles;

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
  // Undo: the most recent pure move (no attack) can be reverted from the
  // pause menu unless it revealed a new enemy or the enemy phase ran.
  int undoUnit = -1;  // -1 = nothing to undo
  int8_t undoX = 0, undoY = 0;
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
  void loopTargetSelect();
  void loopPauseMenu();
  void loopInspect();
  void loopEnemyTurn();
  void loopGameOver();
  bool moveCursor();  // shared cursor movement, returns true if moved
  int buildPauseActions(PauseAction* out) const;     // fills, returns count
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
  void drawDangerSword(int px, int py) const;
  void beginTargetSelect();  // collects targets, transitions mode
  void finishSelectedUnit();

  // Battle save/resume (versioned blob in /.crosspoint/tactics/)
  std::string currentLevelFile;  // set by loadLevel
  uint32_t levelCrc = 0;
  std::vector<bool> levelHasSave;  // parallel to levelFiles
  void savePath(char* out, size_t outLen) const;
  bool saveBattle();
  bool tryResumeBattle();  // true if a valid save was imported
  void deleteSave();
};
