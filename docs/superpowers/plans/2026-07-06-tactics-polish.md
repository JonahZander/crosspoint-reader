# Tactics Polish + Gameplay Update Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix reported Tactics UI issues and add approved gameplay features: richer info (HUD/inspect), auto-advance cursor, undo, danger zones, objective display, full refresh, minimap landmarks, battle save/resume, and balance tweaks.

**Architecture:** Game rules stay in hardware-free `lib/Tactics` (host-tested with gtest); all rendering/input in `src/activities/tactics/TacticsActivity`. Engine gains three small APIs (`undoMove`, `computeDangerZones`, state export/import); the activity gains two modes (`ActionSelect`, `Inspect`), a full-screen pause menu, and a versioned save file under `/.crosspoint/tactics/`.

**Tech Stack:** C++20 (no exceptions/RTTI), PlatformIO (ESP32-C3), gtest via CMake on host, custom i18n (`tr()`), HalStorage for SD.

**Spec:** `docs/superpowers/specs/2026-07-06-tactics-polish-design.md`

**Verify commands:**

```bash
# Host tests (from repo root; first configure fetches gtest)
cmake -S test -B build/test && cmake --build build/test -j && ctest --test-dir build/test --output-on-failure -j
# Firmware build (pio lives in the session venv; plain `pio` if installed)
pio run -e default
```

---

### Task 1: Engine — `Game::undoMove`

**Files:**
- Modify: `lib/Tactics/TacticsGame.h` (public API), `lib/Tactics/TacticsGame.cpp`
- Test: `test/tactics/TacticsTest.cpp`

- [ ] **Step 1: Write the failing tests** (append to `test/tactics/TacticsTest.cpp`; the `ARENA` fixture and `parse()` helper already exist at the top of the file)

```cpp
// ---------------------------------------------------------------- undo

TEST(Undo, RestoresPositionAndAllowsReMove) {
  Level level;
  ASSERT_TRUE(parse(ARENA, level));
  Game game;
  game.start(level);
  // Unit 0 = human_soldier at (1, 0).
  game.computeReach(0);
  ASSERT_TRUE(game.moveUnit(0, 3, 0));
  EXPECT_EQ(game.unit(0).x, 3);
  EXPECT_TRUE(game.undoMove(0, 1, 0));
  EXPECT_EQ(game.unit(0).x, 1);
  EXPECT_EQ(game.unit(0).y, 0);
  EXPECT_FALSE(game.unit(0).acted);
  // Unit can move again after undo.
  game.computeReach(0);
  EXPECT_TRUE(game.moveUnit(0, 2, 1));
}

TEST(Undo, RejectedAfterActing) {
  Level level;
  ASSERT_TRUE(parse(ARENA, level));
  Game game;
  game.start(level);
  game.computeReach(0);
  ASSERT_TRUE(game.moveUnit(0, 3, 0));
  game.finishUnit(0);
  EXPECT_FALSE(game.undoMove(0, 1, 0));
  EXPECT_EQ(game.unit(0).x, 3);
}

TEST(Undo, VisionRecedesButExploredStays) {
  Level level;
  ASSERT_TRUE(parse(ARENA, level));
  Game game;
  game.start(level);
  // Soldier vision 4 from (1,0): (7,0) is manhattan 6 — hidden at start.
  ASSERT_FALSE(game.isVisible(7, 0));
  game.computeReach(0);
  ASSERT_TRUE(game.moveUnit(0, 4, 0));  // now (7,0) is manhattan 3 — visible
  ASSERT_TRUE(game.isVisible(7, 0));
  ASSERT_TRUE(game.undoMove(0, 1, 0));
  EXPECT_FALSE(game.isVisible(7, 0));   // vision recedes
  EXPECT_TRUE(game.isExplored(7, 0));   // explored layer is permanent
}
```

- [ ] **Step 2: Run tests to verify they fail**

```bash
cmake -S test -B build/test && cmake --build build/test -j --target TacticsTest
```
Expected: compile error `'undoMove' is not a member of 'Tactics::Game'`.

- [ ] **Step 3: Implement.** In `TacticsGame.h`, after `bool moveUnit(int unitIdx, int x, int y);` add:

```cpp
  // Reverts a not-yet-committed move (before attack/wait). Restores the
  // pre-move position and recomputes fog; the explored layer is additive
  // and deliberately keeps anything revealed by the trial move.
  bool undoMove(int unitIdx, int x, int y);
```

In `TacticsGame.cpp`, after the `moveUnit` definition add:

```cpp
bool Game::undoMove(const int unitIdx, const int x, const int y) {
  Unit& u = units_[unitIdx];
  if (!u.alive || u.acted || status_ != Status::Playing) return false;
  if (x < 0 || y < 0 || x >= level_.width || y >= level_.height) return false;
  u.x = static_cast<int8_t>(x);
  u.y = static_cast<int8_t>(y);
  reachUnit_ = -1;  // invalidate the movement cache
  if (u.side == Side::Player) recomputeFog();
  return true;
}
```

- [ ] **Step 4: Run tests to verify they pass**

```bash
cmake --build build/test -j --target TacticsTest && ctest --test-dir build/test --output-on-failure -R Undo
```
Expected: 3 tests PASS (and the pre-existing suite still passes: run full `ctest --test-dir build/test -j`).

- [ ] **Step 5: Commit**

```bash
git add lib/Tactics/TacticsGame.h lib/Tactics/TacticsGame.cpp test/tactics/TacticsTest.cpp
git commit -m "feat(tactics): add Game::undoMove for pre-commit move undo"
```

---

### Task 2: Engine — danger zones

**Files:**
- Modify: `lib/Tactics/TacticsGame.h`, `lib/Tactics/TacticsGame.cpp`
- Test: `test/tactics/TacticsTest.cpp`

