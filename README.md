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
  branching factor is ~4.2x per beat against a theoretical 16x — essentially the
  full square-root reduction, which means move ordering has nothing left to give
  and the remaining cost is the simulation itself.
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
−    120 my shield broken   /   +10 theirs
−     18 holding shield vs a fighter who is not attacking
```

Two of these are asymmetric on purpose, for different reasons.

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

### Threading

The search runs on a dedicated `SearchWorker` thread, so rendering and input
never block. That is the threading win that matters.

Splitting the four root subtrees across cores is also implemented, and is worth
much less than you would hope: alpha-beta makes the tree extremely unbalanced, so
roughly three quarters of the work sits in the single best-ordered root move and
Amdahl's law caps the rest. Searching that move first to establish alpha and only
then fanning the siblings out against a real lower bound recovers the pruning the
naive split threw away (it had doubled the node count), but the end result is
still only **about 10%**, and only at depth 7–8. It is on above
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
| overall win rate | 0% | 21% | 50% | 65% | 76% | 88% |
| 1σ | ±0 | ±5 | ±6 | ±5 | ±5 | ±4 |

A full round robin gets expensive at the top of the ladder, so levels 7 and 8 are
measured against a fixed depth-4 reference instead — `./build/bench gauntlet`,
168 matches:

| level | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| win rate vs depth 4 | 0% | 4% | 8% | ref | 71% | 79% | 88% | 100% |
| self-KOs per match | 1.00 | 0.46 | 0.46 | — | 0.67 | 0.54 | 0.25 | 0.12 |

Both win-rate curves are monotonic, and the harness says so in as many words
rather than leaving it to the reader. Self-KOs go from one a match at level 1 to
one every eight matches at level 8, though not monotonically in between — and the
deeper levels survive longer, which gives them more match to self-destruct in.

### Against a player who does nothing

Every level closes out a passive opponent 3-0 across five different openings, and
never stalls out — the shield-drain, shield-break and stale-shield terms do their
job. But deeper is *slower*, not faster:

| level | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| mean time to 3-0 | 36s | 40s | 39s | 41s | 91s | 70s | 72s | 97s |

That is maximin being consistent, not a bug. The search cannot see your inputs,
so it models you as an adversary playing the worst possible reply to whatever it
does. A deeper search sees more of those replies and prices its approaches more
cautiously; against an opponent who is genuinely harmless that caution is wasted
effort, and it shows up as a longer match. Fixing it would mean giving the CPU an
opponent model that adapts to how you actually play, which is a different project
from this one.

### What the extra beats actually buy

Win rates say the ladder is ordered, not what changes. `./build/bench diag`
reports the action mix, and `./build/bench probe` runs positions where a human
can name the right move:

| level | JUMP | NORMAL | SMASH | BLOCK |
| --- | --- | --- | --- | --- |
| 1 | 19% | 40% | 0% | 41% |
| 4 | 20% | 42% | 6% | 33% |
| 8 | 16% | 50% | 3% | 31% |

Deeper search **turtles less**: block falls from 41% to 30% as lookahead grows,
which is the anti-turtle property the evaluation is tuned for.

Depth 1 never throws a Smash, and cannot. A Smash's 16–20 frame wind-up outlasts
the 10-frame beat, so one ply ahead it has dealt no damage and left its owner
locked — strictly worse than a Normal, always. Smash only becomes findable once
the search can see past the wind-up:

| probe position | depth 1 | depth 2 | depth 4+ |
| --- | --- | --- | --- |
| opponent whiffed a Smash | JUMP | **SMASH** | SMASH |
| opponent open, CPU can kill at 145% | NORMAL | **SMASH** (sees the KO) | SMASH |
| opponent winding up a Smash | NORMAL (stands in it) | NORMAL | **JUMP** (evades) |

Depth 2 finds the punish; depth 4 finds the evasion. Press `Tab` mid-match to
watch it score all four options in real time.

### Search cost

Mean over 16 live mid-match positions where the CPU genuinely has a choice, on an
M4 Pro:

| depth | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| nodes | 9 | 62 | 311 | 1,672 | 8,353 | 37,411 | 169,385 | 700,632 |
| ms | 0.00 | 0.02 | 0.08 | 0.37 | 1.7 | 7.3 | 32.4 | 129 |
| ms, root split | — | — | — | — | 1.6 | 6.6 | 28.8 | 117 |

One beat is 167 ms of wall clock and the search owns its own thread, so every
level fits. Level 8 is the only one close to the line; if a decision does overrun
its budget, iterative deepening has already published a complete shallower answer
and the CPU acts on that rather than stalling.

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

One coupling is worth knowing about: `JUMP_LOCK` must be at least `BEAT_FRAMES`,
because a CPU holds one action for a whole beat. At 9 against a 10-frame beat a
held `ACT_JUMP` silently spent *both* jumps inside a single decision, which quietly
wrecked offstage recovery for every level.

## Known limitation: the CPU has no ground movement

The four searched actions all leave `ST_IDLE` the moment they are applied —
Block pins you, Normal and Smash commit you, Jump puts you in the air — and the
auto-walk rule only moves a grounded fighter while it *is* idle. Measured over a
real match, a grounded CPU walks on about 5% of its frames. In practice it
approaches by jumping, and it cannot back off at all.

So the CPU's spacing vocabulary is thinner than this README previously claimed:
it does not wall out approaches by walking, because it never walks. Closing that
gap is a design decision rather than a bug fix — either let the auto-walk rule
run while shielding (turning Block into "advance behind shield", keeping four
actions and all eight levels), or promote movement to searched `Advance`/`Retreat`
actions as originally sketched. The second is the more honest fix and the more
expensive one: it takes the joint branching factor from 16 to 36 per node, and
since alpha-beta already runs at the square-root optimum here that is an estimated
6x per beat instead of 4x — about 1.5^depth more work, which is affordable through
depth 6 and puts 7 and 8 out of reach. Choosing it means trading two levels of
ladder for a CPU that can actually space.

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
