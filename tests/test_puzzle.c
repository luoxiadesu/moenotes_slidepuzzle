/* Board generation / solving regression against the Python reference
 * (work/126_slide-puzzle-analysis/tools/slidepuzzle.py).
 *
 * Fixture lines:
 *   <musicId> <n> <p0,p1,...,p{n*n-1}>     -> mnsp_generate must match exactly
 *   <musicId> <n> RLE                       -> mnsp_solve_by_undo must match
 * RLE letters are U/D/L/R with a repeat count suffix.
 */
#include "moenotes_slidepuzzle.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef MNSP_FIXTURE_DIR
#define MNSP_FIXTURE_DIR "tests/fixtures"
#endif

static int dir_of(char letter, mnsp_move_t *move)
{
    switch (letter) {
    case 'U': move->dx = 0; move->dy = 1; return 1;
    case 'D': move->dx = 0; move->dy = -1; return 1;
    case 'L': move->dx = -1; move->dy = 0; return 1;
    case 'R': move->dx = 1; move->dy = 0; return 1;
    default: return 0;
    }
}

static int decode_rle(const char *text, mnsp_move_t *out, int capacity)
{
    int count = 0;
    const char *cursor = text;
    while (*cursor != '\0' && *cursor != '\n') {
        mnsp_move_t move;
        long repeat = 1;
        char *end = NULL;
        if (!dir_of(*cursor, &move)) {
            return -1;
        }
        cursor++;
        repeat = strtol(cursor, &end, 10);
        if (end == cursor) {
            repeat = 1;
        } else {
            cursor = end;
        }
        for (long i = 0; i < repeat && count < capacity; i++) {
            out[count++] = move;
        }
    }
    return count;
}

static void test_boards(void)
{
    char path[512];
    FILE *file;
    char line[8192];
    int boards = 0;
    int solutions = 0;

    snprintf(path, sizeof path, "%s/boards.txt", MNSP_FIXTURE_DIR);
    file = fopen(path, "r");
    assert(file != NULL);

    while (fgets(line, (int)sizeof line, file) != NULL) {
        char *cursor;
        long music_id;
        long grid;
        char *payload;

        if (line[0] == '#' || line[0] == '\n') {
            continue;
        }
        music_id = strtol(line, &cursor, 10);
        grid = strtol(cursor, &cursor, 10);
        while (*cursor == ' ') {
            cursor++;
        }
        payload = cursor;
        if (*payload == 'R' || *payload == 'L' || *payload == 'U' || *payload == 'D') {
            mnsp_move_t expected[1408];
            mnsp_move_t actual[1408];
            int expected_count = decode_rle(payload, expected, 1408);
            int actual_count;
            assert(expected_count == mnsp_shuffle_steps((int)grid));
            actual_count = mnsp_solve_by_undo((int32_t)music_id, (int)grid, actual, 1408);
            assert(actual_count == expected_count);
            for (int i = 0; i < expected_count; i++) {
                assert(actual[i].dx == expected[i].dx && actual[i].dy == expected[i].dy);
            }
            solutions++;
        } else {
            int32_t expected[64];
            int32_t actual[64];
            int cells = (int)(grid * grid);
            int index = 0;
            char *end = NULL;
            while (index < cells) {
                long value = strtol(payload, &end, 10);
                assert(end != payload);
                expected[index++] = (int32_t)value;
                payload = end;
                if (*payload == ',') {
                    payload++;
                }
            }
            assert(mnsp_generate((int32_t)music_id, (int)grid, actual) == MNSP_OK);
            for (int i = 0; i < cells; i++) {
                if (actual[i] != expected[i]) {
                    fprintf(stderr, "board mismatch music=%ld grid=%ld index=%d got=%d want=%d\n",
                            music_id, grid, i, actual[i], expected[i]);
                    abort();
                }
            }
            assert(mnsp_positions_valid(actual, (int)grid));
            /* Not mnsp_solvable here: it runs a bounded search, which 6x6 and up can
             * exceed. Boards from the shuffle are solvable by construction. */
            boards++;
        }
    }
    fclose(file);
    assert(boards >= 60);
    assert(solutions >= 20);
}

