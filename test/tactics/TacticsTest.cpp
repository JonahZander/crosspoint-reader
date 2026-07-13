#include <gtest/gtest.h>

#include <cstring>

#include "TacticsGame.h"
#include "TacticsLevel.h"

using namespace Tactics;

namespace {

bool parse(const char* text, Level& level, char* err = nullptr, size_t errLen = 0) {
  return parseLevel(text, std::strlen(text), level, err, errLen);
}

// A small open arena: soldier and archer vs a goblin and an orc.
constexpr const char* ARENA = R"(name: Arena
win: defeat_all
aggro: 30
map: |
  ..........
  ..f.......
  ..f...^...
  ..........
  ....~~....
units:
  - {type: human_soldier, at: [1, 0], side: player}
  - {type: human_archer, at: [0, 1], side: player}
  - {type: goblin, at: [8, 0], side: enemy}
  - {type: orc, at: [9, 3], side: enemy}
)";

}  // namespace

// ---------------------------------------------------------------- parser

TEST(LevelParser, ParsesBasicFields) {
  Level level;
  ASSERT_TRUE(parse(ARENA, level));
  EXPECT_STREQ(level.name, "Arena");
  EXPECT_EQ(level.win, WinCondition::DefeatAll);
  EXPECT_EQ(level.width, 10);
  EXPECT_EQ(level.height, 5);
  EXPECT_EQ(level.unitCount, 4);
  EXPECT_EQ(level.at(2, 1), Terrain::Forest);
  EXPECT_EQ(level.at(6, 2), Terrain::Mountain);
  EXPECT_EQ(level.at(4, 4), Terrain::Water);
  EXPECT_EQ(level.at(0, 0), Terrain::Plain);
  EXPECT_EQ(level.units[0].side, Side::Player);
  EXPECT_EQ(level.units[2].side, Side::Enemy);
  EXPECT_EQ(level.units[1].x, 0);
  EXPECT_EQ(level.units[1].y, 1);
}

TEST(LevelParser, ShortRowsArePaddedWithVoid) {
  Level level;
  const char* text =
      "map: |\n"
      "  .....\n"
      "  ..\n"
      "units:\n"
      "  - {type: goblin, at: [0, 0], side: player}\n";
  ASSERT_TRUE(parse(text, level));
  EXPECT_EQ(level.width, 5);
  EXPECT_EQ(level.at(4, 1), Terrain::Void);  // beyond the short row
}

TEST(LevelParser, CommentsAndBlankLinesIgnored) {
  Level level;
  const char* text =
      "# a comment\n"
      "name: Test  # trailing comment\n"
      "\n"
      "map: |\n"
      "  ...\n"
      "units:\n"
      "  - {type: orc, at: [1, 0], side: player}\n";
  ASSERT_TRUE(parse(text, level));
  EXPECT_STREQ(level.name, "Test");
}

TEST(LevelParser, RockInMapIsNotAComment) {
  Level level;
  const char* text =
      "map: |\n"
      "  .#.\n"
      "units:\n"
      "  - {type: orc, at: [0, 0], side: player}\n";
  ASSERT_TRUE(parse(text, level));
  EXPECT_EQ(level.at(1, 0), Terrain::Rock);
}

TEST(LevelParser, RejectsUnknownUnitType) {
  Level level;
  char err[64];
  const char* text =
      "map: |\n"
      "  ...\n"
      "units:\n"
      "  - {type: balrog, at: [0, 0], side: player}\n";
  EXPECT_FALSE(parse(text, level, err, sizeof(err)));
  EXPECT_NE(std::strstr(err, "unknown unit type"), nullptr);
}

TEST(LevelParser, RejectsLevelWithoutPlayerUnits) {
  Level level;
  const char* text =
      "map: |\n"
      "  ...\n"
      "units:\n"
      "  - {type: orc, at: [0, 0], side: enemy}\n";
  EXPECT_FALSE(parse(text, level));
}

