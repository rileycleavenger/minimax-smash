#include "game.h"
#include <cmath>

const char* actionName(Action a) {
    switch (a) {
        case ACT_JUMP:   return "JUMP";
        case ACT_NORMAL: return "NORMAL";
        case ACT_SMASH:  return "SMASH";
        case ACT_BLOCK:  return "BLOCK";
        default:         return "-";
    }
}

static void spawnAt(Fighter& f, float x) {
    const CharStats& c = charStats(f.charId);
    f.x = x;
    f.y = RESPAWN_Y;
    f.vx = f.vy = 0.0f;
    f.damage = 0.0f;
    f.shield = c.shieldMax;
    f.state = ST_RESPAWN;
    f.stateTimer = RESPAWN_FRAMES;
    f.invuln = RESPAWN_INVULN;
    f.jumpLock = 0;
    f.jumpsLeft = (int8_t)c.maxJumps;
    f.walkDir = 0;
    f.moveId = MOVE_NORMAL;
    f.onGround = false;
    f.hitUsed = false;
}

void initGame(GameState& s, uint8_t char0, uint8_t char1) {
    s = GameState{};
    s.frame = 0;
    for (int i = 0; i < 2; ++i) {
        Fighter& f = s.f[i];
        f = Fighter{};
        f.charId = (i == 0) ? char0 : char1;
        f.stocks = START_STOCKS;
        f.facing = (i == 0) ? 1 : -1;
        f.autoWalk = (i == 1);
        spawnAt(f, (i == 0) ? -140.0f : 140.0f);
    }
}

bool matchOver(const GameState& s) {
    return s.f[0].state == ST_DEAD || s.f[1].state == ST_DEAD;
}

int winnerOf(const GameState& s) {
    if (s.f[0].state == ST_DEAD) return 1;
    if (s.f[1].state == ST_DEAD) return 0;
    return -1;
}

int lockRemaining(const Fighter& f) {
    const MoveStats& m = moveOf(f);
    switch (f.state) {
        case ST_STARTUP:  return f.stateTimer + m.active + m.recovery;
        case ST_ACTIVE:   return f.stateTimer + m.recovery;
        case ST_RECOVERY:
        case ST_HITSTUN:
        case ST_BLOCKLAG:
        case ST_SHIELDBREAK:
        case ST_RESPAWN:  return f.stateTimer;
        case ST_DEAD:     return 9999;
        default:          return 0;
    }
}

void hitboxOf(const Fighter& f, float& x0, float& y0, float& x1, float& y1) {
    const MoveStats& m = moveOf(f);
    if (f.facing > 0) {
        x0 = f.x + BODY_HALF_W * 0.5f;
        x1 = x0 + m.reach;
    } else {
        x1 = f.x - BODY_HALF_W * 0.5f;
        x0 = x1 - m.reach;
    }
    y0 = f.y + m.hitY - m.hitH;
    y1 = f.y + m.hitY + m.hitH;
}

// --------------------------------------------------------------------------
// Per-frame update stages
// --------------------------------------------------------------------------

static void advanceState(Fighter& f) {
    if (f.invuln > 0) f.invuln--;
    if (f.jumpLock > 0) f.jumpLock--;

    switch (f.state) {
        case ST_STARTUP:
            if (--f.stateTimer <= 0) {
                f.state = ST_ACTIVE;
                f.stateTimer = (int16_t)moveOf(f).active;
                f.hitUsed = false;
            }
            break;
        case ST_ACTIVE:
            if (--f.stateTimer <= 0) {
                f.state = ST_RECOVERY;
                f.stateTimer = (int16_t)moveOf(f).recovery;
            }
            break;
        case ST_RECOVERY:
        case ST_HITSTUN:
        case ST_BLOCKLAG:
        case ST_RESPAWN:
            if (--f.stateTimer <= 0) f.state = ST_IDLE;
            break;
        case ST_SHIELDBREAK:
            if (--f.stateTimer <= 0) {
                f.state = ST_IDLE;
                f.shield = charStats(f.charId).shieldMax * 0.4f;
            }
            break;
        default:
            break;
    }
}