- [ ] **Step 1: Write the failing tests**

```cpp
// ---------------------------------------------------------------- danger

TEST(DangerZones, HiddenEnemyContributesNothing) {
  Level level;
  ASSERT_TRUE(parse(ARENA, level));
  Game game;
  game.start(level);
  // Goblin at (8,0) is outside soldier/archer vision at start.
  ASSERT_FALSE(game.isVisible(8, 0));
  game.computeDangerZones();
  for (int y = 0; y < level.height; y++)
    for (int x = 0; x < level.width; x++) EXPECT_FALSE(game.isDangerous(x, y));
}

TEST(DangerZones, VisibleAwakeEnemyMarksItsThreatRange) {
  Level level;
  ASSERT_TRUE(parse(ARENA, level));  // aggro 30: enemies wake immediately
  Game game;
  game.start(level);
  game.computeReach(0);
  ASSERT_TRUE(game.moveUnit(0, 4, 0));  // goblin at (8,0) now visible
  ASSERT_TRUE(game.isVisible(8, 0));
  // Wake it the way the engine does mid-game (attacked or aggro radius);
  // ARENA aggro radius is 30, so the AI would wake it — danger zones must
  // treat radius-woken enemies as awake too.
  game.computeDangerZones();
  // Goblin: move 5, range 1. (4,1) is well within move+range.
  EXPECT_TRUE(game.isDangerous(4, 1));
  // A tile far beyond move+range stays safe: (0,4).
  EXPECT_FALSE(game.isDangerous(0, 4));
}
```

- [ ] **Step 2: Run to verify failure** — compile error `'computeDangerZones' is not a member`.

- [ ] **Step 3: Implement.** In `TacticsGame.h` public section, after the fog accessors add:

```cpp
  // Danger overlay: tiles attackable next turn by enemies that are visible
  // to the player and awake (aggro'd or within the level's aggro radius).
  // Invalidates the computeReach() cache.
  void computeDangerZones();
  bool isDangerous(int x, int y) const;
```

In the private member block, after `uint8_t explored_[MAX_TILES / 8];` add:

```cpp
  uint8_t danger_[MAX_TILES / 8];
```

In `TacticsGame.cpp`: in `Game::start()`, next to the `explored_` memset add `std::memset(danger_, 0, sizeof(danger_));`. Then implement (after `isExplored`):

```cpp
void Game::computeDangerZones() {
  std::memset(danger_, 0, sizeof(danger_));
  for (int i = 0; i < unitCount_; i++) {
    const Unit& e = units_[i];
    if (!e.alive || e.side != Side::Enemy) continue;
    if (!isVisible(e.x, e.y)) continue;  // player can't know hidden threats
    // Awake = already aggro'd, or the aggro radius would wake it now.
    bool awake = e.aggro;
    if (!awake) {
      for (int p = 0; p < unitCount_ && !awake; p++) {
        const Unit& pl = units_[p];
        if (pl.alive && pl.side == Side::Player &&
            manhattan(e.x, e.y, pl.x, pl.y) <= level_.aggroRadius) {
          awake = true;
        }
      }
    }
    if (!awake) continue;
    computeReach(i);
    const int range = e.info().range;
    for (int y = 0; y < level_.height; y++) {
      for (int x = 0; x < level_.width; x++) {
        const bool standable =
            (x == e.x && y == e.y) || (reach_[tileIndex(x, y)] != UNREACHABLE && isReachable(x, y));
        if (!standable) continue;
        for (int dy = -range; dy <= range; dy++) {
          for (int dx = -range + (dy < 0 ? -dy : dy); ; dx++) {
            const int adx = dx < 0 ? -dx : dx;
            const int ady = dy < 0 ? -dy : dy;
            if (adx + ady > range) break;
            const int tx = x + dx;
            const int ty = y + dy;
            if (adx + ady >= 1 && tx >= 0 && ty >= 0 && tx < level_.width && ty < level_.height) {
              const int idx = tileIndex(tx, ty);
              danger_[idx / 8] = static_cast<uint8_t>(danger_[idx / 8] | (1 << (idx % 8)));
            }
          }
        }
      }
    }
  }
  reachUnit_ = -1;  // reach cache now belongs to the last enemy scanned
}

bool Game::isDangerous(const int x, const int y) const {
  if (x < 0 || y < 0 || x >= level_.width || y >= level_.height) return false;
  const int idx = tileIndex(x, y);
  return (danger_[idx / 8] >> (idx % 8)) & 1;
}
```

Note the diamond loop: for each `dy`, `dx` starts at `-(range-|dy|)` and the `break` guard exits past `+(range-|dy|)` — it enumerates manhattan-distance ≤ range tiles only. If that reads too clever during implementation, a plain `for dx in [-range..range]` with an `adx+ady<=range` filter is equally correct — prefer the plain version if the tests disagree with the fancy one.

- [ ] **Step 4: Run tests** — `ctest --test-dir build/test --output-on-failure -R DangerZones` → 2 PASS; full suite green.

- [ ] **Step 5: Commit**

```bash
git add lib/Tactics/TacticsGame.h lib/Tactics/TacticsGame.cpp test/tactics/TacticsTest.cpp
git commit -m "feat(tactics): engine danger-zone computation for threat overlay"
```

---

### Task 3: Engine — state export/import

**Files:**
- Modify: `lib/Tactics/TacticsGame.h`, `lib/Tactics/TacticsGame.cpp`
- Test: `test/tactics/TacticsTest.cpp`

- [ ] **Step 1: Write the failing test**

