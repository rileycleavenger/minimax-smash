# Minimax Smash

A native 2D platform-fighter prototype in C++17 + raylib. Two characters, four
moves, damage-percent knockback, three stocks each, and a CPU that picks every
move — including where to stand — with a maximin game-tree search whose depth
*is* the difficulty setting.

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
| `1`–`6` | set CPU level live |
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
a plain alternating MAX(6) / MIN(6) tree where the world only advances after each
MIN ply — so textbook alpha-beta applies verbatim. Three things make it cheap:

- **Alpha-beta with iterative-deepening move ordering** at the root.
- **Killer-move ordering** inside the tree. With six actions a side the fixed
  order was not good enough: measured effective branching was 7–9x per beat
  against a square-root optimum of 6x. Trying the action that last caused a
  cutoff at this ply first brings it to 5.0–5.5x and cut depth 6 from 972k nodes
  to 195k — a 5x reduction, and the single reason level 6 fits inside a beat.
- **Locked-fighter collapse.** A fighter locked for the entire upcoming beat
  cannot act, so all six of its actions are provably identical and are searched
  as one. A whiffed Smash locks its owner for three or four beats, so this prunes
  hard and is exact — it is also why the measured branching sits *below* the
  theoretical optimum.

The search calls the **same** `stepFrame()` the game does, so the CPU can never
be wrong about what a move does. Its one modelling assumption is about you: it
cannot see your stick, so it assumes you also choose from these six actions, and
falls back to the built-in locomotion rule (approach, and scramble back when
offstage) for the beats in which you are doing something else.

### Evaluation

```
+10000 * stock differential          dominates everything, as it should
+ 1.25 * opponent damage − own damage
−  edge risk, self + opponent        (|x| / halfStage)^2 * (1 + damage/100) * 60
−     25 offstage / recovering
+     12 opponent in hitstun (and the reverse)
−   0.55 * my shield stamina spent   /   +0.30 * theirs
−    120 my shield broken            /   +10 theirs
−     18 holding shield vs a fighter who is not attacking
−   0.12 * frames since anyone last took damage
```

Several of these are asymmetric on purpose, for different reasons.

**Stale shield** is asymmetric because a symmetric "both blocking is bad" pair
cancels and maximin goes back to turtling. Stalling is *your* problem, because
you have to win the match.

**Shield break** is asymmetric because the two cases are not the same quantity.
My own broken shield is an emergency that persists whatever I do. The opponent's
is worth only what I can convert it into — and a large bonus there is really a
bonus for *not* taking the punish, since landing the hit is what ends the break.
Priced symmetrically at 120, the CPU would stand and admire a helpless opponent:
a free hit scored 27 against 129 for leaving them alone, so it left them alone.
At 10 it converts, and converts harder the deeper it looks:

| depth | 1 | 2 | 3 | 4 | 5 |
| --- | --- | --- | --- | --- | --- |
| value of a 90-frame shield break | 27 | 38 | 55 | 79 | 101 |

**Spending their shield** is scored at all, which it was not before: attacking a
held shield used to be worth exactly as much as doing nothing, so a shallow
search had no reason to throw the first punch.

**The stall clock** is the one that is not about shields. Maximin has no reason
to seek a fight: approaching always has a worst-case answer, so standing off is
never worse. That stayed hidden while the CPU could only ever walk toward you —
the locomotion rule was quietly supplying the aggression the evaluation never
had. The moment Retreat became searchable, every level drew every match,
oscillating advance/retreat on a two-beat cycle.

Pricing the *distance* between the fighters does not fix it, and it is worth
saying why, because it looks like the obvious answer: in a zero-sum tree the
opponent controls separation too, so the pessimistic model simply keeps away and
the term becomes a constant that distinguishes nothing. Measured, it changed
nothing at all. Pricing *elapsed time without damage* does work, because whether
a hit lands is something the CPU can act on and the opponent cannot simply
cancel. `GameState` carries a `stallFrames` counter for it, reset by any damage
or KO, and because the counter is absolute rather than per-horizon, the longer a
stalemate runs the more a hit is worth — the pressure to break it grows.

### Threading

The search runs on a dedicated `SearchWorker` thread, so rendering and input
never block. That is the threading win that matters.