// The deterministic locomotion rule. It runs identically inside the search and
// inside the live game, so the CPU never mispredicts its own movement. When the
// CPU searches, it assumes the human uses this same rule (they approach).
static int8_t autoWalkDir(const Fighter& f, const Fighter& other) {
    bool offstage = (f.x < STAGE_LEFT || f.x > STAGE_RIGHT);
    if (!f.onGround && offstage) {
        return (f.x < 0.0f) ? (int8_t)1 : (int8_t)-1;   // scramble back to stage
    }
    float dx = other.x - f.x;
    float spacing = charStats(f.charId).moves[MOVE_NORMAL].reach * 0.7f;
    if (std::fabs(dx) <= spacing) return 0;
    int8_t dir = (dx > 0.0f) ? (int8_t)1 : (int8_t)-1;
    // Never walk yourself off the ledge.
    if (f.onGround) {
        float nx = f.x + dir * charStats(f.charId).walkSpeed;
        if (nx < STAGE_LEFT || nx > STAGE_RIGHT) return 0;
    }
    return dir;
}

static void applyAction(GameState& s, int i, Action a) {
    Fighter& f = s.f[i];
    const Fighter& other = s.f[1 - i];
    const CharStats& c = charStats(f.charId);

    if (f.state == ST_DEAD) { f.walkDir = 0; return; }

    if (f.autoWalk) f.walkDir = autoWalkDir(f, other);

    if (!actionable(f)) {
        if (f.state != ST_STARTUP && f.state != ST_ACTIVE && f.state != ST_RECOVERY) f.walkDir = 0;
        return;
    }

    // Face the opponent whenever free to do so.
    f.facing = (other.x >= f.x) ? (int8_t)1 : (int8_t)-1;

    // Dropping the shield costs a full lockout window: block cannot be
    // cancelled straight into a punish, which is what keeps maximin honest.
    if (f.state == ST_BLOCK && a != ACT_BLOCK) {
        f.state = ST_BLOCKLAG;
        f.stateTimer = BLOCKLAG_FRAMES;
        f.walkDir = 0;
        return;
    }

    switch (a) {
        case ACT_BLOCK:
            if (f.onGround) {
                f.state = ST_BLOCK;
                f.walkDir = 0;
            }
            break;

        case ACT_JUMP:
            if (f.jumpLock == 0 && f.jumpsLeft > 0) {
                f.vy = c.jumpVel;
                f.jumpsLeft--;
                f.jumpLock = JUMP_LOCK;
                f.onGround = false;
                f.y += 0.5f;
            }
            break;

        case ACT_NORMAL:
        case ACT_SMASH:
            f.moveId = (a == ACT_NORMAL) ? MOVE_NORMAL : MOVE_SMASH;
            f.state = ST_STARTUP;
            f.stateTimer = (int16_t)moveOf(f).startup;
            f.hitUsed = false;
            f.walkDir = 0;
            break;

        default:
            break;
    }
}

static void shieldUpdate(Fighter& f) {
    const CharStats& c = charStats(f.charId);
    if (f.state == ST_BLOCK) {
        f.shield -= c.shieldDrain;
        if (f.shield <= 0.0f) {
            f.shield = 0.0f;
            f.state = ST_SHIELDBREAK;
            f.stateTimer = SHIELDBREAK_FRAMES;
        }
    } else if (f.state != ST_SHIELDBREAK) {
        f.shield += c.shieldRegen;
        if (f.shield > c.shieldMax) f.shield = c.shieldMax;
    }
}

static void physics(Fighter& f) {
    if (f.state == ST_DEAD) return;
    const CharStats& c = charStats(f.charId);
    const float prevY = f.y;
    const bool freeMove = (f.state == ST_IDLE);

    if (f.onGround) {
        if (freeMove) f.x += f.walkDir * c.walkSpeed;
        f.x += f.vx;
        f.vx *= GROUND_FRICTION;
        if (std::fabs(f.vx) < 0.05f) f.vx = 0.0f;
    } else {
        f.vx *= AIR_FRICTION;
        // Air drift may always slow you down, but only speed you up to the cap.
        if (freeMove || f.state == ST_STARTUP || f.state == ST_RECOVERY) {
            float nvx = f.vx + f.walkDir * c.airAccel;
            if (std::fabs(nvx) <= c.airSpeedMax || std::fabs(nvx) < std::fabs(f.vx)) f.vx = nvx;
        }
        f.vy -= c.gravity;
        if (f.vy < -c.fallMax) f.vy = -c.fallMax;
        f.x += f.vx;
        f.y += f.vy;
    }

    const bool overStage = (f.x > STAGE_LEFT - 2.0f && f.x < STAGE_RIGHT + 2.0f);
    if (!f.onGround) {
        if (overStage && prevY >= 0.0f && f.y <= 0.0f && f.vy <= 0.0f) {
            f.y = 0.0f;
            f.vy = 0.0f;
            f.onGround = true;
            f.jumpsLeft = (int8_t)c.maxJumps;
            f.jumpLock = 0;
            if (f.state == ST_HITSTUN) f.stateTimer = (int16_t)(f.stateTimer / 2);
        }
    } else if (!overStage) {
        f.onGround = false;   // walked off the ledge
    } else {
        f.y = 0.0f;
        f.vy = 0.0f;
    }
}

