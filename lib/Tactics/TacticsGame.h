#pragma once
// Rules engine for the tactics mini-game: movement (terrain costs + zones
// of control), deterministic combat with counterattacks, fog of war with
// a persistent "explored" layer, base capture, and the monster AI.
//
// The whole game state lives inside this object (~6KB), so the activity
// allocates exactly one Game on the heap and nothing else. No allocation
// happens inside the engine.
#include <cstddef>

#include "TacticsTypes.h"

namespace Tactics {

class Game {
 public:
  enum class Status : uint8_t { Playing, Victory, Defeat };
  enum class Phase : uint8_t { Player, Enemy };

  // Combat preview shown before the player commits to an attack.
  struct Forecast {
    int8_t damageToTarget;
    int8_t counterDamage;  // 0 when the target cannot strike back
    bool targetDies;
    bool attackerDies;
  };

  // One enemy unit's completed action, for UI/log purposes.
  struct AiAction {
    bool acted;    // false when the enemy phase is over
    bool visible;  // action happened inside the player's current vision
    int8_t unit;
    int8_t fromX, fromY;
    int8_t toX, toY;
    int8_t target;  // -1 when the unit only moved / stayed put
    int8_t damage;
    int8_t counter;
    bool targetDied;
    bool attackerDied;
  };

  void start(const Level& level);

  const Level& level() const { return level_; }
  int turn() const { return turn_; }
  Status status() const { return status_; }
  Phase phase() const { return phase_; }
  const Unit& unit(const int i) const { return units_[i]; }
  int unitCount() const { return unitCount_; }
  // Index of the living unit on (x, y), or -1.
  int unitIndexAt(int x, int y) const;

  // Fog of war (player perspective).
  bool isVisible(int x, int y) const;
  bool isExplored(int x, int y) const;

  // Danger overlay: tiles attackable next turn by enemies that are visible
  // to the player and awake (aggro'd or within the level's aggro radius).
  // Invalidates the computeReach() cache.
  void computeDangerZones();
  bool isDangerous(int x, int y) const;

  // Movement. computeReach() fills the reachability field used by both
  // isReachable() and the movement-range overlay in the UI.
  void computeReach(int unitIdx);
  bool isReachable(int x, int y) const;
  bool moveUnit(int unitIdx, int x, int y);
  // Reverts a pure move: restores the pre-move position, clears `acted`,
  // and recomputes fog (the explored layer stays — it is additive). The
  // caller enforces the gameplay rules: attacks and fog-revealing moves
  // must never be undone.
  bool undoMove(int unitIdx, int x, int y);

  // Combat. Targets are enemies within range of (x, y) for the given unit.
  int targetsFrom(int unitIdx, int x, int y, int8_t* out, int maxOut) const;
  Forecast forecast(int attackerIdx, int targetIdx) const;
  bool attack(int attackerIdx, int targetIdx);

  // Marks a unit as done for this turn without attacking.
  void finishUnit(int unitIdx);
  bool allPlayerUnitsActed() const;

  // Hands the turn to the enemy. Drive the enemy phase by calling
  // aiStep() repeatedly (one unit action per call, so the UI can pace and
  // repaint between actions) until it returns acted == false.
  void endPlayerTurn();
  AiAction aiStep();

  // Effective % damage reduction for a unit standing on (x, y).
  int defenseAt(const Unit& u, int x, int y) const;

  // Whole-state snapshot for battle save/resume. The Game object is
  // trivially copyable (no pointers, no virtuals), so a raw byte copy is
  // the entire serialisation; the caller owns file format/versioning.
  static constexpr size_t stateSize() { return sizeof(Game); }
  void exportState(void* out) const;
  bool importState(const void* in);

 private:
  static int tileIndex(const int x, const int y) { return y * MAX_MAP_W + x; }
  static int manhattan(const int ax, const int ay, const int bx, const int by) {
    const int dx = ax > bx ? ax - bx : bx - ax;
    const int dy = ay > by ? ay - by : by - ay;
    return dx + dy;
  }

  int moveCostFor(const Unit& u, int x, int y) const;  // 0 = impassable
  bool inEnemyZoc(const Unit& u, int x, int y) const;
  int damage(const Unit& attacker, const Unit& defender) const;
  void applyUpkeep(Side side);
  void recomputeFog();
  void checkVictory();
  void wakePackNear(int x, int y);
  // Multi-source distance-to-nearest-player field used by the AI to
  // approach targets it cannot yet attack. Cached per enemy phase.
  void computeApproachField(bool forFlyer);

  // AI internals (implemented in TacticsAi.cpp).
  int nextAiUnit();
  AiAction executeAiUnit(int unitIdx);

  Level level_;
  Unit units_[MAX_UNITS];
  int unitCount_ = 0;
  int turn_ = 1;
  Status status_ = Status::Playing;
  Phase phase_ = Phase::Player;

  uint8_t reach_[MAX_TILES];     // movement cost to tile, 0xFF unreachable
  int8_t reachUnit_ = -1;        // unit computeReach() was run for
  uint8_t approach_[MAX_TILES];  // AI approach field
  bool approachValid_ = false;
  bool approachForFlyer_ = false;
  uint8_t visible_[MAX_TILES / 8];
  uint8_t explored_[MAX_TILES / 8];
  uint8_t danger_[MAX_TILES / 8];
};

}  // namespace Tactics
