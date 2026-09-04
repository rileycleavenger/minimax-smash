#pragma once
#include "raylib.h"
#include "game.h"

constexpr int   SCREEN_W        = 1600;
constexpr int   SCREEN_H        = 900;
constexpr float GROUND_SCREEN_Y = 560.0f;

inline float w2sx(float x) { return SCREEN_W * 0.5f + x; }
inline float w2sy(float y) { return GROUND_SCREEN_Y - y; }
inline Vector2 w2s(float x, float y) { return Vector2{ w2sx(x), w2sy(y) }; }

// Purely cosmetic; lives outside GameState so the search never touches it.
void fxUpdate();
void fxDraw();
void fxClear();
void fxHit(float wx, float wy, int attackerChar, float power, bool blocked);
void fxShieldBreak(float wx, float wy);
void fxKO(float wx, float wy);
float fxShake();

// Smash-style framing: zooms in when the fighters are close, pulls back for
// offstage chases. Without it the blast zones force a permanent wide shot.
void updateCamera(Camera2D& cam, const GameState& s, bool snap);

void drawStage();
void drawFighter(const Fighter& f, bool debug);
void drawHud(const GameState& s, const char* label0, const char* label1, const char* headline);