TEST(LevelParser, RejectsCaptureBaseWithoutBaseTile) {
  Level level;
  const char* text =
      "win: capture_base\n"
      "map: |\n"
      "  ...\n"
      "units:\n"
      "  - {type: orc, at: [0, 0], side: player}\n";
  EXPECT_FALSE(parse(text, level));
}

TEST(LevelParser, RejectsUnitOnImpassableTerrain) {
  Level level;
  const char* text =
      "map: |\n"
      "  .~.\n"
      "units:\n"
      "  - {type: orc, at: [1, 0], side: player}\n";
  EXPECT_FALSE(parse(text, level));
}

TEST(LevelParser, FlyerMayStartOverWater) {
  Level level;
  const char* text =
      "map: |\n"
      "  .~.\n"
      "units:\n"
      "  - {type: wyvern, at: [1, 0], side: player}\n";
  EXPECT_TRUE(parse(text, level));
}

// ---------------------------------------------------------------- movement

TEST(Movement, TerrainCostsLimitReach) {
  Level level;
  ASSERT_TRUE(parse(ARENA, level));
  Game game;
  game.start(level);
  // Soldier: move 5 from (1,0). Plain tiles cost 1.
  game.computeReach(0);
  EXPECT_TRUE(game.isReachable(6, 0));   // 5 straight
  EXPECT_FALSE(game.isReachable(7, 0));  // 6 > move
  // Forest at (2,1) costs 2: (1,0)->(2,0)->(2,1) = 1+2 = 3.
  EXPECT_TRUE(game.isReachable(2, 1));
}

TEST(Movement, WaterBlocksGroundButNotFlyers) {
  Level level;
  const char* text =
      "map: |\n"
      "  .~.\n"
      "  .~.\n"
      "units:\n"
      "  - {type: human_soldier, at: [0, 0], side: player}\n"
      "  - {type: wyvern, at: [0, 1], side: player}\n";
  ASSERT_TRUE(parse(text, level));
  Game game;
  game.start(level);
  game.computeReach(0);
  EXPECT_FALSE(game.isReachable(2, 0));  // soldier can't cross
  game.computeReach(1);
  EXPECT_TRUE(game.isReachable(2, 1));  // wyvern flies over
}

TEST(Movement, ZocStopsMovement) {
  Level level;
  const char* text =
      "map: |\n"
      "  ........\n"
      "units:\n"
      "  - {type: human_knight, at: [0, 0], side: player}\n"
      "  - {type: orc, at: [4, 0], side: enemy}\n";
  ASSERT_TRUE(parse(text, level));
  Game game;
  game.start(level);
  game.computeReach(0);
  // Knight (move 5) walks into the tile next to the orc but not past it.
  EXPECT_TRUE(game.isReachable(3, 0));
  EXPECT_FALSE(game.isReachable(5, 0));  // behind the orc, blocked by ZOC + body
}

TEST(Movement, CannotEndOnOccupiedTile) {
  Level level;
  ASSERT_TRUE(parse(ARENA, level));
  Game game;
  game.start(level);
  game.computeReach(0);
  EXPECT_FALSE(game.isReachable(0, 1));  // archer stands there
  EXPECT_TRUE(game.isReachable(1, 0));   // own tile is fine (stay put)
}

// ---------------------------------------------------------------- combat

TEST(Combat, DamageRespectsTerrainDefense) {
  Level level;
  const char* text =
      "map: |\n"
      "  ..f\n"
      "units:\n"
      "  - {type: human_soldier, at: [1, 0], side: player}\n"
      "  - {type: goblin, at: [2, 0], side: enemy}\n";
  ASSERT_TRUE(parse(text, level));
  Game game;
  game.start(level);
  // Goblin (def 0) in forest (+15): soldier atk 8 -> 8*85/100 = 6.
  const Game::Forecast f = game.forecast(0, 1);
  EXPECT_EQ(f.damageToTarget, 6);
  // Goblin counter: atk 6 vs soldier def 10 on plain -> 6*90/100 = 5.
  EXPECT_EQ(f.counterDamage, 5);
}

