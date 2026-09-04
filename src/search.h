#pragma once
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include "game.h"

// Actions scoring within this many points of the best are treated as equal and
// chosen between at random.
constexpr float TIE_EPSILON = 0.2f;

struct SearchStats {
    Action    best = ACT_BLOCK;
    float     value = 0.0f;
    float     actionValue[ACTION_COUNT] = { 0, 0, 0, 0 };
    bool      actionExact[ACTION_COUNT] = { false, false, false, false };
    bool      actionTied[ACTION_COUNT]  = { false, false, false, false };
    bool      actionLegal[ACTION_COUNT] = { true, true, true, true };
    long long nodes = 0;
    double    ms = 0.0;
    int       depth = 0;        // deepest fully completed iteration
    bool      aborted = false;
};

// Full maximin search to a fixed depth, on the calling thread.
SearchStats searchRoot(const GameState& s, int me, int depth);

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
