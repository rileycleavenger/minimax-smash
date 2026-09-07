#include "eval.h"
#include <cmath>

// Being near a blast zone is only mildly bad at 0% and catastrophic at 150%,
// so the risk term is scaled by the fighter's own damage.
static float edgeRisk(const Fighter& f) {
    const float t = std::fabs(f.x) / STAGE_HALF;
    return t * t * (1.0f + f.damage / 100.0f) * W_EDGE;
}

static bool offstage(const Fighter& f) {
    return !f.onGround && (f.x < STAGE_LEFT || f.x > STAGE_RIGHT || f.y < 0.0f);
}

static bool committed(const Fighter& f) {
    return f.state == ST_STARTUP || f.state == ST_ACTIVE || f.state == ST_RECOVERY
        || f.state == ST_HITSTUN;
}

float evaluate(const GameState& s, int me) {
    const Fighter& a = s.f[me];
    const Fighter& b = s.f[1 - me];

    float v = W_STOCK * (float)(a.stocks - b.stocks);
    // Dealing damage is worth more than taking it, so a trade is slightly
    // positive. Symmetric damage made maximin refuse every exchange.
    v += W_DAMAGE * (W_DMG_DEALT * b.damage - a.damage);

    v -= edgeRisk(a);
    v += edgeRisk(b);

    if (offstage(a)) v -= W_OFFSTAGE;
    if (offstage(b)) v += W_OFFSTAGE;

    if (b.state == ST_HITSTUN) v += W_HITSTUN;
    if (a.state == ST_HITSTUN) v -= W_HITSTUN;

    // Shield is a spendable resource, not free safety. Without this, pure
    // maximin discovers that blocking never loses an exchange and turtles.
    v -= W_SHIELD_SPENT * (charStats(a.charId).shieldMax - a.shield);
    if (a.state == ST_SHIELDBREAK) v -= W_SHIELDBROKEN;
    // Small on purpose: see the note in stats.h. A large bonus here is a bonus
    // for *not* taking the punish, because taking it ends the break.
    if (b.state == ST_SHIELDBREAK) v += W_OPP_SHIELDBROKEN;

    // Asymmetric on purpose: stalling is *my* problem because I have to win.
    // The symmetric version cancels when both hold shield and the turtle remains.
    if (a.state == ST_BLOCK && !committed(b)) v -= W_STALE_SHIELD;

    return v;
}
