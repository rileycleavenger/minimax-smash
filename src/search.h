#pragma once
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include "game.h"

// Actions scoring within this many points of the best are treated as equal and
// chosen between at random.
constexpr float TIE_EPSILON = 0.2f;

// Sentinel for gRootSplitMinDepth meaning "never split the root".
constexpr int ROOT_SPLIT_OFF = MAX_CPU_LEVEL + 1;

// Measured default. With six root moves the work spreads across more siblings
// than it did with four, so the split is worth ~20% from here up; below this
// depth the thread hand-off costs more than it saves. `bench time` prints the
// sequential and split columns side by side so this stays checkable.
constexpr int ROOT_SPLIT_DEFAULT = 5;

struct SearchStats {
    Action    best = ACT_BLOCK;
    float     value = 0.0f;
    float     actionValue[ACTION_COUNT] = {};
    bool      actionExact[ACTION_COUNT] = {};
    bool      actionTied[ACTION_COUNT]  = {};
    bool      actionLegal[ACTION_COUNT] = {};
    long long nodes = 0;
    double    ms = 0.0;
    int       depth = 0;        // deepest fully completed iteration
    bool      aborted = false;
};

// Depth at which searchRoot splits its four root subtrees across threads.
// Splitting gives up the pruning alpha-beta gets from searching root siblings in
// sequence, so it only pays at the top of the ladder. Set it above MAX_CPU_LEVEL
// to force a purely sequential search -- which is what the harness wants when it
// is already saturating the cores with parallel matches, and what makes the
// serial/parallel comparison measurable rather than asserted.
extern std::atomic<int> gRootSplitMinDepth;

// Wall-clock budget for one decision. Iterative deepening publishes the best
// action from the last completed iteration, so exceeding this degrades the CPU
// to a shallower search rather than stalling -- which is right for the live game
// and wrong for the harness, where it would quietly turn a measured "depth 8"
// into a depth 6 and flatter the ladder. The harness raises it.
extern std::atomic<int> gSearchBudgetMs;
constexpr int SEARCH_BUDGET_DEFAULT_MS = 400;

// Full maximin search to a fixed depth, on the calling thread.
//
// `depth` is clamped to [1, MAX_CPU_LEVEL].
//
// `rngState` seeds the coin flip between near-tied actions. The live game
// passes nullptr and gets a per-thread stream, which is what keeps the CPU from
// being a metronome; the harness passes its own state so a measured match is
// reproducible and so two matches can be made to differ.
SearchStats searchRoot(const GameState& s, int me, int depth, uint32_t* rngState = nullptr);

// Background worker: one thread, one outstanding request.
class SearchWorker {
public:
    SearchWorker();
    ~SearchWorker();

    void request(const GameState& s, int me, int depth);
    bool poll(SearchStats& out);          // true if a fresh result was consumed
    bool busy() const { return busy_.load(std::memory_order_acquire); }

private:
    void loop();

    std::thread             th_;
    std::mutex              m_;
    std::condition_variable cv_;
    GameState               pending_{};
    int                     pendingMe_ = 1;
    int                     pendingDepth_ = 1;
    bool                    hasRequest_ = false;
    bool                    quit_ = false;

    std::mutex        rm_;
    SearchStats       result_;
    std::atomic<bool> ready_{ false };
    std::atomic<bool> busy_{ false };
};
