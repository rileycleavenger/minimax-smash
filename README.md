# Minimax Smash

A native 2D platform-fighter prototype in C++17 + raylib. Two characters, four
moves, damage-percent knockback, three stocks each, and a CPU that picks every
move with a maximin game-tree search whose depth *is* the difficulty setting.

## Build and run

```sh
brew install raylib
make
make run
```

Only raylib is required. There is no asset pipeline: both fighters and every hit
effect are drawn from shape primitives, and the HUD uses raylib's built-in font.

## Controls

| key | action |
| --- | --- |
| `A` / `D` or arrows | walk |
| `W` / `Space` / `Up` | jump (two jumps, also your recovery) |
| `J` | normal attack |
| `K` | smash attack |
| `L` | block (hold) |
| `1`–`8` | set CPU level live |
| `C` / `V` | swap your / the CPU's character |
| `Tab` | search debug overlay |
| `R` | reset |
| `Esc` | quit |

Command-line flags: `--demo` (CPU plays both sides), `--p0 N` / `--p1 N` (levels),
`--c0 N` / `--c1 N` (characters), `--debug`, `--shot N --out file.png`.

Watching two levels fight each other is the quickest way to see the difference:

```sh
./build/smash --demo --debug --p0 2 --p1 7
```

## Mechanics

Each move is `{startup, active, recovery}` frames, and a fighter is **locked**
for all of them plus hitstun. That single rule is what makes Smash risky and
what the search reasons about.

- **Normal** – fast, low damage, short reach.
- **Smash** – long wind-up *and* long recovery, big damage and knockback growth.
- **Block** – negates damage and cuts knockback to ~15%, but drains shield
  stamina. At zero the shield **breaks** for 100 frames. Dropping the shield
  costs a 12-frame full lockout, so you cannot shield and instantly punish.
- **Jump** – two jumps, used for approach, evasion, and recovery from offstage.

Knockback follows a simplified Smash formula, so damage genuinely compounds:

```
kb      = (baseKB + victimDamage * growth) * (100 / weight)
hitstun = kb * 2.0
```

Three stocks each. Crossing a blast zone costs a stock and respawns you at
centre stage with 0% and brief invulnerability.

**Characters.** *Sparkmouse* is fast and light with quick low-damage pokes and
electric hit effects; *Plumber* is heavier and slower with more reach and
noticeably harder hits.

## The CPU

Both fighters commit on the same beat (10 frames), so the CPU assumes the worst
case and plays the action whose best opponent reply is least bad:

```
V(s, d) = max over my action ( min over opponent action  V(stepBeat(s), d-1) )
V(s, 0) = evaluate(s)
```

Because the pessimistic opponent is allowed to see our choice, this expands into
a plain alternating MAX(4) / MIN(4) tree where the world only advances after each
MIN ply — so textbook alpha-beta applies verbatim. Two things make it cheap:

- **Alpha-beta with iterative-deepening move ordering.** The measured effective
  branching factor is ~4x per beat against a theoretical 16x, i.e. the full
  square-root reduction.
- **Locked-fighter collapse.** A fighter locked for the entire upcoming beat
  cannot act, so all four of its actions are provably identical and are searched
  as one. A whiffed Smash locks its owner for three or four beats, so this prunes
  hard and is exact.

The search calls the **same** `stepFrame()` the game does, so the CPU can never
be wrong about what a move does. Its one modelling assumption is that the human
uses the built-in locomotion rule (approach, and scramble back when offstage),
since it cannot know your movement inputs.

### Evaluation

```
+10000 * stock differential          dominates everything, as it should
+ 1.25 * opponent damage − own damage
−  edge risk, self + opponent        (|x| / halfStage)^2 * (1 + damage/100) * 60
−     25 offstage / recovering
+     12 opponent in hitstun (and the reverse)
−   0.55 * shield stamina spent
−    120 shield currently broken
−     18 holding shield vs a fighter who is not attacking
```

The last term is asymmetric on purpose. A symmetric "both blocking is bad" pair cancels, and maximin goes back to turtling. Stalling is *your* problem because you have to win the match. Combined with knockback that actually reaches the blast zone, a level-5 CPU closes out a player who never touches the stick in about a minute.

### Threading

The search runs on a dedicated `SearchWorker` thread, so rendering and input
never block. At level 8 the four root actions are additionally split across
cores. Worth being honest about the measured result: alpha-beta's sequential
pruning is worth *more* than naive parallelism until the very top of the ladder,
so the root split only engages at depth 8, where it takes ~93ms down to ~59ms.
The real threading win is the worker thread keeping the frame loop at 60fps.

## Difficulty is literally the depth

Level N searches N beats ahead. Measured by `make bench && ./build/bench ladder`,
which plays every pairing from both seats and with both characters:

| level | 1 | 2 | 3 | 4 | 5 | 6 |
| --- | --- | --- | --- | --- | --- | --- |
| overall win rate | 0% | 20% | 40% | 60% | 95% | 85% |

Search cost per decision, averaged over live mid-match positions on an M4 Pro:

| depth | 1 | 3 | 5 | 6 | 7 | 8 |
| --- | --- | --- | --- | --- | --- | --- |
| nodes | 8 | 204 | 3,962 | 17,026 | 79,021 | 661,952 |
| ms | 0.00 | 0.06 | 0.84 | 3.34 | 13.7 | 59.3 |

Level 1 throws out Smash into obvious punishes and self-destructs. By level 5-6
it walls out approaches, waits for a Smash wind-up, and answers into the recovery
window. Press `Tab` mid-match to watch it score all four options in real time.

## Headless harness

```sh
make bench
./build/bench            # everything below
./build/bench time       # search cost per depth
./build/bench ladder     # win rates across the depth ladder
./build/bench sample     # one depth-1 vs depth-5 match summary
./build/bench trace 1 6  # every decision of a single match, with values
```

The trace output is the fastest way to sanity-check a tuning change; each line
shows the chosen action, its maximin value, and both fighters' damage and state.

## Where to tune

Nearly every number lives in [src/stats.h](src/stats.h): stage and blast-zone
geometry, physics constants, beat length, per-character frame data and knockback,
and the evaluation weights. [src/eval.cpp](src/eval.cpp) is the scoring function
and [src/search.cpp](src/search.cpp) is the search itself.

## Layout

| file | role |
| --- | --- |
| [src/game.cpp](src/game.cpp) | deterministic simulation: `stepFrame`, knockback, stocks |
| [src/stats.h](src/stats.h) | every tunable constant |
| [src/eval.cpp](src/eval.cpp) | leaf evaluation |
| [src/search.cpp](src/search.cpp) | maximin alpha-beta + the worker thread |
| [src/render.cpp](src/render.cpp) | camera, characters, effects, HUD |
| [src/main.cpp](src/main.cpp) | window, fixed-timestep loop, input, CPU driver |
| [tools/bench.cpp](tools/bench.cpp) | headless timing and strength measurement |
