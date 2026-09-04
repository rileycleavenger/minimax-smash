#include "render.h"

#include <cmath>
#include <cstdlib>

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

static Color mix(Color a, Color b, float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return Color{ (unsigned char)(a.r + (b.r - a.r) * t),
                  (unsigned char)(a.g + (b.g - a.g) * t),
                  (unsigned char)(a.b + (b.b - a.b) * t),
                  (unsigned char)(a.a + (b.a - a.a) * t) };
}

// raylib culls by winding; drawing both orders keeps every triangle visible
// regardless of how the vertices came out.
static void tri(Vector2 a, Vector2 b, Vector2 c, Color col) {
    DrawTriangle(a, b, c, col);
    DrawTriangle(a, c, b, col);
}

static float frand(float lo, float hi) {
    return lo + (hi - lo) * ((float)std::rand() / (float)RAND_MAX);
}

// ---------------------------------------------------------------------------
// particles
// ---------------------------------------------------------------------------

enum FxKind { FX_SPARK, FX_PUFF, FX_RING, FX_STAR };

struct Particle {
    float x, y, vx, vy;
    float life, maxLife;
    float size;
    float grav;
    Color c;
    int   kind;
    bool  alive;
};

static Particle gParticles[512];
static int      gNext = 0;
static float    gShake = 0.0f;

static Particle& alloc() {
    Particle& p = gParticles[gNext];
    gNext = (gNext + 1) % 512;
    p.alive = true;
    p.grav = 0.0f;
    return p;
}

void fxClear() {
    for (auto& p : gParticles) p.alive = false;
    gShake = 0.0f;
}

float fxShake() { return gShake; }

void fxHit(float wx, float wy, int attackerChar, float power, bool blocked) {
    gShake += blocked ? 1.5f : (2.0f + power * 0.35f);
    if (gShake > 14.0f) gShake = 14.0f;

    Particle& ring = alloc();
    ring.x = wx; ring.y = wy; ring.vx = ring.vy = 0.0f;
    ring.maxLife = ring.life = blocked ? 8.0f : 12.0f;
    ring.size = blocked ? 16.0f : 14.0f + power * 1.4f;
    ring.kind = FX_RING;
    ring.c = blocked ? SKYBLUE : (attackerChar == CHAR_SPARKMOUSE ? Color{ 190, 240, 255, 255 } : Color{ 255, 220, 150, 255 });

    const int n = blocked ? 6 : 12 + (int)power;
    for (int i = 0; i < n; ++i) {
        Particle& p = alloc();
        const float ang = frand(0.0f, 6.2831853f);
        const float spd = frand(1.5f, 4.0f + power * 0.55f);
        p.x = wx; p.y = wy;
        p.vx = std::cos(ang) * spd;
        p.vy = std::sin(ang) * spd;
        p.maxLife = p.life = frand(10.0f, 24.0f);

        if (blocked) {
            p.kind = FX_SPARK; p.size = frand(3.0f, 7.0f);
            p.c = mix(SKYBLUE, WHITE, frand(0.0f, 1.0f));
        } else if (attackerChar == CHAR_SPARKMOUSE) {
            // Electric: long thin sparks, cyan through to pale yellow.
            p.kind = FX_SPARK; p.size = frand(6.0f, 15.0f);
            p.c = mix(Color{ 120, 230, 255, 255 }, Color{ 255, 250, 170, 255 }, frand(0.0f, 1.0f));
        } else {
            // Impact: heavier puffs that fall away under gravity.
            p.kind = FX_PUFF; p.size = frand(3.0f, 8.0f);
            p.grav = -0.16f;
            p.c = mix(Color{ 255, 170, 60, 255 }, Color{ 255, 245, 220, 255 }, frand(0.0f, 1.0f));
        }
    }
}

void fxShieldBreak(float wx, float wy) {
    gShake += 10.0f;
    for (int i = 0; i < 26; ++i) {
        Particle& p = alloc();
        const float ang = frand(0.0f, 6.2831853f);
        const float spd = frand(2.0f, 6.5f);
        p.x = wx; p.y = wy;
        p.vx = std::cos(ang) * spd;
        p.vy = std::sin(ang) * spd;
        p.grav = -0.22f;
        p.maxLife = p.life = frand(18.0f, 40.0f);
        p.kind = FX_PUFF;
        p.size = frand(3.0f, 9.0f);
        p.c = mix(SKYBLUE, WHITE, frand(0.0f, 1.0f));
    }
}

