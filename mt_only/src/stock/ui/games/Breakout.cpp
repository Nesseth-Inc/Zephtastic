// SPDX-License-Identifier: GPL-3.0-only
// Portable Meshtastic 8e6a88d game core. The Zephyr display binding lives in GamesModule.h.
#include "Breakout.h"


// ===========================================================================
// Pure BreakoutGame logic (no display/FS dependencies; always compiled)
// ===========================================================================

uint32_t BreakoutGame::nextRandom()
{
    uint32_t x = rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng = x;
    return x;
}

void BreakoutGame::buildBricks()
{
    for (uint8_t r = 0; r < BRICK_ROWS; r++)
        for (uint8_t c = 0; c < BRICK_COLS; c++)
            bricks[r][c] = 1;
    bricksLeft = static_cast<uint16_t>(BRICK_ROWS) * BRICK_COLS;
}

void BreakoutGame::serveBall()
{
    // Centre the paddle and launch the ball upward from just above it, at a slight angle whose
    // side is chosen randomly so successive serves are not identical.

    ballPxX = static_cast<int32_t>(BOARD_W / 2) * SUBPX;
    ballPxY = static_cast<int32_t>(PADDLE_Y - 2) * SUBPX;
    ballVx = (nextRandom() & 1u) ? 28 : -28;
    ballVy = -BALL_VY;
    ballDocked = true; // wait for the player to fire before the ball moves
}

void BreakoutGame::launchBall()
{
    if (alive)
        ballDocked = false;
}

void BreakoutGame::nextLevel()
{
    levelNum++;
    buildBricks();
    serveBall();
}

void BreakoutGame::reset(uint32_t seed)
{
    rng = seed ? seed : 0xA5A5A5A5u; // xorshift32 must never be seeded with 0
    points = 0;
    livesLeft = START_LIVES;
    levelNum = 1;
    alive = true;
    ballTick = false;
    // A new game starts centred. serveBall() deliberately does NOT re-centre, so the paddle keeps
    // the player's position between lives.
    paddleLeft = (BOARD_W - PADDLE_W) / 2;
    buildBricks();
    serveBall();
}

void BreakoutGame::movePaddle(int16_t dxPx)
{
    if (!alive)
        return;
    paddleLeft += dxPx;
    if (paddleLeft < 0)
        paddleLeft = 0;
    else if (paddleLeft > BOARD_W - PADDLE_W)
        paddleLeft = BOARD_W - PADDLE_W;
}

void BreakoutGame::moveLeft()
{
    movePaddle(-PADDLE_STEP);
}

void BreakoutGame::moveRight()
{
    movePaddle(PADDLE_STEP);
}

bool BreakoutGame::step()
{
    if (!alive)
        return false;

    // Waiting to serve: the ball rides the centre of the paddle (so it still aims by sliding) and
    // no physics, collisions or life losses happen until the player fires it.
    if (ballDocked) {
        ballPxX = static_cast<int32_t>(paddleLeft + PADDLE_W / 2) * SUBPX;
        ballPxY = static_cast<int32_t>(PADDLE_Y - 2) * SUBPX;
        return true;
    }

    // The ball advances on every other step() so the caller can tick (and poll the paddle) at twice
    // the ball's rate -- this keeps the ball speed constant while paddle control refreshes faster.
    ballTick = !ballTick;
    if (!ballTick)
        return true;

    ballPxX += ballVx;
    ballPxY += ballVy;

    int16_t px = static_cast<int16_t>(ballPxX / SUBPX);
    int16_t py = static_cast<int16_t>(ballPxY / SUBPX);

    // Side walls.
    if (px <= 0) {
        ballPxX = 0;
        px = 0;
        ballVx = -ballVx;
    } else if (px >= BOARD_W - 1) {
        ballPxX = static_cast<int32_t>(BOARD_W - 1) * SUBPX;
        px = BOARD_W - 1;
        ballVx = -ballVx;
    }
    // Top wall.
    if (py <= 0) {
        ballPxY = 0;
        py = 0;
        ballVy = -ballVy;
    }

    // Bricks: at most one brick per step (single, blocky reflection off the bottom/top face).
    if (py >= BRICK_TOP && py < BRICK_TOP + BRICK_ROWS * BRICK_H) {
        const int r = (py - BRICK_TOP) / BRICK_H;
        const int c = px / BRICK_W;
        if (r >= 0 && r < BRICK_ROWS && c >= 0 && c < BRICK_COLS && bricks[r][c]) {
            bricks[r][c] = 0;
            bricksLeft--;
            points += POINTS_PER_BRICK;
            ballVy = -ballVy;
            if (bricksLeft == 0) {
                nextLevel();
                return true;
            }
        }
    }

    // Paddle: bounce up and steer horizontally based on where the ball struck.
    if (ballVy > 0 && py >= PADDLE_Y - 1 && py <= PADDLE_Y + PADDLE_H) {
        if (px >= paddleLeft && px < paddleLeft + PADDLE_W) {
            ballPxY = static_cast<int32_t>(PADDLE_Y - 1) * SUBPX;
            ballVy = -BALL_VY;
            // Six zones across the paddle map to increasing outward angles; no zone is vertical.
            static const int16_t vxByZone[6] = {-48, -28, -8, 8, 28, 48};
            int zone = ((px - paddleLeft) * 6) / PADDLE_W;
            if (zone < 0)
                zone = 0;
            else if (zone > 5)
                zone = 5;
            ballVx = vxByZone[zone];
        }
    }

    // Ball lost past the bottom edge.
    if (py >= BOARD_H) {
        if (livesLeft > 0)
            livesLeft--;
        if (livesLeft == 0) {
            alive = false;
            return false;
        }
        serveBall();
    }

    return alive;
}

// ===========================================================================
// Breakout adapter (display + persistence; BaseUI games build only)
// ===========================================================================
