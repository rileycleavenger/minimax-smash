// Headless harness. Runs CPU-vs-CPU matches with no window so the search can
// be timed and, more importantly, so "higher level = stronger play" can be
// measured instead of eyeballed.
//
//   make bench && ./build/bench            full report
//   ./build/bench time                     search cost per depth only
//   ./build/bench ladder                   win rates across the depth ladder
//   ./build/bench trace <dA> <dB>          frame-by-frame log of one match

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "eval.h"
#include "game.h"
#include "search.h"

static const char* stName(uint8_t st) {
    static const char* n[] = { "IDLE", "STARTUP", "ACTIVE", "RECOVERY", "HITSTUN",
                               "BLOCK", "BLOCKLAG", "SHDBREAK", "RESPAWN", "DEAD" };
    return n[st];
}

struct MatchResult {
    int       winner = -1;          // -1 = timeout
    int       frames = 0;
    int       stocks[2] = { 0, 0 };
    float     damage[2] = { 0, 0 };
    int       kos[2] = { 0, 0 };    // stocks each fighter took off the other
    long long nodes[2] = { 0, 0 };
    double    ms[2] = { 0, 0 };
    int       searches[2] = { 0, 0 };
    int       actions[2][ACTION_COUNT] = { { 0, 0, 0, 0 }, { 0, 0, 0, 0 } };
};

