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
    // ...and spending *theirs* is progress. Without this, attacking into a held
    // shield scored exactly the same as doing nothing, so a shallow search had
    // no reason to throw the first punch and the match never started. Weighted
    // below my own for the usual reason: it is worth what it leads to.
    v += W_OPP_SHIELD_SPENT * (charStats(b.charId).shieldMax - b.shield);
    if (a.state == ST_SHIELDBREAK) v -= W_SHIELDBROKEN;
    // Small on purpose: see the note in stats.h. A large bonus here is a bonus
    // for *not* taking the punish, because taking it ends the break.
    if (b.state == ST_SHIELDBREAK) v += W_OPP_SHIELDBROKEN;

    // Maximin has no reason to seek a fight. Approaching always has a worst-case
    // answer, so two pessimistic players will stand and look at each other --
    // and once Retreat became searchable that is exactly what happened: every
    // level drew every match, oscillating advance/retreat on a two-beat cycle.
    //
    // Pricing the *distance* between them does not fix it. In a zero-sum tree
    // the opponent controls separation too, so it simply keeps away and the term
    // becomes a constant that distinguishes nothing (measured: no effect at all).
    // Pricing elapsed time without damage does work, because whether a hit lands
    // is something I can act on and the opponent cannot simply cancel.
    v -= W_STALL * (float)s.stallFrames;

    // Asymmetric on purpose: stalling is *my* problem because I have to win.
    // The symmetric version cancels when both hold shield and the turtle remains.
    //
    // Scoped to holding shield rather than to being uncommitted generally:
    // widening it to "not committed" is gameable, because throwing an attack
    // from outside anyone's range counts as committing. Measured, a depth-1 CPU
    // did exactly that -- whiff-spamming Normal from 200 units away forever.
    if (a.state == ST_BLOCK && !committed(b)) v -= W_STALE_SHIELD;

    return v;
}
