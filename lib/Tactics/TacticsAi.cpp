// Enemy AI for the tactics mini-game.
//
// Monsters start dormant and "guard" their patch of the map — this is what
// makes hidden-base levels explorable instead of a turn-one zerg rush. A
// monster wakes (aggro) when a player unit comes within the level's aggro
// radius, when it is attacked, or when combat happens nearby (pack wake).
//
// An awake unit is a greedy scorer: it evaluates every (reachable tile,
// attackable target) pair and picks the highest-scoring attack, preferring
// kills, avoiding lethal counterattacks and favouring defensive terrain.
// With no target in reach it walks down a cached distance-to-nearest-player
// field instead.
#include <cstring>

#include "TacticsGame.h"

namespace Tactics {

namespace {
constexpr uint8_t UNREACHABLE = 0xFF;
constexpr uint8_t FIELD_MAX = 0xFE;
}  // namespace

int Game::nextAiUnit() {
  for (int i = 0; i < unitCount_; i++) {
    if (units_[i].alive && units_[i].side == Side::Enemy && !units_[i].acted) return i;
  }
  return -1;
}

Game::AiAction Game::aiStep() {
  AiAction none{};
  none.acted = false;
  none.target = -1;
  if (status_ != Status::Playing || phase_ != Phase::Enemy) return none;

  const int idx = nextAiUnit();
  if (idx < 0) {
    // Enemy phase complete — roll over to the next player turn.
    phase_ = Phase::Player;
    turn_++;
    for (int i = 0; i < unitCount_; i++) {
      if (units_[i].side == Side::Player) units_[i].acted = false;
    }
    applyUpkeep(Side::Player);
    recomputeFog();
    if (level_.turnLimit > 0 && turn_ > level_.turnLimit && status_ == Status::Playing) {
      status_ = level_.win == WinCondition::Survive ? Status::Victory : Status::Defeat;
    }
    return none;
  }
  return executeAiUnit(idx);
}

void Game::computeApproachField(const bool forFlyer) {
  std::memset(approach_, FIELD_MAX, sizeof(approach_));
  for (int i = 0; i < unitCount_; i++) {
    const Unit& u = units_[i];
    if (u.alive && u.side == Side::Player) approach_[tileIndex(u.x, u.y)] = 0;
  }
  static constexpr int8_t DX[4] = {1, -1, 0, 0};
  static constexpr int8_t DY[4] = {0, 0, 1, -1};
  bool changed = true;
  // Full propagation across the largest map is bounded by W+H relax passes;
  // the changed flag exits far earlier on real maps.
  for (int pass = 0; pass < MAX_MAP_W + MAX_MAP_H && changed; pass++) {
    changed = false;
    for (int y = 0; y < level_.height; y++) {
      for (int x = 0; x < level_.width; x++) {
        const uint8_t here = approach_[tileIndex(x, y)];
        if (here >= FIELD_MAX) continue;
        for (int d = 0; d < 4; d++) {
          const int nx = x + DX[d];
          const int ny = y + DY[d];
          if (nx < 0 || ny < 0 || nx >= level_.width || ny >= level_.height) continue;
          const Terrain t = level_.at(nx, ny);
          int cost;
          if (forFlyer) {
            cost = t == Terrain::Void ? 0 : 1;
          } else {
            cost = TERRAIN_INFO[static_cast<int>(t)].moveCost;
          }
          if (cost == 0) continue;
          const int total = here + cost;
          if (total < approach_[tileIndex(nx, ny)] && total < FIELD_MAX) {
            approach_[tileIndex(nx, ny)] = static_cast<uint8_t>(total);
            changed = true;
          }
        }
      }
    }
  }
  approachValid_ = true;
  approachForFlyer_ = forFlyer;
}

Game::AiAction Game::executeAiUnit(const int unitIdx) {
  Unit& u = units_[unitIdx];
  AiAction act{};
  act.acted = true;
  act.unit = static_cast<int8_t>(unitIdx);
  act.fromX = u.x;
  act.fromY = u.y;
  act.toX = u.x;
  act.toY = u.y;
  act.target = -1;

  // Dormant guards only wake when the player gets close.
  if (!u.aggro) {
    for (int i = 0; i < unitCount_; i++) {
      const Unit& p = units_[i];
      if (p.alive && p.side == Side::Player && manhattan(u.x, u.y, p.x, p.y) <= level_.aggroRadius) {
        u.aggro = true;
        break;
      }
    }
  }
  if (!u.aggro) {
    u.acted = true;
    act.visible = isVisible(u.x, u.y);
    return act;
  }

  computeReach(unitIdx);

  // Score every (destination, target) pair.
  int bestScore = -1000000;
  int bestX = u.x, bestY = u.y;
  int bestTarget = -1;
  for (int y = 0; y < level_.height; y++) {
    for (int x = 0; x < level_.width; x++) {
      if (reach_[tileIndex(x, y)] == UNREACHABLE || !isReachable(x, y)) continue;
      const int ownDef = defenseAt(u, x, y);
      for (int i = 0; i < unitCount_; i++) {
        const Unit& p = units_[i];
        if (!p.alive || p.side != Side::Player) continue;
        const int dist = manhattan(x, y, p.x, p.y);
        if (dist < 1 || dist > u.info().range) continue;
        const int dmgRaw = u.info().attack * (100 - defenseAt(p, p.x, p.y)) / 100;
        const int dmg = dmgRaw < 1 ? 1 : dmgRaw;
        const bool kills = dmg >= p.hp;
        int counter = 0;
        if (!kills && dist <= p.info().range) {
          const int c = p.info().attack * (100 - ownDef) / 100;
          counter = c < 1 ? 1 : c;
        }
        int score = dmg * 10 + ownDef;
        if (kills) score += 500;
        score -= counter * 7;
        if (counter >= u.hp) score -= 400;  // don't suicide into counters
        if (score > bestScore) {
          bestScore = score;
          bestX = x;
          bestY = y;
          bestTarget = i;
        }
      }
    }
  }

  if (bestTarget >= 0) {
    u.x = static_cast<int8_t>(bestX);
    u.y = static_cast<int8_t>(bestY);
    act.toX = u.x;
    act.toY = u.y;
    const Forecast f = forecast(unitIdx, bestTarget);
    act.target = static_cast<int8_t>(bestTarget);
    act.damage = f.damageToTarget;
    act.counter = f.counterDamage;
    act.targetDied = f.targetDies;
    act.attackerDied = f.attackerDies;
    const Unit& t = units_[bestTarget];
    act.visible = isVisible(act.fromX, act.fromY) || isVisible(u.x, u.y) || isVisible(t.x, t.y);
    attack(unitIdx, bestTarget);
    return act;
  }

  // Nothing in reach: close in on the nearest player unit.
  if (!approachValid_ || approachForFlyer_ != u.info().flies) {
    computeApproachField(u.info().flies);
  }
  int bestField = approach_[tileIndex(u.x, u.y)];
  int bestDef = defenseAt(u, u.x, u.y);
  for (int y = 0; y < level_.height; y++) {
    for (int x = 0; x < level_.width; x++) {
      if (reach_[tileIndex(x, y)] == UNREACHABLE || !isReachable(x, y)) continue;
      const int field = approach_[tileIndex(x, y)];
      const int def = defenseAt(u, x, y);
      if (field < bestField || (field == bestField && def > bestDef)) {
        bestField = field;
        bestDef = def;
        bestX = x;
        bestY = y;
      }
    }
  }
  u.x = static_cast<int8_t>(bestX);
  u.y = static_cast<int8_t>(bestY);
  u.acted = true;
  act.toX = u.x;
  act.toY = u.y;
  act.visible = isVisible(act.fromX, act.fromY) || isVisible(u.x, u.y);
  return act;
}

}  // namespace Tactics