```cpp
// ---------------------------------------------------------------- state

TEST(SaveState, RoundTripsMidBattle) {
  Level level;
  ASSERT_TRUE(parse(ARENA, level));
  Game game;
  game.start(level);
  game.computeReach(0);
  ASSERT_TRUE(game.moveUnit(0, 3, 0));
  game.finishUnit(0);

  static uint8_t blob[sizeof(Game)];
  ASSERT_EQ(Game::stateSize(), sizeof(Game));
  game.exportState(blob);

  Game restored;
  ASSERT_TRUE(restored.importState(blob));
  EXPECT_EQ(restored.unit(0).x, 3);
  EXPECT_TRUE(restored.unit(0).acted);
  EXPECT_EQ(restored.turn(), 1);
  EXPECT_EQ(restored.status(), Game::Status::Playing);
}

TEST(SaveState, RejectsGarbage) {
  static uint8_t blob[sizeof(Game)];
  std::memset(blob, 0xAB, sizeof(blob));
  Game g;
  EXPECT_FALSE(g.importState(blob));
}
```

- [ ] **Step 2: Run to verify failure** — compile error `'stateSize' is not a member`.

- [ ] **Step 3: Implement.** In `TacticsGame.h` public section add:

```cpp
  // Whole-state snapshot for battle save/resume. The Game object is
  // trivially copyable (no pointers, no virtuals), so a raw byte copy is
  // the entire serialisation; the caller owns file format/versioning.
  static constexpr size_t stateSize() { return sizeof(Game); }
  void exportState(void* out) const;
  bool importState(const void* in);
```

Add `#include <cstddef>` to the header's includes. In `TacticsGame.cpp` add:

```cpp
void Game::exportState(void* out) const {
  static_assert(std::is_trivially_copyable<Game>::value, "raw snapshot requires trivially copyable Game");
  std::memcpy(out, this, sizeof(Game));
}

bool Game::importState(const void* in) {
  Game tmp;
  std::memcpy(&tmp, in, sizeof(Game));
  // Sanity-check the untrusted blob before adopting it.
  if (tmp.unitCount_ < 1 || tmp.unitCount_ > MAX_UNITS) return false;
  if (tmp.level_.width < 1 || tmp.level_.width > MAX_MAP_W) return false;
  if (tmp.level_.height < 1 || tmp.level_.height > MAX_MAP_H) return false;
  if (tmp.status_ != Status::Playing) return false;  // only mid-battle saves exist
  std::memcpy(this, &tmp, sizeof(Game));
  return true;
}
```

Add `#include <type_traits>` to `TacticsGame.cpp`.

Note: `tmp` is ~6.3KB on the host test stack (fine there). On device the caller allocates the file blob on the heap; `importState`'s local `tmp` would also land on an ESP32 task stack — too big. To keep the engine allocation-free AND stack-safe, validate in place instead:

```cpp
bool Game::importState(const void* in) {
  const Game* src = static_cast<const Game*>(in);
  if (src->unitCount_ < 1 || src->unitCount_ > MAX_UNITS) return false;
  if (src->level_.width < 1 || src->level_.width > MAX_MAP_W) return false;
  if (src->level_.height < 1 || src->level_.height > MAX_MAP_H) return false;
  if (src->status_ != Status::Playing) return false;
  std::memcpy(this, src, sizeof(Game));
  return true;
}
```

Use this second version (the `static_cast` read of a byte buffer is safe here: the blob was produced by `memcpy` from a real `Game`, and all fields read pre-copy are byte-sized or bounds-checked ints — but to be strictly alignment-safe on RISC-V, the activity must pass a 4-byte-aligned buffer; `makeUniqueNoThrow<uint8_t[]>` returns `new`-aligned memory, which is).

- [ ] **Step 4: Run tests** — `ctest -R SaveState` → 2 PASS; full suite green.

- [ ] **Step 5: Commit**

```bash
git add lib/Tactics/TacticsGame.h lib/Tactics/TacticsGame.cpp test/tactics/TacticsTest.cpp
git commit -m "feat(tactics): trivially-copyable state export/import for save games"
```

---

### Task 4: Balance — Knight MV 5, new goblin_slinger

**Files:**
- Modify: `lib/Tactics/TacticsTypes.h`, `lib/Tactics/TacticsSprites.h`
- Test: `test/tactics/TacticsTest.cpp`

- [ ] **Step 1: Check for existing assertions on the old values**

```bash
grep -n "human_knight\|move, 7\|goblin_slinger" test/tactics/TacticsTest.cpp
```
Update any test asserting Knight `move == 7` to `5` (as of writing there are none).

- [ ] **Step 2: Write the failing tests**

```cpp
// ---------------------------------------------------------------- balance

TEST(Balance, KnightIsSlowedToFive) {
  const int idx = unitTypeFromId("human_knight");
  ASSERT_GE(idx, 0);
  EXPECT_EQ(UNIT_TYPES[idx].move, 5);
}

TEST(Balance, GoblinSlingerExists) {
  const int idx = unitTypeFromId("goblin_slinger");
  ASSERT_GE(idx, 0);
  const UnitType& t = UNIT_TYPES[idx];
  EXPECT_EQ(t.maxHp, 16);
  EXPECT_EQ(t.move, 5);
  EXPECT_EQ(t.attack, 6);
  EXPECT_EQ(t.range, 2);
  EXPECT_EQ(t.defense, 0);
  EXPECT_EQ(t.race, Race::Monster);
  EXPECT_FALSE(t.flies);
}
```

- [ ] **Step 3: Run to verify failure** — Knight test fails with `7 != 5`; slinger test fails with `idx >= 0`.

- [ ] **Step 4: Implement.** In `lib/Tactics/TacticsTypes.h`:
- Knight row: change `38, 7, 10, 1, 15` → `38, 5, 10, 1, 15`.
- Append to `UNIT_TYPES` after the wyvern row (appending keeps existing indices stable):

```cpp
    {"goblin_slinger", {'g', 's', 0}, Race::Monster, 16, 5, 6, 2, 0, 4, 0, false},
```