void fxKO(float wx, float wy) {
    gShake += 12.0f;
    Particle& ring = alloc();
    ring.x = wx; ring.y = wy; ring.vx = ring.vy = 0.0f;
    ring.maxLife = ring.life = 26.0f;
    ring.size = 90.0f;
    ring.kind = FX_RING;
    ring.c = WHITE;
    for (int i = 0; i < 34; ++i) {
        Particle& p = alloc();
        const float ang = frand(0.0f, 6.2831853f);
        const float spd = frand(3.0f, 11.0f);
        p.x = wx; p.y = wy;
        p.vx = std::cos(ang) * spd;
        p.vy = std::sin(ang) * spd;
        p.maxLife = p.life = frand(20.0f, 44.0f);
        p.kind = FX_STAR;
        p.size = frand(4.0f, 11.0f);
        p.c = mix(Color{ 255, 240, 120, 255 }, WHITE, frand(0.0f, 1.0f));
    }
}

void fxUpdate() {
    gShake *= 0.82f;
    if (gShake < 0.15f) gShake = 0.0f;
    for (auto& p : gParticles) {
        if (!p.alive) continue;
        p.x += p.vx;
        p.y += p.vy;
        p.vy += p.grav;
        p.vx *= 0.94f;
        if (p.kind != FX_RING) p.vy *= 0.97f;
        if ((p.life -= 1.0f) <= 0.0f) p.alive = false;
    }
}

void fxDraw() {
    for (const auto& p : gParticles) {
        if (!p.alive) continue;
        const float t = p.life / p.maxLife;
        const Color c = Fade(p.c, t);
        const Vector2 s = w2s(p.x, p.y);
        switch (p.kind) {
            case FX_RING: {
                const float r = p.size * (1.0f - t) + 4.0f;
                DrawRing(s, r, r + 3.0f + 4.0f * t, 0.0f, 360.0f, 32, c);
                break;
            }
            case FX_SPARK: {
                const float len = p.size * t;
                const float m = std::sqrt(p.vx * p.vx + p.vy * p.vy) + 0.001f;
                const Vector2 e{ s.x + p.vx / m * len, s.y - p.vy / m * len };
                DrawLineEx(s, e, 1.0f + 2.0f * t, c);
                break;
            }
            case FX_STAR: {
                const float r = p.size * t;
                DrawPoly(s, 4, r, p.life * 9.0f, c);
                break;
            }
            default:
                DrawCircleV(s, p.size * t, c);
                break;
        }
    }
}

// ---------------------------------------------------------------------------
// camera
// ---------------------------------------------------------------------------

void updateCamera(Camera2D& cam, const GameState& s, bool snap) {
    const float ax = w2sx(s.f[0].x), ay = w2sy(s.f[0].y + BODY_H * 0.5f);
    const float bx = w2sx(s.f[1].x), by = w2sy(s.f[1].y + BODY_H * 0.5f);

    const float cx = (ax + bx) * 0.5f;
    // Bias downward so the stage lip stays visible; judging the ledge matters.
    const float cy = (ay + by) * 0.5f * 0.72f + (w2sy(0.0f) - 60.0f) * 0.28f;

    const float spanX = std::fabs(ax - bx) + 720.0f;
    const float spanY = std::fabs(ay - by) + 450.0f;
    float z = SCREEN_W / spanX;
    if (SCREEN_H / spanY < z) z = SCREEN_H / spanY;
    if (z < 0.68f) z = 0.68f;
    if (z > 1.85f) z = 1.85f;

    if (snap) {
        cam.target = Vector2{ cx, cy };
        cam.zoom = z;
    } else {
        cam.target.x += (cx - cam.target.x) * 0.10f;
        cam.target.y += (cy - cam.target.y) * 0.10f;
        cam.zoom += (z - cam.zoom) * 0.06f;
    }
    cam.offset = Vector2{ SCREEN_W * 0.5f, SCREEN_H * 0.5f };
}

// ---------------------------------------------------------------------------
// stage
// ---------------------------------------------------------------------------

