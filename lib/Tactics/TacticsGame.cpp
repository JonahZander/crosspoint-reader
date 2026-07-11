#include "TacticsGame.h"

#include <cstring>
#include <type_traits>

namespace Tactics {

namespace {
constexpr uint8_t UNREACHABLE = 0xFF;
constexpr int MAX_DEFENSE = 60;  // cap on stacked % damage reduction
constexpr int MOUNTAIN_VISION_BONUS = 2;
constexpr int PACK_WAKE_RADIUS = 3;
}  // namespace

void Game::start(const Level& level) {
  level_ = level;
  unitCount_ = level.unitCount;
  for (int i = 0; i < unitCount_; i++) {
    const UnitDef& def = level.units[i];
    Unit& u = units_[i];
    u.type = def.type;
    u.side = def.side;
    u.x = def.x;
    u.y = def.y;
    u.hp = static_cast<int8_t>(u.info().maxHp);
    u.acted = false;
    u.alive = true;
    u.aggro = false;
  }
  turn_ = 1;
  status_ = Status::Playing;
  phase_ = Phase::Player;
  reachUnit_ = -1;
  approachValid_ = false;
  std::memset(explored_, 0, sizeof(explored_));
  std::memset(danger_, 0, sizeof(danger_));
  recomputeFog();
}

int Game::unitIndexAt(const int x, const int y) const {
  for (int i = 0; i < unitCount_; i++) {
    if (units_[i].alive && units_[i].x == x && units_[i].y == y) return i;
  }
  return -1;
}

bool Game::isVisible(const int x, const int y) const {
  if (x < 0 || y < 0 || x >= level_.width || y >= level_.height) return false;
  const int idx = tileIndex(x, y);
  return (visible_[idx / 8] >> (idx % 8)) & 1;
}

bool Game::isExplored(const int x, const int y) const {
  if (x < 0 || y < 0 || x >= level_.width || y >= level_.height) return false;
  const int idx = tileIndex(x, y);
  return (explored_[idx / 8] >> (idx % 8)) & 1;
}

void Game::computeDangerZones(const int onlyUnit) {
  std::memset(danger_, 0, sizeof(danger_));
  for (int i = 0; i < unitCount_; i++) {
    const Unit& e = units_[i];
    if (!e.alive || e.side != Side::Enemy) continue;
    if (onlyUnit >= 0) {
      // Hover mode: exactly this enemy, even if dormant — the player is
      // asking "what could this one do if it woke up".
      if (i != onlyUnit) continue;
    } else {
      if (!isVisible(e.x, e.y)) continue;  // player can't know hidden threats
      // Awake = already aggro'd, or the aggro radius would wake it now.
      bool awake = e.aggro;
      for (int p = 0; p < unitCount_ && !awake; p++) {
        const Unit& pl = units_[p];
        if (pl.alive && pl.side == Side::Player && manhattan(e.x, e.y, pl.x, pl.y) <= level_.aggroRadius) {
          awake = true;
        }
      }
      if (!awake) continue;
    }
    computeReach(i);
    const int range = e.info().range;
    for (int y = 0; y < level_.height; y++) {
      for (int x = 0; x < level_.width; x++) {
        const bool standable = (x == e.x && y == e.y) || (reach_[tileIndex(x, y)] != UNREACHABLE && isReachable(x, y));
        if (!standable) continue;
        for (int dy = -range; dy <= range; dy++) {
          for (int dx = -range; dx <= range; dx++) {
            const int adx = dx < 0 ? -dx : dx;
            const int ady = dy < 0 ? -dy : dy;
            if (adx + ady < 1 || adx + ady > range) continue;
            const int tx = x + dx;
            const int ty = y + dy;
            if (tx < 0 || ty < 0 || tx >= level_.width || ty >= level_.height) continue;
            const int idx = tileIndex(tx, ty);
            danger_[idx / 8] = static_cast<uint8_t>(danger_[idx / 8] | (1 << (idx % 8)));
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

int Game::moveCostFor(const Unit& u, const int x, const int y) const {
  const Terrain t = level_.at(x, y);
  if (t == Terrain::Void) return 0;
  if (u.info().flies) return 1;  // flyers glide over everything but the void
  const int cost = TERRAIN_INFO[static_cast<int>(t)].moveCost;
  if (cost == 0) return 0;
  // Dwarves are at home in the mountains.
  if (t == Terrain::Mountain && u.info().race == Race::Dwarf) return 1;
  return cost;
}

bool Game::inEnemyZoc(const Unit& u, const int x, const int y) const {
  if (u.info().flies) return false;
  static constexpr int8_t DX[4] = {1, -1, 0, 0};
  static constexpr int8_t DY[4] = {0, 0, 1, -1};
  for (int d = 0; d < 4; d++) {
    const int n = unitIndexAt(x + DX[d], y + DY[d]);
    if (n >= 0 && units_[n].side != u.side) return true;
  }
  return false;
}

void Game::computeReach(const int unitIdx) {
  std::memset(reach_, UNREACHABLE, sizeof(reach_));
  reachUnit_ = static_cast<int8_t>(unitIdx);
  const Unit& u = units_[unitIdx];
  const int budget = u.info().move;
  reach_[tileIndex(u.x, u.y)] = 0;

  // Cost-limited relaxation: `budget` passes are enough because every step
  // costs at least 1. Cheap and allocation-free (~50k ops worst case).
  static constexpr int8_t DX[4] = {1, -1, 0, 0};
  static constexpr int8_t DY[4] = {0, 0, 1, -1};
  for (int pass = 0; pass < budget; pass++) {
    bool changed = false;
    for (int y = 0; y < level_.height; y++) {
      for (int x = 0; x < level_.width; x++) {
        const uint8_t here = reach_[tileIndex(x, y)];
        if (here >= budget) continue;
        // A tile in an enemy zone of control swallows all movement:
        // you can step in, but not through.
        if (here > 0 && inEnemyZoc(u, x, y)) continue;
        for (int d = 0; d < 4; d++) {
          const int nx = x + DX[d];
          const int ny = y + DY[d];
          if (nx < 0 || ny < 0 || nx >= level_.width || ny >= level_.height) continue;
          const int cost = moveCostFor(u, nx, ny);
          if (cost == 0) continue;
          // Enemy units block; friendly units can be passed through.
          const int occupant = unitIndexAt(nx, ny);
          if (occupant >= 0 && units_[occupant].side != u.side) continue;
          const int total = here + cost;
          if (total <= budget && total < reach_[tileIndex(nx, ny)]) {
            reach_[tileIndex(nx, ny)] = static_cast<uint8_t>(total);
            changed = true;
          }
        }
      }
    }
    if (!changed) break;
  }
}

bool Game::isReachable(const int x, const int y) const {
  if (x < 0 || y < 0 || x >= level_.width || y >= level_.height) return false;
  if (reach_[tileIndex(x, y)] == UNREACHABLE) return false;
  // Can't end movement on an occupied tile (self excluded).
  const int occupant = unitIndexAt(x, y);
  return occupant < 0 || occupant == reachUnit_;
}

bool Game::moveUnit(const int unitIdx, const int x, const int y) {
  Unit& u = units_[unitIdx];
  if (!u.alive || u.acted || status_ != Status::Playing) return false;
  if (reachUnit_ != unitIdx) computeReach(unitIdx);
  if (!isReachable(x, y)) return false;
  // Parting blows: disengaging from an adjacent foe lets it strike once
  // for half damage (defense of the tile being left; no flank bonus).
  // Flyers disengage freely — ZoC never bound them in the first place —
  // and dormant monsters sleep through it.
  if (!u.info().flies && (x != u.x || y != u.y)) {
    for (int i = 0; i < unitCount_ && u.alive; i++) {
      const Unit& e = units_[i];
      if (!e.alive || e.side == u.side) continue;
      // Awake matches the danger-overlay definition: aggro'd, or woken by
      // the level's radius (an adjacent unit is within any radius >= 1).
      if (e.side == Side::Enemy && !e.aggro && level_.aggroRadius < 1) continue;
      if (manhattan(e.x, e.y, u.x, u.y) != 1) continue;  // engaged now
      if (manhattan(e.x, e.y, x, y) <= 1) continue;      // still engaged after
      const int def = defenseAt(u, u.x, u.y);
      const int hit = e.info().attack * (100 - def) / 200;
      u.hp = static_cast<int8_t>(u.hp - (hit < 1 ? 1 : hit));
      if (u.hp <= 0) u.alive = false;
    }
    if (!u.alive) {  // cut down mid-retreat; the move never completes
      recomputeFog();
      checkVictory();
      return true;
    }
  }
  u.x = static_cast<int8_t>(x);
  u.y = static_cast<int8_t>(y);
  if (u.side == Side::Player) {
    recomputeFog();
    if (level_.win == WinCondition::CaptureBase && level_.at(x, y) == Terrain::EnemyBase) {
      status_ = Status::Victory;
    }
  }
  return true;
}

bool Game::undoMove(const int unitIdx, const int x, const int y) {
  // Moves auto-commit, so undo also un-commits (`acted` is cleared). The
  // caller must enforce the gameplay rules: never undo an attack, a move
  // that revealed new enemies, or anything after the enemy phase started.
  Unit& u = units_[unitIdx];
  if (!u.alive || status_ != Status::Playing) return false;
  if (x < 0 || y < 0 || x >= level_.width || y >= level_.height) return false;
  u.x = static_cast<int8_t>(x);
  u.y = static_cast<int8_t>(y);
  u.acted = false;
  reachUnit_ = -1;  // invalidate the movement cache
  if (u.side == Side::Player) recomputeFog();
  return true;
}

int Game::targetsFrom(const int unitIdx, const int x, const int y, int8_t* out, const int maxOut) const {
  const Unit& u = units_[unitIdx];
  int n = 0;
  for (int i = 0; i < unitCount_ && n < maxOut; i++) {
    const Unit& other = units_[i];
    if (!other.alive || other.side == u.side) continue;
    const int dist = manhattan(x, y, other.x, other.y);
    if (dist >= 1 && dist <= u.info().range) out[n++] = static_cast<int8_t>(i);
  }
  return n;
}

int Game::defenseAt(const Unit& u, const int x, const int y) const {
  int def = u.info().defense;
  if (!u.info().flies) {
    const Terrain t = level_.at(x, y);
    def += TERRAIN_INFO[static_cast<int>(t)].defense;
    // Racial terrain affinities.
    if (t == Terrain::Forest && u.info().race == Race::Elf) def += 15;
    if (t == Terrain::Mountain && u.info().race == Race::Dwarf) def += 15;
  }
  return def > MAX_DEFENSE ? MAX_DEFENSE : def;
}

bool Game::flanks(const Unit& attacker, const Unit& defender) const {
  if (manhattan(attacker.x, attacker.y, defender.x, defender.y) != 1) return false;
  const int occ = unitIndexAt(2 * defender.x - attacker.x, 2 * defender.y - attacker.y);
  return occ >= 0 && units_[occ].side == attacker.side;
}

int Game::damage(const Unit& attacker, const Unit& defender) const {
  const int def = defenseAt(defender, defender.x, defender.y);
  int dmg = attacker.info().attack * (100 - def) / 100;
  if (flanks(attacker, defender)) dmg = dmg * 3 / 2;  // pincer bonus
  return dmg < 1 ? 1 : dmg;
}

Game::Forecast Game::forecast(const int attackerIdx, const int targetIdx) const {
  const Unit& a = units_[attackerIdx];
  const Unit& t = units_[targetIdx];
  Forecast f{};
  f.damageToTarget = static_cast<int8_t>(damage(a, t));
  f.flank = flanks(a, t);
  f.targetDies = f.damageToTarget >= t.hp;
  // The defender strikes back only if it survives and the attacker is
  // within its own reach — so archers poking melee from afar are safe.
  const int dist = manhattan(a.x, a.y, t.x, t.y);
  if (!f.targetDies && dist <= t.info().range) {
    f.counterDamage = static_cast<int8_t>(damage(t, a));
    f.attackerDies = f.counterDamage >= a.hp;
  }
  return f;
}

bool Game::attack(const int attackerIdx, const int targetIdx) {
  Unit& a = units_[attackerIdx];
  Unit& t = units_[targetIdx];
  if (!a.alive || !t.alive || a.acted || a.side == t.side) return false;
  const int dist = manhattan(a.x, a.y, t.x, t.y);
  if (dist < 1 || dist > a.info().range) return false;

  const Forecast f = forecast(attackerIdx, targetIdx);
  t.hp = static_cast<int8_t>(t.hp - f.damageToTarget);
  if (t.hp <= 0) {
    t.alive = false;
  } else if (f.counterDamage > 0) {
    a.hp = static_cast<int8_t>(a.hp - f.counterDamage);
    if (a.hp <= 0) a.alive = false;
  }
  a.acted = true;

  // Combat wakes the neighbourhood.
  if (t.side == Side::Enemy) {
    t.aggro = true;
    wakePackNear(t.x, t.y);
  }
  if (a.side == Side::Enemy) a.aggro = true;

  approachValid_ = false;  // unit deaths change the AI's target field
  recomputeFog();
  checkVictory();
  return true;
}

void Game::finishUnit(const int unitIdx) { units_[unitIdx].acted = true; }

bool Game::allPlayerUnitsActed() const {
  for (int i = 0; i < unitCount_; i++) {
    if (units_[i].alive && units_[i].side == Side::Player && !units_[i].acted) return false;
  }
  return true;
}

void Game::endPlayerTurn() {
  if (status_ != Status::Playing) return;
  phase_ = Phase::Enemy;
  approachValid_ = false;
  for (int i = 0; i < unitCount_; i++) {
    if (units_[i].side == Side::Enemy) units_[i].acted = false;
  }
  applyUpkeep(Side::Enemy);
}

void Game::applyUpkeep(const Side side) {
  for (int i = 0; i < unitCount_; i++) {
    Unit& u = units_[i];
    if (!u.alive || u.side != side) continue;
    int heal = u.info().regen;
    if (!u.info().flies) {
      heal += TERRAIN_INFO[static_cast<int>(level_.at(u.x, u.y))].heal;
    }
    if (heal > 0) {
      const int maxHp = u.info().maxHp;
      u.hp = static_cast<int8_t>(u.hp + heal > maxHp ? maxHp : u.hp + heal);
    }
  }
}

void Game::recomputeFog() {
  std::memset(visible_, 0, sizeof(visible_));
  for (int i = 0; i < unitCount_; i++) {
    const Unit& u = units_[i];
    if (!u.alive || u.side != Side::Player) continue;
    int vision = u.info().vision;
    if (level_.at(u.x, u.y) == Terrain::Mountain) vision += MOUNTAIN_VISION_BONUS;
    const int x0 = u.x - vision < 0 ? 0 : u.x - vision;
    const int x1 = u.x + vision >= level_.width ? level_.width - 1 : u.x + vision;
    const int y0 = u.y - vision < 0 ? 0 : u.y - vision;
    const int y1 = u.y + vision >= level_.height ? level_.height - 1 : u.y + vision;
    for (int y = y0; y <= y1; y++) {
      for (int x = x0; x <= x1; x++) {
        if (manhattan(u.x, u.y, x, y) > vision) continue;
        const int idx = tileIndex(x, y);
        visible_[idx / 8] = static_cast<uint8_t>(visible_[idx / 8] | (1 << (idx % 8)));
        explored_[idx / 8] = static_cast<uint8_t>(explored_[idx / 8] | (1 << (idx % 8)));
      }
    }
  }
}

void Game::checkVictory() {
  if (status_ != Status::Playing) return;
  bool anyPlayer = false;
  bool anyEnemy = false;
  for (int i = 0; i < unitCount_; i++) {
    if (!units_[i].alive) continue;
    if (units_[i].side == Side::Player) anyPlayer = true;
    if (units_[i].side == Side::Enemy) anyEnemy = true;
  }
  if (!anyPlayer) {
    status_ = Status::Defeat;
  } else if (!anyEnemy && level_.win == WinCondition::DefeatAll) {
    status_ = Status::Victory;
  }
}

void Game::exportState(void* out) const {
  static_assert(std::is_trivially_copyable<Game>::value, "raw snapshot requires trivially copyable Game");
  std::memcpy(out, this, sizeof(Game));
}

bool Game::importState(const void* in) {
  // Validate in place before adopting: all fields read pre-copy are
  // byte-sized or bounds-checked ints, and callers pass new-aligned
  // buffers, so the cast read is alignment-safe on RISC-V.
  const Game* src = static_cast<const Game*>(in);
  if (src->unitCount_ < 1 || src->unitCount_ > MAX_UNITS) return false;
  if (src->level_.width < 1 || src->level_.width > MAX_MAP_W) return false;
  if (src->level_.height < 1 || src->level_.height > MAX_MAP_H) return false;
  if (src->status_ != Status::Playing) return false;  // only mid-battle saves exist
  std::memcpy(this, src, sizeof(Game));
  return true;
}

void Game::wakePackNear(const int x, const int y) {
  for (int i = 0; i < unitCount_; i++) {
    Unit& u = units_[i];
    if (u.alive && u.side == Side::Enemy && manhattan(x, y, u.x, u.y) <= PACK_WAKE_RADIUS) {
      u.aggro = true;
    }
  }
}

}  // namespace Tactics
