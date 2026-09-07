#pragma once

// All tunable constants live here. Deliberately free of raylib so the
// simulation and search translation units stay independent of the renderer.

// ---------------------------------------------------------------- arena ----
// World units. +y is up, y == 0 is the stage surface, x == 0 is stage center.
constexpr float STAGE_HALF   = 300.0f;
constexpr float STAGE_LEFT   = -STAGE_HALF;
constexpr float STAGE_RIGHT  =  STAGE_HALF;
constexpr float BLAST_X      = 600.0f;
constexpr float BLAST_BOTTOM = -260.0f;
constexpr float BLAST_TOP    =  520.0f;
constexpr float RESPAWN_Y    =  240.0f;

constexpr float BODY_HALF_W = 17.0f;
constexpr float BODY_H      = 52.0f;

// -------------------------------------------------------------- physics ----
constexpr float GROUND_FRICTION = 0.82f;
// Knockback decays geometrically, so a launched fighter travels about
// vx / (1 - AIR_FRICTION) before stopping. That divisor sets every KO
// percentage in the game: at 0.95 the cap was ~20x vx and normals could not
// kill at any realistic damage.
constexpr float AIR_FRICTION    = 0.962f;
constexpr float HITSTUN_SCALE   = 2.0f;

// ---------------------------------------------------------------- timing ----
// One "beat" is the decision granularity the CPU searches over: it commits to
// a single action and holds it for this many frames.
constexpr int BEAT_FRAMES        = 10;
constexpr int BLOCKLAG_FRAMES    = 12;   // full lockout after dropping shield
constexpr int SHIELDBREAK_FRAMES = 100;
constexpr int RESPAWN_FRAMES     = 20;
constexpr int RESPAWN_INVULN     = 80;
// A CPU holds one action for a whole beat, so this must be >= BEAT_FRAMES or a
// held ACT_JUMP silently spends both jumps inside a single decision. Setting it
// equal to the beat makes "hold jump for one beat" mean exactly one jump, which
// is what the search assumes it is buying and what recovery depends on.
constexpr int JUMP_LOCK          = BEAT_FRAMES;

constexpr float SHIELD_DMG_SCALE = 1.5f;
constexpr float SHIELD_DMG_BASE  = 5.0f;

constexpr int START_STOCKS = 3;

// ----------------------------------------------------------------- moves ----
enum MoveId { MOVE_NORMAL = 0, MOVE_SMASH = 1 };

struct MoveStats {
    int   startup;    // frames before the hitbox appears
    int   active;     // frames the hitbox is live
    int   recovery;   // frames of helplessness afterwards
    float damage;     // percent added on hit
    float reach;      // forward extent measured from the body edge
    float hitY;       // hitbox center height above the fighter's feet
    float hitH;       // hitbox half-height
    float baseKB;     // knockback at 0%
    float kbGrowth;   // knockback added per point of the victim's damage
    float angleDeg;   // launch angle
};

struct CharStats {
    const char* name;
    float walkSpeed;
    float airAccel;
    float airSpeedMax;
    float jumpVel;
    float gravity;
    float fallMax;
    float weight;      // heavier fighters take less knockback
    int   maxJumps;
    float shieldMax;
    float shieldDrain; // per frame while holding block
    float shieldRegen; // per frame while not blocking
    MoveStats moves[2];
};

enum CharId { CHAR_SPARKMOUSE = 0, CHAR_PLUMBER = 1, CHAR_COUNT = 2 };

