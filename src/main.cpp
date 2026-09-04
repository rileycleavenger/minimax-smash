#include "raylib.h"

#include <chrono>
#include <cstring>
#include <thread>

#include "game.h"
#include "render.h"
#include "search.h"

static const char* stateName(uint8_t st) {
    switch (st) {
        case ST_IDLE:        return "IDLE";
        case ST_STARTUP:     return "STARTUP";
        case ST_ACTIVE:      return "ACTIVE";
        case ST_RECOVERY:    return "RECOVERY";
        case ST_HITSTUN:     return "HITSTUN";
        case ST_BLOCK:       return "BLOCK";
        case ST_BLOCKLAG:    return "BLOCKLAG";
        case ST_SHIELDBREAK: return "SHIELDBREAK";
        case ST_RESPAWN:     return "RESPAWN";
        default:             return "DEAD";
    }
}

// A CPU commits to one action and holds it for exactly BEAT_FRAMES frames,
// which is precisely what stepBeat() assumes inside the search.
struct CpuDriver {
    Action      action = ACT_NONE;
    int         beatEnd = 0;
    bool        pending = false;
    SearchStats stats;
    bool        hasStats = false;
};

struct InputBuffer {
    Action act = ACT_NONE;
    int    timer = 0;
};

struct Options {
    bool        demo = false;          // let the search play both sides
    bool        debug = false;
    int         level[2] = { 3, 4 };
    int         chr[2] = { CHAR_PLUMBER, CHAR_SPARKMOUSE };
    int         shotFrame = -1;
    const char* shotPath = "shot.png";
};

static void drainWorker(SearchWorker& w) {
    while (w.busy()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    SearchStats tmp;
    while (w.poll(tmp)) {}
}

static void drawDebug(const GameState& s, const CpuDriver& cpu, int who, int level, float px) {
    const float py = 74.0f, w = 292.0f, h = 236.0f;
    DrawRectangleRounded(Rectangle{ px, py, w, h }, 0.06f, 8, Color{ 10, 12, 20, 215 });
    DrawRectangleRoundedLinesEx(Rectangle{ px, py, w, h }, 0.06f, 8, 1.0f, Color{ 60, 72, 96, 255 });

    const int x = (int)px + 14;
    const int lh = 16;
    int ty = (int)py + 12;
    const Color head{ 120, 220, 180, 255 };
    const Color dim{ 148, 158, 180, 255 };

    DrawText(TextFormat("SEARCH THREAD  -  P%d", who), x, ty, 14, head);
    ty += lh + 4;

    if (!cpu.hasStats) {
        DrawText("waiting for first result...", x, ty, 12, GRAY);
        ty += lh;
    } else {
        const SearchStats& st = cpu.stats;
        DrawText(TextFormat("depth    %d / %d%s", st.depth, level, st.aborted ? "  (capped)" : ""), x, ty, 12, dim);
        ty += lh;
        DrawText(TextFormat("nodes    %lld", st.nodes), x, ty, 12, dim); ty += lh;
        DrawText(TextFormat("time     %.2f ms", st.ms), x, ty, 12, dim); ty += lh;
        DrawText(TextFormat("chose    %s", actionName(st.best)), x, ty, 12, Color{ 255, 200, 110, 255 });
        ty += lh + 8;

        DrawText("MAXIMIN VALUE PER ACTION", x, ty, 12, head);
        ty += lh + 2;
        for (int a = 0; a < ACTION_COUNT; ++a) {
            const bool chosen = ((Action)a == st.best);
            if (!st.actionLegal[a]) {
                DrawText(TextFormat("  %-7s   locked", actionName((Action)a)), x, ty, 12, Color{ 80, 86, 102, 255 });
            } else {
                // '>' is the pick, '=' marks the others it was tied with, and a
                // leading '<' means alpha-beta cut the row so it is only a bound.
                const char* mark = chosen ? ">" : (st.actionTied[a] ? "=" : " ");
                const Color c = chosen ? Color{ 255, 220, 130, 255 }
                                       : (st.actionTied[a] ? Color{ 190, 190, 160, 255 }
                                                           : Color{ 140, 150, 172, 255 });
                DrawText(TextFormat("%s %-7s %s%8.2f", mark, actionName((Action)a),
                                    st.actionExact[a] ? " " : "<", st.actionValue[a]),
                         x, ty, 12, c);
            }
            ty += lh;
        }
        ty += 8;
    }

    const Fighter& f = s.f[who];
    DrawText(TextFormat("state  %s  lock %d", stateName(f.state), lockRemaining(f)), x, ty, 12,
             Color{ 110, 120, 145, 255 });
    ty += lh;
    DrawText(TextFormat("holding  %s", actionName(cpu.action)), x, ty, 12, Color{ 110, 120, 145, 255 });
}

static Options parseArgs(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        auto next = [&](int def) { return (i + 1 < argc) ? std::atoi(argv[++i]) : def; };
        if (!std::strcmp(a, "--demo"))       o.demo = true;
        else if (!std::strcmp(a, "--debug")) o.debug = true;
        else if (!std::strcmp(a, "--p0"))    o.level[0] = next(3);
        else if (!std::strcmp(a, "--p1"))    o.level[1] = next(4);
        else if (!std::strcmp(a, "--c0"))    o.chr[0] = next(CHAR_PLUMBER) % CHAR_COUNT;
        else if (!std::strcmp(a, "--c1"))    o.chr[1] = next(CHAR_SPARKMOUSE) % CHAR_COUNT;
        else if (!std::strcmp(a, "--shot"))  o.shotFrame = next(120);
        else if (!std::strcmp(a, "--out") && i + 1 < argc) o.shotPath = argv[++i];
    }
    for (int i = 0; i < 2; ++i) {
        if (o.level[i] < 1) o.level[i] = 1;
        if (o.level[i] > MAX_CPU_LEVEL) o.level[i] = MAX_CPU_LEVEL;
    }
    return o;
}

