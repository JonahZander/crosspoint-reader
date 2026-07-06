// Automated playtest for the shipped campaign levels (data/tactics/).
//
// A deliberately simple greedy bot plays every battle: if this bot can win
// within the turn budget, a human comfortably can. The bot reads the real
// level files, so these tests also catch map typos, unreachable bases, and
// units placed on rock. (The bot ignores fog — it proves balance and
// reachability, not stealth UX.)
#include <gtest/gtest.h>

#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "TacticsGame.h"
#include "TacticsLevel.h"

using namespace Tactics;

namespace {

bool loadLevelFile(const char* name, Level& level) {
  std::ifstream in(std::string(CAMPAIGN_DIR) + "/" + name);
  if (!in) return false;
  std::stringstream ss;
  ss << in.rdbuf();
  const std::string text = ss.str();
  char err[64] = {0};
  const bool ok = parseLevel(text.c_str(), text.size(), level, err, sizeof(err));
  if (!ok) ADD_FAILURE() << name << ": " << err;
  return ok;
}

int manhattan(const int ax, const int ay, const int bx, const int by) {
  return (ax > bx ? ax - bx : bx - ax) + (ay > by ? ay - by : by - ay);
}

// Uniform-cost BFS distance over ground-passable tiles from a set of goals.
// 0xFFFF = unreachable.
void computeField(const Level& level, const std::vector<std::pair<int, int>>& goals, uint16_t* field) {
  for (int i = 0; i < MAX_TILES; i++) field[i] = 0xFFFF;
  std::vector<std::pair<int, int>> queue;
  queue.reserve(256);
  for (const auto& g : goals) {
    field[g.second * MAX_MAP_W + g.first] = 0;
    queue.push_back(g);
  }
  static constexpr int DX[4] = {1, -1, 0, 0};
  static constexpr int DY[4] = {0, 0, 1, -1};
  for (size_t head = 0; head < queue.size(); head++) {
    const auto [x, y] = queue[head];
    const uint16_t here = field[y * MAX_MAP_W + x];
    for (int d = 0; d < 4; d++) {
      const int nx = x + DX[d];
      const int ny = y + DY[d];
      if (nx < 0 || ny < 0 || nx >= level.width || ny >= level.height) continue;
      const Terrain t = level.at(nx, ny);
      if (TERRAIN_INFO[static_cast<int>(t)].moveCost == 0) continue;
      if (field[ny * MAX_MAP_W + nx] != 0xFFFF) continue;
      field[ny * MAX_MAP_W + nx] = static_cast<uint16_t>(here + 1);
      queue.emplace_back(nx, ny);
    }
  }
}

struct BotResult {
  Game::Status status = Game::Status::Playing;
  int turns = 0;
  int survivors = 0;
};

BotResult runBot(const Level& level, const int maxTurns) {
  Game game;
  game.start(level);

  int baseX = -1, baseY = -1;
  for (int y = 0; y < level.height; y++) {
    for (int x = 0; x < level.width; x++) {
      if (level.at(x, y) == Terrain::EnemyBase) {
        baseX = x;
        baseY = y;
      }
    }
  }

  static uint16_t field[MAX_TILES];

  int guard = 0;
  while (game.status() == Game::Status::Playing && guard++ < maxTurns + 2) {
    // Objective field for this turn.
    std::vector<std::pair<int, int>> goals;
    if (level.win == WinCondition::CaptureBase) {
      goals.emplace_back(baseX, baseY);
    } else if (level.win == WinCondition::DefeatAll) {
      for (int i = 0; i < game.unitCount(); i++) {
        const Unit& e = game.unit(i);
        if (e.alive && e.side == Side::Enemy) goals.emplace_back(e.x, e.y);
      }
    }
    if (!goals.empty()) computeField(level, goals, field);

    for (int u = 0; u < game.unitCount(); u++) {
      const Unit& unit = game.unit(u);
      if (!unit.alive || unit.side != Side::Player || unit.acted) continue;
      if (game.status() != Game::Status::Playing) break;
      game.computeReach(u);

      // Capture maps: stand on the base the moment it is reachable.
      if (level.win == WinCondition::CaptureBase && game.isReachable(baseX, baseY)) {
        game.moveUnit(u, baseX, baseY);
        break;
      }

      // Best (tile, target) attack by the same scoring style as the AI.
      int bestScore = -1000000, bestX = unit.x, bestY = unit.y, bestT = -1;
      for (int y = 0; y < level.height; y++) {
        for (int x = 0; x < level.width; x++) {
          const bool standable = (x == unit.x && y == unit.y) || game.isReachable(x, y);
          if (!standable) continue;
          int8_t targets[8];
          const int n = game.targetsFrom(u, x, y, targets, 8);
          const int ownDef = game.defenseAt(unit, x, y);
          for (int t = 0; t < n; t++) {
            const Unit& tgt = game.unit(targets[t]);
            const int dmgRaw = unit.info().attack * (100 - game.defenseAt(tgt, tgt.x, tgt.y)) / 100;
            const int dmg = dmgRaw < 1 ? 1 : dmgRaw;
            const bool kills = dmg >= tgt.hp;
            const int dist = manhattan(x, y, tgt.x, tgt.y);
            int counter = 0;
            if (!kills && dist <= tgt.info().range) {
              const int c = tgt.info().attack * (100 - ownDef) / 100;
              counter = c < 1 ? 1 : c;
            }
            int score = dmg * 10 + ownDef + (kills ? 500 : 0) - counter * 7;
            if (counter >= unit.hp) score -= 800;  // never trade into death
            if (score > bestScore) {
              bestScore = score;
              bestX = x;
              bestY = y;
              bestT = targets[t];
            }
          }
        }
      }
      if (bestT >= 0) {
        if (bestX != unit.x || bestY != unit.y) game.moveUnit(u, bestX, bestY);
        game.attack(u, bestT);
        continue;
      }

      // No attack available: objective movement.
      int mvX = unit.x, mvY = unit.y;
      if (level.win == WinCondition::Survive) {
        // Hold the strongest nearby ground; don't wander.
        int bestDef = game.defenseAt(unit, unit.x, unit.y);
        for (int y = 0; y < level.height; y++) {
          for (int x = 0; x < level.width; x++) {
            if (!game.isReachable(x, y)) continue;
            if (manhattan(x, y, unit.x, unit.y) > 3) continue;
            const int def = game.defenseAt(unit, x, y);
            if (def > bestDef) {
              bestDef = def;
              mvX = x;
              mvY = y;
            }
          }
        }
      } else {
        // Walk down the BFS field toward the objective.
        uint16_t bestField = field[unit.y * MAX_MAP_W + unit.x];
        int bestDef = game.defenseAt(unit, unit.x, unit.y);
        for (int y = 0; y < level.height; y++) {
          for (int x = 0; x < level.width; x++) {
            if (!game.isReachable(x, y)) continue;
            const uint16_t f = field[y * MAX_MAP_W + x];
            const int def = game.defenseAt(unit, x, y);
            if (f < bestField || (f == bestField && def > bestDef)) {
              bestField = f;
              bestDef = def;
              mvX = x;
              mvY = y;
            }
          }
        }
      }
      if (mvX != unit.x || mvY != unit.y) game.moveUnit(u, mvX, mvY);
      if (game.status() != Game::Status::Playing) break;  // walked onto the base
      game.finishUnit(u);
    }
    if (game.status() != Game::Status::Playing) break;

    for (int i = 0; i < game.unitCount(); i++) {
      if (game.unit(i).alive && game.unit(i).side == Side::Player && !game.unit(i).acted) game.finishUnit(i);
    }
    game.endPlayerTurn();
    while (game.aiStep().acted) {
    }
  }

  BotResult r;
  r.status = game.status();
  r.turns = game.turn();
  for (int i = 0; i < game.unitCount(); i++) {
    if (game.unit(i).alive && game.unit(i).side == Side::Player) r.survivors++;
  }
  return r;
}

}  // namespace

