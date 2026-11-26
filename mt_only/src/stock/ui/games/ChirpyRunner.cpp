// SPDX-License-Identifier: GPL-3.0-only
// Portable Meshtastic 8e6a88d game core. The Zephyr display binding lives in GamesModule.h.
#include "ChirpyRunner.h"

// ===========================================================================
// Pure ChirpyRunnerGame logic (no display/FS dependencies; always compiled)
// ===========================================================================

uint32_t ChirpyRunnerGame::nextRandom()
{
    uint32_t x = rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng = x;
    return x;
}

int16_t ChirpyRunnerGame::pickGapSteps()
{
    return static_cast<int16_t>(GAP_STEPS_MIN + static_cast<int16_t>(nextRandom() % (GAP_STEPS_MAX - GAP_STEPS_MIN + 1)));
}

void ChirpyRunnerGame::resetClouds()
{
    // Spread the clouds across the sky at staggered x, at varied heights near the top.
    for (uint8_t i = 0; i < CLOUD_COUNT; i++) {
        cloud[i].xSub = static_cast<int32_t>(i * (BOARD_W / CLOUD_COUNT) + 6) * SUBPX;
        cloud[i].y = static_cast<int16_t>(10 + nextRandom() % 10u); // 10..19 (below the score row)
    }
}

void ChirpyRunnerGame::scrollClouds()
{
    // Slow parallax drift; wrap back to the right (at a fresh height) once off the left edge.
    for (uint8_t i = 0; i < CLOUD_COUNT; i++) {
        cloud[i].xSub -= CLOUD_SPEED_SUB;
        if (cloud[i].xSub / SUBPX + CLOUD_W < 0) {
            cloud[i].xSub = static_cast<int32_t>(BOARD_W + nextRandom() % 24u) * SUBPX;
            cloud[i].y = static_cast<int16_t>(10 + nextRandom() % 10u);
        }
    }
}

void ChirpyRunnerGame::spawnObstacle()
{
    for (uint8_t i = 0; i < MAX_OBSTACLES; i++) {
        if (obst[i].active)
            continue;
        obst[i].active = true;
        obst[i].scored = false;
        obst[i].xSub = static_cast<int32_t>(BOARD_W) * SUBPX;
        obst[i].w = OBST_W;
        // Three height tiers so timing varies (kept clearable with margin for a forgiving jump).
        const uint32_t tier = nextRandom() % 3u;
        obst[i].h = BUILDING_HEIGHTS[tier];
        obst[i].colorIdx = static_cast<uint8_t>((spawnCount / 10u) % OBST_COLOR_COUNT);
        spawnCount++;
        return;
    }
}

void ChirpyRunnerGame::reset(uint32_t seed)
{
    rng = seed ? seed : 0xA5A5A5A5u; // xorshift32 must never be seeded with 0
    points = 0;
    alive = true;
    chirpyTop = groundedTopSub();
    vy = 0;
    jumpGravity = GRAVITY;
    grounded = true;
    for (uint8_t i = 0; i < MAX_OBSTACLES; i++)
        obst[i] = {};
    speedSub = SPEED_BASE;
    spawnTimer = 0; // first obstacle spawns on the first step
    spawnCount = 0;
    resetClouds();
}

int32_t ChirpyRunnerGame::jumpScaleQ8() const
{
    // ratio = current scroll speed / base scroll speed, in Q8 (256 == 1.0).
    const int32_t ratioQ8 = (speedSub * 256) / SPEED_BASE;
    // Feed only JUMP_SPEEDUP_PCT of the speed increase into the jump scale: k = 1 + (ratio-1)*pct.
    const int32_t kQ8 = 256 + ((ratioQ8 - 256) * JUMP_SPEEDUP_PCT) / 100;
    return kQ8 < 256 ? 256 : kQ8; // never slower than the base hop
}

void ChirpyRunnerGame::jump()
{
    if (!alive || !grounded)
        return;
    // Scale velocity by k and gravity by k*k: the apex height is unchanged (Chirpy still clears the
    // same buildings) but the air-time shrinks by k, so the clearing window tightens with the speed.
    const int32_t kQ8 = jumpScaleQ8();
    vy = -(JUMP_V * kQ8) / 256;
    jumpGravity = (GRAVITY * kQ8 * kQ8) / (256 * 256);
    if (jumpGravity < GRAVITY)
        jumpGravity = GRAVITY;
    grounded = false;
}

bool ChirpyRunnerGame::step()
{
    if (!alive)
        return false;

    scrollClouds(); // decorative background parallax

    // --- Chirpy vertical motion (gravity latched at launch, so the arc stays consistent mid-air) ---
    vy += jumpGravity;
    chirpyTop += vy;
    const int32_t gt = groundedTopSub();
    if (chirpyTop >= gt) {
        chirpyTop = gt;
        vy = 0;
        grounded = true;
    } else {
        grounded = false;
    }

    // --- Scroll obstacles, score, retire off-screen ones ---
    for (uint8_t i = 0; i < MAX_OBSTACLES; i++) {
        if (!obst[i].active)
            continue;
        obst[i].xSub -= speedSub;
        const int16_t ox = obstacleX(i);
        if (!obst[i].scored && ox + obst[i].w < CHIRPY_X) {
            obst[i].scored = true;
            points++;
        }
        if (ox + obst[i].w < 0)
            obst[i].active = false;
    }

    // --- Spawn on a tick timer (time-based spacing that scales with speed) ---
    if (spawnTimer > 0)
        spawnTimer--;
    if (spawnTimer <= 0) {
        spawnObstacle();
        spawnTimer = pickGapSteps();
    }

    // --- Difficulty ramp (scroll speed grows with score, then caps) ---
    const uint32_t capped = points < SPEED_CAP_PTS ? points : SPEED_CAP_PTS;
    speedSub = SPEED_BASE + static_cast<int32_t>(capped) * SPEED_INC;

    // --- Collision (forgiving hitbox: skip the antenna, inset the sides) ---
    const int16_t hx = CHIRPY_X + 2;
    const int16_t hxr = hx + (CHIRPY_W - 4);
    const int16_t hBottom = chirpyY() + CHIRPY_H;
    const int16_t hTop = chirpyY() + 4;
    for (uint8_t i = 0; i < MAX_OBSTACLES; i++) {
        if (!obst[i].active)
            continue;
        const int16_t ox = obstacleX(i);
        const int16_t oxr = ox + obst[i].w;
        const int16_t oTop = GROUND_Y - obst[i].h;
        if (hx < oxr && hxr > ox && hTop < GROUND_Y && hBottom > oTop) {
            alive = false;
            return false;
        }
    }

    return alive;
}

// ===========================================================================
// ChirpyRunner adapter (display + persistence; BaseUI games build only)
// ===========================================================================
