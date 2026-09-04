#include "search.h"
#include "eval.h"

#include <algorithm>
#include <chrono>
#include <cmath>

// ---------------------------------------------------------------------------
// Maximin over the 4x4 joint-action matrix at every beat:
//
//     V(s, d) = max over my action ( min over opponent action V(step(s), d-1) )
//
// Because the pessimistic opponent is allowed to see our choice, this expands
// into a plain alternating MAX(4) / MIN(4) tree in which the world only
// advances after each MIN ply. Textbook alpha-beta therefore applies verbatim.
// ---------------------------------------------------------------------------

namespace {

constexpr float INF = 1e30f;
constexpr long long NODE_CHECK_MASK = 4095;

struct Ctx {
    int       me;
    long long nodes;
    std::chrono::steady_clock::time_point deadline;
    bool      aborted;
};

inline bool outOfTime(Ctx& ctx) {
    if (ctx.aborted) return true;
    if ((ctx.nodes & NODE_CHECK_MASK) == 0 &&
        std::chrono::steady_clock::now() > ctx.deadline) {
        ctx.aborted = true;
    }
    return ctx.aborted;
}

// A fighter locked for the whole upcoming beat cannot act, so all four actions
// are provably identical. Collapsing them is exact and prunes hard, since a
// whiffed Smash locks its owner for three or four beats.
inline int candidates(const Fighter& f, Action* out) {
    if (f.state == ST_DEAD || lockRemaining(f) > BEAT_FRAMES) {
        out[0] = ACT_BLOCK;
        return 1;
    }
    out[0] = ACT_BLOCK;
    out[1] = ACT_NORMAL;
    out[2] = ACT_SMASH;
    out[3] = ACT_JUMP;
    return ACTION_COUNT;
}

float minNode(const GameState& s, Action mine, int depth, float alpha, float beta, Ctx& ctx);

float maxNode(const GameState& s, int depth, float alpha, float beta, Ctx& ctx) {
    if (depth <= 0 || matchOver(s) || outOfTime(ctx)) return evaluate(s, ctx.me);

    Action acts[ACTION_COUNT];
    const int n = candidates(s.f[ctx.me], acts);

    float best = -INF;
    for (int i = 0; i < n; ++i) {
        const float v = minNode(s, acts[i], depth, alpha, beta, ctx);
        if (v > best) best = v;
        if (best > alpha) alpha = best;
        if (alpha >= beta) break;
    }
    return best;
}

float minNode(const GameState& s, Action mine, int depth, float alpha, float beta, Ctx& ctx) {
    Action acts[ACTION_COUNT];
    const int n = candidates(s.f[1 - ctx.me], acts);

    float best = INF;
    for (int i = 0; i < n; ++i) {
        GameState ns = s;
        if (ctx.me == 0) stepBeat(ns, mine, acts[i]);
        else             stepBeat(ns, acts[i], mine);
        ctx.nodes++;

        const float v = maxNode(ns, depth - 1, alpha, beta, ctx);
        if (v < best) best = v;
        if (best < beta) beta = best;
        if (alpha >= beta) break;
    }
    return best;
}

uint32_t rngNext(uint32_t& st) {
    st ^= st << 13;
    st ^= st >> 17;
    st ^= st << 5;
    return st;
}

// One sweep over the root's candidate actions. `exact` distinguishes a true
// value from an upper bound produced by an alpha cutoff, so the debug overlay
// and the tie-break never treat a bound as a real score.
struct RootPass {
    float     value[ACTION_COUNT];
    bool      exact[ACTION_COUNT] = { false, false, false, false };
    long long nodes = 0;
    bool      aborted = false;
};

using Deadline = std::chrono::steady_clock::time_point;

RootPass rootPassSerial(const GameState& s, int me, int d, const int* order, int n, Deadline dl) {
    RootPass out;
    for (float& v : out.value) v = -INF;
    Ctx ctx{ me, 0, dl, false };
    float alpha = -INF;
    for (int i = 0; i < n; ++i) {
        const Action a = (Action)order[i];
        const float v = minNode(s, a, d, alpha, INF, ctx);
        if (ctx.aborted) { out.aborted = true; break; }
        out.value[a] = v;
        // A row that fails low was cut short, so v is only an upper bound.
        out.exact[a] = (v > alpha);
        if (v > alpha) alpha = v;
    }
    out.nodes = ctx.nodes;
    return out;
}

// Root split: the four candidate actions are independent subtrees, so they run
// on separate cores. This gives up pruning between root siblings and buys back
// far more in wall-clock time at the depths where it matters.
RootPass rootPassParallel(const GameState& s, int me, int d, const int* order, int n, Deadline dl) {
    RootPass out;
    for (float& v : out.value) v = -INF;

    Ctx         ctx[ACTION_COUNT];
    float       val[ACTION_COUNT];
    std::thread th[ACTION_COUNT];

    for (int i = 0; i < n; ++i) {
        ctx[i] = Ctx{ me, 0, dl, false };
        val[i] = -INF;
        th[i] = std::thread([&s, me, d, &order, i, &ctx, &val] {
            (void)me;
            val[i] = minNode(s, (Action)order[i], d, -INF, INF, ctx[i]);
        });
    }
    for (int i = 0; i < n; ++i) {
        th[i].join();
        out.value[order[i]] = val[i];
        out.exact[order[i]] = true;   // full window, so nothing was cut short
        out.nodes += ctx[i].nodes;
        if (ctx[i].aborted) out.aborted = true;
    }
    return out;
}

// Splitting the root costs the pruning that alpha-beta gets from searching
// siblings in sequence, and at these tree sizes that pruning is worth more than
// four cores until the very top of the ladder. Measured on an M4 Pro: depth 7
// is a wash, depth 8 goes from ~93ms to ~65ms.
constexpr int PARALLEL_MIN_DEPTH = 8;

}  // namespace