void drawStage() {
    const float top = w2sy(0.0f);
    const float l = w2sx(STAGE_LEFT);
    const float r = w2sx(STAGE_RIGHT);

    // Blast zones. Drawn generously past the viewport so they stay correct at
    // any camera zoom.
    const Color bz = Color{ 220, 70, 90, 60 };
    for (int side = -1; side <= 1; side += 2) {
        const float bx = w2sx(BLAST_X * side);
        for (float y = -600.0f; y < SCREEN_H + 600.0f; y += 22.0f) DrawRectangle((int)bx - 1, (int)y, 3, 12, bz);
    }
    const float bby = w2sy(BLAST_BOTTOM);
    for (float x = -900.0f; x < SCREEN_W + 900.0f; x += 22.0f) DrawRectangle((int)x, (int)bby - 1, 12, 3, bz);

    // Centre reference so drift toward the ledge reads clearly.
    for (float y = top - 190.0f; y < top; y += 18.0f)
        DrawRectangle((int)w2sx(0.0f), (int)y, 1, 9, Color{ 255, 255, 255, 22 });

    // Stage slab.
    DrawRectangleGradientV((int)l, (int)top, (int)(r - l), 54, Color{ 92, 104, 138, 255 }, Color{ 38, 44, 66, 255 });
    DrawRectangle((int)l, (int)top, (int)(r - l), 5, Color{ 168, 200, 235, 255 });
    DrawRectangle((int)l, (int)top + 5, (int)(r - l), 2, Color{ 60, 76, 108, 255 });

    // Ledge caps.
    for (int side = -1; side <= 1; side += 2) {
        const float x = (side < 0) ? l : r - 8.0f;
        DrawRectangle((int)x, (int)top, 8, 54, Color{ 130, 150, 190, 190 });
    }
}

// ---------------------------------------------------------------------------
// fighters
// ---------------------------------------------------------------------------

static void drawSparkmouse(float cx, float fy, int facing, Color tint) {
    const Color body   = mix(Color{ 252, 220, 60, 255 }, tint, tint.a / 255.0f * 0.55f);
    const Color dark   = Color{ 190, 140, 25, 255 };
    const Color black  = Color{ 30, 26, 20, 255 };
    const float f = (float)facing;

    // Tail: a chunky lightning bolt trailing behind.
    {
        const float tx = cx - f * 16.0f;
        const float ty = fy - 26.0f;
        tri(Vector2{ tx, ty }, Vector2{ tx - f * 16.0f, ty - 10.0f }, Vector2{ tx - f * 6.0f, ty - 20.0f }, dark);
        tri(Vector2{ tx - f * 6.0f, ty - 20.0f }, Vector2{ tx - f * 24.0f, ty - 22.0f }, Vector2{ tx - f * 12.0f, ty - 34.0f }, body);
        tri(Vector2{ tx - f * 12.0f, ty - 34.0f }, Vector2{ tx - f * 26.0f, ty - 34.0f }, Vector2{ tx - f * 18.0f, ty - 44.0f }, body);
    }

    // Feet.
    DrawEllipse((int)(cx - 9.0f), (int)fy - 3, 8.0f, 5.0f, dark);
    DrawEllipse((int)(cx + 9.0f), (int)fy - 3, 8.0f, 5.0f, dark);

    // Body and head.
    DrawEllipse((int)cx, (int)(fy - 19.0f), 17.0f, 18.0f, body);
    DrawEllipse((int)cx, (int)(fy - 36.0f), 16.0f, 14.0f, body);

    // Ears with black tips.
    for (int side = -1; side <= 1; side += 2) {
        const float ex = cx + side * 8.0f;
        const float ey = fy - 46.0f;
        const Vector2 a{ ex, ey };
        const Vector2 b{ ex + side * 8.0f, ey };
        const Vector2 c{ ex + side * 9.0f, ey - 17.0f };
        tri(a, b, c, body);
        tri(Vector2{ ex + side * 5.0f, ey - 10.0f }, Vector2{ ex + side * 9.0f, ey - 9.0f }, c, black);
    }

    // Cheeks, eyes, mouth.
    DrawCircle((int)(cx - 12.0f), (int)(fy - 32.0f), 4.5f, Color{ 226, 62, 52, 255 });
    DrawCircle((int)(cx + 12.0f), (int)(fy - 32.0f), 4.5f, Color{ 226, 62, 52, 255 });
    DrawCircle((int)(cx - 5.0f + f * 1.0f), (int)(fy - 40.0f), 3.2f, black);
    DrawCircle((int)(cx + 6.0f + f * 1.0f), (int)(fy - 40.0f), 3.2f, black);
    DrawCircle((int)(cx - 4.0f + f * 1.0f), (int)(fy - 41.0f), 1.1f, RAYWHITE);
    DrawCircle((int)(cx + 7.0f + f * 1.0f), (int)(fy - 41.0f), 1.1f, RAYWHITE);
    DrawCircle((int)(cx + f * 1.0f), (int)(fy - 34.0f), 1.8f, black);
}

