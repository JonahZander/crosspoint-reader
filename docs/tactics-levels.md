# Tactics Level Format

The tactics mini-game (Home → Tactics) loads levels from the `/tactics`
directory on the SD card. Levels are plain text files with a `.yaml`
extension — write them in any text editor and copy them to the card (the
web File Transfer works too). Two sample levels ship in `data/tactics/`;
copy them to `/tactics/` on your SD card to get started.

The game engine lives in `lib/Tactics/` and is hardware-free; it is unit
tested on the host (`test/tactics/`) and playable on a desktop through the
simulator (`test/tactics_sim/`, see below).

## File format

Only the YAML subset shown here is supported (parsed by
`lib/Tactics/TacticsLevel.cpp` — not a general YAML parser):

```yaml
name: The Hidden Warren     # shown in the level list / HUD
win: capture_base           # defeat_all | capture_base | survive
turns: 30                   # turn limit; required for survive, optional
                            # (0/absent = unlimited) for the others
aggro: 5                    # monster wake-up radius in tiles (default 7)
map: |
  ....ff....^^^
  ...ff...C..^B
  ..........^^^
units:
  - {type: elf_archer, at: [3, 1], side: player}
  - {type: troll, at: [11, 1], side: enemy}
```

- `#` starts a comment anywhere except inside the map block (where `#` is
  the rock tile). Blank lines are ignored.
- Coordinates are `[x, y]`, zero-based from the top-left map corner.
- Maps can be up to **48x32** tiles — far larger than the screen; the game
  scrolls. Rows shorter than the widest row are padded with void.
- Up to 32 units per level; at least one must be `side: player`.

## Terrain characters

| Char | Terrain  | Move cost | Defense | Notes |
|------|----------|-----------|---------|-------|
| `.`  | Plain    | 1         | +0%     | |
| `f`  | Forest   | 2         | +15%    | elves get another +15% |
| `^`  | Mountain | 3         | +25%    | dwarves move for 1 and get +15% |
| `~`  | Water    | —         | —       | flyers only |
| `#`  | Rock     | —         | —       | impassable wall |
| `C`  | Castle   | 1         | +30%    | heals occupant 4 HP per turn |
| `B`  | Enemy base | 1       | +20%    | capture target |
| space| Void     | —         | —       | outside the map shape |

## Unit types

`side: player` and `side: enemy` (alias `monsters`) accept any type.

| id | HP | Move | Atk | Rng | Def | Notes |
|----|----|------|-----|-----|-----|-------|
| `human_soldier`   | 30 | 5 | 8  | 1 | 10% | |
| `human_archer`    | 24 | 5 | 7  | 2 | 5%  | |
| `human_knight`    | 38 | 7 | 10 | 1 | 15% | fastest ground unit |
| `elf_fighter`     | 28 | 6 | 8  | 1 | 10% | forest bonus |
| `elf_archer`      | 22 | 6 | 9  | 2 | 5%  | forest bonus |
| `elf_mage`        | 20 | 5 | 11 | 2 | 0%  | forest bonus |
| `dwarf_warrior`   | 36 | 4 | 10 | 1 | 20% | mountain bonus |
| `dwarf_thunderer` | 28 | 4 | 12 | 2 | 10% | mountain bonus |
| `dwarf_guard`     | 42 | 3 | 7  | 1 | 30% | mountain bonus |
| `drake_clasher`   | 32 | 5 | 10 | 1 | 15% | |
| `drake_flare`     | 26 | 5 | 10 | 2 | 5%  | |
| `drake_flyer`     | 34 | 8 | 11 | 1 | 10% | flies |
| `goblin`          | 18 | 5 | 6  | 1 | 0%  | |
| `orc`             | 30 | 5 | 9  | 1 | 10% | |
| `troll`           | 44 | 4 | 10 | 1 | 10% | regenerates 4 HP/turn |
| `wyvern`          | 30 | 7 | 10 | 1 | 5%  | flies |

## Rules summary

- Deterministic combat, no dice: damage = attack × (100 − defense)%,
  minimum 1. Defense stacks unit + terrain + racial bonus, capped at 60%.
  Flyers never get terrain defense.
- The defender counterattacks if it survives and the attacker is within
  its range — ranged units can safely poke melee units from 2 tiles.
- Zones of control: entering a tile adjacent to an enemy ends movement
  (flyers ignore this).
- Fog of war: you see a manhattan radius around each unit (+2 on a
  mountain). Explored terrain stays visible; enemy units don't.
- Monsters are dormant guards until a player unit comes within `aggro`
  tiles, they are attacked, or combat happens within 3 tiles of them.
- `capture_base` wins the moment any player unit ends a move on the `B`
  tile — even with enemies alive next to it. Losing every unit (or
  running out the turn limit on non-survive missions) is defeat.

## Playing on the desktop

```bash
cmake -S test -B build/test && cmake --build build/test --target TacticsSim
build/test/tactics_sim/TacticsSim data/tactics/02_hidden_warren.yaml --frames /tmp/frames
```

Commands: `p` print board, `s <u>` show range, `m <u> <x> <y>` move,
`a <u> <t>` attack, `f <u>` finish unit, `e` end turn, `q` quit.
`--frames` writes a BMP of the mocked e-ink screen after every action.
