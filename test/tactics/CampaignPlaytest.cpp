// Automated playtest for the shipped campaign levels (data/tactics/).
//
// A greedy bot plays every battle: it knows flanking, focus fire (globally
// best attack first, kills before trades) and parting-blow avoidance, and
// approximates a competent human. Each test asserts a margin band, not just
// victory: the survivor floor catches "unwinnable", the survivor ceiling
// catches "too easy" (a flawless sweep means the level poses no threat).
// The bot reads the real level files, so these tests also catch map typos,
// unreachable bases, and units placed on rock. (The bot ignores fog — it
// proves balance and reachability, not stealth UX.)
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

    // Capture maps: stand on the base the moment any unit can reach it.
    if (level.win == WinCondition::CaptureBase) {
      for (int u = 0; u < game.unitCount(); u++) {
        const Unit& unit = game.unit(u);
        if (!unit.alive || unit.side != Side::Player || unit.acted) continue;
        game.computeReach(u);
        if (game.isReachable(baseX, baseY)) {
          game.moveUnit(u, baseX, baseY);
          break;
        }
      }
    }
    if (game.status() != Game::Status::Playing) break;

    // Best (tile, target) attack for one unit, same scoring style as the AI.
    struct Plan {
      int score = -1000000;
      int x = 0, y = 0, target = -1;
    };
    const auto evalAttack = [&](const int u) {
      const Unit& unit = game.unit(u);
      Plan best;
      best.x = unit.x;
      best.y = unit.y;
      game.computeReach(u);
      for (int y = 0; y < level.height; y++) {
        for (int x = 0; x < level.width; x++) {
          const bool standable = (x == unit.x && y == unit.y) || game.isReachable(x, y);
          if (!standable) continue;
          int8_t targets[8];
          const int n = game.targetsFrom(u, x, y, targets, 8);
          const int ownDef = game.defenseAt(unit, x, y);
          // Disengaging from an adjacent awake enemy costs a parting blow;
          // never pick a destination where the blows would be lethal.
          int parting = 0;
          if (!unit.info().flies && (x != unit.x || y != unit.y)) {
            for (int e = 0; e < game.unitCount(); e++) {
              const Unit& en = game.unit(e);
              if (!en.alive || en.side != Side::Enemy) continue;
              if (manhattan(en.x, en.y, unit.x, unit.y) != 1) continue;
              if (manhattan(en.x, en.y, x, y) <= 1) continue;
              const int hit = en.info().attack * (100 - game.defenseAt(unit, unit.x, unit.y)) / 200;
              parting += hit < 1 ? 1 : hit;
            }
          }
          if (parting >= unit.hp) continue;
          for (int t = 0; t < n; t++) {
            const Unit& tgt = game.unit(targets[t]);
            int dmgRaw = unit.info().attack * (100 - game.defenseAt(tgt, tgt.x, tgt.y)) / 100;
            // Flank: an allied unit on the tile directly opposite (x, y)
            // relative to the target boosts the strike by 50%.
            if (manhattan(x, y, tgt.x, tgt.y) == 1) {
              const int ally = game.unitIndexAt(2 * tgt.x - x, 2 * tgt.y - y);
              if (ally >= 0 && game.unit(ally).side == Side::Player && ally != u) dmgRaw = dmgRaw * 3 / 2;
            }
            const int dmg = dmgRaw < 1 ? 1 : dmgRaw;
            const bool kills = dmg >= tgt.hp;
            const int dist = manhattan(x, y, tgt.x, tgt.y);
            int counter = 0;
            if (!kills && dist <= tgt.info().range) {
              const int c = tgt.info().attack * (100 - ownDef) / 100;
              counter = c < 1 ? 1 : c;
            }
            int score = dmg * 10 + ownDef + (kills ? 500 : 0) - counter * 7 - parting * 10;
            // Backline pressure: hard hitters and ranged units are worth more.
            score += tgt.info().attack * 2 + (tgt.info().range > 1 ? 10 : 0);
            if (counter + parting >= unit.hp) score -= 800;  // never trade into death
            if (score > best.score) {
              best.score = score;
              best.x = x;
              best.y = y;
              best.target = targets[t];
            }
          }
        }
      }
      return best;
    };

    // Attack phase: repeatedly execute the globally best attack across all
    // idle units. Kills score +500, so lethal strikes land first and dead
    // enemies never counter or act — focus fire falls out naturally.
    while (game.status() == Game::Status::Playing) {
      int bestU = -1;
      Plan best;
      for (int u = 0; u < game.unitCount(); u++) {
        const Unit& unit = game.unit(u);
        if (!unit.alive || unit.side != Side::Player || unit.acted) continue;
        const Plan pl = evalAttack(u);
        if (pl.target >= 0 && pl.score > best.score) {
          best = pl;
          bestU = u;
        }
      }
      if (bestU < 0) break;
      const Unit& unit = game.unit(bestU);
      if (best.x != unit.x || best.y != unit.y) game.moveUnit(bestU, best.x, best.y);
      // A failed attack (unit cut down mid-retreat) must not spin the loop.
      if (!game.attack(bestU, best.target)) game.finishUnit(bestU);
    }
    if (game.status() != Game::Status::Playing) break;

    // Movement phase for units with no attack available.
    for (int u = 0; u < game.unitCount(); u++) {
      const Unit& unit = game.unit(u);
      if (!unit.alive || unit.side != Side::Player || unit.acted) continue;
      if (game.status() != Game::Status::Playing) break;
      game.computeReach(u);

      // Objective movement. If leaving the current tile means eating
      // parting blows, hold position and fight it out — walking away from
      // an engagement bleeds HP for nothing.
      bool engaged = false;
      if (!unit.info().flies) {
        for (int e = 0; e < game.unitCount() && !engaged; e++) {
          const Unit& en = game.unit(e);
          if (en.alive && en.side == Side::Enemy && manhattan(en.x, en.y, unit.x, unit.y) == 1) engaged = true;
        }
      }
      if (engaged) {
        game.finishUnit(u);
        continue;
      }
      // Threat awareness: refresh the danger overlay (tiles any awake,
      // visible enemy could strike next turn), then re-derive this unit's
      // reach — computeDangerZones clobbers the shared reach cache.
      game.computeDangerZones();
      game.computeReach(u);
      int mvX = unit.x, mvY = unit.y;
      if (level.win == WinCondition::Survive) {
        // Hold the safest strong ground nearby; don't wander.
        int bestScore = game.defenseAt(unit, unit.x, unit.y) - (game.isDangerous(unit.x, unit.y) ? 100 : 0);
        for (int y = 0; y < level.height; y++) {
          for (int x = 0; x < level.width; x++) {
            if (!game.isReachable(x, y)) continue;
            if (manhattan(x, y, unit.x, unit.y) > 3) continue;
            const int score = game.defenseAt(unit, x, y) - (game.isDangerous(x, y) ? 100 : 0);
            if (score > bestScore) {
              bestScore = score;
              mvX = x;
              mvY = y;
            }
          }
        }
      } else {
        // Walk down the BFS field toward the objective, but treat tiles the
        // enemy can strike as a detour cost — ranged units hang back out of
        // reach, melee only sidestep when it's nearly free.
        const int dangerPenalty = unit.info().range > 1 ? 6 : 2;
        int bestCost = field[unit.y * MAX_MAP_W + unit.x] + (game.isDangerous(unit.x, unit.y) ? dangerPenalty : 0);
        int bestDef = game.defenseAt(unit, unit.x, unit.y);
        for (int y = 0; y < level.height; y++) {
          for (int x = 0; x < level.width; x++) {
            if (!game.isReachable(x, y)) continue;
            const int cost = field[y * MAX_MAP_W + x] + (game.isDangerous(x, y) ? dangerPenalty : 0);
            const int def = game.defenseAt(unit, x, y);
            if (cost < bestCost || (cost == bestCost && def > bestDef)) {
              bestCost = cost;
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
    if (getenv("BOT_DEBUG")) {  // per-turn roster dump for balance work
      fprintf(stderr, "turn %d:", game.turn());
      for (int i = 0; i < game.unitCount(); i++) {
        const Unit& du = game.unit(i);
        fprintf(stderr, " %s%s(%d,%d)%d/%d", du.side == Side::Player ? "P:" : "E:", du.info().id, du.x, du.y, du.hp,
                du.info().maxHp);
      }
      fprintf(stderr, "\n");
    }
  }

  BotResult r;
  r.status = game.status();
  r.turns = game.turn();
  for (int i = 0; i < game.unitCount(); i++) {
    if (game.unit(i).alive && game.unit(i).side == Side::Player) r.survivors++;
  }
  fprintf(stderr, "[bot] %s turns=%d survivors=%d\n",
          r.status == Game::Status::Victory ? "victory" : (r.status == Game::Status::Defeat ? "DEFEAT" : "timeout"),
          r.turns, r.survivors);
  return r;
}

}  // namespace

// ---------------------------------------------------------------- files

TEST(Campaign, AllBattleFilesParse) {
  const char* files[] = {"deep_road/11_the_gates.yaml", "deep_road/13_the_fords.yaml",
                         "deep_road/15_the_long_night.yaml", "deep_road/17_the_dark_galleries.yaml",
                         "deep_road/19_the_maw.yaml"};
  for (const char* f : files) {
    Level level;
    EXPECT_TRUE(loadLevelFile(f, level)) << f;
  }
}

TEST(Campaign, AllStoryFilesExist) {
  const char* files[] = {"deep_road/10_deeproad_1.story", "deep_road/12_deeproad_2.story",
                         "deep_road/14_deeproad_3.story", "deep_road/16_deeproad_4.story",
                         "deep_road/18_deeproad_5.story", "deep_road/20_deeproad_epilogue.story"};
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
  ASSERT_TRUE(loadLevelFile("deep_road/11_the_gates.yaml", level));
  const BotResult r = runBot(level, 30);
  EXPECT_EQ(r.status, Game::Status::Victory) << "turns=" << r.turns;
  EXPECT_GE(r.survivors, 2) << "intro level should not cost lives";
}

TEST(Campaign, TheFordsIsWinnable) {
  Level level;
  ASSERT_TRUE(loadLevelFile("deep_road/13_the_fords.yaml", level));
  const BotResult r = runBot(level, 35);
  EXPECT_EQ(r.status, Game::Status::Victory) << "turns=" << r.turns;
  EXPECT_GE(r.survivors, 2);
  EXPECT_LE(r.survivors, 2) << "flawless sweep: the fords pose no threat";
}

TEST(Campaign, TheLongNightIsSurvivable) {
  Level level;
  ASSERT_TRUE(loadLevelFile("deep_road/15_the_long_night.yaml", level));
  const BotResult r = runBot(level, 14);
  EXPECT_EQ(r.status, Game::Status::Victory) << "turns=" << r.turns;
  EXPECT_GE(r.survivors, 2);
  EXPECT_LE(r.survivors, 2) << "flawless sweep: the long night poses no threat";
}

TEST(Campaign, TheDarkGalleriesIsWinnable) {
  Level level;
  ASSERT_TRUE(loadLevelFile("deep_road/17_the_dark_galleries.yaml", level));
  const BotResult r = runBot(level, 25);
  EXPECT_EQ(r.status, Game::Status::Victory) << "turns=" << r.turns;
  EXPECT_GE(r.survivors, 1);
  EXPECT_LE(r.survivors, 2) << "flawless sweep: the galleries pose no threat";
}

TEST(Campaign, TheMawIsWinnable) {
  Level level;
  ASSERT_TRUE(loadLevelFile("deep_road/19_the_maw.yaml", level));
  const BotResult r = runBot(level, 40);
  EXPECT_EQ(r.status, Game::Status::Victory) << "turns=" << r.turns;
  EXPECT_GE(r.survivors, 1);
  EXPECT_LE(r.survivors, 3) << "flawless sweep: the Maw is the finale, it must bite";
}

// ------------------------------------------------- The Broken Crown

TEST(CrownCampaign, AllFilesParseAndStoriesExist) {
  const char* battles[] = {"broken_crown/11_the_tollgate.yaml", "broken_crown/13_the_river_barges.yaml",
                           "broken_crown/15_the_burned_manor.yaml", "broken_crown/17_the_kings_road.yaml",
                           "broken_crown/19_the_throne_hall.yaml"};
  for (const char* f : battles) {
    Level level;
    EXPECT_TRUE(loadLevelFile(f, level)) << f;
  }
  const char* stories[] = {"broken_crown/10_crown_1.story", "broken_crown/12_crown_2.story",
                           "broken_crown/14_crown_3.story", "broken_crown/16_crown_4.story",
                           "broken_crown/18_crown_5.story", "broken_crown/20_crown_epilogue.story"};
  for (const char* f : stories) {
    std::ifstream in(std::string(CAMPAIGN_DIR) + "/" + f);
    EXPECT_TRUE(in.good()) << f;
    std::stringstream ss;
    ss << in.rdbuf();
    EXPECT_GT(ss.str().size(), 200u) << f << " suspiciously short";
    EXPECT_LT(ss.str().size(), 16u * 1024u) << f << " exceeds MAX_LEVEL_FILE";
  }
}

TEST(CrownCampaign, TheTollgateIsWinnable) {
  Level level;
  ASSERT_TRUE(loadLevelFile("broken_crown/11_the_tollgate.yaml", level));
  const BotResult r = runBot(level, 25);
  EXPECT_EQ(r.status, Game::Status::Victory) << "turns=" << r.turns;
  EXPECT_GE(r.survivors, 2) << "intro level should not cost lives";
}

TEST(CrownCampaign, TheRiverBargesIsWinnable) {
  Level level;
  ASSERT_TRUE(loadLevelFile("broken_crown/13_the_river_barges.yaml", level));
  const BotResult r = runBot(level, 20);
  EXPECT_EQ(r.status, Game::Status::Victory) << "turns=" << r.turns;
  EXPECT_GE(r.survivors, 2);
  EXPECT_LE(r.survivors, 3) << "flawless sweep: the barges pose no threat";
}

TEST(CrownCampaign, TheBurnedManorIsSurvivable) {
  Level level;
  ASSERT_TRUE(loadLevelFile("broken_crown/15_the_burned_manor.yaml", level));
  const BotResult r = runBot(level, 12);
  EXPECT_EQ(r.status, Game::Status::Victory) << "turns=" << r.turns;
  EXPECT_GE(r.survivors, 2);
  EXPECT_LE(r.survivors, 3) << "flawless sweep: the manor poses no threat";
}

TEST(CrownCampaign, TheKingsRoadIsWinnable) {
  Level level;
  ASSERT_TRUE(loadLevelFile("broken_crown/17_the_kings_road.yaml", level));
  const BotResult r = runBot(level, 30);
  EXPECT_EQ(r.status, Game::Status::Victory) << "turns=" << r.turns;
  EXPECT_GE(r.survivors, 3);
  EXPECT_LE(r.survivors, 4) << "flawless sweep: the field army poses no threat";
}

TEST(CrownCampaign, TheThroneHallIsWinnable) {
  Level level;
  ASSERT_TRUE(loadLevelFile("broken_crown/19_the_throne_hall.yaml", level));
  const BotResult r = runBot(level, 25);
  EXPECT_EQ(r.status, Game::Status::Victory) << "turns=" << r.turns;
  EXPECT_GE(r.survivors, 2);
  EXPECT_LE(r.survivors, 4) << "flawless sweep: the throne hall is the finale, it must bite";
}
