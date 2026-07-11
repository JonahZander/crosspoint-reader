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
    Rules,         // paged "how to play" reference
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
    HowToPlay,
    Danger,
    Refresh,
    EndTurn,
    Restart,
    Quit,
  };

  // Level selection. The list holds .yaml battles and .story interludes,
  // sorted by filename — that order *is* the campaign. Subdirectories of
  // /tactics are campaigns of their own: the root lists them as "name/"
  // entries (folders first); entering one lists its files with the folder
  // prefix kept in the entry ("deep_road/11_x.yaml"), so every downstream
  // path (loading, saves, completion) keys off the same relative path.
  void scanLevels();
  bool loadLevel(const std::string& fileName);
  bool openEntry(int index);                   // dispatches folder/story/battle
  void advanceAfter(const std::string& file);  // campaign chain: open next entry
  static bool isStoryFile(const std::string& name);
  static bool isDirEntry(const std::string& name) { return !name.empty() && name.back() == '/'; }
  std::vector<std::string> levelFiles;
  std::string currentDir;  // "" = /tactics root
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
  // Cursor resting on a visible enemy shows that enemy's threat: dashed
  // border on its walk area, danger markers on the attack fringe.
  int hoverEnemy = -1;
  void updateHoverThreat();
  int inspectUnit = -1;  // -1 = terrain-only inspect
  int rulesPage = 0;
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
  void loopRules();
  void loopEnemyTurn();
  void loopGameOver();
  bool moveCursor();                                 // shared cursor movement, returns true if moved
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
  void renderRules();
  void drawTerrainTile(int px, int py, Tactics::Terrain t) const;
  void drawUnitTile(int px, int py, const Tactics::Unit& u) const;
  void drawDangerShade(int px, int py) const;
  void drawReachBorder(int px, int py, bool halo = false) const;
  void beginTargetSelect();  // collects targets, transitions mode
  void finishSelectedUnit();

  // Battle save/resume (versioned blob in /.crosspoint/tactics/)
  std::string currentLevelFile;  // set by loadLevel
  uint32_t levelCrc = 0;
  std::vector<bool> levelHasSave;  // parallel to levelFiles
  static void savFileName(const std::string& rel, char* out, size_t outLen);
  void savePath(char* out, size_t outLen) const;
  bool saveBattle();
  bool tryResumeBattle();  // true if a valid save was imported
  void deleteSave();
};
