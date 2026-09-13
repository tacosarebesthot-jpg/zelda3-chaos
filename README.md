# Zelda 3 chaos build

A Twitch-driven build of *The Legend of Zelda: A Link to the Past* on the PC port
[snesrev/zelda3](https://github.com/snesrev/zelda3). Chat plays against the streamer:
viewers spawn monsters, flip the screen, steal items, fight a chat boss, bet points on
the outcome, and the NPCs talk back with lines written from the stream's own chat.

Everything here is built on top of the upstream port. The port's own README covers
how the game is reverse engineered and how the assets are built; this file covers what
this build adds.

## What it adds

**Twitch chat in the game.** Connect from the pause menu (START, then M on the keyboard
or SELECT + L on a pad, then A on TWITCH): a login link is copied, you authorize in the
browser, done. From then on chat types `!verb` commands. `tools/TWITCH_COMMANDS.txt`
lists them all: heal, hurt, spawn, swarm, freeze, confuse, flip, party, curse, root,
steal, speed, slow, ice, illusion, deny, arise (Link becomes a cucco), tax, dmgup,
attrition, and more. Cooldowns and per-viewer limits keep one account from running the
show.

**Chat boss.** Every so often a named monster appears and chat has to beat it with
commands before the timer runs out. Win and Link gets healed; lose and the boss punishes
him. Boss names rotate, and every other fight is named after the last fight's MVP.

**Viewer points and bets.** Chatting earns points, commands earn more. `!bet 50 win`
during a boss fight, `!bet 50 good` on the next dungeon chest. Winners get double.

**Channel points.** Rewards are mapped to verbs on the REWARDS page of the pause menu,
inside the game, nothing to edit.

**Quick chat box.** A Rocket League style overlay in the corner cycles short in-game
one-liners as chat plays; it goes quiet if GAME JOKES is switched off (below).
`quickchat.txt` next to the game adds your own extra lines to the pool.

**Chatter counter and credits.** The game keeps its own tally of who has chatted and how
much (`chatters.txt`); the ending staff roll can show that leaderboard, and Patreon
supporters from `patrons.txt`, in place of the original credits.

**Dialogue.** `dialogue.txt` next to the game rewrites the script: every message can
have several random alternates, `{chatter}` becomes a real chatter's name, `{boss}` the
current chat boss, and a NORMAL / CLEAN / DIRTY switch controls the cursing. Hints can
stay vanilla, tell the truth about this seed's placements, or be jokes. A GAME JOKES
switch (OPTIONS > GAME FEATURES) turns the pop-culture one-off lines - and the quick
chat box above - on or off independently of all that.

**Stream overlays.** `tools/tracker.html` and `tools/feed.html` are browser sources for
OBS: item tracker, activity feed, chat boss state, scoreboard.

## Custom music

Drop your own `.ogg` / `.mp3` / `.wav` files into a `music/` folder next to the game and
turn MUSIC FOLDER on under OPTIONS > AUDIO (FOLDER VOL and SHUFFLE sit next to it). The
player watches which tune the game is asking for and picks from a matching pool, so the
right kind of music comes up in the right place. Eight optional subfolders are each their
own pool:

    music/title   music/overworld   music/darkworld   music/dungeon
    music/boss    music/town        music/cave        music/ending

Loose files straight in `music/` are the fallback for any pool you leave empty; a pool
with nothing in it and no fallback just lets the original SNES track play. While the game
keeps asking for tracks from the same pool the current file keeps playing across screens,
and the SNES tune is muted underneath rather than layered on top. Chat can ask
`!nowplaying` for the current file and `!skipsong` for the next one in the pool.

Don't combine this with an MSU-1 pack - they are two different music replacements. The
switches live in `music.ini`, and the folder is scanned once at startup, so newly added
files need a restart.

## Randomizer

An eight-page RANDOMIZER menu (player select > name a new file, or the defaults page
off the file-select screen) drives a real item fill from the ALttP Door Randomizer's
own logic - the reference generator is bundled under `randomizer_ref/` and does the
actual solving; the engine only applies the result. Chests, NPC gifts, the pedestal,
the tablets, boss heart containers, shop stock, enemy key drops, pot keys and bonk/tree
prizes all follow the fill wherever the matching row below turns them on. Besides
RANDOMIZER (on/off), SEED, LOGIC (glitch tier) and START (open/standard), there are 38
settings rows, paged six to a screen and grouped roughly like this:

- **Goal and crystals** - GOAL (ganon / pedestal / dungeons / crystals / triforce hunt),
  GT CRYSTALS and GANON CRYSTALS (how many of the seven you need for each)
- **Pool, power, access** - ITEM POOL and ITEM POWER (normal/hard/expert strip or weaken
  upgrades), ACCESS (everything reachable, or only what the win needs)
- **Swords, pyramid, progression, boss drops** - SWORDS (random/assured/vanilla start),
  PYRAMID HOLE (open/auto), PROGRESSIVE (chain items in order, any order, or random),
  BOSS ITEMS (what a boss may drop relative to its own dungeon)
- **Flute and bow** - FLUTE (normal or active from the start), BOW (progressive bow, or
  start toward Silver Arrows)
- **Small keys, big keys, maps, compasses** - each independently OWN DUNGEON / ANYWHERE
  / UNIVERSAL (small keys only) / NEARBY - NEARBY keeps the item a fixed placement but
  lets it land anywhere in its dungeon's district, not just inside the dungeon itself
- **Enemy health and damage** - EASY through EXPERT tiers, and shuffled/random damage,
  rolled per seed
- **Shops** - SHOPS (normal, or shop stock joins the item shuffle - this is also what
  puts the new bomb/arrow capacity upgrades into the pool)
- **Key drops and pots** - KEY DROPS (the 14 enemy-held dungeon keys) and POTS (the 19
  pot-held dungeon keys) each independently join the shuffle
- **Bomb bag and retro** - BOMB BAG (no bombs at all until a capacity upgrade drops),
  RETRO (no arrow drops, buy them instead, small keys go into one shared pool)
- **Dungeon count and collection rate** - HUD-only: a chests-remaining counter per
  dungeon, and a "items found of the seed" line on the MODS page
- **Cosmetics** - HEART COLOR, MENU SPEED, HEART BEEP rate
- **Enemies and enemy color** - an engine-side, vanilla-logic-safe enemy swap (same
  sprite sheet, same kill weapons, so no shutter door gets easier) and an independent
  sprite palette shuffle; both are seeded per file
- **Overworld and dungeon colors** - background palette shuffle or blackout, kept off
  sprite, HUD, Link and text palettes
- **SFX shuffle, music, flashing** - a seeded sound-effect id permutation, a music
  on/off switch, and a reduced-flash mode for spell/screen flash effects
- **Bonk drops** - the 42 tree, bonk-rock and statue prizes join the shuffle as a set
- **Entrances** - the reference's door/hole shuffle: NORMAL, SIMPLE, RESTRICT, FULL,
  CROSSED, INSANITY, two dungeons-only modes, LEAN (partial) and SWAPPED
- **Bosses** - NORMAL, SIMPLE, FULL, RANDOM, and UNIQUE (no boss repeats outside
  Ganon's Tower)

A combination that has not been built yet gets its rule folder generated on the spot
by the bundled Python the first time you hit BEGIN with it - about six seconds, shown
as "BUILDING THE RULES" - and every combination after that boots instantly from the
saved folder.

**Not offered, on purpose:**
- **Door shuffle** - the reference implements it as a binary 65816 ROM patch this tree
  has no source for, so it cannot be reproduced or verified here.
- **Inverted start** - same reason: it depends on that same binary patch for the
  bunny-inversion and Ganon-warp behavior. The engine refuses to fill any rule folder
  that claims inverted mode rather than silently mis-playing it.
- **Swordless mode, retro bow, the wider pottery modes, underworld bonk/tree drops** -
  each needs placements or sprite tables the engine cannot yet reproduce from the
  reference's own solve; shipping the setting without the matching placement could
  hand out an unwinnable seed.
- **Triforce hunt's sibling goals (trinity, ganon hunt, completionist)** - not wired to
  an ending condition in this engine today.

## OPTIONS (player select > OPTIONS)

- **Controller and keyboard setup** - separate pad-button and keyboard-key binding
  screens, a TEST BUTTONS page that lights up whatever you actually press, and reset
  to default; saved to `controls.ini`.
- **Video** - widescreen toggle, fullscreen/window size, renderer choice, and a second
  page of the port's own display options.
- **Game features** - the upstream port's gameplay toggles (item switch and the rest),
  plus GAME JOKES (on/off for the pop-culture dialogue lines and the quick chat box).
- **Audio** - volume and the port's sound options.
- **Hints, credits, crash checkpoint** - hint verbosity, the STAFF/CHAT credits switch
  described above, and a rolling save every minute that restores automatically after a
  crash.

Settings made here write `options.ini` on top of whatever `zelda3.ini` already says.

## Building

Same as the upstream port: you need your own copy of the game to extract the assets
(see the upstream README for `extract_assets` and `zelda3_assets.dat`). On Windows,
`build_msvc.cmd` builds with the Visual Studio Build Tools and the bundled SDL2. Run
`python tools/get_python_embed.py` once so the game can build randomizer rules for new
setting combinations (it downloads the official embeddable Python plus PyYAML into
`python_embed/`). The very first time a save file uses any non-default randomizer
setting, expect a one-time ~6 second pause at BEGIN while that combination's rule
folder is built; every other combination that has already been played boots straight
from its saved folder.

## Files next to the game

| File | What |
|---|---|
| `zelda3.ini` | the port's settings; the OPTIONS page writes `options.ini` over it |
| `options.ini` | video/game-features/audio choices made from OPTIONS |
| `twitch_config.txt` | written on first boot; the in-game CONNECT fills in the token |
| `dialogue.txt` | the rewritten script and its alternates |
| `text.ini` | the TEXT/hints/credits/GAME JOKES switches from the MODS and OPTIONS pages |
| `controls.ini` | your pad and keyboard bindings from the OPTIONS page |
| `chatters.txt` | chatter names and message counts, kept by the game |
| `patrons.txt` | names shown first in the ending credits |
| `points.txt` | the viewer points ledger |
| `quickchat.txt` | optional extra lines for the quick chat box |
| `music.ini` | the custom music folder: on/off, volume, shuffle |
| `randomizer.ini` | randomizer defaults for the next new file |
| `saves/rando_slots.ini` | the randomizer settings locked to each save file |

`tools/` holds the harness that verifies the build against the running game, the OBS
overlays, the channel-points helpers, and `make_dist.py`, which packages a player zip.

## License

The port and this build are under the license in `LICENSE.txt`. The game itself is not
included; bring your own copy.