Splitting the root subtrees across cores is also implemented, and is worth less
than you would hope: alpha-beta makes the tree extremely unbalanced, so most of
the work sits in the single best-ordered root move and Amdahl's law caps the
rest. It was worth only ~10% with four actions; with six there are more siblings
to spread, and it is now worth ~20%. Searching that move first to establish alpha and only
then fanning the siblings out against a real lower bound recovers the pruning the
naive split threw away — it had doubled the node count. It is on above
`ROOT_SPLIT_DEFAULT` and can be turned off at runtime through
`gRootSplitMinDepth`; `./build/bench time` prints both columns so the claim stays
checkable rather than asserted.

## Difficulty is literally the depth

Level N searches N beats ahead. Everything below comes from `make bench`, which
plays seeded matches from varied starting positions — the earlier version of this
table was four matches per cell and was noise.

**Round robin, 240 matches, every pairing in both seats and with both characters:**

| level | 1 | 2 | 3 | 4 | 5 | 6 |
| --- | --- | --- | --- | --- | --- | --- |
| overall win rate | 0% | 22% | 42% | 72% | 78% | 85% |
| 1σ | ±0 | ±5 | ±6 | ±5 | ±5 | ±4 |

Monotonic, and the harness says so in as many words rather than leaving it to the
reader. The earlier version of this table was four matches per cell and reported
a d5/d6 inversion that does not exist. `./build/bench gauntlet` measures every
level against one fixed reference instead, which is O(levels) rather than
O(levels²) — it mattered when the ladder ran to 8 and is still the cheap way to
check a tuning change.

### Against a player who does nothing

Every level closes out a passive opponent 3-0 across five different openings,
never stalling, and deeper closes *faster*:

| level | 1 | 2 | 3 | 4 | 5 | 6 |
| --- | --- | --- | --- | --- | --- | --- |
| mean time to 3-0 | 46s | 25s | 16s | 20s | 20s | 15s |

This is the test that caught the stall problem. It is worth keeping precisely
because it is the case maximin handles worst: the search cannot see your inputs,
so it models you as an adversary playing the worst possible reply to whatever it
does, and an opponent who is genuinely harmless never looks harmless. Before the
stall clock, every level here ran the full 180-second cap and drew.

### What the extra beats actually buy

Win rates say the ladder is ordered, not what changes. `./build/bench diag`
reports the action mix, and `./build/bench probe` runs positions where a human
can name the right move:

| level | JUMP | NORMAL | SMASH | BLOCK | ADVANCE | RETREAT | self-KO/match |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | 8% | 34% | 0% | 20% | 35% | 3% | 1.33 |
| 3 | 18% | 23% | 5% | 25% | 22% | 7% | 1.00 |
| 6 | 15% | 32% | 3% | 18% | 22% | 9% | 0.25 |

Deeper search **turtles less** (block 20% → 18%), **spaces more** (retreat 3% →
9%, advance 35% → 22% — it stops walking blindly forward), and **kills itself
less** (1.33 → 0.25 self-KOs a match). Level 1 barely retreats at all, because
backing off only pays if you can see what it buys you.

Depth 1 never throws a Smash, and cannot. A Smash's 16–20 frame wind-up outlasts
the 10-frame beat, so one ply ahead it has dealt no damage and left its owner
locked — strictly worse than a Normal, always. Smash only becomes findable once
the search can see past the wind-up:

| probe position | depth 1 | depth 2 | depth 4+ |
| --- | --- | --- | --- |
| opponent whiffed a Smash | ADVANCE | **SMASH** | SMASH |
| opponent open, CPU can kill at 145% | NORMAL | **SMASH** (sees the KO) | SMASH |
| opponent winding up a Smash | NORMAL (stands in it) | NORMAL | **JUMP** (evades) |
| opponent's shield broken, 90f | ADVANCE (walks in) | SMASH | SMASH |

Depth 2 finds the punish; depth 4 finds the evasion. Press `Tab` mid-match to
watch it score all six options in real time.

### Search cost

Mean over 16 live mid-match positions where the CPU genuinely has a choice, on an
M4 Pro:

| depth | 1 | 2 | 3 | 4 | 5 | 6 |
| --- | --- | --- | --- | --- | --- | --- |
| nodes | 14 | 107 | 638 | 4,861 | 26,573 | 139,505 |
| ms | 0.00 | 0.03 | 0.15 | 1.1 | 6.0 | 31.2 |
| ms, root split | — | — | — | — | 5.3 | 26.9 |