In `lib/Tactics/TacticsSprites.h`, append a matching 17th sprite after the wyvern entry (goblin silhouette with a raised sling):

```cpp
    {// goblin_slinger
     "................",
     "..........#.....",
     ".........#.#....",
     "....##...#.#....",
     "...####...#.....",
     "...#.##..#......",
     "...####.#.......",
     "....##.#........",
     "..######........",
     ".#.####.........",
     ".#.####.........",
     "...####.........",
     "....##..........",
     "...#..#.........",
     "..##..##........",
     "................"},
```

- [ ] **Step 5: Run tests** — `ctest -R Balance` → 2 PASS; **full suite** must stay green (AI/movement tests may exercise the knight — if any fail on movement distances, update their expectations to MV 5 and note it in the commit).

- [ ] **Step 6: Commit**

```bash
git add lib/Tactics/TacticsTypes.h lib/Tactics/TacticsSprites.h test/tactics/TacticsTest.cpp
git commit -m "feat(tactics): balance - knight MV 7->5, add ranged goblin_slinger"
```

---

### Task 5: i18n strings

**Files:**
- Modify: `lib/I18n/translations/english.yaml` (only; other languages fall back)
- Generated (not committed): `lib/I18n/I18nKeys.h`, `I18nStrings.h`, `I18nStrings.cpp`

- [ ] **Step 1: Add keys.** In `english.yaml`, directly after `STR_UNIT_WYVERN: "Wyvern"` (line ~416) add:

```yaml
STR_UNIT_GOBLIN_SLINGER: "Slinger"
STR_TERRAIN_VOID: "Void"
STR_TERRAIN_PLAIN: "Plain"
STR_TERRAIN_FOREST: "Forest"
STR_TERRAIN_MOUNTAIN: "Mountain"
STR_TERRAIN_WATER: "Water"
STR_TERRAIN_ROCK: "Rock"
STR_TERRAIN_CASTLE: "Castle"
STR_TERRAIN_ENEMY_BASE: "Enemy Base"
STR_TACTICS_PAUSED: "Paused"
STR_TACTICS_NEXT_UNIT: "Next unit"
STR_TACTICS_INSPECT: "Inspect"
STR_TACTICS_DANGER_ON: "Danger zones: On"
STR_TACTICS_DANGER_OFF: "Danger zones: Off"
STR_TACTICS_REFRESH: "Refresh screen"
STR_TACTICS_ATTACK: "Attack"
STR_TACTICS_WAIT: "Wait"
STR_TACTICS_UNDO: "Undo"
STR_TACTICS_OBJ_DEFEAT_ALL: "Defeat all enemies"
STR_TACTICS_OBJ_CAPTURE: "Capture the enemy base"
STR_TACTICS_OBJ_SURVIVE: "Survive %d turns"
STR_TACTICS_TURN_OF: "Turn %d/%d"
STR_TACTICS_STATLINE: "A%d R%d D%d%% M%d"
STR_TACTICS_TERRAIN_DEF: "DEF+%d%%"
STR_TACTICS_TERRAIN_MOVE: "MOV %d"
STR_TACTICS_TERRAIN_HEAL: "heals %d"
STR_TACTICS_RACIAL_BONUS: "+15 racial"
STR_TACTICS_DEF_BREAKDOWN: "Defense %d%% = %d base + %d terrain + %d racial"
STR_TACTICS_DMG_RULE: "Damage = ATK x (100 - DEF)%%, min 1"
STR_TACTICS_FLIES: "Flies"
STR_TACTICS_REGEN: "Regen %d"
STR_TACTICS_HP: "HP %d/%d"
STR_TACTICS_VISION: "Vision %d"
```

- [ ] **Step 2: Regenerate**

```bash
python scripts/gen_i18n.py lib/I18n/translations lib/I18n/
```
Expected: exits 0; `lib/I18n/I18nKeys.h` now contains `STR_TACTICS_PAUSED` etc. (generated files are gitignored — do NOT stage them).

- [ ] **Step 3: Commit**

```bash
git add lib/I18n/translations/english.yaml
git commit -m "feat(tactics): i18n strings for terrain, inspect, pause menu, slinger"
```

---

### Task 6: Activity — name tables, header fix, centered move dot

**Files:**
- Modify: `src/activities/tactics/TacticsActivity.cpp`

- [ ] **Step 1: Extend `UNIT_NAME_IDS`** (it is sized `[Tactics::UNIT_TYPE_COUNT]`, so Task 4 made it a compile error — this un-breaks the firmware build). Append to the initializer:

```cpp
    StrId::STR_UNIT_GOBLIN_SLINGER,
```

Below it, add the terrain table:

```cpp
// Display names for terrain, indexed like Tactics::TERRAIN_INFO.
constexpr StrId TERRAIN_NAME_IDS[Tactics::TERRAIN_COUNT] = {
    StrId::STR_TERRAIN_VOID,   StrId::STR_TERRAIN_PLAIN, StrId::STR_TERRAIN_FOREST,
    StrId::STR_TERRAIN_MOUNTAIN, StrId::STR_TERRAIN_WATER, StrId::STR_TERRAIN_ROCK,
    StrId::STR_TERRAIN_CASTLE, StrId::STR_TERRAIN_ENEMY_BASE,
};
```

- [ ] **Step 2: Header fix.** In `renderLevelSelect()` replace:

```cpp
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, 60}, tr(STR_TACTICS));
  const int top = metrics.topPadding + 70;
```
with:
```cpp
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_TACTICS));
  const int top = metrics.topPadding + metrics.headerHeight + 10;
```

- [ ] **Step 3: Center the move indicator.** In `renderBoard()` replace:

