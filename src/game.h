#pragma once
#include <cstdint>
#include "stats.h"

// The searchable moves. Movement is one of them: the other four all leave
// ST_IDLE the instant they are applied -- Block pins you, Normal and Smash
// commit you, Jump puts you in the air -- and a grounded fighter only walks
// while it is idle, so without Advance/Retreat the CPU had no ground movement at
// all and could never back off. That costs branching: 36 joint actions per node
// against 16, which is what caps the ladder at MAX_CPU_LEVEL.
//
// ACT_NONE is *not* part of the search space; it is only used by the live game
// while the CPU is waiting on a decision, and by the human player when no button
// is held.
enum Action : uint8_t {
    ACT_JUMP = 0,
    ACT_NORMAL = 1,
    ACT_SMASH = 2,
    ACT_BLOCK = 3,
    ACT_ADVANCE = 4,
    ACT_RETREAT = 5,
    ACTION_COUNT = 6,
    ACT_NONE = 6,
};

const char* actionName(Action a);

enum FState : uint8_t {
    ST_IDLE = 0,
    ST_STARTUP,
    ST_ACTIVE,
    ST_RECOVERY,
    ST_HITSTUN,
    ST_BLOCK,
    ST_BLOCKLAG,     // shield-drop lockout: the punish window for turtling
    ST_SHIELDBREAK,
    ST_RESPAWN,
    ST_DEAD,
};

// Trivially copyable on purpose: a search node snapshot is a memcpy.
struct Fighter {
    float   x, y;
    float   vx, vy;
    float   damage;
    float   shield;
    int16_t stateTimer;
    int16_t invuln;
    int16_t jumpLock;
    int8_t  stocks;
    int8_t  facing;      // +1 right, -1 left
    int8_t  jumpsLeft;
    int8_t  walkDir;     // -1 / 0 / +1 locomotion intent for this frame
    uint8_t state;
    uint8_t moveId;
    uint8_t charId;
    bool    onGround;
    bool    hitUsed;     // this move's hitbox already connected
    bool    autoWalk;    // derive walkDir from the built-in rule instead of input
};

struct GameState {
    Fighter f[2];
    int32_t frame;
    // Frames since either fighter last took damage or lost a stock. The
    // evaluation prices this: without it two maximin players have no reason to
    // ever commit, because committing is the only thing that can go wrong.
    int16_t stallFrames;
};

// Side-channel for the renderer. The search always passes nullptr, so the
// simulation stays pure and allocation-free on the worker thread.
struct FrameEvents {
    struct Hit { int8_t attacker; float x, y; float damage; float kb; bool blocked; bool shieldBreak; };
    struct Ko  { int8_t who; float x, y; };
    int hitCount = 0;
    int koCount  = 0;
    Hit hits[2];
    Ko  kos[2];
};

void initGame(GameState& s, uint8_t char0, uint8_t char1);
void stepFrame(GameState& s, Action a0, Action a1, FrameEvents* ev = nullptr);
void stepBeat(GameState& s, Action a0, Action a1);

bool matchOver(const GameState& s);
int  winnerOf(const GameState& s);            // -1 if still running

// Frames until this fighter can act again (0 if it can act right now).
int  lockRemaining(const Fighter& f);
inline bool actionable(const Fighter& f) { return f.state == ST_IDLE || f.state == ST_BLOCK; }

// Would this action actually do anything for this fighter on this frame? A jump
// during the post-jump lock and a block in mid-air are both silently ignored by
// applyAction, so the input buffer needs to know the difference between "came
// out" and "was swallowed" or a quickly tapped double jump disappears.
bool actionHasEffect(const Fighter& f, Action a);

inline const MoveStats& moveOf(const Fighter& f) {
    return charStats(f.charId).moves[f.moveId];
}

// Hitbox of an attacker currently in ST_ACTIVE, in world units.
void hitboxOf(const Fighter& f, float& x0, float& y0, float& x1, float& y1);
