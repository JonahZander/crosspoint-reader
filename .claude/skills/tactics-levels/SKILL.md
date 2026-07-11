---
name: tactics-levels
description: Author, playtest and balance levels and campaigns for the Tactics mini-game (Home → Tactics). Use when writing or tuning .yaml battle files, .story interludes, campaign folders, or when a level plays too easy/too hard.
---

# Writing and Playtesting Tactics Levels

Full format reference: `docs/tactics-levels.md`. This skill is the workflow.

## Writing a level

1. Levels are plain text `.yaml` files (restricted subset, parsed by
   `lib/Tactics/TacticsLevel.cpp` — not a general YAML parser). Skeleton:

   ```yaml
   name: My Battle             # shown in list + HUD
   win: defeat_all             # defeat_all | capture_base | survive
   turns: 20                   # required for survive, optional otherwise
   aggro: 5                    # monster wake radius (default 7, 0 = passive)
   map: |
     ....ff....^^^
     ...ff...C..^B
   units:
     - {type: dwarf_warrior, at: [3, 1], side: player}
     - {type: orc, at: [11, 1], side: enemy}
   ```

2. Terrain chars: `.` plain, `f` forest, `^` mountain, `~` water (flyers
   only), `#` rock (wall), `C` castle (heals 4/turn), `B` enemy base,
   space = void. Max 48x32, up to 32 units, ≥1 player unit.
3. **Any type can be on either side** — same-race matchups work; enemy-side
   units render inverted and the AI plays them. Unit stats table lives in
   `docs/tactics-levels.md` and `lib/Tactics/TacticsTypes.h`.
4. Combat rules that shape balance:
   - Deterministic: damage = ATK × (100 − DEF)%, min 1; DEF caps at 60%.
   - **Flanking**: melee hit with an ally directly opposite the attacker =
     +50%. Works for the AI too — swarms punish surrounded units hard.
   - **Parting blows**: leaving an adjacent awake enemy costs a free
     half-damage hit. Kills kiting; ranged monsters (goblin_slinger) also
     punish it. Melee-only enemy rosters are exploitable from range 2.
   - Trolls regen 4/turn — a wall, not a defeat_all objective for
     low-attack squads.

## Playtesting (in this order — cheap to expensive)

1. **Parse + geometry sanity**: add the file to a test or load it once via
   the simulator (step 3); the parser reports row/line errors.
2. **Automated bot playthrough** — the balance gate. Add a test in
   `test/tactics/CampaignPlaytest.cpp` (pattern: load file, `runBot(level,
   maxTurns)`, assert `Victory` + a survivor *band*: floor catches
   unwinnable, ceiling catches flawless-sweep-too-easy; intros may omit
   the ceiling). The bot knows flanking, focus fire (globally best attack
   first), threat-zone avoidance and parting blows — it approximates a
   competent human, and every run logs `[bot] victory turns=N survivors=M`
   to stderr for band-setting. The enemy AI seeks flanks and hunts your
   backline, so squishy player units near the front line will be punished.
   Run:

   ```bash
   cmake -S test -B build/test && cmake --build build/test -j
   ctest --test-dir build/test -R Campaign --output-on-failure
   ```

   Debug a failing balance test with a per-turn roster dump:

   ```bash
   BOT_DEBUG=1 build/test/tactics/TacticsTest --gtest_filter='*MyLevel*'
   ```

   Read the dump: negative HP = dead; watch which side bleeds first and
   which enemy (often a held-back flyer) cleans up.
3. **Hand-play on the desktop**:

   ```bash
   cmake --build build/test --target TacticsSim
   build/test/tactics_sim/TacticsSim data/tactics/my_level.yaml --frames /tmp/frames
   ```

   Commands: `p` board, `s <u>` range, `m <u> <x> <y>` move, `a <u> <t>`
   attack, `f <u>` finish, `e` end turn. `--frames` writes a BMP per action.
4. **On device**: copy to `/tactics/` on the SD card (or a campaign
   folder). Delete `/.crosspoint/tactics/<name>.sav` if a stale save
   interferes.

## Tuning heuristics

- Bot wipes but kills most enemies → the level is *nearly* right; add one
  player unit or remove one late-wave enemy rather than nerfing globally.
- Tune in soldier-sized steps: adding a basic soldier costs the bot ~1
  survivor, adding a knight ~2. When a flawless sweep needs one casualty,
  a knight overshoots — reach for the cheapest unit that fits the story.
- Fresh enemies that never engaged until the end (dormant, far aggro) are
  disproportionately strong — they mop up wounded survivors.
- Two waves: place the second wave outside the first wave's pack-wake
  radius (3 tiles from combat) or it joins immediately.
- Assert a survivor floor (`EXPECT_GE(r.survivors, N)`) so victories that
  cost the whole squad still fail the gate.

## Campaigns

- Each **subdirectory of `/tactics` is a campaign** (one level deep). The
  root lists folders first as `[name]`. Repo-side, shipped campaigns live
  in `data/tactics/<campaign>/`.
- Filename sort order **is** the campaign order: prefix with numbers
  (`10_intro.story`, `11_first_battle.yaml`, …). After a victory, Continue
  opens the next file.
- `.story` files are plain text interludes (paged viewer, Back skips).
  Keep under 16 KB, blank lines separate paragraphs. Keep story facts in
  sync with the levels (unit count, who joins/dies).

## Writing story interludes

The reader plays battles days apart on a small paged e-ink screen — each
interlude must stand nearly alone. The failure mode is not bad prose, it
is *reference density*: too many proper nouns, and callbacks that assume
the reader remembers a line from three files ago.

- **One narrator, one frame** per campaign (a chronicle, letters home).
  Pick it in file 10 and never break it.
- **Name budget**: introduce at most 2 new proper nouns (people or
  places) per interlude. The opener may name the squad, one line each,
  tied to their unit type ("Vala, thunderer — a bolt through a keyhole
  at forty paces").
- **Re-anchor on return**: when a name reappears after an absence,
  re-attach its role in apposition — "Wren, our scout", "Corben — the
  man we lost at the tollgate". Never let a callback ride on a detail
  alone ("Corben who snored" fails if battle 2 was last week).
- **End on the mission**: the final paragraph states the next battle's
  objective and stakes in one concrete line that matches the win
  condition — hold N turns (survive — put the actual number on the
  page), reach the threshold (capture), break them (defeat_all). "Hold
  ten turns of the glass" is the model; "hold until relief comes" is
  the miss.
- **Sync the roster**: whoever joins or dies on the page must join or
  die in the units list, and squad size on the page locks squad size in
  the level ("Four of us go to the Maw" means you cannot add a fifth
  without editing the story).
- Off-page events between battles (a march, a betrayal) get one plain
  sentence of what changed before any poetry about it.
- Completion markers: `/.crosspoint/tactics/completed.txt`, one relative
  path per line (`deep_road/11_the_gates.yaml`). Saves flatten `/` to `~`
  in `/.crosspoint/tactics/*.sav`.
- If you change a shipped campaign level, its playtest in
  `CampaignPlaytest.cpp` must still pass — that file is the balance
  contract.