int main(int argc, char** argv) {
    Options opt = parseArgs(argc, argv);

    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
    if (opt.shotFrame >= 0) SetTraceLogLevel(LOG_WARNING);
    InitWindow(SCREEN_W, SCREEN_H, "Minimax Smash");
    SetTargetFPS(60);

    const bool cpuControlled[2] = { opt.demo, true };
    bool debug = opt.debug;

    GameState s;
    initGame(s, (uint8_t)opt.chr[0], (uint8_t)opt.chr[1]);

    SearchWorker worker[2];
    CpuDriver    cpu[2];
    InputBuffer  buf;
    fxClear();

    Camera2D cam{};
    updateCamera(cam, s, true);

    const float DT = 1.0f / 60.0f;
    float acc = 0.0f;

    auto reset = [&]() {
        for (int i = 0; i < 2; ++i) drainWorker(worker[i]);
        initGame(s, (uint8_t)opt.chr[0], (uint8_t)opt.chr[1]);
        for (int i = 0; i < 2; ++i) cpu[i] = CpuDriver{};
        buf = InputBuffer{};
        fxClear();
        updateCamera(cam, s, true);
        acc = 0.0f;
    };

    while (!WindowShouldClose()) {
        // -------------------------------------------------------- input ----
        for (int k = 0; k < MAX_CPU_LEVEL; ++k)
            if (IsKeyPressed(KEY_ONE + k)) opt.level[1] = k + 1;
        if (IsKeyPressed(KEY_TAB)) debug = !debug;
        if (IsKeyPressed(KEY_R)) reset();
        if (IsKeyPressed(KEY_C)) { opt.chr[0] = (opt.chr[0] + 1) % CHAR_COUNT; reset(); }
        if (IsKeyPressed(KEY_V)) { opt.chr[1] = (opt.chr[1] + 1) % CHAR_COUNT; reset(); }

        if (IsKeyPressed(KEY_K))      { buf.act = ACT_SMASH;  buf.timer = 8; }
        else if (IsKeyPressed(KEY_J)) { buf.act = ACT_NORMAL; buf.timer = 8; }
        else if (IsKeyPressed(KEY_W) || IsKeyPressed(KEY_SPACE) || IsKeyPressed(KEY_UP)) {
            buf.act = ACT_JUMP; buf.timer = 8;
        }

        int8_t walk = 0;
        if (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT))  walk -= 1;
        if (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT)) walk += 1;
        const bool blockHeld = IsKeyDown(KEY_L);

        // ---------------------------------------------------- simulation ----
        acc += GetFrameTime();
        if (acc > DT * 4.0f) acc = DT * 4.0f;

        while (acc >= DT) {
            acc -= DT;
            if (matchOver(s)) break;

            Action intent[2] = { ACT_NONE, ACT_NONE };

            for (int i = 0; i < 2; ++i) {
                if (!cpuControlled[i]) continue;
                s.f[i].autoWalk = true;

                if (s.frame >= cpu[i].beatEnd) cpu[i].action = ACT_NONE;
                if (!cpu[i].pending && s.frame >= cpu[i].beatEnd && !worker[i].busy() && actionable(s.f[i])) {
                    GameState snap = s;
                    // The search assumes both fighters use the built-in
                    // locomotion rule, since it cannot know the human's stick.
                    snap.f[0].autoWalk = snap.f[1].autoWalk = true;
                    worker[i].request(snap, i, opt.level[i]);
                    cpu[i].pending = true;
                }
                SearchStats fresh;
                if (worker[i].poll(fresh)) {
                    cpu[i].stats = fresh;
                    cpu[i].hasStats = true;
                    if (cpu[i].pending) {
                        cpu[i].action = fresh.best;
                        cpu[i].beatEnd = s.frame + BEAT_FRAMES;
                        cpu[i].pending = false;
                    }
                }
                intent[i] = cpu[i].action;
            }

            // --- human intent, with a short buffer so inputs pressed during
            //     recovery still come out once the fighter is free again.
            if (!cpuControlled[0]) {
                Action human = ACT_NONE;
                if (buf.timer > 0) {
                    human = buf.act;
                    if (actionable(s.f[0])) buf.timer = 0;
                } else if (blockHeld) {
                    human = ACT_BLOCK;
                }
                if (buf.timer > 0) buf.timer--;
                s.f[0].autoWalk = false;
                s.f[0].walkDir = walk;
                intent[0] = human;
            }

            const uint8_t prevState0 = s.f[0].state;

            FrameEvents ev;
            stepFrame(s, intent[0], intent[1], &ev);

            // Dropping shield eats the input; keep it buffered through the
            // lockout so the attack still comes out on the other side.
            if (!cpuControlled[0] && prevState0 == ST_BLOCK && s.f[0].state == ST_BLOCKLAG && buf.act != ACT_BLOCK)
                buf.timer = BLOCKLAG_FRAMES + 4;

            for (int i = 0; i < ev.hitCount; ++i) {
                const auto& h = ev.hits[i];
                fxHit(h.x, h.y, s.f[h.attacker].charId, h.damage, h.blocked);
                if (h.shieldBreak) fxShieldBreak(s.f[1 - h.attacker].x, s.f[1 - h.attacker].y + BODY_H * 0.5f);
            }
            for (int i = 0; i < ev.koCount; ++i) fxKO(ev.kos[i].x, ev.kos[i].y);

            fxUpdate();
        }

        // ----------------------------------------------------- rendering ----
        updateCamera(cam, s, false);
        const int sh = (int)fxShake();
        cam.offset.x += (float)GetRandomValue(-sh, sh);
        cam.offset.y += (float)GetRandomValue(-sh, sh);

        BeginDrawing();
        ClearBackground(Color{ 14, 16, 26, 255 });
        DrawRectangleGradientV(0, 0, SCREEN_W, SCREEN_H, Color{ 24, 28, 48, 255 }, Color{ 10, 11, 18, 255 });

        BeginMode2D(cam);
        drawStage();
        drawFighter(s.f[0], debug);
        drawFighter(s.f[1], debug);
        fxDraw();
        EndMode2D();

        drawHud(s,
                cpuControlled[0] ? TextFormat("CPU  LV %d", opt.level[0]) : "PLAYER",
                TextFormat("CPU  LV %d", opt.level[1]),
                opt.demo ? TextFormat("DEMO  -  DEPTH %d  vs  DEPTH %d", opt.level[0], opt.level[1])
                         : TextFormat("MINIMAX CPU LEVEL %d   -   SEARCH DEPTH %d BEATS", opt.level[1], opt.level[1]));

        if (debug) {
            drawDebug(s, cpu[1], 1, opt.level[1], 16.0f);
            if (opt.demo) drawDebug(s, cpu[0], 0, opt.level[0], SCREEN_W - 292.0f - 16.0f);
        }

        if (matchOver(s)) {
            DrawRectangle(0, 0, SCREEN_W, SCREEN_H, Color{ 0, 0, 0, 165 });
            const int win = winnerOf(s);
            const char* msg = cpuControlled[0] ? TextFormat("DEPTH %d WINS", opt.level[win])
                                               : (win == 0 ? "PLAYER WINS" : "CPU WINS");
            const Color mc = (win == 0) ? Color{ 120, 220, 255, 255 } : Color{ 255, 140, 110, 255 };
            DrawText(msg, SCREEN_W / 2 - MeasureText(msg, 72) / 2, 270, 72, mc);
            const char* sub = TextFormat("press R for a rematch");
            DrawText(sub, SCREEN_W / 2 - MeasureText(sub, 20) / 2, 360, 20, Color{ 175, 188, 210, 255 });
        }

        EndDrawing();

        // TakeScreenshot reads the front buffer, so it has to run after the
        // swap. Wait one extra presented frame so the result overlay is in it.
        if (opt.shotFrame >= 0 && (s.frame >= opt.shotFrame || matchOver(s))) {
            if (matchOver(s) && opt.shotFrame == 99999) {
                opt.shotFrame = (int)s.frame;   // present one more frame, then fire
                continue;
            }
            TakeScreenshot(opt.shotPath);
            break;
        }
    }

    CloseWindow();
    return 0;
}