static MatchResult playMatch(int charA, int charB, int depthA, int depthB,
                             int maxFrames = 60 * 120, bool trace = false) {
    GameState s;
    initGame(s, (uint8_t)charA, (uint8_t)charB);
    s.f[0].autoWalk = true;
    s.f[1].autoWalk = true;

    const int depth[2] = { depthA, depthB };
    Action held[2] = { ACT_NONE, ACT_NONE };
    int beatEnd[2] = { 0, 0 };
    int8_t prevStocks[2] = { s.f[0].stocks, s.f[1].stocks };

    MatchResult r;

    while (!matchOver(s) && s.frame < maxFrames) {
        for (int i = 0; i < 2; ++i) {
            if (s.frame < beatEnd[i]) continue;
            if (!actionable(s.f[i])) { held[i] = ACT_NONE; continue; }
            const SearchStats st = searchRoot(s, i, depth[i]);
            held[i] = st.best;
            beatEnd[i] = s.frame + BEAT_FRAMES;
            r.nodes[i] += st.nodes;
            r.ms[i] += st.ms;
            r.searches[i]++;
            r.actions[i][st.best]++;
            if (trace) {
                std::printf("f%5d  P%d %-6s  v=%8.1f d=%d/%d  | P0 %5.1f%% x%d %-8s  P1 %5.1f%% x%d %-8s\n",
                            s.frame, i, actionName(st.best), st.value, st.depth, depth[i],
                            s.f[0].damage, s.f[0].stocks, stName(s.f[0].state),
                            s.f[1].damage, s.f[1].stocks, stName(s.f[1].state));
            }
        }
        stepFrame(s, held[0], held[1], nullptr);

        for (int i = 0; i < 2; ++i) {
            if (s.f[i].stocks < prevStocks[i]) {
                r.kos[1 - i]++;
                prevStocks[i] = s.f[i].stocks;
            }
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

static void reportTiming() {
    // Sample real positions from a live match rather than one arbitrary frame:
    // a single frame can catch the fighter mid-Smash, where every action is
    // provably equivalent and the search legitimately returns instantly.
    constexpr int SAMPLES = 8;
    GameState pos[SAMPLES];
    int found = 0;

    GameState s;
    initGame(s, CHAR_PLUMBER, CHAR_SPARKMOUSE);
    s.f[0].autoWalk = s.f[1].autoWalk = true;
    Action held[2] = { ACT_NONE, ACT_NONE };
    int beatEnd[2] = { 0, 0 };

    int fr = 0, lastKeep = -999;
    for (; fr < 6000 && found < SAMPLES; ++fr) {
        for (int i = 0; i < 2; ++i) {
            if (s.frame >= beatEnd[i] && actionable(s.f[i])) {
                held[i] = searchRoot(s, i, 3).best;
                beatEnd[i] = s.frame + BEAT_FRAMES;
            } else if (!actionable(s.f[i])) {
                held[i] = ACT_NONE;
            }
        }
        stepFrame(s, held[0], held[1], nullptr);
        if (matchOver(s)) break;
        // Keep positions, well spaced, where the CPU genuinely has a choice.
        if (fr > 100 && fr - lastKeep >= 40 && actionable(s.f[1])) {
            pos[found++] = s;
            lastKeep = fr;
        }
    }

    std::printf("\n=== search cost by depth ===\n");
    std::printf("(mean over %d live mid-match positions where the CPU is actionable;\n", found);
    std::printf(" warm-up ran %d frames, %s)\n\n", fr, matchOver(s) ? "match ended" : "still live");
    std::printf("  depth        nodes       ms   branching\n");

    double prevMs = 0.0;
    for (int d = 1; d <= MAX_CPU_LEVEL; ++d) {
        long long nodes = 0;
        double ms = 0.0;
        int capped = 0;
        for (int i = 0; i < found; ++i) {
            const SearchStats st = searchRoot(pos[i], 1, d);
            nodes += st.nodes;
            ms += st.ms;
            if (st.aborted || st.depth < d) capped++;
        }
        const double avgMs = ms / (found ? found : 1);
        std::printf("  %5d   %10lld  %7.2f", d, nodes / (found ? found : 1), avgMs);
        if (d > 1 && prevMs > 0.001) std::printf("      %4.1fx", avgMs / prevMs);
        if (capped) std::printf("   <- %d/%d time-capped", capped, found);
        std::printf("\n");
        prevMs = avgMs;
    }
}

static void reportLadder(int games) {
    std::printf("\n=== depth ladder (each pairing played on both sides) ===\n");
    std::printf("higher depth should win more; row = depth, col = opponent depth\n\n");

    const int depths[] = { 1, 2, 3, 4, 5, 6 };
    const int nd = (int)(sizeof(depths) / sizeof(depths[0]));

    std::printf("        ");
    for (int j = 0; j < nd; ++j) std::printf("   d%-4d", depths[j]);
    std::printf("    total\n");

    for (int i = 0; i < nd; ++i) {
        std::printf("  d%-4d ", depths[i]);
        int tw = 0, tg = 0;
        for (int j = 0; j < nd; ++j) {
            if (i == j) { std::printf("      - "); continue; }
            int wins = 0, played = 0;
            for (int g = 0; g < games; ++g) {
                // Alternate seats and characters so neither is an advantage.
                const int cA = (g % 2) ? CHAR_SPARKMOUSE : CHAR_PLUMBER;
                const int cB = (g % 2) ? CHAR_PLUMBER : CHAR_SPARKMOUSE;
                MatchResult r = playMatch(cA, cB, depths[i], depths[j]);
                if (r.winner == 0) wins++;
                else if (r.winner == -1) {
                    // Timeout: award it on stocks then damage.
                    if (r.stocks[0] > r.stocks[1]) wins++;
                    else if (r.stocks[0] == r.stocks[1] && r.damage[0] < r.damage[1]) wins++;
                }
                played++;

                MatchResult q = playMatch(cB, cA, depths[j], depths[i]);
                if (q.winner == 1) wins++;
                else if (q.winner == -1) {
                    if (q.stocks[1] > q.stocks[0]) wins++;
                    else if (q.stocks[1] == q.stocks[0] && q.damage[1] < q.damage[0]) wins++;
                }
                played++;
            }
            std::printf("  %3d%%  ", (int)(100.0 * wins / played));
            tw += wins; tg += played;
        }
        std::printf("    %3d%%\n", (int)(100.0 * tw / tg));
    }
}

// A human who stops pressing buttons must still lose. Maximin is pessimistic by
// construction, so this checks it does not freeze up against a target that
// never threatens it.
static void reportPassive(int lo = 1, int hi = MAX_CPU_LEVEL, int capSec = 180) {
    std::printf("\n=== closing out a passive opponent (P0 never acts) ===\n");
    std::printf("  level   result             time     P0 dmg   blocks\n");

    for (int d = lo; d <= hi; ++d) {
        GameState s;
        initGame(s, CHAR_PLUMBER, CHAR_SPARKMOUSE);
        s.f[0].autoWalk = false;   // stand still, like a player who never touches the stick
        s.f[0].walkDir = 0;
        s.f[1].autoWalk = true;

        Action held = ACT_NONE;
        int beatEnd = 0, blocks = 0, searches = 0;
        const int cap = 60 * capSec;

        while (!matchOver(s) && s.frame < cap) {
            if (s.frame >= beatEnd) {
                if (actionable(s.f[1])) {
                    held = searchRoot(s, 1, d).best;
                    beatEnd = s.frame + BEAT_FRAMES;
                    if (held == ACT_BLOCK) blocks++;
                    searches++;
                } else {
                    held = ACT_NONE;
                }
            }
            stepFrame(s, ACT_NONE, held, nullptr);
        }

        const bool won = (winnerOf(s) == 1);
        std::printf("  %5d   %-17s %5.1fs   %6.0f%%   %3d%%\n", d,
                    won ? "CPU wins 3-0" : (matchOver(s) ? "CPU LOSES" : "STALEMATE (timeout)"),
                    s.frame / 60.0, s.f[0].damage,
                    searches ? (int)(100.0 * blocks / searches) : 0);
    }
}

static void reportSample() {
    std::printf("\n=== sample match: depth 1 vs depth 5 ===\n");
    MatchResult r = playMatch(CHAR_PLUMBER, CHAR_SPARKMOUSE, 1, 5);
    const char* w = (r.winner < 0) ? "timeout" : (r.winner == 0 ? "P0 (depth 1)" : "P1 (depth 5)");
    std::printf("  winner        %s  after %.1fs\n", w, r.frames / 60.0);
    std::printf("  stocks left   P0 %d   P1 %d\n", r.stocks[0], r.stocks[1]);
    std::printf("  KOs scored    P0 %d   P1 %d\n", r.kos[0], r.kos[1]);
    for (int i = 0; i < 2; ++i) {
        const int tot = r.searches[i] ? r.searches[i] : 1;
        std::printf("  P%d depth %d   %4d searches, %6.2f ms avg  |  mix: ", i, i == 0 ? 1 : 5,
                    r.searches[i], r.ms[i] / tot);
        for (int a = 0; a < ACTION_COUNT; ++a)
            std::printf("%s %d%%  ", actionName((Action)a), (int)(100.0 * r.actions[i][a] / tot));
        std::printf("\n");
    }
}

int main(int argc, char** argv) {
    const std::string mode = (argc > 1) ? argv[1] : "all";

    if (mode == "trace") {
        const int dA = (argc > 2) ? std::atoi(argv[2]) : 1;
        const int dB = (argc > 3) ? std::atoi(argv[3]) : 5;
        std::printf("=== trace: depth %d vs depth %d ===\n", dA, dB);
        MatchResult r = playMatch(CHAR_PLUMBER, CHAR_SPARKMOUSE, dA, dB, 60 * 120, true);
        std::printf("winner %d after %.1fs (stocks %d / %d)\n", r.winner, r.frames / 60.0,
                    r.stocks[0], r.stocks[1]);
        return 0;
    }
    if (mode == "time" || mode == "all") reportTiming();
    if (mode == "passive") {
        const int lo = (argc > 2) ? std::atoi(argv[2]) : 1;
        const int hi = (argc > 3) ? std::atoi(argv[3]) : MAX_CPU_LEVEL;
        const int cap = (argc > 4) ? std::atoi(argv[4]) : 180;
        reportPassive(lo, hi, cap);
        return 0;
    }
    if (mode == "all") reportPassive();
    if (mode == "sample" || mode == "all") reportSample();
    if (mode == "ladder" || mode == "all") reportLadder(2);
    return 0;
}