TEST(Combat, ArcherOutrangesMeleeCounter) {
  Level level;
  const char* text =
      "map: |\n"
      "  ....\n"
      "units:\n"
      "  - {type: human_archer, at: [0, 0], side: player}\n"
      "  - {type: orc, at: [2, 0], side: enemy}\n";
  ASSERT_TRUE(parse(text, level));
  Game game;
  game.start(level);
  const Game::Forecast f = game.forecast(0, 1);
  EXPECT_GT(f.damageToTarget, 0);
  EXPECT_EQ(f.counterDamage, 0);  // orc range 1, attacker at distance 2
}

TEST(Combat, KillPreventsCounter) {
  Level level;
  const char* text =
      "map: |\n"
      "  ...\n"
      "units:\n"
      "  - {type: dwarf_thunderer, at: [0, 0], side: player}\n"
      "  - {type: goblin, at: [1, 0], side: enemy}\n";
  ASSERT_TRUE(parse(text, level));
  Game game;
  game.start(level);
  // First strike wears the goblin down into one-shot range (and eats a
  // counter, since both stand adjacent).
  ASSERT_TRUE(game.attack(0, 1));
  EXPECT_EQ(game.unit(1).hp, 18 - 12);
  EXPECT_EQ(game.unit(0).hp, 28 - 5);
  // A lethal follow-up would draw no counter at all.
  const Game::Forecast f = game.forecast(0, 1);
  EXPECT_TRUE(f.targetDies);
  EXPECT_EQ(f.counterDamage, 0);
}

TEST(Combat, ElfDefenseBonusInForest) {
  Level level;
  const char* text =
      "map: |\n"
      "  .f\n"
      "units:\n"
      "  - {type: elf_fighter, at: [1, 0], side: player}\n"
      "  - {type: orc, at: [0, 0], side: enemy}\n";
  ASSERT_TRUE(parse(text, level));
  Game game;
  game.start(level);
  // Elf: def 10 + forest 15 + racial 15 = 40.
  EXPECT_EQ(game.defenseAt(game.unit(0), 1, 0), 40);
}

// ---------------------------------------------------------------- fog

TEST(Fog, HidesUnexploredAndRemembersExplored) {
  Level level;
  const char* text =
      "win: capture_base\n"
      "map: |\n"
      "  ....................\n"
      "  ...................B\n"
      "units:\n"
      "  - {type: human_knight, at: [0, 0], side: player}\n"
      "  - {type: orc, at: [19, 0], side: enemy}\n";
  ASSERT_TRUE(parse(text, level));
  Game game;
  game.start(level);
  EXPECT_TRUE(game.isVisible(0, 0));
  EXPECT_TRUE(game.isVisible(4, 0));    // knight vision 4
  EXPECT_FALSE(game.isVisible(19, 1));  // base hidden at the far end
  EXPECT_FALSE(game.isExplored(19, 1));
  // March toward the base; explored area persists behind us.
  ASSERT_TRUE(game.moveUnit(0, 5, 0));  // knight move 5
  EXPECT_TRUE(game.isExplored(0, 0));   // remembered
  EXPECT_TRUE(game.isVisible(9, 0));    // new frontier (vision 4 from x=5)
  EXPECT_FALSE(game.isVisible(0, 0));   // out of live sight now (vision 4)
}

TEST(Fog, CaptureBaseWins) {
  Level level;
  const char* text =
      "win: capture_base\n"
      "map: |\n"
      "  ..B\n"
      "units:\n"
      "  - {type: human_knight, at: [0, 0], side: player}\n"
      "  - {type: orc, at: [1, 0], side: enemy}\n";
  ASSERT_TRUE(parse(text, level));
  Game game;
  game.start(level);
  // Kill the guard, then walk onto the base.
  while (game.unit(1).alive) {
    game.attack(0, 1);
    if (game.status() != Game::Status::Playing) break;
    game.endPlayerTurn();
    while (game.aiStep().acted) {
    }
  }
  ASSERT_FALSE(game.unit(1).alive);
  ASSERT_TRUE(game.moveUnit(0, 2, 0));
  EXPECT_EQ(game.status(), Game::Status::Victory);
}