static void drawPlumber(float cx, float fy, int facing, Color tint) {
    const Color shirt = mix(Color{ 214, 46, 42, 255 }, tint, tint.a / 255.0f * 0.55f);
    const Color denim = mix(Color{ 52, 86, 176, 255 }, tint, tint.a / 255.0f * 0.55f);
    const Color skin  = Color{ 244, 194, 152, 255 };
    const Color hair  = Color{ 74, 46, 28, 255 };
    const Color shoe  = Color{ 96, 56, 28, 255 };
    const float f = (float)facing;

    // Shoes and jeans.
    DrawEllipse((int)(cx - 8.0f + f * 3.0f), (int)fy - 3, 10.0f, 5.0f, shoe);
    DrawEllipse((int)(cx + 8.0f + f * 3.0f), (int)fy - 3, 10.0f, 5.0f, shoe);
    DrawRectangle((int)(cx - 11.0f), (int)(fy - 24.0f), 22, 20, denim);
    DrawRectangle((int)(cx - 1.0f), (int)(fy - 24.0f), 2, 19, Color{ 34, 58, 124, 255 });

    // Red shirt with overall straps over it.
    DrawRectangle((int)(cx - 12.0f), (int)(fy - 40.0f), 24, 17, shirt);
    DrawRectangle((int)(cx - 9.0f), (int)(fy - 40.0f), 5, 16, denim);
    DrawRectangle((int)(cx + 4.0f), (int)(fy - 40.0f), 5, 16, denim);
    DrawCircle((int)(cx - 7.0f), (int)(fy - 25.0f), 2.0f, Color{ 250, 214, 90, 255 });
    DrawCircle((int)(cx + 6.0f), (int)(fy - 25.0f), 2.0f, Color{ 250, 214, 90, 255 });

    // Arms: sleeve plus a glove out in front.
    DrawRectangle((int)(cx + (f > 0 ? 10.0f : -16.0f)), (int)(fy - 38.0f), 6, 13, shirt);
    DrawCircle((int)(cx + f * 17.0f), (int)(fy - 25.0f), 4.5f, RAYWHITE);

    // Head.
    DrawEllipse((int)cx, (int)(fy - 48.0f), 12.0f, 11.0f, skin);
    DrawEllipse((int)(cx + f * 10.0f), (int)(fy - 47.0f), 4.5f, 4.0f, skin);   // nose
    DrawRectangle((int)(cx - 11.0f), (int)(fy - 55.0f), 22, 5, hair);          // hairline
    DrawRectangle((int)(cx - f * 11.0f), (int)(fy - 52.0f), 4, 9, hair);       // sideburn

    // Mustache and eye.
    DrawRectangle((int)(cx + (f > 0 ? 1.0f : -9.0f)), (int)(fy - 44.0f), 9, 4, Color{ 44, 28, 18, 255 });
    DrawCircle((int)(cx + f * 4.0f), (int)(fy - 51.0f), 1.9f, Color{ 40, 34, 30, 255 });

    // Cap with a brim pointing the way he faces.
    DrawRectangle((int)(cx - 12.0f), (int)(fy - 61.0f), 24, 7, shirt);
    DrawEllipse((int)cx, (int)(fy - 61.0f), 12.0f, 6.0f, shirt);
    DrawRectangle((int)(cx + (f > 0 ? 10.0f : -22.0f)), (int)(fy - 57.0f), 12, 4, shirt);
    DrawCircle((int)cx, (int)(fy - 60.0f), 4.0f, RAYWHITE);
}