```cpp
      if (mode == Mode::MoveSelect && game->isReachable(mx, my)) {
        renderer.fillRect(px + 2, py + 2, 7, 7, true);
      }
```
with:
```cpp
      if (mode == Mode::MoveSelect && game->isReachable(mx, my)) {
        renderer.fillRect(px + (TILE - 7) / 2, py + (TILE - 7) / 2, 7, 7, true);
      }
```

- [ ] **Step 4: Build** — `pio run -e default` → SUCCESS.

- [ ] **Step 5: Commit**

```bash
git add src/activities/tactics/TacticsActivity.cpp
git commit -m "fix(tactics): header height per theme metrics, centered move indicator"
```

---

### Task 7: Activity — HUD unit + terrain info lines

**Files:**
- Modify: `src/activities/tactics/TacticsActivity.cpp` (`renderHud()`)

- [ ] **Step 1: Implement.** In `renderHud()`, replace the final `else` branch (the "unit under cursor" block, currently `Name hp/max` at `top + 30`) with two right-aligned lines — line 1 unit (skip if none), line 2 terrain (always, when tile explored):

```cpp
  } else {
    // Line 1: unit under cursor — name, HP, compact stat line.
    const int ui = game->unitIndexAt(cursorX, cursorY);
    if (ui >= 0 && game->isVisible(cursorX, cursorY)) {
      const Unit& u = game->unit(ui);
      char stats[48];
      std::snprintf(stats, sizeof(stats), tr(STR_TACTICS_STATLINE), u.info().attack, u.info().range,
                    game->defenseAt(u, u.x, u.y), u.info().move);
      std::snprintf(line, sizeof(line), "%s %d/%d  %s", I18N.get(UNIT_NAME_IDS[u.type]), u.hp, u.info().maxHp,
                    stats);
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
        if (!u.info().flies &&
            ((t == Terrain::Forest && u.info().race == Tactics::Race::Elf) ||
             (t == Terrain::Mountain && u.info().race == Tactics::Race::Dwarf))) {
          racial = tr(STR_TACTICS_RACIAL_BONUS);
        }
      }
      std::snprintf(line, sizeof(line), "%s  %s %s %s %s", I18N.get(TERRAIN_NAME_IDS[static_cast<int>(t)]),
                    defBuf, movBuf, healBuf, racial);
      const int tw = renderer.getTextWidth(SMALL_FONT_ID, line);
      renderer.drawText(SMALL_FONT_ID, pageWidth - tw - 16, top + 50, line, true);
    }
  }
```

Also move the TargetSelect forecast line (the `if` branch above it) from `top + 30` to `top + 10` so both layouts share the same baseline.

Add `using Tactics::Race;` is NOT needed (qualified use above). `game->defenseAt` is public already.

- [ ] **Step 2: Build** — `pio run -e default` → SUCCESS.

- [ ] **Step 3: Commit**

```bash
git add src/activities/tactics/TacticsActivity.cpp
git commit -m "feat(tactics): HUD stat line and terrain info under cursor"
```

---

### Task 8: Activity — minimap landmarks

**Files:**
- Modify: `src/activities/tactics/TacticsActivity.cpp` (`renderHud()` minimap block)

- [ ] **Step 1: Implement.** Inside the existing minimap block (after the viewport rect is drawn), add:

```cpp
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
```

- [ ] **Step 2: Build** — `pio run -e default` → SUCCESS.

- [ ] **Step 3: Commit**

```bash
git add src/activities/tactics/TacticsActivity.cpp
git commit -m "feat(tactics): explored landmarks (castle/base) on the minimap"
```

---

### Task 9: Activity — full-refresh support

**Files:**
- Modify: `src/activities/tactics/TacticsActivity.h`, `src/activities/tactics/TacticsActivity.cpp`

- [ ] **Step 1: State.** In the header's private section add:

```cpp
  bool nextRefreshFull = false;
  uint8_t turnsSinceFullRefresh = 0;
```

- [ ] **Step 2: Consume in `render()`.** Replace `renderer.displayBuffer();` with:

```cpp
  renderer.displayBuffer(nextRefreshFull ? HalDisplay::FULL_REFRESH : HalDisplay::FAST_REFRESH);
  nextRefreshFull = false;
```

`HalDisplay` is already reachable via the GfxRenderer include chain (`GfxRenderer.h` uses `HalDisplay::RefreshMode` in its API).

- [ ] **Step 3: Auto-refresh every 5 turn rollovers.** In `loopEnemyTurn()`, in the `!act.acted` branch where `mode = Mode::Cursor` is set (turn rollover), add:

```cpp
        if (++turnsSinceFullRefresh >= 5) {
          turnsSinceFullRefresh = 0;
          nextRefreshFull = true;
        }
```

Also set `nextRefreshFull = true;` at the end of a successful `loadLevel()` (fresh board deserves a clean panel) and reset `turnsSinceFullRefresh = 0;` there.

- [ ] **Step 4: Build** — `pio run -e default` → SUCCESS.

- [ ] **Step 5: Commit**

```bash
git add src/activities/tactics/TacticsActivity.h src/activities/tactics/TacticsActivity.cpp
git commit -m "feat(tactics): full e-ink refresh on load and every 5th turn"
```

---

### Task 10: Activity — ActionSelect mode with undo

**Files:**
- Modify: `src/activities/tactics/TacticsActivity.h`, `src/activities/tactics/TacticsActivity.cpp`

- [ ] **Step 1: Header changes.** Add `ActionSelect` to the `Mode` enum between `MoveSelect` and `TargetSelect`. Add members + method decls:

```cpp
  int actionSel = 0;
  int8_t undoX = 0, undoY = 0;  // selected unit's pre-move position
  void loopActionSelect();
  void renderActionSelect();  // draws the option strip into the HUD
```

- [ ] **Step 2: Capture the undo position.** In `loopMoveSelect()`, just before `game->moveUnit(selectedUnit, cursorX, cursorY);`:

```cpp
      undoX = game->unit(selectedUnit).x;
      undoY = game->unit(selectedUnit).y;
```

- [ ] **Step 3: Route move → ActionSelect.** Replace the `beginTargetSelect();` call in `loopMoveSelect()` with:

```cpp
      targetCount = game->targetsFrom(selectedUnit, cursorX, cursorY, targets, 8);
      actionSel = 0;
      mode = Mode::ActionSelect;
```

(`beginTargetSelect()` stays — it is still used when the player picks "Attack".) Change `beginTargetSelect()` itself to not auto-finish; new body:

```cpp
void TacticsActivity::beginTargetSelect() {
  const Unit& u = game->unit(selectedUnit);
  targetCount = game->targetsFrom(selectedUnit, u.x, u.y, targets, 8);
  if (targetCount == 0) return;  // shouldn't happen: Attack is hidden without targets
  targetSel = 0;
  mode = Mode::TargetSelect;
}
```

- [ ] **Step 4: The mode loop.** Add the case to `loop()`'s switch (`case Mode::ActionSelect: loopActionSelect(); break;`) and implement:

```cpp
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
    game->undoMove(selectedUnit, undoX, undoY);
    cursorX = undoX;
    cursorY = undoY;
    game->computeReach(selectedUnit);
    ensureCursorVisible();
    mode = Mode::MoveSelect;
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
        game->undoMove(selectedUnit, undoX, undoY);
        cursorX = undoX;
        cursorY = undoY;
        game->computeReach(selectedUnit);
        ensureCursorVisible();
        mode = Mode::MoveSelect;
        break;
      default:
        break;
    }
    requestUpdate();
  }
}
```

- [ ] **Step 5: Back from TargetSelect returns to ActionSelect** (instead of silently finishing). In `loopTargetSelect()` replace the Back branch body:

```cpp
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    actionSel = 0;
    mode = Mode::ActionSelect;
    requestUpdate();
    return;
  }
```

- [ ] **Step 6: Render the strip.** Add to `renderHud()` right after the PauseMenu block:

```cpp
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
```