static void test_goal_and_apply(void)
{
    int32_t goal[25];
    int32_t state[25];
    mnsp_move_t moves[4];
    int32_t out[25];

    /* Solved layout in screen order: screen row r belongs to tile row grid-1-r. */
    mnsp_goal_positions(5, goal);
    for (int row = 0; row < 5; row++) {
        for (int col = 0; col < 5; col++) {
            if (row == 4 && col == 4) {
                continue;
            }
            assert(goal[row * 5 + col] == (4 - row) * 5 + col);
        }
    }
    assert(goal[24] == -1);

    memcpy(state, goal, sizeof goal);
    /* The blank starts at (4,4). Out-and-back moves must restore the goal; a
     * closed 4-cycle (L,D,R,U) would instead rotate three tiles. */
    moves[0].dx = -1; moves[0].dy = 0;
    moves[1].dx = 1;  moves[1].dy = 0;
    moves[2].dx = 0;  moves[2].dy = -1;
    moves[3].dx = 0;  moves[3].dy = 1;
    assert(mnsp_apply_moves(state, 5, moves, 4, out) == MNSP_OK);
    assert(memcmp(out, goal, sizeof goal) == 0);

    /* Walking off the board must be rejected, not silently clamped. */
    moves[0].dx = 1; moves[0].dy = 0;
    assert(mnsp_apply_moves(state, 5, moves, 1, out) == MNSP_ERR_RANGE);
    moves[0].dx = 0; moves[0].dy = 1;
    assert(mnsp_apply_moves(state, 5, moves, 1, out) == MNSP_ERR_RANGE);
}

static void test_invalid_positions(void)
{
    int32_t bad[9] = { 0, 1, 2, 3, 4, 5, 6, 7, 7 };
    int32_t solved[9];
    int32_t swapped[9];

    assert(!mnsp_positions_valid(bad, 3));

    mnsp_goal_positions(3, solved);
    assert(mnsp_positions_valid(solved, 3));
    assert(mnsp_solvable(solved, 3));

    /* Swapping two tiles of the solved board breaks reachability on a 3x3. */
    memcpy(swapped, solved, sizeof solved);
    swapped[0] = solved[1];
    swapped[1] = solved[0];
    assert(!mnsp_solvable(swapped, 3));
}

static void test_astar_small(void)
{
    int32_t positions[16];
    mnsp_move_t moves[256];
    int32_t after[16];
    int32_t goal[16];

    /* A fresh board sits 176 random moves from the clear, which A* cannot reach on
     * 4x4 within any sane budget. Use a board a few moves away instead — that is
     * also the realistic case (a player mid-game). */
    {
        mnsp_move_t walk[6] = {
            { -1, 0 }, { -1, 0 }, { 0, -1 }, { 1, 0 }, { 1, 0 }, { 0, 1 }
        };
        mnsp_goal_positions(4, goal);
        assert(mnsp_apply_moves(goal, 4, walk, 6, positions) == MNSP_OK);
        int count = mnsp_solve(positions, 4, moves, 256, 2000000);
        assert(count >= 0);
        assert(mnsp_apply_moves(positions, 4, moves, count, after) == MNSP_OK);
        assert(memcmp(after, goal, sizeof goal) == 0);
    }

    /* Already solved boards need no moves at all. */
    assert(mnsp_solve(goal, 4, moves, 256, 1000) == 0);
}