// Move-specific attack flourish drawn over the live hitbox.
static void drawAttackFx(const Fighter& f) {
    float x0, y0, x1, y1;
    hitboxOf(f, x0, y0, x1, y1);
    const float sx0 = w2sx(x0), sx1 = w2sx(x1);
    const float sy0 = w2sy(y1), sy1 = w2sy(y0);
    const float cx = (sx0 + sx1) * 0.5f, cy = (sy0 + sy1) * 0.5f;
    const bool smash = (f.moveId == MOVE_SMASH);

    if (f.charId == CHAR_SPARKMOUSE) {
        const Color glow = smash ? Color{ 255, 245, 150, 255 } : Color{ 160, 240, 255, 255 };
        DrawEllipse((int)cx, (int)cy, (sx1 - sx0) * 0.5f, (sy1 - sy0) * 0.45f, Fade(glow, 0.30f));
        // Deterministic zigzag so the bolt reads as electricity, not noise.
        const int seg = smash ? 7 : 5;
        Vector2 prev{ sx0, cy };
        for (int i = 1; i <= seg; ++i) {
            const float t = (float)i / seg;
            const float amp = (smash ? 17.0f : 10.0f) * (i % 2 ? 1.0f : -1.0f);
            const Vector2 cur{ sx0 + (sx1 - sx0) * t, cy + amp * std::sin(t * 3.1f + f.stateTimer) };
            DrawLineEx(prev, cur, smash ? 4.0f : 2.5f, glow);
            prev = cur;
        }
        DrawCircleV(Vector2{ sx1 - (f.facing > 0 ? 6.0f : -6.0f), cy }, smash ? 11.0f : 7.0f, Fade(WHITE, 0.8f));
    } else {
        const Color glow = smash ? Color{ 255, 150, 50, 255 } : Color{ 255, 230, 190, 255 };
        DrawEllipse((int)cx, (int)cy, (sx1 - sx0) * 0.5f, (sy1 - sy0) * 0.45f, Fade(glow, 0.28f));
        const float a0 = f.facing > 0 ? -60.0f : 120.0f;
        DrawRing(Vector2{ w2sx(f.x), cy }, (sx1 - sx0) * 0.55f, (sx1 - sx0) * 0.55f + (smash ? 9.0f : 5.0f),
                 a0, a0 + 120.0f, 24, Fade(glow, 0.9f));
        DrawCircleV(Vector2{ sx1 - (f.facing > 0 ? 8.0f : -8.0f), cy }, smash ? 12.0f : 7.0f, Fade(glow, 0.85f));
    }
}

void drawFighter(const Fighter& f, bool debug) {
    if (f.state == ST_DEAD) return;

    const float cx = w2sx(f.x);
    const float fy = w2sy(f.y);

    // Invulnerability blink.
    if (f.invuln > 0 && (f.invuln / 4) % 2 == 0) return;

    Color tint{ 0, 0, 0, 0 };
    if (f.state == ST_HITSTUN)          tint = Color{ 255, 120, 120, 190 };
    else if (f.state == ST_SHIELDBREAK) tint = Color{ 160, 200, 255, 170 };
    else if (f.state == ST_BLOCKLAG)    tint = Color{ 120, 120, 140, 130 };

    // Smash wind-up glow: telegraphs the commitment the search is betting on.
    if (f.state == ST_STARTUP && f.moveId == MOVE_SMASH) {
        const float total = (float)charStats(f.charId).moves[MOVE_SMASH].startup;
        const float t = 1.0f - (float)f.stateTimer / total;
        const Color c = (f.charId == CHAR_SPARKMOUSE) ? Color{ 255, 250, 140, 255 } : Color{ 255, 140, 50, 255 };
        DrawCircleV(Vector2{ cx, fy - 26.0f }, 30.0f + 16.0f * t, Fade(c, 0.10f + 0.22f * t));
        DrawRing(Vector2{ cx, fy - 26.0f }, 34.0f + 14.0f * t, 37.0f + 14.0f * t, 0.0f, 360.0f * t, 32, Fade(c, 0.85f));
    }

    if (f.charId == CHAR_SPARKMOUSE) drawSparkmouse(cx, fy, f.facing, tint);
    else                             drawPlumber(cx, fy, f.facing, tint);

    if (f.state == ST_ACTIVE) drawAttackFx(f);

    // Shield bubble: shrinks and reddens as stamina drains.
    if (f.state == ST_BLOCK) {
        const float frac = f.shield / charStats(f.charId).shieldMax;
        const float r = 24.0f + 17.0f * frac;
        const Color c = mix(Color{ 255, 80, 80, 255 }, Color{ 110, 220, 255, 255 }, frac);
        DrawCircleV(Vector2{ cx, fy - 26.0f }, r, Fade(c, 0.33f));
        DrawCircleLinesV(Vector2{ cx, fy - 26.0f }, r, Fade(c, 0.95f));
    }

    // Dizzy stars while the shield is broken.
    if (f.state == ST_SHIELDBREAK) {
        for (int i = 0; i < 3; ++i) {
            const float a = f.stateTimer * 0.13f + i * 2.094f;
            DrawPoly(Vector2{ cx + std::cos(a) * 20.0f, fy - 68.0f + std::sin(a) * 6.0f },
                     5, 5.5f, f.stateTimer * 3.0f, Color{ 255, 232, 110, 255 });
        }
    }

    if (debug) {
        DrawRectangleLines((int)(cx - BODY_HALF_W), (int)(fy - BODY_H), (int)(BODY_HALF_W * 2), (int)BODY_H,
                           Fade(GREEN, 0.55f));
        if (f.state == ST_ACTIVE) {
            float x0, y0, x1, y1;
            hitboxOf(f, x0, y0, x1, y1);
            DrawRectangleLines((int)w2sx(x0), (int)w2sy(y1), (int)(x1 - x0), (int)(y1 - y0), Fade(RED, 0.9f));
        }
    }
}