// Namespace-scope constexpr rather than a function-local static: a static local
// carries a thread-safe-initialisation guard that has to be checked on every
// call, and the search reaches for this table several times per simulated frame
// across millions of frames.
inline constexpr CharStats CHAR_TABLE[CHAR_COUNT] = {
    // Sparkmouse: fast, light, low damage per hit, quick startup.
    {   "SPARKMOUSE",
        /*walk*/ 3.7f, /*airAccel*/ 0.40f, /*airMax*/ 4.4f, /*jumpVel*/ 11.6f,
        /*gravity*/ 0.62f, /*fallMax*/ 12.0f, /*weight*/ 80.0f, /*jumps*/ 2,
        /*shield*/ 100.0f, /*drain*/ 0.85f, /*regen*/ 0.45f,
        {
            //  su  act rec  dmg  reach hitY hitH  baseKB  growth  angle
            {    4,  3,  8,  4.0f, 34.f, 30.f, 22.f,  6.0f, 0.105f, 42.f },
            {   16,  4, 26, 12.0f, 46.f, 30.f, 26.f, 11.0f, 0.125f, 38.f },
        }
    },
    // Plumber: heavier and slower, but hits noticeably harder with reach.
    {   "PLUMBER",
        /*walk*/ 3.0f, /*airAccel*/ 0.34f, /*airMax*/ 3.8f, /*jumpVel*/ 12.1f,
        /*gravity*/ 0.66f, /*fallMax*/ 13.0f, /*weight*/ 100.0f, /*jumps*/ 2,
        /*shield*/ 110.0f, /*drain*/ 0.80f, /*regen*/ 0.40f,
        {
            //  su  act rec  dmg  reach hitY hitH  baseKB  growth  angle
            {    6,  3, 10,  6.0f, 40.f, 30.f, 24.f,  6.5f, 0.115f, 42.f },
            {   20,  4, 30, 16.0f, 54.f, 30.f, 28.f, 13.0f, 0.128f, 36.f },
        }
    },
};

inline constexpr const CharStats& charStats(unsigned id) { return CHAR_TABLE[id]; }

// ------------------------------------------------------------ evaluation ----
constexpr float W_STOCK        = 10000.0f;
constexpr float W_DAMAGE       = 1.0f;
constexpr float W_EDGE         = 60.0f;
constexpr float W_OFFSTAGE     = 25.0f;
constexpr float W_SHIELD_SPENT     = 0.55f;   // stamina I have spent
constexpr float W_OPP_SHIELD_SPENT = 0.30f;   // stamina I have made them spend
// These two are deliberately not the same number. My own shield breaking is an
// emergency that persists whatever I do, so it is worth a lot. The *opponent's*
// broken shield is worth only what I can convert it into -- and a bonus that
// large is forfeited the instant I land the hit that ends the break, so pricing
// them symmetrically taught the search to stand and admire a helpless opponent
// rather than punish one. It must stay below the damage a free hit is worth.
constexpr float W_SHIELDBROKEN     = 120.0f;   // mine
constexpr float W_OPP_SHIELDBROKEN = 10.0f;    // theirs
constexpr float W_STALE_SHIELD = 18.0f;  // holding shield vs a non-attacker
constexpr float W_STALL        = 0.12f;  // per frame since anyone last took damage

// Long enough that a real lull is priced, bounded so the term cannot swamp the
// rest of the evaluation once a match has genuinely stagnated.
constexpr int   STALL_CAP      = 600;

// Movement actions are distance targets, in multiples of the fighter's own
// normal-attack reach, rather than raw directions. Advance closes to poke range;
// Retreat opens to just outside it. Neither can be used to leave the fight.
//
// Two fighters who can both disengage at will have no reason to ever fight, and
// measurably did not: every level drew every match on a two-beat advance/retreat
// cycle. Gating retreat on a separation threshold instead just moved the problem
// -- the fighter pinned itself exactly on the threshold and jittered across it.
// Converging on a target has no boundary to sit on.
constexpr float ADVANCE_DIST = 0.7f;   // same range autoWalkDir closes to
constexpr float RETREAT_DIST = 1.6f;   // outside their poke, inside one beat's walk
constexpr float W_HITSTUN      = 12.0f;  // opponent currently in hitstun
constexpr float W_DMG_DEALT    = 1.25f;  // dealing damage is worth more than taking it

// Six actions a side means 36 joint actions per node against 16, and alpha-beta
// already runs at its square-root optimum here, so each extra beat costs ~6x
// rather than ~4x. Depth 6 lands near 80ms against a 167ms beat; 7 and 8 are out
// of reach. Searched movement is worth those two levels.
constexpr int MAX_CPU_LEVEL = 6;