// ---------------------------------------------------------------- turns/AI

TEST(Ai, DormantMonstersHoldPosition) {
  Level level;
  const char* text =
      "aggro: 3\n"
      "map: |\n"
      "  ....................\n"
      "units:\n"
      "  - {type: human_soldier, at: [0, 0], side: player}\n"
      "  - {type: orc, at: [19, 0], side: enemy}\n";
  ASSERT_TRUE(parse(text, level));
  Game game;
  game.start(level);
  game.finishUnit(0);
  game.endPlayerTurn();
  Game::AiAction act = game.aiStep();
  EXPECT_TRUE(act.acted);
  EXPECT_EQ(act.toX, 19);  // still asleep, didn't move
  EXPECT_EQ(act.target, -1);
}

TEST(Ai, WakesAndHuntsWhenClose) {
  Level level;
  const char* text =
      "aggro: 10\n"
      "map: |\n"
      "  ..........\n"
      "units:\n"
      "  - {type: human_soldier, at: [0, 0], side: player}\n"
      "  - {type: orc, at: [9, 0], side: enemy}\n";
  ASSERT_TRUE(parse(text, level));
  Game game;
  game.start(level);
  game.finishUnit(0);
  game.endPlayerTurn();
  const Game::AiAction act = game.aiStep();
  EXPECT_TRUE(act.acted);
  EXPECT_LT(act.toX, 9);  // moved toward the soldier
}

TEST(Ai, SurviveModeReachesVictory) {
  Level level;
  const char* text =
      "win: survive\n"
      "turns: 3\n"
      "aggro: 1\n"
      "map: |\n"
      "  ..........\n"
      "units:\n"
      "  - {type: dwarf_guard, at: [0, 0], side: player}\n"
      "  - {type: goblin, at: [9, 0], side: enemy}\n";
  ASSERT_TRUE(parse(text, level));
  Game game;
  game.start(level);
  for (int i = 0; i < 5 && game.status() == Game::Status::Playing; i++) {
    game.finishUnit(0);
    game.endPlayerTurn();
    while (game.aiStep().acted) {
    }
  }
  EXPECT_EQ(game.status(), Game::Status::Victory);
}

TEST(Ai, FullBattleTerminates) {
  Level level;
  ASSERT_TRUE(parse(ARENA, level));
  Game game;
  game.start(level);
  // Player never moves; let the monsters come and fight it out. The game
  // must reach a terminal state without hanging.
  int guard = 0;
  while (game.status() == Game::Status::Playing && guard++ < 200) {
    for (int i = 0; i < game.unitCount(); i++) {
      const Unit& u = game.unit(i);
      if (!u.alive || u.side != Side::Player || u.acted) continue;
      // Attack anything adjacent, else pass.
      int8_t targets[8];
      const int n = game.targetsFrom(i, u.x, u.y, targets, 8);
      if (n > 0) {
        game.attack(i, targets[0]);
      } else {
        game.finishUnit(i);
      }
    }
    if (game.status() != Game::Status::Playing) break;
    game.endPlayerTurn();
    while (game.aiStep().acted) {
    }
  }
  EXPECT_NE(game.status(), Game::Status::Playing);
  EXPECT_LT(guard, 200);
}

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

TEST(Undo, RevertsAnActedPureMove) {
  // Moves auto-commit (acted = true immediately); undo must un-commit.
  // The activity is responsible for never undoing attacks or fog reveals.
  Level level;
  ASSERT_TRUE(parse(ARENA, level));
  Game game;
  game.start(level);
  game.computeReach(0);
  ASSERT_TRUE(game.moveUnit(0, 3, 0));
  game.finishUnit(0);
  EXPECT_TRUE(game.unit(0).acted);
  EXPECT_TRUE(game.undoMove(0, 1, 0));
  EXPECT_EQ(game.unit(0).x, 1);
  EXPECT_FALSE(game.unit(0).acted);  // ready to act again
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
  EXPECT_FALSE(game.isVisible(7, 0));  // vision recedes
  EXPECT_TRUE(game.isExplored(7, 0));  // explored layer is permanent
}

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
  // ARENA aggro radius is 30, so the goblin counts as awake even though it
  // has not acted yet — danger zones must treat radius-woken enemies as live
  // threats.
  game.computeDangerZones();
  // Goblin: move 5, range 1. (4,1) is well within move+range.
  EXPECT_TRUE(game.isDangerous(4, 1));
  // A tile far beyond move+range stays safe: (0,4).
  EXPECT_FALSE(game.isDangerous(0, 4));
}

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

