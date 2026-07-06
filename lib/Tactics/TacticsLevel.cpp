#include "TacticsLevel.h"

#include <cstdio>
#include <cstring>

namespace Tactics {

namespace {

struct Cursor {
  const char* text;
  size_t len;
  size_t pos = 0;
  int lineNo = 0;

  // Returns the next line (not null-terminated) or false at end of input.
  bool nextLine(const char*& line, size_t& lineLen) {
    if (pos >= len) return false;
    const size_t start = pos;
    while (pos < len && text[pos] != '\n') pos++;
    lineLen = pos - start;
    if (pos < len) pos++;  // consume '\n'
    // Tolerate CRLF files
    if (lineLen > 0 && text[start + lineLen - 1] == '\r') lineLen--;
    line = text + start;
    lineNo++;
    return true;
  }
};

size_t indentOf(const char* line, const size_t len) {
  size_t i = 0;
  while (i < len && line[i] == ' ') i++;
  return i;
}

bool isBlank(const char* line, const size_t len) { return indentOf(line, len) == len; }

// Copies [src, src+n) into dst (capped), trimming surrounding whitespace.
void copyTrimmed(char* dst, const size_t dstLen, const char* src, size_t n) {
  size_t a = 0;
  while (a < n && (src[a] == ' ' || src[a] == '\t')) a++;
  while (n > a && (src[n - 1] == ' ' || src[n - 1] == '\t')) n--;
  size_t outLen = n - a;
  if (outLen >= dstLen) outLen = dstLen - 1;
  std::memcpy(dst, src + a, outLen);
  dst[outLen] = '\0';
}

// If the trimmed line starts with "key:", points value at what follows.
bool matchKey(const char* line, const size_t len, const char* key, const char*& value, size_t& valueLen) {
  const size_t ind = indentOf(line, len);
  const size_t keyLen = std::strlen(key);
  if (len - ind < keyLen + 1) return false;
  if (std::strncmp(line + ind, key, keyLen) != 0) return false;
  if (line[ind + keyLen] != ':') return false;
  value = line + ind + keyLen + 1;
  valueLen = len - ind - keyLen - 1;
  return true;
}

// Extracts the value of `key:` inside a flow mapping like
// "- {type: elf_archer, at: [3, 2], side: player}". Value runs until the
// next ',', '}' or ']' that is outside brackets.
bool flowValue(const char* line, const size_t len, const char* key, char* out, const size_t outLen) {
  const size_t keyLen = std::strlen(key);
  for (size_t i = 0; i + keyLen + 1 <= len; i++) {
    if (std::strncmp(line + i, key, keyLen) != 0 || line[i + keyLen] != ':') continue;
    // Reject partial matches like "subtype:"
    if (i > 0 && line[i - 1] != ' ' && line[i - 1] != ',' && line[i - 1] != '{') continue;
    size_t start = i + keyLen + 1;
    int depth = 0;
    size_t end = start;
    while (end < len) {
      const char c = line[end];
      if (c == '[') depth++;
      if (c == ']' && depth > 0) {
        depth--;
        end++;
        continue;
      }
      if (depth == 0 && (c == ',' || c == '}' || c == ']')) break;
      end++;
    }
    copyTrimmed(out, outLen, line + start, end - start);
    return true;
  }
  return false;
}

// Parses "[3, 12]" (brackets optional) into x/y.
bool parsePair(const char* s, int& x, int& y) {
  int vals[2] = {0, 0};
  int found = 0;
  bool inNum = false;
  bool neg = false;
  int cur = 0;
  for (const char* p = s;; p++) {
    const char c = *p;
    if (c >= '0' && c <= '9') {
      if (!inNum) {
        inNum = true;
        cur = 0;
      }
      cur = cur * 10 + (c - '0');
    } else {
      if (inNum) {
        if (found < 2) vals[found] = neg ? -cur : cur;
        found++;
        inNum = false;
        neg = false;
      }
      if (c == '-') neg = true;
      if (c == '\0') break;
    }
  }
  if (found != 2) return false;
  x = vals[0];
  y = vals[1];
  return true;
}

int parseInt(const char* s, const int fallback) {
  int v = 0;
  bool any = false;
  for (const char* p = s; *p; p++) {
    if (*p >= '0' && *p <= '9') {
      v = v * 10 + (*p - '0');
      any = true;
    } else if (any) {
      break;
    }
  }
  return any ? v : fallback;
}

void fail(char* errOut, const size_t errLen, const int lineNo, const char* msg) {
  if (errOut && errLen > 0) std::snprintf(errOut, errLen, "line %d: %s", lineNo, msg);
}

// Strips a trailing "# comment" (never called on map rows, where '#' is rock).
size_t stripComment(const char* line, const size_t len) {
  for (size_t i = 0; i < len; i++) {
    if (line[i] == '#') return i;
  }
  return len;
}

}  // namespace

bool parseLevel(const char* text, const size_t len, Level& out, char* errOut, const size_t errLen) {
  std::memset(&out, 0, sizeof(Level));
  out.win = WinCondition::DefeatAll;
  out.aggroRadius = 7;
  std::strncpy(out.name, "Unnamed", MAX_NAME_LEN - 1);

  Cursor cur{text, len};
  const char* line;
  size_t lineLen;

  enum class Section { TopLevel, Map, Units };
  Section section = Section::TopLevel;
  size_t mapIndent = 0;
  size_t rowIndent = 0;
  int mapRow = 0;
  int mapWidth = 0;
  bool sawBase = false;
  char buf[MAX_NAME_LEN];

  while (cur.nextLine(line, lineLen)) {
    if (section == Section::Map) {
      // Map rows are every non-blank line indented deeper than "map:".
      // '#' is the rock tile here, so no comment stripping.
      if (!isBlank(line, lineLen) && indentOf(line, lineLen) > mapIndent) {
        if (mapRow >= MAX_MAP_H) {
          fail(errOut, errLen, cur.lineNo, "map is too tall");
          return false;
        }
        // The first row fixes the indentation of the whole block.
        if (mapRow == 0) rowIndent = indentOf(line, lineLen);
        const size_t start = rowIndent < lineLen ? rowIndent : lineLen;
        const int rowLen = static_cast<int>(lineLen - start);
        if (rowLen > MAX_MAP_W) {
          fail(errOut, errLen, cur.lineNo, "map is too wide");
          return false;
        }
        for (int x = 0; x < rowLen; x++) {
          const Terrain t = line[start + x] == ' ' ? Terrain::Void : terrainFromChar(line[start + x]);
          if (t == Terrain::EnemyBase) sawBase = true;
          out.tiles[mapRow * MAX_MAP_W + x] = static_cast<uint8_t>(t);
        }
        if (rowLen > mapWidth) mapWidth = rowLen;
        mapRow++;
        continue;
      }
      if (isBlank(line, lineLen)) continue;  // blank lines don't end the block
      section = Section::TopLevel;           // dedented line: fall through
    }

    const size_t effLen = stripComment(line, lineLen);
    if (isBlank(line, effLen)) continue;

    const char* value;
    size_t valueLen;
    if (section != Section::Units && matchKey(line, effLen, "name", value, valueLen)) {
      copyTrimmed(out.name, MAX_NAME_LEN, value, valueLen);
    } else if (matchKey(line, effLen, "win", value, valueLen)) {
      copyTrimmed(buf, sizeof(buf), value, valueLen);
      if (std::strcmp(buf, "defeat_all") == 0) {
        out.win = WinCondition::DefeatAll;
      } else if (std::strcmp(buf, "capture_base") == 0) {
        out.win = WinCondition::CaptureBase;
      } else if (std::strcmp(buf, "survive") == 0) {
        out.win = WinCondition::Survive;
      } else {
        fail(errOut, errLen, cur.lineNo, "win must be defeat_all, capture_base or survive");
        return false;
      }
    } else if (matchKey(line, effLen, "turns", value, valueLen)) {
      copyTrimmed(buf, sizeof(buf), value, valueLen);
      out.turnLimit = static_cast<uint8_t>(parseInt(buf, 0));
    } else if (matchKey(line, effLen, "aggro", value, valueLen)) {
      copyTrimmed(buf, sizeof(buf), value, valueLen);
      out.aggroRadius = static_cast<uint8_t>(parseInt(buf, 7));
    } else if (matchKey(line, effLen, "map", value, valueLen)) {
      section = Section::Map;
      mapIndent = indentOf(line, lineLen);
    } else if (matchKey(line, effLen, "units", value, valueLen)) {
      section = Section::Units;
    } else if (section == Section::Units) {
      const size_t ind = indentOf(line, effLen);
      if (ind >= effLen || line[ind] != '-') {
        fail(errOut, errLen, cur.lineNo, "expected '- {type: ..., at: [x, y], side: ...}'");
        return false;
      }
      if (out.unitCount >= MAX_UNITS) {
        fail(errOut, errLen, cur.lineNo, "too many units");
        return false;
      }
      UnitDef& def = out.units[out.unitCount];
      if (!flowValue(line, effLen, "type", buf, sizeof(buf))) {
        fail(errOut, errLen, cur.lineNo, "unit is missing type:");
        return false;
      }
      const int type = unitTypeFromId(buf);
      if (type < 0) {
        fail(errOut, errLen, cur.lineNo, "unknown unit type");
        return false;
      }
      def.type = static_cast<uint8_t>(type);
      if (!flowValue(line, effLen, "at", buf, sizeof(buf))) {
        fail(errOut, errLen, cur.lineNo, "unit is missing at: [x, y]");
        return false;
      }
      int x, y;
      if (!parsePair(buf, x, y)) {
        fail(errOut, errLen, cur.lineNo, "at: must be [x, y]");
        return false;
      }
      def.x = static_cast<int8_t>(x);
      def.y = static_cast<int8_t>(y);
      if (!flowValue(line, effLen, "side", buf, sizeof(buf))) {
        fail(errOut, errLen, cur.lineNo, "unit is missing side:");
        return false;
      }
      if (std::strcmp(buf, "player") == 0) {
        def.side = Side::Player;
      } else if (std::strcmp(buf, "enemy") == 0 || std::strcmp(buf, "monsters") == 0) {
        def.side = Side::Enemy;
      } else {
        fail(errOut, errLen, cur.lineNo, "side must be player or enemy");
        return false;
      }
      out.unitCount++;
    } else {
      // Unknown top-level keys are ignored so future fields stay
      // backwards-compatible with old firmware.
    }
  }

  out.width = static_cast<int8_t>(mapWidth);
  out.height = static_cast<int8_t>(mapRow);

  // Validation
  if (out.width <= 0 || out.height <= 0) {
    fail(errOut, errLen, cur.lineNo, "level has no map");
    return false;
  }
  if (out.unitCount == 0) {
    fail(errOut, errLen, cur.lineNo, "level has no units");
    return false;
  }
  bool hasPlayer = false;
  for (int i = 0; i < out.unitCount; i++) {
    const UnitDef& def = out.units[i];
    if (def.x < 0 || def.y < 0 || def.x >= out.width || def.y >= out.height) {
      fail(errOut, errLen, 0, "unit is outside the map");
      return false;
    }
    const Terrain t = out.at(def.x, def.y);
    const bool passable =
        UNIT_TYPES[def.type].flies ? t != Terrain::Void : TERRAIN_INFO[static_cast<int>(t)].moveCost > 0;
    if (!passable) {
      fail(errOut, errLen, 0, "unit starts on impassable terrain");
      return false;
    }
    for (int j = 0; j < i; j++) {
      if (out.units[j].x == def.x && out.units[j].y == def.y) {
        fail(errOut, errLen, 0, "two units share a tile");
        return false;
      }
    }
    if (def.side == Side::Player) hasPlayer = true;
  }
  if (!hasPlayer) {
    fail(errOut, errLen, 0, "level needs at least one player unit");
    return false;
  }
  if (out.win == WinCondition::CaptureBase && !sawBase) {
    fail(errOut, errLen, 0, "capture_base needs a B tile on the map");
    return false;
  }
  if (out.win == WinCondition::Survive && out.turnLimit == 0) {
    fail(errOut, errLen, 0, "survive needs turns: > 0");
    return false;
  }
  return true;
}

}  // namespace Tactics
