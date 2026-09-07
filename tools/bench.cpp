// Headless harness. Runs CPU-vs-CPU matches with no window so the search can be
// timed and, more importantly, so "higher level = stronger play" can be measured
// instead of eyeballed.
//
//   make bench && ./build/bench              full report
//   ./build/bench time                       search cost per depth, serial vs root-split
//   ./build/bench ladder [games] [maxDepth]  round robin, every pairing both seats
//   ./build/bench gauntlet [games] [ref]     every level against one reference level
//   ./build/bench diag [games] [ref]         what each level actually *does*
//   ./build/bench passive [lo] [hi] [secs]   closing out an opponent who never acts
//   ./build/bench probe                      chosen move in positions with a known answer
//   ./build/bench sample                     one depth-1 vs depth-5 match summary
//   ./build/bench trace <dA> <dB> [seed]     every decision of a single match
//
// Matches are seeded and start from varied positions, so a win rate is a sample
// of many openings rather than one deterministic trajectory replayed N times.
// Independent matches run on separate cores; the search itself is forced
// sequential while that happens so the two do not fight over the machine.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <thread>
#include <vector>

#include "eval.h"
#include "game.h"
#include "search.h"

// ---------------------------------------------------------------------------
// small utilities
// ---------------------------------------------------------------------------

static const char* stName(uint8_t st) {
    static const char* n[] = { "IDLE", "STARTUP", "ACTIVE", "RECOVERY", "HITSTUN",
                               "BLOCK", "BLOCKLAG", "SHDBREAK", "RESPAWN", "DEAD" };
    return n[st];
}

