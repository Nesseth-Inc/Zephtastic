// SPDX-License-Identifier: GPL-3.0-only
// Portable Meshtastic 8e6a88d game core. The Zephyr display binding lives in GamesModule.h.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/**
 * Pure, self-contained Breakout game logic.
 *
 * No Arduino/display dependencies - designed to be unit-tested natively (see test/test_breakout)
 * and reused by the Breakout adapter below without pulling in the display stack.
 *
 * The board is a fixed BOARD_W x BOARD_H pixel field. A grid of bricks sits near the top, a paddle
 * slides along the bottom, and a ball bounces between them. Ball position/velocity are tracked in
 * fixed-point sub-pixels (SUBPX per pixel) so movement is smooth and deterministic; everything is
 * integer math and statically sized (no heap).
 */
class BreakoutGame
{
  public:
    // Playfield, in pixels (a standard 128x64 OLED in landscape).
    static constexpr int16_t BOARD_W = 128;
    static constexpr int16_t BOARD_H = 64;

    // Brick grid.
    static constexpr uint8_t BRICK_COLS = 8;
    static constexpr uint8_t BRICK_ROWS = 5;
    static constexpr int16_t BRICK_W = BOARD_W / BRICK_COLS; // 16
    static constexpr int16_t BRICK_H = 4;
    static constexpr int16_t BRICK_TOP = 12; // top of the first course; leaves room for the score row

    // Paddle.
    static constexpr int16_t PADDLE_W = 24;
    static constexpr int16_t PADDLE_H = 2;
    static constexpr int16_t PADDLE_Y = BOARD_H - 3; // top edge of the paddle
    static constexpr int16_t PADDLE_STEP = 6;        // pixels moved per input event

    static constexpr uint8_t START_LIVES = 3;
    static constexpr uint8_t POINTS_PER_BRICK = 10;

    /** (Re)start a full game: rebuild bricks, reset lives/score, serve the ball. `seed` drives the
     * xorshift32 RNG used for the initial serve direction. */
    void reset(uint32_t seed);

    /** Advance the simulation by one tick (move the ball, resolve collisions). Returns true if the
     * game is still in progress, false once the last life is lost. No-op returning false once dead. */
    bool step();

    /** Slide the paddle; the ball is unaffected. moveLeft/moveRight use the default per-press step;
     * movePaddle takes an explicit signed pixel delta (used when polling a held joystick). */
    void movePaddle(int16_t dxPx);
    void moveLeft();
    void moveRight();

    /** After every serve the ball rides the paddle until the player fires it. launchBall() releases
     * it; while docked the ball tracks the paddle and no collisions or life losses occur. */
    void launchBall();
    bool isBallDocked() const { return ballDocked; }

    bool isPlaying() const { return alive; }
    uint32_t score() const { return points; }
    uint8_t lives() const { return livesLeft; }
    uint8_t level() const { return levelNum; }
    uint16_t bricksRemaining() const { return bricksLeft; }

    // --- Rendering accessors (all in board pixels) ---
    int16_t paddleX() const { return paddleLeft; }
    int16_t ballX() const { return static_cast<int16_t>(ballPxX / SUBPX); }
    int16_t ballY() const { return static_cast<int16_t>(ballPxY / SUBPX); }
    bool brickAt(uint8_t row, uint8_t col) const { return row < BRICK_ROWS && col < BRICK_COLS && bricks[row][col]; }

  private:
    static constexpr int32_t SUBPX = 16;    // fixed-point sub-pixels per pixel
    static constexpr int32_t BALL_VY = 40;  // vertical ball speed (sub-pixels/step)
    static constexpr int16_t BALL_SIZE = 2; // ball is drawn BALL_SIZE x BALL_SIZE

    void buildBricks();
    void serveBall();
    void nextLevel();
    uint32_t nextRandom();

    uint8_t bricks[BRICK_ROWS][BRICK_COLS] = {0}; // 0 == cleared, 1 == present
    uint16_t bricksLeft = 0;

    int16_t paddleLeft = 0; // left edge of the paddle, in pixels

    int32_t ballPxX = 0, ballPxY = 0; // ball centre, in sub-pixels
    int32_t ballVx = 0, ballVy = 0;   // ball velocity, in sub-pixels/step

    uint32_t points = 0;
    uint8_t livesLeft = 0;
    uint8_t levelNum = 1;
    uint32_t rng = 1; // xorshift32 state (never 0)
    bool alive = false;
    bool ballTick = false;   // ball advances on every other step() (see step())
    bool ballDocked = false; // ball is waiting on the paddle to be launched
};