SearchStats searchRoot(const GameState& s, int me, int depth) {
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();

    SearchStats out;
    for (int i = 0; i < ACTION_COUNT; ++i) {
        out.actionValue[i] = -INF;
        out.actionExact[i] = false;
        out.actionLegal[i] = false;
    }

    Action acts[ACTION_COUNT];
    const int n = candidates(s.f[me], acts);
    for (int i = 0; i < n; ++i) out.actionLegal[acts[i]] = true;

    if (n == 1) {
        out.best = acts[0];
        out.value = evaluate(s, me);
        out.actionValue[acts[0]] = out.value;
        out.actionExact[acts[0]] = true;
        out.actionTied[acts[0]] = true;
        out.depth = depth;
        out.ms = std::chrono::duration<double, std::milli>(clock::now() - t0).count();
        return out;
    }

    const auto deadline = t0 + std::chrono::milliseconds(400);

    // Root ordering carried between iterations; searching the previous best
    // first is what makes alpha-beta pay off at higher depths.
    int order[ACTION_COUNT];
    for (int i = 0; i < n; ++i) order[i] = acts[i];

    float bestValue[ACTION_COUNT];
    bool  exact[ACTION_COUNT] = { false, false, false, false };
    static thread_local uint32_t rng = 0x9e3779b9u;

    for (int d = 1; d <= depth; ++d) {
        const RootPass pass = (d >= PARALLEL_MIN_DEPTH)
                                  ? rootPassParallel(s, me, d, order, n, deadline)
                                  : rootPassSerial(s, me, d, order, n, deadline);
        out.nodes += pass.nodes;
        if (pass.aborted) { out.aborted = true; break; }

        for (int i = 0; i < ACTION_COUNT; ++i) {
            bestValue[i] = pass.value[i];
            exact[i] = pass.exact[i];
        }
        out.depth = d;

        std::stable_sort(order, order + n, [&](int x, int y) { return pass.value[x] > pass.value[y]; });
    }

    if (out.depth == 0) {
        // Not even depth 1 finished; fall back to a static read of the position.
        out.best = ACT_BLOCK;
        out.value = evaluate(s, me);
        out.aborted = true;
        out.ms = std::chrono::duration<double, std::milli>(clock::now() - t0).count();
        return out;
    }

    float top = -INF;
    for (int i = 0; i < n; ++i) {
        const Action a = acts[i];
        if (exact[a] && bestValue[a] > top) top = bestValue[a];
    }

    // Break near-ties at random so the CPU is not a metronome the player can
    // set their watch by.
    Action tied[ACTION_COUNT];
    int tiedCount = 0;
    for (int i = 0; i < n; ++i) {
        const Action a = acts[i];
        if (exact[a] && bestValue[a] >= top - TIE_EPSILON) tied[tiedCount++] = a;
    }
    out.best = tiedCount > 0 ? tied[rngNext(rng) % (uint32_t)tiedCount] : acts[0];
    out.value = top;
    for (int i = 0; i < ACTION_COUNT; ++i) {
        out.actionValue[i] = bestValue[i];
        out.actionExact[i] = exact[i];
        out.actionTied[i] = exact[i] && bestValue[i] >= top - TIE_EPSILON;
    }
    out.ms = std::chrono::duration<double, std::milli>(clock::now() - t0).count();
    return out;
}

// ---------------------------------------------------------------------------
// Worker thread
// ---------------------------------------------------------------------------

SearchWorker::SearchWorker() {
    th_ = std::thread([this] { loop(); });
}

SearchWorker::~SearchWorker() {
    {
        std::lock_guard<std::mutex> lk(m_);
        quit_ = true;
    }
    cv_.notify_all();
    if (th_.joinable()) th_.join();
}

void SearchWorker::request(const GameState& s, int me, int depth) {
    {
        std::lock_guard<std::mutex> lk(m_);
        pending_ = s;
        pendingMe_ = me;
        pendingDepth_ = depth;
        hasRequest_ = true;
    }
    busy_.store(true, std::memory_order_release);
    cv_.notify_one();
}

bool SearchWorker::poll(SearchStats& out) {
    if (!ready_.load(std::memory_order_acquire)) return false;
    std::lock_guard<std::mutex> lk(rm_);
    out = result_;
    ready_.store(false, std::memory_order_release);
    return true;
}

void SearchWorker::loop() {
    for (;;) {
        GameState s;
        int me, depth;
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait(lk, [this] { return hasRequest_ || quit_; });
            if (quit_) return;
            s = pending_;
            me = pendingMe_;
            depth = pendingDepth_;
            hasRequest_ = false;
        }

        const SearchStats r = searchRoot(s, me, depth);

        {
            std::lock_guard<std::mutex> lk(rm_);
            result_ = r;
        }
        ready_.store(true, std::memory_order_release);
        busy_.store(false, std::memory_order_release);
    }
}