Also extend the board cursor-highlight condition so the selected unit stays highlighted: add `|| mode == Mode::ActionSelect` to the `if (mode == Mode::Cursor || ...)` check in `renderBoard()` (cursor sits on the moved unit's tile).

- [ ] **Step 7: Build** — `pio run -e default` → SUCCESS. (`renderActionSelect` declared in Step 1 ended up inlined into `renderHud`; remove the stray declaration from the header.)

- [ ] **Step 8: Commit**

```bash
git add src/activities/tactics/TacticsActivity.h src/activities/tactics/TacticsActivity.cpp
git commit -m "feat(tactics): post-move action menu with attack/wait/undo"
```

---

### Task 11: Activity — cursor auto-advance + next-unit cycling

**Files:**
- Modify: `src/activities/tactics/TacticsActivity.h`, `src/activities/tactics/TacticsActivity.cpp`

- [ ] **Step 1: Helpers.** Declare in the header:

```cpp
  int nearestReadyUnit(int fromX, int fromY) const;  // -1 if none
  void jumpToNextReadyUnit();                        // cycles from cursor
```

Implement in the .cpp (near the camera helpers):

```cpp
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
```

- [ ] **Step 2: Auto-advance after a unit finishes.** In `finishSelectedUnit()`, in the `else` branch (`mode = Mode::Cursor;`), add before it:

```cpp
    const int next = nearestReadyUnit(cursorX, cursorY);
    if (next >= 0) {
      cursorX = game->unit(next).x;
      cursorY = game->unit(next).y;
      ensureCursorVisible();
    }
```

Apply the same 5 lines in `loopTargetSelect()`'s Confirm branch, in its `else { mode = Mode::Cursor; }` arm (after an attack completes).

- [ ] **Step 3: Turn start lands on a ready unit.** In `loopEnemyTurn()`, the rollover branch currently picks the *first alive player unit*; replace that `for` loop with:

```cpp
        const int next = nearestReadyUnit(cursorX, cursorY);
        if (next >= 0) {
          cursorX = game->unit(next).x;
          cursorY = game->unit(next).y;
        }
```

- [ ] **Step 4: Build** — `pio run -e default` → SUCCESS.

- [ ] **Step 5: Commit**

```bash
git add src/activities/tactics/TacticsActivity.h src/activities/tactics/TacticsActivity.cpp
git commit -m "feat(tactics): cursor auto-advance to next ready unit"
```

---

### Task 12: Activity — Inspect panel

**Files:**
- Modify: `src/activities/tactics/TacticsActivity.h`, `src/activities/tactics/TacticsActivity.cpp`

- [ ] **Step 1: Mode + members.** Add `Inspect` to the `Mode` enum (after `PauseMenu`). Header decls:

```cpp
  int inspectUnit = -1;  // -1 = terrain-only inspect
  void loopInspect();
  void renderInspect();
```

- [ ] **Step 2: Loop.** Add the switch case (`case Mode::Inspect: loopInspect(); break;`) and:

```cpp
void TacticsActivity::loopInspect() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    mode = Mode::Cursor;
    nextRefreshFull = true;  // wipe the full-screen overlay cleanly
    requestUpdate();
  }
}
```

- [ ] **Step 3: Render.** In `render()`, branch before the board: `if (mode == Mode::Inspect) { renderInspect(); }` (instead of board+HUD). Implementation:

```cpp
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
    for (int py2 = 0; py2 < Tactics::SPRITE_SIZE; py2++) {
      for (int px2 = 0; px2 < Tactics::SPRITE_SIZE; px2++) {
        if (Tactics::spritePixel(u.type, px2, py2)) {
          renderer.fillRect(sx + px2 * 3, y + py2 * 3, 3, 3, true);
        }
      }
    }
    renderer.drawText(UI_12_FONT_ID, sx + Tactics::SPRITE_SIZE * 3 + 16, y + 8,
                      I18N.get(UNIT_NAME_IDS[u.type]), true, EpdFontFamily::BOLD);
    std::snprintf(line, sizeof(line), tr(STR_TACTICS_HP), u.hp, info.maxHp);
    renderer.drawText(UI_12_FONT_ID, sx + Tactics::SPRITE_SIZE * 3 + 16, y + 8 + lineH, line, true);
    y += Tactics::SPRITE_SIZE * 3 + 24;

    std::snprintf(line, sizeof(line), tr(STR_TACTICS_STATLINE), info.attack, info.range, (int)info.defense,
                  info.move);
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
    std::snprintf(line, sizeof(line), tr(STR_TACTICS_DEF_BREAKDOWN), game->defenseAt(u, u.x, u.y),
                  (int)info.defense, terrainDef, racialDef);
    renderer.drawText(UI_12_FONT_ID, 24, y, line, true);
    y += lineH;
  }

  // Terrain block (always shown).
  y += 8;
  renderer.drawText(UI_12_FONT_ID, 24, y, I18N.get(TERRAIN_NAME_IDS[static_cast<int>(t)]), true,
                    EpdFontFamily::BOLD);
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
```

Entry happens from the pause menu (Task 14): sets `inspectUnit = game->unitIndexAt(cursorX, cursorY)` (only if visible, else -1), `mode = Mode::Inspect`, `nextRefreshFull = true`.

- [ ] **Step 4: Build** — `pio run -e default` → SUCCESS (the mode is unreachable until Task 14 wires it; that's fine).

- [ ] **Step 5: Commit**

```bash
git add src/activities/tactics/TacticsActivity.h src/activities/tactics/TacticsActivity.cpp
git commit -m "feat(tactics): full-screen inspect panel for unit and terrain"
```

---

### Task 13: Activity — danger overlay rendering + recompute triggers

**Files:**
- Modify: `src/activities/tactics/TacticsActivity.h`, `src/activities/tactics/TacticsActivity.cpp`

- [ ] **Step 1: State.** Header: `bool dangerOverlay = false;`

- [ ] **Step 2: Render.** In `renderBoard()`'s per-tile loop, after the movement-overlay block add:

```cpp
      // Danger overlay: dithered strip along the tile bottom.
      if (dangerOverlay && game->isDangerous(mx, my)) {
        renderer.fillRectDither(px + 2, py + TILE - 8, TILE - 4, 6, Color::DarkGray);
      }
```

- [ ] **Step 3: Recompute triggers.** The mask must be fresh whenever shown:
- In `finishSelectedUnit()` start: `if (dangerOverlay) game->computeDangerZones();`
- In `loopTargetSelect()` Confirm branch after `game->attack(...)`: same call.
- In `loopEnemyTurn()` rollover branch (where `mode = Mode::Cursor`): same call.
- After undo in `loopActionSelect()` (both Back and menu paths), after `game->undoMove(...)`: same call — undo changes fog, which changes which enemies are visible. NOTE: `computeDangerZones()` invalidates the reach cache; both undo paths already call `game->computeReach(selectedUnit)` — make sure `computeDangerZones()` runs BEFORE `computeReach(selectedUnit)` so the movement overlay survives.
- When the toggle turns on (Task 14 pause menu): `game->computeDangerZones();`

- [ ] **Step 4: Build** — `pio run -e default` → SUCCESS.

- [ ] **Step 5: Commit**

```bash
git add src/activities/tactics/TacticsActivity.h src/activities/tactics/TacticsActivity.cpp
git commit -m "feat(tactics): danger-zone overlay rendering and refresh triggers"
```

---

### Task 14: Activity — full-screen pause menu with objective

**Files:**
- Modify: `src/activities/tactics/TacticsActivity.h`, `src/activities/tactics/TacticsActivity.cpp`

- [ ] **Step 1: Render.** Add `void renderPauseMenu();` to the header. In `render()`, branch: `if (mode == Mode::PauseMenu) { renderPauseMenu(); }` (full-screen, replaces board+HUD). Remove the old PauseMenu block from `renderHud()`. Implementation:

```cpp
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
      tr(STR_TACTICS_RESUME),
      tr(STR_TACTICS_NEXT_UNIT),
      tr(STR_TACTICS_INSPECT),
      dangerOverlay ? tr(STR_TACTICS_DANGER_ON) : tr(STR_TACTICS_DANGER_OFF),
      tr(STR_TACTICS_REFRESH),
      tr(STR_TACTICS_END_TURN),
      tr(STR_TACTICS_RESTART),
      tr(STR_TACTICS_QUIT),
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
```

- [ ] **Step 2: Loop.** Replace `loopPauseMenu()`:

```cpp
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
        mode = Mode::LevelSelect;
        break;
      default:
        break;
    }
    requestUpdate();
  }
}
```

`deleteSave()` arrives in Task 15 — to keep this task buildable, add a stub now (header decl + empty body `void TacticsActivity::deleteSave() {}` with a `// filled in by save/resume` comment).

- [ ] **Step 3: Build** — `pio run -e default` → SUCCESS.

- [ ] **Step 4: Commit**

```bash
git add src/activities/tactics/TacticsActivity.h src/activities/tactics/TacticsActivity.cpp
git commit -m "feat(tactics): full-screen pause menu with objective and new actions"
```

---

### Task 15: Activity — battle save/resume

**Files:**
- Modify: `src/activities/tactics/TacticsActivity.h`, `src/activities/tactics/TacticsActivity.cpp`

- [ ] **Step 1: Header.** Members + decls:

```cpp
  // Save/resume
  std::string currentLevelFile;  // set by loadLevel
  uint32_t levelCrc = 0;
  void savePath(char* out, size_t outLen) const;
  bool saveBattle();
  bool tryResumeBattle();  // true if a valid save was imported
  void deleteSave();       // stubbed in Task 14
```

- [ ] **Step 2: Implement** (file-scope helper + methods; `SAVE_MAGIC` next to the other constants in the anonymous namespace):

```cpp
constexpr uint32_t SAVE_MAGIC = 0x54534156;  // "TSAV"
constexpr uint8_t SAVE_VERSION = 1;

uint32_t fnv1a(const char* data, const size_t len) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < len; i++) {
    h ^= static_cast<uint8_t>(data[i]);
    h *= 16777619u;
  }
  return h;
}
```

```cpp
void TacticsActivity::savePath(char* out, const size_t outLen) const {
  std::snprintf(out, outLen, "/.crosspoint/tactics/%s.sav", currentLevelFile.c_str());
}

bool TacticsActivity::saveBattle() {
  if (!game || game->status() != Game::Status::Playing) return false;
  auto blob = makeUniqueNoThrow<uint8_t[]>(Game::stateSize());
  if (!blob) {
    LOG_ERR("TACTICS", "OOM: %u bytes for save", (unsigned)Game::stateSize());
    return false;
  }
  game->exportState(blob.get());
  Storage.mkdir("/.crosspoint/tactics");
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
  ok = ok && file.write(blob.get(), size) == static_cast<int>(size);
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

void TacticsActivity::deleteSave() {
  char path[128];
  savePath(path, sizeof(path));
  if (Storage.exists(path)) Storage.remove(path);
}
```

(Replace the Task 14 stub body of `deleteSave()` with this real one.)

- [ ] **Step 3: Wire into `loadLevel`.** After the parse succeeds and before `game->start(*level);`:

```cpp
  currentLevelFile = fileName;
  levelCrc = fnv1a(buffer.get(), size);
```

Then replace `game->start(*level);` with:

```cpp
  game->start(*level);
  const bool resumed = tryResumeBattle();
  if (resumed) LOG_INF("TACTICS", "Resumed battle for %s", fileName.c_str());
```

(`start()` first, unconditionally: a rejected save must leave a playable fresh board.)

- [ ] **Step 4: Save triggers.**
- `onExit()`: at the top, before `game.reset()`: `if (mode != Mode::LevelSelect && mode != Mode::GameOver) saveBattle();`
- End of each enemy phase — in `loopEnemyTurn()` rollover branch (after `mode = Mode::Cursor;` etc.): `saveBattle();`
- On game over: in the two places `mode = Mode::GameOver;` is set in `loopMoveSelect`/`loopTargetSelect` and the two in `loopEnemyTurn`: add `deleteSave();` after each.

- [ ] **Step 5: Level-select marker.** In the header add `std::vector<bool> levelHasSave;`. In `scanLevels()` after the sort:

```cpp
  levelHasSave.assign(levelFiles.size(), false);
  for (size_t i = 0; i < levelFiles.size(); i++) {
    char path[128];
    std::snprintf(path, sizeof(path), "/.crosspoint/tactics/%s.sav", levelFiles[i].c_str());
    levelHasSave[i] = Storage.exists(path);
  }
```

In `renderLevelSelect()`'s list loop, prefix saved levels:

```cpp
      char nameBuf[80];
      std::snprintf(nameBuf, sizeof(nameBuf), "%s%s",
                    (first + i < static_cast<int>(levelHasSave.size()) && levelHasSave[first + i]) ? "> " : "",
                    levelFiles[first + i].c_str());
      renderer.drawText(UI_12_FONT_ID, 20, y, nameBuf, !sel);
```

- [ ] **Step 6: Build** — `pio run -e default` → SUCCESS.

- [ ] **Step 7: Commit**

```bash
git add src/activities/tactics/TacticsActivity.h src/activities/tactics/TacticsActivity.cpp
git commit -m "feat(tactics): battle save/resume via /.crosspoint/tactics saves"
```

---

### Task 16: Docs + level authorship notes

**Files:**
- Modify: `docs/tactics-levels.md`

- [ ] **Step 1:** Add `goblin_slinger` to the unit roster table (HP 16, MV 5, ATK 6, RNG 2, DEF 0 — "the monsters' only ranged unit; use it to punish archer kiting"). Add an authoring note to the troll entry: "regen 4 makes trolls a 20+ turn grind for low-ATK squads — on `defeat_all` maps pair them with a reachable objective or high-ATK player units." Mention the Knight's new MV 5.

- [ ] **Step 2: Commit**

```bash
git add docs/tactics-levels.md
git commit -m "docs(tactics): slinger unit, troll authoring note, knight MV change"
```

---

### Task 17: Final verification

- [ ] **Step 1: Full host suite**

```bash
cmake -S test -B build/test && cmake --build build/test -j && ctest --test-dir build/test --output-on-failure -j
```
Expected: all tests pass (115+: 108 pre-existing + the new ones).

- [ ] **Step 2: Firmware build** — `pio run -e default` → SUCCESS; note RAM/Flash deltas vs the pre-change build (RAM 31.0%, Flash 80.3%).

- [ ] **Step 3: Format**

```bash
clang-format -i src/activities/tactics/TacticsActivity.cpp src/activities/tactics/TacticsActivity.h lib/Tactics/*.cpp lib/Tactics/*.h test/tactics/TacticsTest.cpp
git diff --stat  # if formatting changed anything, commit it
```

- [ ] **Step 4: Commit any format fixes + push**

```bash
git push origin claude/code-complexity-explanation-kgbzwh
```

- [ ] **Step 5: Flag for human device testing** — pause menu + inspect in all 4 orientations, ghosting after 10+ turns, save/resume across power cycle, danger overlay accuracy vs actual AI attacks.