// ---------------------------------------------------------------------------
// HUD
// ---------------------------------------------------------------------------

static Color damageColor(float d) {
    if (d < 60.0f)  return mix(RAYWHITE, Color{ 255, 226, 90, 255 }, d / 60.0f);
    if (d < 120.0f) return mix(Color{ 255, 226, 90, 255 }, Color{ 255, 150, 40, 255 }, (d - 60.0f) / 60.0f);
    if (d < 190.0f) return mix(Color{ 255, 150, 40, 255 }, Color{ 235, 45, 45, 255 }, (d - 120.0f) / 70.0f);
    return Color{ 235, 45, 45, 255 };
}

static void drawPanel(float px, const Fighter& f, const char* label, Color accent) {
    const float py = SCREEN_H - 132.0f;
    DrawRectangleRounded(Rectangle{ px, py, 300.0f, 108.0f }, 0.14f, 8, Color{ 16, 19, 30, 205 });
    DrawRectangleRoundedLinesEx(Rectangle{ px, py, 300.0f, 108.0f }, 0.14f, 8, 2.0f, Fade(accent, 0.75f));

    DrawText(charStats(f.charId).name, (int)px + 16, (int)py + 11, 18, accent);
    DrawText(label, (int)px + 16, (int)py + 32, 12, Color{ 150, 160, 180, 255 });

    const char* pct = TextFormat("%d%%", (int)f.damage);
    const int fs = 46;
    DrawText(pct, (int)(px + 288 - MeasureText(pct, fs)), (int)py + 16, fs, damageColor(f.damage));

    for (int i = 0; i < START_STOCKS; ++i) {
        const Vector2 c{ px + 24.0f + i * 26.0f, py + 78.0f };
        if (i < f.stocks) {
            DrawCircleV(c, 8.5f, accent);
            DrawCircleV(c, 4.0f, Color{ 16, 19, 30, 255 });
        } else {
            DrawCircleLinesV(c, 8.5f, Color{ 70, 76, 92, 255 });
        }
    }

    // Shield stamina bar: the resource that keeps the CPU from turtling.
    const float frac = f.shield / charStats(f.charId).shieldMax;
    DrawRectangle((int)px + 118, (int)py + 72, 166, 13, Color{ 34, 38, 52, 255 });
    DrawRectangle((int)px + 118, (int)py + 72, (int)(166 * frac), 13,
                  mix(Color{ 255, 70, 70, 255 }, Color{ 90, 210, 255, 255 }, frac));
    DrawText("SHIELD", (int)px + 122, (int)py + 74, 10, Color{ 10, 14, 22, 255 });
}

void drawHud(const GameState& s, const char* label0, const char* label1, const char* headline) {
    drawPanel(64.0f, s.f[0], label0, Color{ 96, 200, 255, 255 });
    drawPanel(SCREEN_W - 364.0f, s.f[1], label1, Color{ 255, 140, 110, 255 });

    DrawText(headline, SCREEN_W / 2 - MeasureText(headline, 18) / 2, 18, 18, Color{ 180, 195, 220, 255 });

    const char* keys = "A/D move   W jump   J normal   K smash   L block   |   1-8 cpu level   C/V swap chars   TAB debug   R reset";
    DrawText(keys, SCREEN_W / 2 - MeasureText(keys, 13) / 2, 44, 13, Color{ 105, 118, 142, 255 });
}
