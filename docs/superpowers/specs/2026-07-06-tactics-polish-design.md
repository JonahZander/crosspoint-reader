# Tactics Polish + Gameplay Update — Design Spec

Date: 2026-07-06
Status: Approved by user
Branch: `claude/code-complexity-explanation-kgbzwh`

## Context

User playtested the Tactics mini-game on the Xteink X4 and reported UI issues plus
requests for richer information and quality-of-life features. A game-dev review of
`lib/Tactics` and `src/activities/tactics/` identified balance findings and three
high-value gameplay additions. All phases (A: fixes, B: gameplay QoL, C:
save/resume + balance) were approved.

## §1 UI fixes

- **Header**: `renderLevelSelect()` must pass `metrics.headerHeight` (not hardcoded
  60) to `GUI.drawHeader`; Lyra theme expects 84px and draws its divider at the rect
  bottom, so 60px puts the title on the line. Level list top derives from
  `topPadding + headerHeight + 10`.
- **Move indicator**: center the 7×7 reachable-tile dot: `px + (TILE-7)/2` both axes
  (`renderBoard()`; reachable tiles are never occupied).
- **HUD right side** becomes two stacked SMALL_FONT lines:
  - Line 1 — unit under cursor: `<Name> <hp>/<maxHp>  A<atk> R<rng> D<def> M<mv>`
  - Line 2 — cursor tile terrain: `<Terrain>  DEF+<n>  MOV <cost>`, plus racial
    bonus (`+15 Elf`/`+15 Dwarf`) when it applies and heal for Castle. Requires 8
    new `STR_TERRAIN_*` i18n strings.
- **Minimap landmarks**: explored `Castle`/`EnemyBase` tiles drawn as 3×3 dots on
  the minimap (only once explored — fog-respecting).
- **Full refresh**: pause-menu "Refresh screen" entry + automatic `FULL_REFRESH` on
  every 5th turn rollover. A `nextRefreshFull` flag on the activity is consumed by
  `render()`, which passes the mode to `renderer.displayBuffer()`.

## §2 Flow changes

New mode `ActionSelect` between move and target selection:

```
Cursor → MoveSelect → [move] → ActionSelect → TargetSelect → done
                                    ├ Attack (only if targets exist)
                                    ├ Wait
                                    └ Undo  (also on Back press)
```

- **Undo**: engine API `Game::undoMove(unitIdx, oldX, oldY)` — restores position +
  `recomputeFog()`, only while `!acted`. Visible area recedes; explored stays
  (deliberate, casual-friendly). Capture-base victory triggers on move as today; no
  undo interaction.
- **Auto-advance**: after a unit finishes, cursor+camera jump to the nearest (
  manhattan) not-yet-acted player unit. Turn start jumps to first ready unit.
- **Pause menu** becomes a full-screen vertical list: Resume · Next unit · Inspect ·
  Danger zones On/Off · Refresh screen · End turn · Restart · Quit. Header shows
  objective + turn: e.g. "Capture the enemy base — Turn 12/30". Requires objective
  i18n strings (3 win conditions).
- **Next unit**: cycles cursor through ready player units.

## §3 Information

- **Inspect panel** (pause menu → Inspect): full-screen overlay for the unit under
  the cursor — sprite (2× scale), name, HP, ATK/RNG/DEF/MOV/vision, regen/flies,
  defense breakdown (`5 innate + 15 forest + 15 elf = 35%`), damage rule one-liner.
  Back closes. No unit → terrain-only info.
- **Danger zone overlay** (toggle): tiles attackable next turn by visible, awake
  enemies. `Game::computeDangerZones()` fills a new 192-byte bitmask (reuses
  `computeReach()` per enemy, then restores the selected unit's reach cache).
  Recompute on: toggle-on, after each player action, at enemy phase end. Rendered
  as a dithered 6px strip along the tile bottom (tunable on device). Session-only
  state.

## §4 Balance

| Change | Rationale |
|---|---|
| Knight MV 7 → 5 | Removes strict dominance over Soldier |
| New `goblin_slinger` (HP 16, MV 5, ATK 6, RNG 2, DEF 0, VIS 4, monster) | First ranged monster; punishes kiting. New 16×16 sprite, `UNIT_TYPES` + `UNIT_NAME_IDS` + i18n + `docs/tactics-levels.md` |
| Troll unchanged | Wall role intended; documented warning for DefeatAll authors |

Host gtest suite (`test/tactics/`) extended: undo semantics, danger mask
correctness, slinger stats, knight regression; existing MV assertions updated.

## §5 Battle save/resume

- `Game` is trivially copyable (`static_assert`). Engine gains `stateSize()`,
  `exportState(void*)`, `importState(const void*)` — lib stays hardware-free.
- File: `/.crosspoint/tactics/<levelfile>.sav` = magic + format version +
  level-file checksum + state blob (~6.3KB). Version/checksum mismatch → discard.
- Save on activity exit mid-battle and after each enemy phase. Delete on
  victory/defeat/restart/quit-to-list.
- Level select marks saved levels (`▸` prefix); selecting resumes.

## Error handling

Existing pattern: `LOG_ERR` + graceful fallback. Bad save = fresh start, never a
crash. No exceptions, no abort.

## Memory

+192B danger mask + ~40B undo/menu state inside existing objects; one transient
~6.3KB `makeUniqueNoThrow` buffer during save/load. No render-loop allocations.

## Verification

1. Host suite green (cmake in `test/`), simulator run for flow changes
2. `pio run` clean
3. On device: 4 orientations for pause menu/inspect, ghosting after 10+ turns,
   save/resume across power cycle, danger overlay vs actual AI attacks