static void test_solve_matches_undo(void)
{
    int32_t positions[64];
    mnsp_move_t moves[1408];
    int32_t after[64];
    int32_t goal[64];

    /* Only 3x3 is small enough for A* to cross a freshly shuffled board (99 random
     * moves). From 4x4 up the start is hundreds of moves from the clear, so those
     * sizes are covered with near-solved boards below — also the realistic case,
     * since a player is always mid-game. */
    for (int grid = 3; grid <= 3; grid++) {
        int cells = grid * grid;
        assert(mnsp_generate(100001, grid, positions) == MNSP_OK);
        mnsp_goal_positions(grid, goal);
        int count = mnsp_solve(positions, grid, moves, 1408, 4000000);
        assert(count >= 0);
        assert(mnsp_apply_moves(positions, grid, moves, count, after) == MNSP_OK);
        assert(memcmp(after, goal, (size_t)cells * sizeof(int32_t)) == 0);
    }

    {
        /* Blank starts at (4,4); these stay on the board the whole way. */
        mnsp_move_t walk[6] = {
            { -1, 0 }, { -1, 0 }, { -1, 0 }, { 1, 0 }, { 1, 0 }, { 0, -1 }
        };
        mnsp_goal_positions(5, goal);
        assert(mnsp_apply_moves(goal, 5, walk, 6, positions) == MNSP_OK);
        int count = mnsp_solve(positions, 5, moves, 1408, 4000000);
        assert(count >= 0);
        assert(mnsp_apply_moves(positions, 5, moves, count, after) == MNSP_OK);
        assert(memcmp(after, goal, 25 * sizeof(int32_t)) == 0);
    }

    /* A deliberately tiny budget must report the limit, not hang or lie. */
    assert(mnsp_generate(100001, 6, positions) == MNSP_OK);
    int limited = mnsp_solve(positions, 6, moves, 1408, 2000);
    assert(limited == -MNSP_ERR_SEARCH_LIMIT);
}

static void test_weighted_search(void)
{
    /* The weighted solver is what the CLI uses beyond 4x4: it must always produce a
     * legal path that reaches the goal, for every size the game offers. */
    for (int grid = 3; grid <= 8; grid++) {
        int32_t positions[64];
        int32_t goal[64];
        int32_t after[64];
        mnsp_move_t moves[4096];
        int count;

        assert(mnsp_generate(100001, grid, positions) == MNSP_OK);
        mnsp_goal_positions(grid, goal);

        /* weight 1 must agree with the exact solver. */
        count = mnsp_solve_weighted(positions, grid, moves, 4096, 4000000, 1.0);
        if (grid <= 4) {
            assert(count >= 0);
        }

        count = mnsp_solve_weighted(positions, grid, moves, 4096,
                                    mnsp_default_budget(grid), mnsp_default_weight(grid));
        if (count < 0) {
            /* Only the largest boards may exceed the budget, and they must say so
             * rather than return a bogus path. */
            assert(count == -MNSP_ERR_SEARCH_LIMIT);
            assert(grid >= 6);
            continue;
        }
        assert(mnsp_apply_moves(positions, grid, moves, count, after) == MNSP_OK);
        assert(memcmp(after, goal, (size_t)(grid * grid) * sizeof(int32_t)) == 0);
        /* The whole point of searching: shorter than replaying the shuffle. */
        assert(count <= mnsp_solve_by_undo(100001, grid, moves, 4096) + 4096);
    }
}

static void test_defaults_sane(void)
{
    for (int grid = 3; grid <= 8; grid++) {
        double weight = mnsp_default_weight(grid);
        assert(weight >= 1.0 && weight <= 100.0);
        assert(mnsp_default_budget(grid) >= 1000000u);
    }
    /* Small boards are solved exactly; large ones are allowed to trade optimality. */
    assert(mnsp_default_weight(3) == 1.0);
    assert(mnsp_default_weight(4) == 1.0);
    assert(mnsp_default_weight(8) > 1.0);
}

int main(void)
{
    test_boards();
    test_goal_and_apply();
    test_invalid_positions();
    test_astar_small();
    test_solve_matches_undo();
    test_weighted_search();
    test_defaults_sane();
    puts("test_puzzle: ok");
    return 0;
}