One beat is 167 ms of wall clock and the search owns its own thread, so every
level fits with room to spare. If a decision ever does overrun its budget,
iterative deepening has already published a complete shallower answer and the CPU
acts on that rather than stalling.

## Headless harness

```sh
make bench
./build/bench                # everything below
./build/bench time           # search cost per depth, sequential vs root split
./build/bench ladder 8 6     # round robin: games per pairing, max depth
./build/bench gauntlet 12 4  # every level vs one reference level
./build/bench diag 6 4       # action mix, self-KOs, shield breaks per level
./build/bench probe          # chosen move in positions with a known answer
./build/bench passive        # closing out an opponent who never presses anything
./build/bench sample         # one depth-1 vs depth-5 match summary
./build/bench trace 1 6      # every decision of a single match, with values
```

Matches are seeded and start from varied positions, so a win rate samples many
openings instead of replaying one deterministic trajectory; independent matches
run across cores. Two knobs exist so measurement does not lie to itself: the
harness forces the search sequential while it is saturating the machine with
parallel matches, and it lifts the live game's per-decision time budget, which
would otherwise quietly turn a contended depth-8 search into a depth-6 one and
flatter the top of the ladder.

## Where to tune

Nearly every number lives in [src/stats.h](src/stats.h): stage and blast-zone
geometry, physics constants, beat length, per-character frame data and knockback,
and the evaluation weights. [src/eval.cpp](src/eval.cpp) is the scoring function
and [src/search.cpp](src/search.cpp) is the search itself.

Two couplings are worth knowing about. `JUMP_LOCK` must be at least
`BEAT_FRAMES`, because a CPU holds one action for a whole beat; at 9 against a
10-frame beat a held `ACT_JUMP` silently spent *both* jumps inside a single
decision, which quietly wrecked offstage recovery at every level. And the killer
table is sized from `MAX_CPU_LEVEL`, so `searchRoot` clamps the depth it is asked
for rather than running off the end of it.

## Movement is a searched action

The four *moves* are Jump, Normal, Smash and Block. The CPU searches **six**
actions: those four, plus Advance and Retreat.

It has to. The other four all leave `ST_IDLE` the instant they are applied —
Block pins you, Normal and Smash commit you, Jump puts you in the air — and a
grounded fighter only walks while it *is* idle. Before movement was searchable, a
grounded CPU walked on about 5% of its frames: it approached only by jumping,
could not back off at all, and could not even walk in to punish a shield break,
which it valued the same from any distance. That number is now 46%, and it walks
in to take the punish.

The cost is real, and it is why the ladder stops at 6:

| | 4 actions | 6 actions | 6 actions + killers |
| --- | --- | --- | --- |
| joint actions per node | 16 | 36 | 36 |
| effective branching / beat | 4.2x | 7–9x | 5.2x |
| depth 6 nodes | 37k | 972k | 140k |
| depth 6 ms | 7.3 | 181 | 31 |

Six actions a side is 36 joint actions per node against 16, so the square-root
optimum moves from 4x to 6x per beat — but the old fixed move order could not
reach it and came in at 7–9x, putting depth 6 over the beat budget at 181ms.
Killer-move ordering recovered a factor of five. Even so, depth 7 needs 172ms
against a 167ms beat, so 6 is the honest cap: two levels of ladder traded for a
CPU that can actually space.

The second cost was not a performance one, and it was larger. Being able to
retreat means being able to *not fight*, and two maximin players who can both
decline will decline forever — see the stall clock above. The old four-action
ladder was quietly relying on its locomotion rule to supply an aggression its
evaluation never had.

## Layout

| file | role |
| --- | --- |
| [src/game.cpp](src/game.cpp) | deterministic simulation: `stepFrame`, knockback, stocks |
| [src/stats.h](src/stats.h) | every tunable constant |
| [src/eval.cpp](src/eval.cpp) | leaf evaluation |
| [src/search.cpp](src/search.cpp) | maximin alpha-beta + the worker thread |
| [src/render.cpp](src/render.cpp) | camera, characters, effects, HUD |
| [src/main.cpp](src/main.cpp) | window, fixed-timestep loop, input, CPU driver |
| [tools/bench.cpp](tools/bench.cpp) | headless timing, strength and behaviour measurement |