TEST(SaveState, ResumesMidEnemyPhaseAndFinishesItOnce) {
  // A snapshot can be taken mid-enemy-phase (the activity saves on exit in
  // any mode). After import, driving aiStep() must finish the phase exactly
  // once: remaining enemies act, the phase rolls over to Player, and every
  // player unit is ready again. The importer resuming in the player phase
  // (or requiring a second endPlayerTurn()) would hand enemies double turns.
  Level level;
  ASSERT_TRUE(parse(ARENA, level));
  Game game;
  game.start(level);
  for (int i = 0; i < game.unitCount(); i++) {
    if (game.unit(i).side == Side::Player) game.finishUnit(i);
  }
  game.endPlayerTurn();
  ASSERT_EQ(game.phase(), Game::Phase::Enemy);
  ASSERT_TRUE(game.aiStep().acted);  // partway through the enemy phase

  static uint8_t blob[sizeof(Game)];
  game.exportState(blob);
  Game restored;
  ASSERT_TRUE(restored.importState(blob));
  EXPECT_EQ(restored.phase(), Game::Phase::Enemy);
  EXPECT_EQ(restored.turn(), 1);

  int steps = 0;
  while (restored.aiStep().acted && steps <= MAX_UNITS) steps++;
  EXPECT_LE(steps, MAX_UNITS);  // the phase terminated on its own
  EXPECT_EQ(restored.phase(), Game::Phase::Player);
  EXPECT_EQ(restored.turn(), 2);
  for (int i = 0; i < restored.unitCount(); i++) {
    const Unit& u = restored.unit(i);
    if (u.alive && u.side == Side::Player) {
      EXPECT_FALSE(u.acted) << "unit " << i;
    }
  }
}

TEST(SaveState, RejectsGarbage) {
  static uint8_t blob[sizeof(Game)];
  std::memset(blob, 0xAB, sizeof(blob));
  Game g;
  EXPECT_FALSE(g.importState(blob));
}

namespace {

// Byte offset of `pat` inside the blob, or -1. Used to corrupt specific
// fields of an exported snapshot without reaching into Game's privates.
int findPattern(const uint8_t* blob, const size_t blobLen, const uint8_t* pat, const size_t patLen) {
  for (size_t i = 0; i + patLen <= blobLen; i++) {
    if (std::memcmp(blob + i, pat, patLen) == 0) return static_cast<int>(i);
  }
  return -1;
}

}  // namespace

TEST(SaveState, RejectsOutOfRangeUnitType) {
  // The save header CRC covers the level file, not the blob: a valid-looking
  // snapshot with units_[i].type = 255 would index UNIT_TYPES[255] on the
  // first info() call. Import must reject it.
  Level level;
  ASSERT_TRUE(parse(ARENA, level));
  Game game;
  game.start(level);
  static uint8_t blob[sizeof(Game)];
  game.exportState(blob);
  Game g;
  ASSERT_TRUE(g.importState(blob));  // pristine blob is fine

  // Unit 2 is the goblin at (8, 0): {type, side, x, y, hp, acted, alive,
  // aggro} = {12, 1, 8, 0, 18, 0, 1, 0}.
  const uint8_t goblin[8] = {12, 1, 8, 0, 18, 0, 1, 0};
  const int off = findPattern(blob, sizeof(blob), goblin, sizeof(goblin));
  ASSERT_GE(off, 0);
  blob[off] = 255;  // type
  EXPECT_FALSE(g.importState(blob));
}