static void resolveHits(GameState& s, FrameEvents* ev) {
    // Both hitboxes are tested against the pre-hit state, then applied, so a
    // genuine trade registers for both fighters instead of favouring index 0.
    struct Pending { bool on; float hx, hy; } pend[2] = {};

    for (int a = 0; a < 2; ++a) {
        const Fighter& atk = s.f[a];
        const Fighter& def = s.f[1 - a];
        if (atk.state != ST_ACTIVE || atk.hitUsed) continue;
        if (def.invuln > 0 || def.state == ST_DEAD || def.state == ST_RESPAWN) continue;

        float x0, y0, x1, y1;
        hitboxOf(atk, x0, y0, x1, y1);
        const float dx0 = def.x - BODY_HALF_W, dx1 = def.x + BODY_HALF_W;
        const float dy0 = def.y, dy1 = def.y + BODY_H;
        if (x1 < dx0 || x0 > dx1 || y1 < dy0 || y0 > dy1) continue;

        pend[a].on = true;
        pend[a].hx = (x0 > dx0 ? x0 : dx0 + (x1 - dx0) * 0.5f);
        pend[a].hy = def.y + BODY_H * 0.5f;
    }

    for (int a = 0; a < 2; ++a) {
        if (!pend[a].on) continue;
        Fighter& atk = s.f[a];
        Fighter& def = s.f[1 - a];
        const MoveStats& m = moveOf(atk);
        atk.hitUsed = true;

        const bool blocked = (def.state == ST_BLOCK);
        float kb = 0.0f;
        bool broke = false;

        if (blocked) {
            def.shield -= m.damage * SHIELD_DMG_SCALE + SHIELD_DMG_BASE;
            kb = m.baseKB * 0.15f;
            def.vx += atk.facing * kb;
            atk.vx -= atk.facing * kb * 0.5f;
            if (def.shield <= 0.0f) {
                def.shield = 0.0f;
                def.state = ST_SHIELDBREAK;
                def.stateTimer = SHIELDBREAK_FRAMES;
                broke = true;
            }
        } else {
            def.damage += m.damage;
            if (def.damage > 999.0f) def.damage = 999.0f;
            kb = (m.baseKB + def.damage * m.kbGrowth) * (100.0f / charStats(def.charId).weight);
            const float rad = m.angleDeg * 0.017453292f;
            def.vx = atk.facing * kb * std::cos(rad);
            def.vy = kb * std::sin(rad);
            def.onGround = false;
            def.state = ST_HITSTUN;
            def.stateTimer = (int16_t)(kb * HITSTUN_SCALE) + 4;
            def.hitUsed = false;
        }

        if (ev && ev->hitCount < 2) {
            ev->hits[ev->hitCount++] = { (int8_t)a, pend[a].hx, pend[a].hy,
                                         m.damage, kb, blocked, broke };
        }
    }
}

static void checkKO(GameState& s, int i, FrameEvents* ev) {
    Fighter& f = s.f[i];
    if (f.state == ST_DEAD) return;
    if (std::fabs(f.x) <= BLAST_X && f.y >= BLAST_BOTTOM && f.y <= BLAST_TOP) return;

    if (ev && ev->koCount < 2) ev->kos[ev->koCount++] = { (int8_t)i, f.x, f.y };

    f.stocks--;
    if (f.stocks <= 0) {
        f.stocks = 0;
        f.state = ST_DEAD;
        f.stateTimer = 0;
        return;
    }
    spawnAt(f, 0.0f);
}

void stepFrame(GameState& s, Action a0, Action a1, FrameEvents* ev) {
    if (matchOver(s)) return;

    const Action acts[2] = { a0, a1 };
    for (int i = 0; i < 2; ++i) advanceState(s.f[i]);
    for (int i = 0; i < 2; ++i) applyAction(s, i, acts[i]);
    for (int i = 0; i < 2; ++i) shieldUpdate(s.f[i]);
    for (int i = 0; i < 2; ++i) physics(s.f[i]);
    resolveHits(s, ev);
    for (int i = 0; i < 2; ++i) checkKO(s, i, ev);
    s.frame++;
}

void stepBeat(GameState& s, Action a0, Action a1) {
    for (int k = 0; k < BEAT_FRAMES; ++k) {
        if (matchOver(s)) return;
        stepFrame(s, a0, a1, nullptr);
    }
}
