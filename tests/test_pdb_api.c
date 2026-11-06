/* Pattern database API contract tests.
 *
 * Building a full 5x5 table set takes about three minutes, which is too slow for the
 * regular test suite. These tests therefore cover everything that does NOT need the
 * tables: the API surface, input validation, and the "no PDB for this grid" contract.
 * The expensive admissibility proof lives in work/128_slide-puzzle-pdb/ and is a
 * manual, documented run -- see docs/110_slide-puzzle-optimal-pdb.md.
 *
 * Set MNSP_SLOW_PDB_TESTS=1 to also build the real tables and check the heuristic
 * against a known optimum (about three minutes plus 530 MiB).
 */
#include "moenotes_slidepuzzle.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_support_and_rejection(void)
{
    /* Only 5x5 has a pattern database. */
    assert(mnsp_pdb_supported(5) == 1);
    assert(mnsp_pdb_supported(3) == 0);
    assert(mnsp_pdb_supported(4) == 0);
    assert(mnsp_pdb_supported(6) == 0);
    assert(mnsp_pdb_supported(8) == 0);

    /* Creating for an unsupported size must fail cleanly, not crash. */
    assert(mnsp_pdb_create(3) == NULL);
    assert(mnsp_pdb_create(4) == NULL);
    assert(mnsp_pdb_create(6) == NULL);
    assert(mnsp_pdb_create(0) == NULL);
    assert(mnsp_pdb_create(-1) == NULL);

    /* The other entry points must reject a NULL handle. */
    assert(mnsp_pdb_ready(NULL) == 0);
    assert(mnsp_pdb_heuristic(NULL, NULL, 5) == -1);
    mnsp_pdb_destroy(NULL); /* must be a no-op */

    {
        int32_t board[25];
        mnsp_move_t moves[64];
        mnsp_generate(100001, 5, board);
        assert(mnsp_solve_pdb(NULL, board, 5, moves, 64, 1.5, 1000) < 0);
        assert(mnsp_solve_optimal(NULL, board, 5, moves, 64, 1000) < 0);
    }
}

static void test_handle_free_paths_are_unchanged(void)
{
    /* The PDB is an addition; everything that existed before must behave the same.
     * These are spot checks that the standard solver still works with no PDB present. */
    int32_t board[25];
    mnsp_move_t moves[4096];
    int count;

    assert(mnsp_generate(100001, 5, board) == MNSP_OK);
    assert(mnsp_positions_valid(board, 5));
    assert(mnsp_solvable(board, 5));

    count = mnsp_solve_best(board, 5, moves, 4096, mnsp_default_budget(5));
    assert(count > 0);

    {
        int32_t goal[25];
        int32_t after[25];
        mnsp_goal_positions(5, goal);
        assert(mnsp_apply_moves(board, 5, moves, count, after) == MNSP_OK);
        assert(memcmp(after, goal, sizeof goal) == 0);
    }
}

static void test_invalid_inputs_rejected(void)
{
    int32_t board[25];
    int32_t bad[25];
    mnsp_move_t moves[64];

    mnsp_generate(100001, 5, board);

    /* Duplicate tile: not a valid position. */
    memcpy(bad, board, sizeof bad);
    bad[0] = bad[1];
    assert(!mnsp_positions_valid(bad, 5));

    /* No blank at all: every other cell keeps a distinct tile. Taking the first 24
     * tile values and repeating the last one yields 25 tiles and zero blanks. */
    {
        int32_t first = -1;
        memcpy(bad, board, sizeof bad);
        for (int i = 0; i < 25; i++) {
            if (bad[i] < 0) { first = bad[i]; break; }
        }
        assert(first < 0); /* the fixture really has a blank */
        /* Fill the blank with a duplicated value so no blank remains. */
        for (int i = 0; i < 25; i++) {
            if (bad[i] < 0) { bad[i] = board[0]; break; }
        }
        assert(!mnsp_positions_valid(bad, 5));
    }

    /* Two blanks. */
    memcpy(bad, board, sizeof bad);
    for (int i = 0; i < 25; i++) {
        if (bad[i] < 0) { bad[i] = -1; break; }
    }
    bad[0] = -1;
    assert(!mnsp_positions_valid(bad, 5));
    (void)moves;
}

static void test_admissibility_against_known_optimum(void)
{
    /* Optima measured independently with a separate IDA* implementation; see
     * docs/110. Admissibility requires h <= optimum for every reachable board. */
    static const struct { int32_t music_id; int optimum; } cases[] = {
        { 100001, 61 }, { 100002, 59 }, { 100050, 53 }, { 100100, 55 }
    };
    mnsp_pdb_t *pdb;
    mnsp_move_t moves[1024];

    printf("  building pattern database (this takes about three minutes) ...\n");
    fflush(stdout);
    pdb = mnsp_pdb_create(5);
    assert(pdb != NULL);
    assert(mnsp_pdb_ready(pdb) == 1);

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        int32_t board[25];
        int h;
        int len;
        assert(mnsp_generate(cases[i].music_id, 5, board) == MNSP_OK);
        h = mnsp_pdb_heuristic(pdb, board, 5);
        assert(h >= 0);
        assert(h <= cases[i].optimum);   /* the whole point of the PDB */

        /* And the exact solver must reproduce a shortest path. */
        len = mnsp_solve_optimal(pdb, board, 5, moves, 1024, 600000000ull);
        assert(len == cases[i].optimum);
    }

    /* Unsupported grid / bad board must be rejected even with a live handle. */
    assert(mnsp_pdb_heuristic(pdb, NULL, 5) == -1);
    assert(mnsp_solve_pdb(pdb, NULL, 5, moves, 1024, 1.5, 1000) < 0);
    {
        int32_t board[25];
        mnsp_generate(100001, 5, board);
        assert(mnsp_solve_pdb(pdb, board, 4, moves, 1024, 1.5, 1000) < 0);
        assert(mnsp_pdb_heuristic(pdb, board, 4) == -1);
    }

    mnsp_pdb_destroy(pdb);
}

int main(void)
{
    test_support_and_rejection();
    test_handle_free_paths_are_unchanged();
    test_invalid_inputs_rejected();

    if (getenv("MNSP_SLOW_PDB_TESTS") != NULL) {
        test_admissibility_against_known_optimum();
    } else {
        puts("  (skipping the 3-minute admissibility check; "
             "set MNSP_SLOW_PDB_TESTS=1 to run it)");
    }
    puts("test_pdb_api: ok");
    return 0;
}