// ---------------------------------------------------------------- files

TEST(Campaign, AllBattleFilesParse) {
  const char* files[] = {"11_the_gates.yaml", "13_the_fords.yaml", "15_the_long_night.yaml",
                         "17_the_dark_galleries.yaml", "19_the_maw.yaml"};
  for (const char* f : files) {
    Level level;
    EXPECT_TRUE(loadLevelFile(f, level)) << f;
  }
}

TEST(Campaign, AllStoryFilesExist) {
  const char* files[] = {"10_deeproad_1.story", "12_deeproad_2.story", "14_deeproad_3.story",
                         "16_deeproad_4.story", "18_deeproad_5.story", "20_deeproad_epilogue.story"};
  for (const char* f : files) {
    std::ifstream in(std::string(CAMPAIGN_DIR) + "/" + f);
    EXPECT_TRUE(in.good()) << f;
    std::stringstream ss;
    ss << in.rdbuf();
    EXPECT_GT(ss.str().size(), 200u) << f << " suspiciously short";
    EXPECT_LT(ss.str().size(), 16u * 1024u) << f << " exceeds MAX_LEVEL_FILE";
  }
}

// ---------------------------------------------------------------- balance

TEST(Campaign, TheGatesIsWinnable) {
  Level level;
  ASSERT_TRUE(loadLevelFile("11_the_gates.yaml", level));
  const BotResult r = runBot(level, 30);
  EXPECT_EQ(r.status, Game::Status::Victory) << "turns=" << r.turns;
  EXPECT_GE(r.survivors, 2) << "intro level should not cost lives";
}

TEST(Campaign, TheFordsIsWinnable) {
  Level level;
  ASSERT_TRUE(loadLevelFile("13_the_fords.yaml", level));
  const BotResult r = runBot(level, 35);
  EXPECT_EQ(r.status, Game::Status::Victory) << "turns=" << r.turns;
  EXPECT_GE(r.survivors, 2);
}

TEST(Campaign, TheLongNightIsSurvivable) {
  Level level;
  ASSERT_TRUE(loadLevelFile("15_the_long_night.yaml", level));
  const BotResult r = runBot(level, 14);
  EXPECT_EQ(r.status, Game::Status::Victory) << "turns=" << r.turns;
  EXPECT_GE(r.survivors, 2);
}

TEST(Campaign, TheDarkGalleriesIsWinnable) {
  Level level;
  ASSERT_TRUE(loadLevelFile("17_the_dark_galleries.yaml", level));
  const BotResult r = runBot(level, 25);
  EXPECT_EQ(r.status, Game::Status::Victory) << "turns=" << r.turns;
  EXPECT_GE(r.survivors, 1);
}

TEST(Campaign, TheMawIsWinnable) {
  Level level;
  ASSERT_TRUE(loadLevelFile("19_the_maw.yaml", level));
  const BotResult r = runBot(level, 40);
  EXPECT_EQ(r.status, Game::Status::Victory) << "turns=" << r.turns;
  EXPECT_GE(r.survivors, 1);
}