TEST(SaveState, RejectsOutOfRangeTile) {
  // Same story for terrain: tiles[i] = 200 would index TERRAIN_INFO[200]
  // in moveCostFor/defenseAt.
  Level level;
  const char* text =
      "aggro: 30\n"
      "map: |\n"
      "  ..........\n"
      "  ..C#~^B...\n"
      "  ..........\n"
      "units:\n"
      "  - {type: human_soldier, at: [0, 0], side: player}\n"
      "  - {type: goblin, at: [9, 2], side: enemy}\n";
  ASSERT_TRUE(parse(text, level));
  Game game;
  game.start(level);
  static uint8_t blob[sizeof(Game)];
  game.exportState(blob);
  Game g;
  ASSERT_TRUE(g.importState(blob));

  // Row 1 of the map, "..C#~^B" = {Plain, Plain, Castle, Rock, Water,
  // Mountain, EnemyBase} — a distinctive tile byte run.
  const uint8_t row[7] = {1, 1, 6, 5, 4, 3, 7};
  const int off = findPattern(blob, sizeof(blob), row, sizeof(row));
  ASSERT_GE(off, 0);
  blob[off + 2] = 200;  // the Castle tile
  EXPECT_FALSE(g.importState(blob));
}

// ---------------------------------------------------------------- balance

TEST(Combat, FlankBonusWithAllyBehindTarget) {
  // Soldier (atk 8) hits a goblin (def 0) for 8 — or 12 when another
  // player unit stands on the tile directly opposite the attacker.
  Level level;
  const char* text =
      "aggro: 30\n"
      "map: |\n"
      "  .....\n"
      "units:\n"
      "  - {type: human_soldier, at: [1, 0], side: player}\n"
      "  - {type: goblin, at: [2, 0], side: enemy}\n"
      "  - {type: human_archer, at: [3, 0], side: player}\n";
  ASSERT_TRUE(parse(text, level));
  Game game;
  game.start(level);
  const Game::Forecast f = game.forecast(0, 1);
  EXPECT_TRUE(f.flank);
  EXPECT_EQ(f.damageToTarget, 12);  // 8 * 1.5
}

TEST(Combat, NoFlankFromTheSide) {
  // Same pincer geometry but the ally is beside the goblin, not behind it.
  Level level;
  const char* text =
      "aggro: 30\n"
      "map: |\n"
      "  .....\n"
      "  .....\n"
      "units:\n"
      "  - {type: human_soldier, at: [1, 0], side: player}\n"
      "  - {type: goblin, at: [2, 0], side: enemy}\n"
      "  - {type: human_archer, at: [2, 1], side: player}\n";
  ASSERT_TRUE(parse(text, level));
  Game game;
  game.start(level);
  const Game::Forecast f = game.forecast(0, 1);
  EXPECT_FALSE(f.flank);
  EXPECT_EQ(f.damageToTarget, 8);
}

TEST(Combat, PartingBlowOnDisengage) {
  // Soldier adjacent to an aggro'd orc (atk 9) walks away: half damage
  // through plain-tile defense (10%) = 9*90/200 = 4. Undoability is the
  // caller's business; the engine just applies the hit.
  Level level;
  const char* text =
      "aggro: 30\n"
      "map: |\n"
      "  ........\n"
      "units:\n"
      "  - {type: human_soldier, at: [2, 0], side: player}\n"
      "  - {type: orc, at: [3, 0], side: enemy}\n";
  ASSERT_TRUE(parse(text, level));
  Game game;
  game.start(level);
  const int8_t hpBefore = game.unit(0).hp;
  ASSERT_TRUE(game.moveUnit(0, 0, 0));  // breaks contact
  EXPECT_EQ(hpBefore - game.unit(0).hp, 4);
}

