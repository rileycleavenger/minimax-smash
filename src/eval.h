#pragma once
#include "game.h"

// Leaf evaluation, in points, from the perspective of fighter `me`.
float evaluate(const GameState& s, int me);