static uint32_t rnd(uint32_t& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

static float rndf(uint32_t& s, float lo, float hi) {
    return lo + (hi - lo) * (float)((rnd(s) >> 8) * (1.0 / 16777216.0));
}

// Decorrelate a match index into a search seed; consecutive indices must not
// produce correlated openings.
static uint32_t mixSeed(uint32_t x) {
    x += 0x9e3779b9u;
    x = (x ^ (x >> 16)) * 0x85ebca6bu;
    x = (x ^ (x >> 13)) * 0xc2b2ae35u;
    return (x ^ (x >> 16)) | 1u;
}

// Run body(0..n-1) across the cores, one index at a time. Each job must write
// only to its own slot.
template <class F>
static void parallelFor(int n, F body) {
    int hw = (int)std::thread::hardware_concurrency();
    if (hw < 1) hw = 1;
    const int workers = std::min(n, hw);
    if (workers <= 1) {
        for (int i = 0; i < n; ++i) body(i);
        return;
    }
    std::atomic<int> next{ 0 };
    std::vector<std::thread> th;
    th.reserve((size_t)workers);
    for (int w = 0; w < workers; ++w)
        th.emplace_back([&] {
            for (int i = next.fetch_add(1); i < n; i = next.fetch_add(1)) body(i);
        });
    for (auto& t : th) t.join();
}

// Progress goes to stderr, and only to a terminal: piping a report to a file
// should not interleave a hundred progress lines through the table.
static void progress(int done, int total) {
    if (!isatty(2)) return;
    if (done % 8 && done != total) return;
    std::fprintf(stderr, "\r  %d/%d matches", done, total);
    std::fflush(stderr);
}

static void progressDone() {
    if (isatty(2)) std::fprintf(stderr, "\r%*s\r", 30, "");
}

// 1-sigma binomial error on a win rate, in percentage points.
static double stderrPct(double wins, double games) {
    if (games <= 0.0) return 0.0;
    const double p = wins / games;
    return 100.0 * std::sqrt(p * (1.0 - p) / games);
}

// ---------------------------------------------------------------------------
// one match
// ---------------------------------------------------------------------------

struct MatchCfg {
    int      chr[2]   = { CHAR_PLUMBER, CHAR_SPARKMOUSE };
    int      depth[2] = { 1, 5 };
    uint32_t seed     = 1;
    int      maxFrames = 60 * 120;
    bool     trace    = false;
    bool     passive0 = false;   // seat 0 stands still and never presses anything
};

struct MatchResult {
    int       winner = -1;              // -1 = timeout
    int       frames = 0;
    int       stocks[2] = { 0, 0 };
    float     damage[2] = { 0, 0 };
    int       kos[2] = { 0, 0 };        // stocks each fighter took off the other
    int       selfDestructs[2] = { 0, 0 };
    int       shieldBreaks[2] = { 0, 0 };
    double    koDamageSum[2] = { 0, 0 }; // damage each fighter was carrying when KO'd
    int       koDamageN[2] = { 0, 0 };
    long long nodes[2] = { 0, 0 };
    double    ms[2] = { 0, 0 };
    int       searches[2] = { 0, 0 };
    int       actions[2][ACTION_COUNT] = {};
};

// Both fighters drop in from the respawn height as they do after a KO, but from
// varied x so a ladder samples many openings instead of one.
static void randomizeStart(GameState& s, uint32_t& rng) {
    float a = rndf(rng, -0.80f, 0.10f) * STAGE_HALF;
    float b = rndf(rng, -0.10f, 0.80f) * STAGE_HALF;
    if (b - a < 80.0f) { a -= 40.0f; b += 40.0f; }
    s.f[0].x = a;
    s.f[1].x = b;
}

static MatchResult playMatch(const MatchCfg& cfg) {
    GameState s;
    initGame(s, (uint8_t)cfg.chr[0], (uint8_t)cfg.chr[1]);
    s.f[0].autoWalk = !cfg.passive0;
    s.f[1].autoWalk = true;
    if (cfg.passive0) s.f[0].walkDir = 0;

    uint32_t startRng = mixSeed(cfg.seed);
    randomizeStart(s, startRng);
    // Independent tie-break streams, so the two seats never mirror each other.
    uint32_t rng[2] = { mixSeed(cfg.seed ^ 0xA5A5A5A5u), mixSeed(cfg.seed ^ 0x5A5A5A5Au) };

    Action held[2] = { ACT_NONE, ACT_NONE };
    int    beatEnd[2] = { 0, 0 };
    int8_t prevStocks[2] = { s.f[0].stocks, s.f[1].stocks };
    float  prevDamage[2] = { s.f[0].damage, s.f[1].damage };
    int    lastHitstun[2] = { -9999, -9999 };
    uint8_t prevState[2] = { s.f[0].state, s.f[1].state };

    MatchResult r;

    while (!matchOver(s) && s.frame < cfg.maxFrames) {
        for (int i = 0; i < 2; ++i) {
            if (cfg.passive0 && i == 0) { held[0] = ACT_NONE; continue; }
            if (s.frame < beatEnd[i]) continue;
            if (!actionable(s.f[i])) { held[i] = ACT_NONE; continue; }
            const SearchStats st = searchRoot(s, i, cfg.depth[i], &rng[i]);
            held[i] = st.best;
            beatEnd[i] = s.frame + BEAT_FRAMES;
            r.nodes[i] += st.nodes;
            r.ms[i] += st.ms;
            r.searches[i]++;
            r.actions[i][st.best]++;
            if (cfg.trace) {
                std::printf("f%5d  P%d %-6s  v=%8.1f d=%d/%d  | P0 %5.1f%% x%d %-8s  P1 %5.1f%% x%d %-8s\n",
                            s.frame, i, actionName(st.best), st.value, st.depth, cfg.depth[i],
                            s.f[0].damage, s.f[0].stocks, stName(s.f[0].state),
                            s.f[1].damage, s.f[1].stocks, stName(s.f[1].state));
            }
        }
        stepFrame(s, held[0], held[1], nullptr);

        for (int i = 0; i < 2; ++i) {
            if (s.f[i].state == ST_HITSTUN) lastHitstun[i] = s.frame;
            if (s.f[i].state == ST_SHIELDBREAK && prevState[i] != ST_SHIELDBREAK) r.shieldBreaks[i]++;
            if (s.f[i].stocks < prevStocks[i]) {
                r.kos[1 - i]++;
                // A knockback KO happens during, or moments after, hitstun.
                // Anything else is the fighter walking or drifting off on its own.
                if (s.frame - lastHitstun[i] > 45) r.selfDestructs[i]++;
                r.koDamageSum[i] += prevDamage[i];
                r.koDamageN[i]++;
                prevStocks[i] = s.f[i].stocks;
            }
            prevDamage[i] = s.f[i].damage;
            prevState[i] = s.f[i].state;
        }
    }

    r.winner = winnerOf(s);
    r.frames = s.frame;
    for (int i = 0; i < 2; ++i) {
        r.stocks[i] = s.f[i].stocks;
        r.damage[i] = s.f[i].damage;
    }
    return r;
}

// Result of a match from one seat's point of view: 1 win, 0 loss, 0.5 dead heat.
// A timeout is awarded on stocks, then on damage taken.
static float scoreFor(const MatchResult& r, int seat) {
    const int o = 1 - seat;
    if (r.winner == seat) return 1.0f;
    if (r.winner == o) return 0.0f;
    if (r.stocks[seat] != r.stocks[o]) return r.stocks[seat] > r.stocks[o] ? 1.0f : 0.0f;
    if (r.damage[seat] != r.damage[o]) return r.damage[seat] < r.damage[o] ? 1.0f : 0.0f;
    return 0.5f;
}

// ---------------------------------------------------------------------------
// search cost
// ---------------------------------------------------------------------------

// Sample real positions from live matches rather than one arbitrary frame: a
// single frame can catch the fighter mid-Smash, where every action is provably
// equivalent and the search legitimately returns instantly.
static std::vector<GameState> samplePositions(int want) {
    std::vector<GameState> pos;
    pos.reserve((size_t)want);

    for (uint32_t seed = 1; pos.size() < (size_t)want && seed < 40; ++seed) {
        GameState s;
        initGame(s, CHAR_PLUMBER, CHAR_SPARKMOUSE);
        s.f[0].autoWalk = s.f[1].autoWalk = true;
        uint32_t sr = mixSeed(seed);
        randomizeStart(s, sr);

        uint32_t rng[2] = { mixSeed(seed * 7u + 1u), mixSeed(seed * 13u + 3u) };
        Action held[2] = { ACT_NONE, ACT_NONE };
        int beatEnd[2] = { 0, 0 };
        int lastKeep = -999;
        const int perSeed = 2;
        int kept = 0;

        for (int fr = 0; fr < 4000 && kept < perSeed && pos.size() < (size_t)want; ++fr) {
            for (int i = 0; i < 2; ++i) {
                if (s.frame >= beatEnd[i] && actionable(s.f[i])) {
                    held[i] = searchRoot(s, i, 3, &rng[i]).best;
                    beatEnd[i] = s.frame + BEAT_FRAMES;
                } else if (!actionable(s.f[i])) {
                    held[i] = ACT_NONE;
                }
            }
            stepFrame(s, held[0], held[1], nullptr);
            if (matchOver(s)) break;
            if (fr > 90 && fr - lastKeep >= 60 && actionable(s.f[1])) {
                pos.push_back(s);
                lastKeep = fr;
                kept++;
            }
        }
    }
    return pos;
}

static void reportTiming() {
    const std::vector<GameState> pos = samplePositions(16);
    const int found = (int)pos.size();

    std::printf("\n=== search cost by depth ===\n");
    std::printf("(mean over %d live mid-match positions where the CPU has a real choice)\n\n", found);
    std::printf("  depth        nodes       ms   branching     root-split ms\n");

    double prevMs = 0.0;
    for (int d = 1; d <= MAX_CPU_LEVEL; ++d) {
        long long nodes = 0;
        double ms = 0.0;
        int capped = 0;

        gRootSplitMinDepth.store(ROOT_SPLIT_OFF);
        for (int i = 0; i < found; ++i) {
            const SearchStats st = searchRoot(pos[i], 1, d);
            nodes += st.nodes;
            ms += st.ms;
            if (st.aborted || st.depth < d) capped++;
        }
        const double avgMs = ms / (found ? found : 1);

        // Same positions again with the root subtrees split across cores.
        gRootSplitMinDepth.store(d);
        double splitMs = 0.0;
        for (int i = 0; i < found; ++i) splitMs += searchRoot(pos[i], 1, d).ms;
        splitMs /= (found ? found : 1);
        gRootSplitMinDepth.store(ROOT_SPLIT_OFF);

        std::printf("  %5d   %10lld  %7.2f", d, nodes / (found ? found : 1), avgMs);
        if (d > 1 && prevMs > 0.0005) std::printf("      %4.1fx", avgMs / prevMs);
        else                          std::printf("           ");
        std::printf("     %7.2f  %s", splitMs, splitMs < avgMs * 0.9 ? "(faster)" : "");
        if (capped) std::printf("   <- %d/%d time-capped", capped, found);
        std::printf("\n");
        prevMs = avgMs;
    }
    std::printf("\n  One beat is %d frames = %.0f ms of wall clock, and the search runs on its\n",
                BEAT_FRAMES, BEAT_FRAMES * 1000.0 / 60.0);
    std::printf("  own thread, so any depth under that never stalls the frame loop.\n");
}

// ---------------------------------------------------------------------------
// depth ladder
// ---------------------------------------------------------------------------

struct LadderJob {
    int      row, col;      // indices into the depth list
    int      rowSeat;       // which seat the row player takes
    int      cRow, cCol;    // character for each of them
    uint32_t seed;
    float    rowScore = 0.0f;
};

static void reportLadder(int games, int maxDepth) {
    std::vector<int> depths;
    for (int d = 1; d <= maxDepth; ++d) depths.push_back(d);
    const int nd = (int)depths.size();

    // Every unordered pair is played `games` times in each seat, with the two
    // characters swapped between seats so neither side is an advantage.
    std::vector<LadderJob> jobs;
    for (int i = 0; i < nd; ++i)
        for (int j = i + 1; j < nd; ++j)
            for (int g = 0; g < games; ++g)
                for (int seat = 0; seat < 2; ++seat) {
                    const int cA = (g % 2) ? CHAR_SPARKMOUSE : CHAR_PLUMBER;
                    const int cB = (g % 2) ? CHAR_PLUMBER : CHAR_SPARKMOUSE;
                    LadderJob jb;
                    jb.row = i; jb.col = j; jb.rowSeat = seat;
                    jb.cRow = seat == 0 ? cA : cB;
                    jb.cCol = seat == 0 ? cB : cA;
                    jb.seed = (uint32_t)(((i * 97 + j) * 31 + g) * 4 + seat) + 1u;
                    jobs.push_back(jb);
                }

    std::printf("\n=== depth ladder (%d matches, every pairing in both seats) ===\n", (int)jobs.size());
    std::printf("row beats column this often; +- is one binomial sigma\n\n");
    std::fflush(stdout);

    gRootSplitMinDepth.store(ROOT_SPLIT_OFF);
    std::atomic<int> done{ 0 };
    const int total = (int)jobs.size();
    parallelFor(total, [&](int k) {
        LadderJob& jb = jobs[(size_t)k];
        MatchCfg cfg;
        cfg.seed = jb.seed;
        cfg.chr[jb.rowSeat] = jb.cRow;
        cfg.chr[1 - jb.rowSeat] = jb.cCol;
        cfg.depth[jb.rowSeat] = depths[(size_t)jb.row];
        cfg.depth[1 - jb.rowSeat] = depths[(size_t)jb.col];
        jb.rowScore = scoreFor(playMatch(cfg), jb.rowSeat);
        progress(done.fetch_add(1) + 1, total);
    });
    progressDone();

    std::vector<std::vector<double>> wins((size_t)nd, std::vector<double>((size_t)nd, 0.0));
    std::vector<std::vector<double>> played((size_t)nd, std::vector<double>((size_t)nd, 0.0));
    for (const LadderJob& jb : jobs) {
        wins[(size_t)jb.row][(size_t)jb.col] += jb.rowScore;
        played[(size_t)jb.row][(size_t)jb.col] += 1.0;
        wins[(size_t)jb.col][(size_t)jb.row] += 1.0f - jb.rowScore;
        played[(size_t)jb.col][(size_t)jb.row] += 1.0;
    }

    std::printf("        ");
    for (int j = 0; j < nd; ++j) std::printf("   d%-4d", depths[(size_t)j]);
    std::printf("      total\n");

    std::vector<double> tot((size_t)nd, 0.0);
    for (int i = 0; i < nd; ++i) {
        std::printf("  d%-4d ", depths[(size_t)i]);
        double tw = 0.0, tg = 0.0;
        for (int j = 0; j < nd; ++j) {
            if (i == j) { std::printf("      - "); continue; }
            std::printf("  %3.0f%%  ", 100.0 * wins[(size_t)i][(size_t)j] / played[(size_t)i][(size_t)j]);
            tw += wins[(size_t)i][(size_t)j];
            tg += played[(size_t)i][(size_t)j];
        }
        tot[(size_t)i] = 100.0 * tw / tg;
        std::printf("   %3.0f%% +-%.0f\n", tot[(size_t)i], stderrPct(tw, tg));
    }

    std::printf("\n  ");
    int inversions = 0;
    for (int i = 1; i < nd; ++i)
        if (tot[(size_t)i] < tot[(size_t)i - 1]) {
            if (inversions++ == 0) std::printf("NOT monotonic:");
            std::printf(" d%d<d%d", depths[(size_t)i], depths[(size_t)i - 1]);
        }
    if (!inversions) std::printf("monotonic: every extra beat of lookahead is worth win rate.");
    std::printf("\n");
}

// ---------------------------------------------------------------------------
// gauntlet: every level against one reference level
// ---------------------------------------------------------------------------
//
// O(levels) instead of the ladder's O(levels^2), which is what makes depth 7 and
// 8 -- the expensive end -- affordable to measure at all.

struct GauntletJob {
    int      level, seat, cSelf, cRef;
    uint32_t seed;
    float    score = 0.0f;
    int      selfDestructs = 0;
    int      frames = 0;
};

static void reportGauntlet(int games, int ref, int maxDepth) {
    std::vector<GauntletJob> jobs;
    for (int d = 1; d <= maxDepth; ++d) {
        if (d == ref) continue;
        for (int g = 0; g < games; ++g)
            for (int seat = 0; seat < 2; ++seat) {
                GauntletJob jb;
                jb.level = d; jb.seat = seat;
                jb.cSelf = (g % 2) ? CHAR_SPARKMOUSE : CHAR_PLUMBER;
                jb.cRef  = (g % 2) ? CHAR_PLUMBER : CHAR_SPARKMOUSE;
                jb.seed = (uint32_t)((d * 211 + g) * 2 + seat) + 1u;
                jobs.push_back(jb);
            }
    }

    std::printf("\n=== gauntlet: every level vs a depth-%d reference (%d matches) ===\n",
                ref, (int)jobs.size());
    std::fflush(stdout);

    gRootSplitMinDepth.store(ROOT_SPLIT_OFF);
    std::atomic<int> done{ 0 };
    const int total = (int)jobs.size();
    parallelFor(total, [&](int k) {
        GauntletJob& jb = jobs[(size_t)k];
        MatchCfg cfg;
        cfg.seed = jb.seed;
        cfg.chr[jb.seat] = jb.cSelf;
        cfg.chr[1 - jb.seat] = jb.cRef;
        cfg.depth[jb.seat] = jb.level;
        cfg.depth[1 - jb.seat] = ref;
        const MatchResult r = playMatch(cfg);
        jb.score = scoreFor(r, jb.seat);
        jb.selfDestructs = r.selfDestructs[jb.seat];
        jb.frames = r.frames;
        progress(done.fetch_add(1) + 1, total);
    });
    progressDone();

    std::printf("\n  level   win rate        match len   self-KOs/match\n");
    double prev = -1.0;
    int inversions = 0;
    for (int d = 1; d <= maxDepth; ++d) {
        if (d == ref) { std::printf("  %5d      -  (reference)\n", d); continue; }
        double w = 0.0, n = 0.0, sd = 0.0, fr = 0.0;
        for (const GauntletJob& jb : jobs)
            if (jb.level == d) { w += jb.score; n += 1.0; sd += jb.selfDestructs; fr += jb.frames; }
        std::printf("  %5d   %5.1f%% +-%-4.1f      %5.1fs        %4.2f\n",
                    d, 100.0 * w / n, stderrPct(w, n), fr / n / 60.0, sd / n);
        const double rate = 100.0 * w / n;
        if (prev >= 0.0 && rate < prev - 1e-9) inversions++;
        prev = rate;
    }
    std::printf("\n  %s\n", inversions ? "NOT monotonic across the ladder."
                                       : "monotonic: win rate rises with every extra beat.");
}

// ---------------------------------------------------------------------------
// behaviour diagnostics
// ---------------------------------------------------------------------------

struct DiagJob {
    int      level, seat, cSelf, cRef;
    uint32_t seed;
    MatchResult r;
};

static void reportDiag(int games, int ref, int maxDepth) {
    std::vector<DiagJob> jobs;
    for (int d = 1; d <= maxDepth; ++d)
        for (int g = 0; g < games; ++g)
            for (int seat = 0; seat < 2; ++seat) {
                DiagJob jb;
                jb.level = d; jb.seat = seat;
                jb.cSelf = (g % 2) ? CHAR_SPARKMOUSE : CHAR_PLUMBER;
                jb.cRef  = (g % 2) ? CHAR_PLUMBER : CHAR_SPARKMOUSE;
                jb.seed = (uint32_t)((d * 419 + g) * 2 + seat) + 101u;
                jobs.push_back(jb);
            }

    std::printf("\n=== what each level actually does (vs depth-%d reference, %d matches) ===\n",
                ref, (int)jobs.size());
    std::fflush(stdout);

    gRootSplitMinDepth.store(ROOT_SPLIT_OFF);
    std::atomic<int> done{ 0 };
    const int total = (int)jobs.size();
    parallelFor(total, [&](int k) {
        DiagJob& jb = jobs[(size_t)k];
        MatchCfg cfg;
        cfg.seed = jb.seed;
        cfg.chr[jb.seat] = jb.cSelf;
        cfg.chr[1 - jb.seat] = jb.cRef;
        cfg.depth[jb.seat] = jb.level;
        cfg.depth[1 - jb.seat] = ref;
        jb.r = playMatch(cfg);
        progress(done.fetch_add(1) + 1, total);
    });
    progressDone();

    std::printf("\n  level   JUMP  NORMAL   SMASH   BLOCK     ADV     RET   self-KO/m   shieldbreaks/m   KO'd at\n");
    for (int d = 1; d <= maxDepth; ++d) {
        double act[ACTION_COUNT] = {};
        double searches = 0, matches = 0, sd = 0, sb = 0, koDmg = 0, koN = 0;
        for (const DiagJob& jb : jobs) {
            if (jb.level != d) continue;
            const int me = jb.seat;
            for (int a = 0; a < ACTION_COUNT; ++a) act[a] += jb.r.actions[me][a];
            searches += jb.r.searches[me];
            sd += jb.r.selfDestructs[me];
            sb += jb.r.shieldBreaks[me];
            koDmg += jb.r.koDamageSum[me];
            koN += jb.r.koDamageN[me];
            matches += 1.0;
        }
        if (searches <= 0.0) searches = 1.0;
        std::printf("  %5d  %4.0f%%   %4.0f%%   %4.0f%%   %4.0f%%   %4.0f%%   %4.0f%%      %5.2f            %5.2f     %5.0f%%\n",
                    d, 100.0 * act[ACT_JUMP] / searches, 100.0 * act[ACT_NORMAL] / searches,
                    100.0 * act[ACT_SMASH] / searches, 100.0 * act[ACT_BLOCK] / searches,
                    100.0 * act[ACT_ADVANCE] / searches, 100.0 * act[ACT_RETREAT] / searches,
                    sd / matches, sb / matches, koN > 0 ? koDmg / koN : 0.0);
    }
}

// ---------------------------------------------------------------------------
// passive opponent
// ---------------------------------------------------------------------------

// A human who stops pressing buttons must still lose, and lose in reasonable
// time. Maximin is pessimistic by construction, so this checks it does not
// freeze up against a target that never threatens it -- and averages over
// several openings, because a single scripted match is not evidence.
struct PassiveJob {
    int      level;
    uint32_t seed;
    bool     won = false, timedOut = false;
    double   secs = 0.0;
    double   blockPct = 0.0;
};

static void reportPassive(int lo, int hi, int capSec, int games) {
    std::vector<PassiveJob> jobs;
    for (int d = lo; d <= hi; ++d)
        for (int g = 0; g < games; ++g) jobs.push_back(PassiveJob{ d, (uint32_t)(d * 131 + g) + 7u });

    std::printf("\n=== closing out a passive opponent (P0 never acts, %d openings each) ===\n", games);
    std::fflush(stdout);

    gRootSplitMinDepth.store(ROOT_SPLIT_OFF);
    std::atomic<int> done{ 0 };
    const int total = (int)jobs.size();
    parallelFor(total, [&](int k) {
        PassiveJob& jb = jobs[(size_t)k];
        MatchCfg cfg;
        cfg.depth[0] = 1;
        cfg.depth[1] = jb.level;
        cfg.passive0 = true;
        cfg.seed = jb.seed;
        cfg.maxFrames = 60 * capSec;
        const MatchResult r = playMatch(cfg);
        const int searches = r.searches[1] ? r.searches[1] : 1;
        jb.won = (r.winner == 1);
        jb.timedOut = (r.winner < 0);
        jb.secs = r.frames / 60.0;
        jb.blockPct = 100.0 * r.actions[1][ACT_BLOCK] / searches;
        progress(done.fetch_add(1) + 1, total);
    });
    progressDone();

    std::printf("\n  level   3-0 wins   mean time   slowest   blocks\n");
    for (int d = lo; d <= hi; ++d) {
        int wins = 0, n = 0, stalls = 0;
        double sum = 0.0, worst = 0.0, blocks = 0.0;
        for (const PassiveJob& jb : jobs) {
            if (jb.level != d) continue;
            n++;
            wins += jb.won ? 1 : 0;
            stalls += jb.timedOut ? 1 : 0;
            sum += jb.secs;
            blocks += jb.blockPct;
            if (jb.secs > worst) worst = jb.secs;
        }
        std::printf("  %5d      %d/%d      %6.1fs   %6.1fs     %3.0f%%%s\n", d, wins, n,
                    sum / n, worst, blocks / n, stalls ? "   <- STALEMATE" : "");
    }
}

// ---------------------------------------------------------------------------
// probe: hand-built positions with a known right answer
// ---------------------------------------------------------------------------
//
// Win rates say the ladder is ordered but not what the extra beats are buying.
// These are positions where a human can name the correct move, so the ladder can
// be read as "at what depth does it start seeing this?".

static void groundedNeutral(GameState& s, float x0, float x1, float dmg0, float dmg1) {
    initGame(s, CHAR_PLUMBER, CHAR_SPARKMOUSE);
    const float x[2] = { x0, x1 };
    const float d[2] = { dmg0, dmg1 };
    for (int i = 0; i < 2; ++i) {
        Fighter& f = s.f[i];
        f.state = ST_IDLE;
        f.stateTimer = 0;
        f.invuln = 0;
        f.jumpLock = 0;
        f.jumpsLeft = (int8_t)charStats(f.charId).maxJumps;
        f.x = x[i];
        f.y = 0.0f;
        f.vx = f.vy = 0.0f;
        f.onGround = true;
        f.damage = d[i];
        f.shield = charStats(f.charId).shieldMax;
        f.autoWalk = true;
        f.hitUsed = false;
    }
    s.f[0].facing = 1;
    s.f[1].facing = -1;
}

struct Scenario {
    const char* name;
    const char* want;
    void (*setup)(GameState&);
};

// In every scenario the CPU under test is P1 and the opponent is P0.
static void scShieldBroken(GameState& s) {
    groundedNeutral(s, 0.0f, 60.0f, 40.0f, 40.0f);
    s.f[0].state = ST_SHIELDBREAK;
    s.f[0].stateTimer = 90;
    s.f[0].shield = 0.0f;
}

static void scWhiffedSmash(GameState& s) {
    groundedNeutral(s, 0.0f, 70.0f, 60.0f, 30.0f);
    s.f[0].moveId = MOVE_SMASH;
    s.f[0].state = ST_RECOVERY;
    s.f[0].stateTimer = 24;
}

static void scSmashWindup(GameState& s) {
    groundedNeutral(s, 0.0f, 55.0f, 20.0f, 90.0f);
    s.f[0].moveId = MOVE_SMASH;
    s.f[0].state = ST_STARTUP;
    s.f[0].stateTimer = 14;
}

static void scKillPercent(GameState& s) {
    groundedNeutral(s, 210.0f, 150.0f, 145.0f, 20.0f);
    s.f[0].state = ST_RECOVERY;
    s.f[0].moveId = MOVE_NORMAL;
    s.f[0].stateTimer = 22;
}

static void scMyLedgeDanger(GameState& s) {
    groundedNeutral(s, 200.0f, 285.0f, 10.0f, 150.0f);
}

static void reportProbe() {
    static const Scenario scen[] = {
        { "opponent shield broken, 90f",  "convert it into damage",   scShieldBroken },
        { "opponent whiffed a Smash",     "punish: SMASH or NORMAL",  scWhiffedSmash },
        { "opponent winding up a Smash",  "do not stand in it",       scSmashWindup  },
        { "opponent open, CPU at 20%",    "kill: SMASH at 145%",      scKillPercent  },
        { "CPU cornered at 150%",         "get off the ledge",        scMyLedgeDanger },
    };

    std::printf("\n=== probe: what each extra beat of lookahead buys ===\n");
    gRootSplitMinDepth.store(ROOT_SPLIT_OFF);

    for (const Scenario& sc : scen) {
        std::printf("\n  %s\n  want -> %s\n    depth: ", sc.name, sc.want);
        for (int d = 1; d <= MAX_CPU_LEVEL; ++d) std::printf("%-8d", d);
        std::printf("\n    picks: ");
        for (int d = 1; d <= MAX_CPU_LEVEL; ++d) {
            GameState s;
            sc.setup(s);
            uint32_t rng = 0x1234567u;
            std::printf("%-8s", actionName(searchRoot(s, 1, d, &rng).best));
        }
        std::printf("\n");

        // Values at the top of the ladder, so a near-tie is visible rather than
        // hidden behind whichever side of it the coin landed on.
        GameState s;
        sc.setup(s);
        uint32_t rng = 0x1234567u;
        const SearchStats st = searchRoot(s, 1, MAX_CPU_LEVEL, &rng);
        std::printf("    d%d values: ", MAX_CPU_LEVEL);
        for (int a = 0; a < ACTION_COUNT; ++a)
            std::printf("%s %s%.1f   ", actionName((Action)a),
                        st.actionExact[a] ? "" : "<", st.actionValue[a]);
        std::printf("\n");
    }
}

// ---------------------------------------------------------------------------
// single-match summary
// ---------------------------------------------------------------------------

static void reportSample() {
    std::printf("\n=== sample match: depth 1 vs depth 5 ===\n");
    MatchCfg cfg;
    cfg.depth[0] = 1;
    cfg.depth[1] = 5;
    cfg.seed = 12345;
    gRootSplitMinDepth.store(ROOT_SPLIT_OFF);
    const MatchResult r = playMatch(cfg);
    const char* w = (r.winner < 0) ? "timeout" : (r.winner == 0 ? "P0 (depth 1)" : "P1 (depth 5)");
    std::printf("  winner        %s  after %.1fs\n", w, r.frames / 60.0);
    std::printf("  stocks left   P0 %d   P1 %d\n", r.stocks[0], r.stocks[1]);
    std::printf("  KOs scored    P0 %d   P1 %d   (self-KOs  P0 %d   P1 %d)\n",
                r.kos[0], r.kos[1], r.selfDestructs[0], r.selfDestructs[1]);
    for (int i = 0; i < 2; ++i) {
        const int tot = r.searches[i] ? r.searches[i] : 1;
        std::printf("  P%d depth %d   %4d searches, %6.2f ms avg  |  mix: ", i, cfg.depth[i],
                    r.searches[i], r.ms[i] / tot);
        for (int a = 0; a < ACTION_COUNT; ++a)
            std::printf("%s %d%%  ", actionName((Action)a), (int)(100.0 * r.actions[i][a] / tot));
        std::printf("\n");
    }
}

// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    const std::string mode = (argc > 1) ? argv[1] : "all";

    // Measurement must not be time-sliced: many matches run concurrently here,
    // and the live game's decision budget would turn a contended depth-8 search
    // into a depth-6 one without saying so.
    gSearchBudgetMs.store(600000);
    auto argInt = [&](int idx, int def) { return (argc > idx) ? std::atoi(argv[idx]) : def; };

    if (mode == "trace") {
        MatchCfg cfg;
        cfg.depth[0] = argInt(2, 1);
        cfg.depth[1] = argInt(3, 5);
        cfg.seed = (uint32_t)argInt(4, 1);
        cfg.trace = true;
        std::printf("=== trace: depth %d vs depth %d (seed %u) ===\n", cfg.depth[0], cfg.depth[1], cfg.seed);
        const MatchResult r = playMatch(cfg);
        std::printf("winner %d after %.1fs (stocks %d / %d)\n", r.winner, r.frames / 60.0,
                    r.stocks[0], r.stocks[1]);
        return 0;
    }
    if (mode == "time" || mode == "all") reportTiming();
    if (mode == "diag")     { reportDiag(argInt(2, 6), argInt(3, 4), MAX_CPU_LEVEL); return 0; }
    if (mode == "gauntlet") { reportGauntlet(argInt(2, 12), argInt(3, 4), MAX_CPU_LEVEL); return 0; }
    if (mode == "passive")  { reportPassive(argInt(2, 1), argInt(3, MAX_CPU_LEVEL), argInt(4, 180), argInt(5, 5)); return 0; }
    if (mode == "ladder")   { reportLadder(argInt(2, 8), argInt(3, 6)); return 0; }

    if (mode == "all") {
        reportPassive(1, MAX_CPU_LEVEL, 180, 5);
        reportSample();
        reportLadder(8, 6);
        reportGauntlet(12, 4, MAX_CPU_LEVEL);
        reportDiag(6, 4, MAX_CPU_LEVEL);
        reportProbe();
    }
    if (mode == "probe") reportProbe();
    if (mode == "sample") reportSample();
    return 0;
}
