// SPDX-License-Identifier: GPL-3.0-only
#include "stock/ui/games/Tetris.h"
#define TetrisGame StockTetrisGame
#include "stock-games/Tetris.h"
#undef TetrisGame
#include <cassert>
#include <cstdio>
#include <initializer_list>

template<class A, class B> static bool samePiece(const A &a, const B &b) {
    return a.col == b.col && a.row == b.row && a.type == b.type && a.rot == b.rot;
}

static unsigned states, kicks, losses;
static void compare(const TetrisGame &zt, const StockTetrisGame &stock) {
    assert(!memcmp(zt.board, stock.board, sizeof(zt.board)));
    assert(samePiece(zt.current(), stock.current()) && samePiece(zt.next(), stock.next()));
    assert(zt.isPlaying() == stock.isPlaying() && zt.score() == stock.score());
    assert(zt.level() == stock.level() && zt.linesCleared() == stock.linesCleared());
    assert(zt.ghostRow() == stock.ghostRow());
    ++states;
}

static void traces() {
    for (uint32_t seed = 0; seed < 128; ++seed) {
        TetrisGame zt; StockTetrisGame stock;
        zt.reset(seed); stock.reset(seed); compare(zt, stock);
        for (unsigned sweep = 0; sweep < 2; ++sweep) {
            for (unsigned i = 0; i < 12; ++i) {
                if (!sweep) assert(zt.moveLeft() == stock.moveLeft());
                else assert(zt.moveRight() == stock.moveRight());
                compare(zt, stock);
            }
            for (unsigned i = 0; i < 4; ++i) {
                const int before = zt.current().col;
                assert(zt.rotate() == stock.rotate());
                kicks += before != zt.current().col;
                compare(zt, stock);
            }
        }
        uint32_t random = seed + 1;
        for (unsigned turn = 0; turn < 512 && stock.isPlaying(); ++turn) {
            random = random * 1664525u + 1013904223u;
            switch ((random >> 16) % 6) {
            case 0: assert(zt.moveLeft() == stock.moveLeft()); break;
            case 1: assert(zt.moveRight() == stock.moveRight()); break;
            case 2: assert(zt.rotate() == stock.rotate()); break;
            case 3: assert(zt.softDrop() == stock.softDrop()); break;
            case 4: zt.hardDrop(); stock.hardDrop(); break;
            case 5: assert(zt.step() == stock.step()); break;
            }
            compare(zt, stock);
        }
        if (!stock.isPlaying()) {
            ++losses; assert(!zt.step() && !stock.step()); compare(zt, stock);
        }
    }
    assert(kicks && losses);
}

// Build rows that the current piece completes with a legal hard drop. The
// shape's lowest occupied row determines its landing against the floor.
static void prepareRows(TetrisGame &zt, StockTetrisGame &stock, unsigned count) {
    memset(zt.board, 0, sizeof(zt.board));
    const auto piece = stock.current();
    int bottom = 0;
    for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x)
        if (StockTetrisGame::pieceCell(piece.type, piece.rot, y, x)) bottom = y;
    for (unsigned off = 0; off < count; ++off) {
        auto &row = zt.board[zt.BOARD_ROWS - 1 - off];
        memset(row, 1, sizeof(row));
        for (int x = 0; x < 4; ++x)
            if (StockTetrisGame::pieceCell(piece.type, piece.rot, bottom - off, x))
                row[piece.col + x] = 0;
    }
    memcpy(stock.board, zt.board, sizeof(zt.board));
}

static void clears() {
    for (unsigned lines = 1; lines <= 4; ++lines) {
        TetrisGame zt; StockTetrisGame stock;
        for (uint32_t seed = 1; ; ++seed) {
            assert(seed < 1024);
            stock.reset(seed); zt.reset(seed);
            if (stock.current().type == 0) break; // vertical I can complete all four rows
        }
        assert(zt.rotate() && stock.rotate());
        prepareRows(zt, stock, lines);
        zt.hardDrop(); stock.hardDrop(); compare(zt, stock);
        assert(stock.linesCleared() == lines);
    }
    TetrisGame zt; StockTetrisGame stock;
    stock.reset(UINT32_MAX); zt.reset(UINT32_MAX);
    for (unsigned line = 1; line <= 205; ++line) {
        prepareRows(zt, stock, 1);
        zt.hardDrop(); stock.hardDrop(); compare(zt, stock);
        assert(stock.linesCleared() == line);
    }
    assert(stock.level() == 20);
}

int main() {
    unsigned shapes = 0;
    for (unsigned t = 0; t <= 7; ++t) for (unsigned r = 0; r <= 4; ++r)
        for (unsigned y = 0; y <= 4; ++y) for (unsigned x = 0; x <= 4; ++x) {
            assert(TetrisGame::pieceCell(t, r, y, x) == StockTetrisGame::pieceCell(t, r, y, x));
            ++shapes;
        }
    traces(); clears();
    std::printf("TETRIS282 states=%u shapes=%u kicks=%u terminal=%u\n", states, shapes, kicks, losses);
}
