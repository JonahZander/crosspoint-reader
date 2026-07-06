#pragma once
// Parser for user-authored level files. Accepts a friendly YAML subset so
// levels can be written by hand and shared as plain text:
//
//   name: Ambush at Thornwood
//   win: capture_base          # defeat_all | capture_base | survive
//   turns: 24                  # optional turn limit (required for survive)
//   aggro: 7                   # optional monster wake-up radius
//   map: |
//     ....ff....^^^
//     ...ff...C..^B
//   units:
//     - {type: elf_archer, at: [3, 1], side: player}
//     - {type: troll, at: [11, 0], side: enemy}
//
// Terrain characters: . plain  f forest  ^ mountain  ~ water  # rock
//                     C castle  B enemy base  (space) void
// Full YAML is NOT supported — only the shape above, plus '#' comments
// outside the map block and blank lines anywhere.
#include <cstddef>

#include "TacticsTypes.h"

namespace Tactics {

// Parses `text` (need not be null-terminated beyond `len`) into `out`.
// On failure returns false and writes a short human-readable reason plus
// the offending line number into errOut (if provided).
bool parseLevel(const char* text, size_t len, Level& out, char* errOut = nullptr, size_t errLen = 0);

}  // namespace Tactics