TEST(Combat, NoPartingBlowWhenStayingEngaged) {
  // Sliding to another tile still adjacent to the same orc is not a
  // disengage.
  Level level;
  const char* text =
      "aggro: 30\n"
      "map: |\n"
      "  .....\n"
      "  .....\n"
      "units:\n"
      "  - {type: human_soldier, at: [2, 0], side: player}\n"
      "  - {type: orc, at: [3, 0], side: enemy}\n";
  ASSERT_TRUE(parse(text, level));
  Game game;
  game.start(level);
  const int8_t hpBefore = game.unit(0).hp;
  ASSERT_TRUE(game.moveUnit(0, 3, 1));  // still adjacent to the orc
  EXPECT_EQ(game.unit(0).hp, hpBefore);
}

TEST(Combat, NoPartingBlowFromFullyDormantGuard) {
  // aggro 0 = passive-until-attacked guards; walking out of contact with
  // one costs nothing.
  Level level;
  const char* text =
      "aggro: 0\n"
      "map: |\n"
      "  .....\n"
      "units:\n"
      "  - {type: human_soldier, at: [2, 0], side: player}\n"
      "  - {type: orc, at: [3, 0], side: enemy}\n";
  ASSERT_TRUE(parse(text, level));
  Game game;
  game.start(level);
  const int8_t hpBefore = game.unit(0).hp;
  ASSERT_TRUE(game.moveUnit(0, 0, 0));  // dormant guards sleep through it
  EXPECT_EQ(game.unit(0).hp, hpBefore);
}

TEST(DangerZones, ThreatCoversMovePlusAttackRange) {
  // Goblin at (8,0): move 5, range 1. Player ZoC at (1,0) is irrelevant to
  // its path, so it can stand on x=3..8 and strike x=2. The danger mask
  // must therefore extend exactly one tile past the walk area.
  Level level;
  const char* text =
      "aggro: 30\n"
      "map: |\n"
      "  ............\n"
      "units:\n"
      "  - {type: human_soldier, at: [0, 0], side: player}\n"
      "  - {type: goblin, at: [8, 0], side: enemy}\n";
  ASSERT_TRUE(parse(text, level));
  Game game;
  game.start(level);
  game.computeDangerZones(1);  // hover variant: ignores fog and aggro state
  game.computeReach(1);
  EXPECT_TRUE(game.isReachable(3, 0));   // walk edge
  EXPECT_FALSE(game.isReachable(2, 0));  // beyond move budget
  EXPECT_TRUE(game.isDangerous(2, 0));   // ...but attackable from (3,0)
  EXPECT_FALSE(game.isDangerous(1, 0));  // beyond move + range
}

TEST(DangerZones, HoverVariantIsolatesOneEnemy) {
  Level level;
  const char* text =
      "aggro: 30\n"
      "map: |\n"
      "  ..........\n"
      "  ..........\n"
      "  ..........\n"
      "  ..........\n"
      "  ..........\n"
      "  ..........\n"
      "  ..........\n"
      "units:\n"
      "  - {type: human_soldier, at: [0, 0], side: player}\n"
      "  - {type: goblin, at: [4, 0], side: enemy}\n"
      "  - {type: goblin, at: [9, 6], side: enemy}\n";
  ASSERT_TRUE(parse(text, level));
  Game game;
  game.start(level);
  game.computeDangerZones(1);            // top goblin only
  EXPECT_TRUE(game.isDangerous(4, 1));   // its threat
  EXPECT_FALSE(game.isDangerous(9, 5));  // far goblin contributes nothing
}

TEST(Ai, AnyFactionCanBeTheEnemy) {
  // Sides are free-form: a "good" race on side enemy is hunted by the AI
  // exactly like a monster (campaigns can pit humans against evil humans).
  Level level;
  const char* text =
      "aggro: 10\n"
      "map: |\n"
      "  ..........\n"
      "units:\n"
      "  - {type: orc, at: [0, 0], side: player}\n"
      "  - {type: human_knight, at: [9, 0], side: enemy}\n";
  ASSERT_TRUE(parse(text, level));
  Game game;
  game.start(level);
  game.finishUnit(0);
  game.endPlayerTurn();
  const Game::AiAction act = game.aiStep();
  EXPECT_TRUE(act.acted);
  EXPECT_LT(act.toX, 9);  // the enemy knight hunts the player's orc
}

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
