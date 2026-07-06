#pragma once
// Core data model for the tactics mini-game. This header (and the whole
// lib/Tactics library) is deliberately hardware-free: no Arduino, no HAL,
// no logging. It is compiled both into the firmware and into the host-side
// gtest/simulator builds, which is how the game rules get verified without
// a device.
#include <cstdint>
#include <cstring>

namespace Tactics {

// Map geometry limits. Maps can be much larger than the visible screen —
// the activity scrolls a viewport over them. 48x32 tiles at one byte each
// keeps the whole board under 2KB.
constexpr int MAX_MAP_W = 48;
constexpr int MAX_MAP_H = 32;
constexpr int MAX_TILES = MAX_MAP_W * MAX_MAP_H;
constexpr int MAX_UNITS = 32;
constexpr int MAX_NAME_LEN = 48;

enum class Terrain : uint8_t {
  Void = 0,  // outside the playable map shape; impassable, drawn blank
  Plain,
  Forest,
  Mountain,
  Water,
  Rock,       // impassable wall
  Castle,     // defensive, heals occupant at turn start
  EnemyBase,  // capture target for WinCondition::CaptureBase
};
constexpr int TERRAIN_COUNT = 8;

struct TerrainInfo {
  char mapChar;      // character used in level files
  uint8_t moveCost;  // 0 = impassable (for non-flyers)
  int8_t defense;    // % damage reduction granted to occupant
  uint8_t heal;      // HP restored at turn start when standing here
};

// Indexed by Terrain. Kept in flash via constexpr.
constexpr TerrainInfo TERRAIN_INFO[TERRAIN_COUNT] = {
    // char cost def heal
    {' ', 0, 0, 0},   // Void
    {'.', 1, 0, 0},   // Plain
    {'f', 2, 15, 0},  // Forest
    {'^', 3, 25, 0},  // Mountain
    {'~', 0, 0, 0},   // Water (flyers only)
    {'#', 0, 0, 0},   // Rock
    {'C', 1, 30, 4},  // Castle
    {'B', 1, 20, 0},  // EnemyBase
};

enum class Race : uint8_t { Human = 0, Elf, Dwarf, Dragonoid, Monster };

enum class Side : uint8_t { Player = 0, Enemy = 1 };

struct UnitType {
  const char* id;  // stable identifier used in level files, e.g. "elf_archer"
  char glyph[3];   // two-character tile label, e.g. "EA"
  Race race;
  uint8_t maxHp;
  uint8_t move;    // movement points per turn
  uint8_t attack;  // base damage
  uint8_t range;   // max attack range in tiles (min is always 1)
  int8_t defense;  // innate % damage reduction
  uint8_t vision;  // fog-of-war sight radius (manhattan)
  uint8_t regen;   // HP restored at turn start regardless of terrain
  bool flies;      // ignores terrain costs, water, and zones of control
};

// The full bestiary. Elves get +15 defense in forest, dwarves +15 in
// mountains and pay only 1 movement on them; flyers never get terrain
// defense (they hover above it). Those racial rules live in rules code,
// not in this table.
constexpr UnitType UNIT_TYPES[] = {
    // id                glyph  race              hp  mv atk rng def vis rgn fly
    {"human_soldier", {'H', 'S', 0}, Race::Human, 30, 5, 8, 1, 10, 4, 0, false},
    {"human_archer", {'H', 'A', 0}, Race::Human, 24, 5, 7, 2, 5, 5, 0, false},
    {"human_knight", {'H', 'K', 0}, Race::Human, 38, 7, 10, 1, 15, 4, 0, false},
    {"elf_fighter", {'E', 'F', 0}, Race::Elf, 28, 6, 8, 1, 10, 4, 0, false},
    {"elf_archer", {'E', 'A', 0}, Race::Elf, 22, 6, 9, 2, 5, 5, 0, false},
    {"elf_mage", {'E', 'M', 0}, Race::Elf, 20, 5, 11, 2, 0, 4, 0, false},
    {"dwarf_warrior", {'D', 'W', 0}, Race::Dwarf, 36, 4, 10, 1, 20, 4, 0, false},
    {"dwarf_thunderer", {'D', 'T', 0}, Race::Dwarf, 28, 4, 12, 2, 10, 4, 0, false},
    {"dwarf_guard", {'D', 'G', 0}, Race::Dwarf, 42, 3, 7, 1, 30, 4, 0, false},
    {"drake_clasher", {'R', 'C', 0}, Race::Dragonoid, 32, 5, 10, 1, 15, 4, 0, false},
    {"drake_flare", {'R', 'F', 0}, Race::Dragonoid, 26, 5, 10, 2, 5, 5, 0, false},
    {"drake_flyer", {'R', 'Y', 0}, Race::Dragonoid, 34, 8, 11, 1, 10, 6, 0, true},
    {"goblin", {'g', 'b', 0}, Race::Monster, 18, 5, 6, 1, 0, 4, 0, false},
    {"orc", {'o', 'c', 0}, Race::Monster, 30, 5, 9, 1, 10, 4, 0, false},
    {"troll", {'t', 'r', 0}, Race::Monster, 44, 4, 10, 1, 10, 4, 4, false},
    {"wyvern", {'w', 'y', 0}, Race::Monster, 30, 7, 10, 1, 5, 6, 0, true},
};
constexpr int UNIT_TYPE_COUNT = static_cast<int>(sizeof(UNIT_TYPES) / sizeof(UNIT_TYPES[0]));

// Returns the UNIT_TYPES index for a level-file id, or -1 if unknown.
inline int unitTypeFromId(const char* id) {
  for (int i = 0; i < UNIT_TYPE_COUNT; i++) {
    if (std::strcmp(UNIT_TYPES[i].id, id) == 0) return i;
  }
  return -1;
}

inline Terrain terrainFromChar(const char c) {
  for (int i = 0; i < TERRAIN_COUNT; i++) {
    if (TERRAIN_INFO[i].mapChar == c) return static_cast<Terrain>(i);
  }
  return Terrain::Plain;  // unknown characters read as plain ground
}

enum class WinCondition : uint8_t {
  DefeatAll = 0,  // kill every enemy unit
  CaptureBase,    // move a unit onto the EnemyBase tile (find it first!)
  Survive,        // still alive after `turnLimit` turns
};

// A unit placement as written in the level file.
struct UnitDef {
  uint8_t type;  // index into UNIT_TYPES
  Side side;
  int8_t x;
  int8_t y;
};

// A fully parsed level. ~1.8KB; allocate on the heap, never on the stack.
struct Level {
  char name[MAX_NAME_LEN];
  int8_t width;
  int8_t height;
  WinCondition win;
  uint8_t turnLimit;    // 0 = unlimited (required for Survive)
  uint8_t aggroRadius;  // monsters wake when a player unit gets this close
  uint8_t tiles[MAX_TILES];
  uint8_t unitCount;
  UnitDef units[MAX_UNITS];

  Terrain at(const int x, const int y) const {
    if (x < 0 || y < 0 || x >= width || y >= height) return Terrain::Void;
    return static_cast<Terrain>(tiles[y * MAX_MAP_W + x]);
  }
};

// A live unit during play.
struct Unit {
  uint8_t type;  // index into UNIT_TYPES
  Side side;
  int8_t x;
  int8_t y;
  int8_t hp;
  bool acted;  // has moved/attacked this turn
  bool alive;
  bool aggro;  // monsters only: has noticed the player

  const UnitType& info() const { return UNIT_TYPES[type]; }
};

}  // namespace Tactics
