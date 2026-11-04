/*
 * moenotes_slidepuzzle.h - SlidePuzzle board generator, screenshot reader and solver.
 *
 * The game (com.bilibili.sirius 1.1.0) generates every sliding-puzzle board locally:
 * the shuffle seed is the song's master id and the board is built by replaying
 * 11 * N * N legal moves from the solved state. Each tile shows the jacket slice of
 * its *correct* position, so a screenshot alone identifies the whole permutation.
 *
 * Coordinates follow the game: Unity origin is bottom-left, y grows upwards, and the
 * empty slot starts at (N-1, N-1) (top-right). tilePositions[currentIndex] holds the
 * tile's correct index, or -1 for the empty slot; index = y * N + x.
 *
 * Ownership: all functions borrow their inputs. Outputs written into caller buffers.
 * No global mutable state; every entry point is safe to call from multiple threads
 * as long as each call uses its own objects.
 */
#ifndef MOENOTES_SLIDEPUZZLE_H
#define MOENOTES_SLIDEPUZZLE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MOENOTES_SLIDEPUZZLE_VERSION_MAJOR 0
#define MOENOTES_SLIDEPUZZLE_VERSION_MINOR 1
#define MOENOTES_SLIDEPUZZLE_VERSION_PATCH 0
#define MOENOTES_SLIDEPUZZLE_VERSION_STRING "0.1.0"

/* Board sizes the game exposes (PuzzleSize Side3..Side8). */
#define MNSP_MIN_GRID 3
#define MNSP_MAX_GRID 8

typedef enum mnsp_result {
    MNSP_OK = 0,
    MNSP_ERR_INVALID_ARGUMENT,
    MNSP_ERR_OUT_OF_MEMORY,
    MNSP_ERR_IO,
    MNSP_ERR_IMAGE,
    MNSP_ERR_RANGE,
    MNSP_ERR_UNSOLVABLE,
    MNSP_ERR_SEARCH_LIMIT,
    MNSP_ERR_INTERNAL
} mnsp_result_t;

const char *mnsp_result_string(mnsp_result_t result);

/* ------------------------------------------------------------------ random */

/* Exact port of .NET System.Random(int), including the 32-bit wraparound that
 * seeds near INT_MIN/INT_MAX rely on. */
typedef struct mnsp_random {
    int32_t state[56];
    int32_t inext;
    int32_t inextp;
} mnsp_random_t;

void mnsp_random_init(mnsp_random_t *rng, int32_t seed);
/* Equivalent to System.Random.Next(minValue, maxValue). */
int32_t mnsp_random_next(mnsp_random_t *rng, int32_t min_value, int32_t max_value);

/* ------------------------------------------------------------------ puzzle */

/* Number of shuffle steps the game performs: 11 * n * n. */
int mnsp_shuffle_steps(int grid);

/* Build the board the game shows for (music_id, grid).
 * out_positions must hold grid*grid int32_t values.
 * Returns MNSP_ERR_RANGE for an unsupported grid. */
mnsp_result_t mnsp_generate(int32_t music_id, int grid, int32_t *out_positions);

/* A move is the blank's displacement: after applying it the blank sits on
 * (previous blank + move). Moves are reported in the same convention. */
typedef struct mnsp_move {
    int8_t dx;
    int8_t dy;
} mnsp_move_t;

/* Number of moves in a solution, or a negative mnsp_result_t. */
int mnsp_solve_by_undo(int32_t music_id, int grid, mnsp_move_t *out_moves, int capacity);

/* A* with Manhattan + linear conflict. Writes at most `capacity` moves.
 * Returns the move count, MNSP_ERR_SEARCH_LIMIT when max_states is exceeded,
 * or another negative mnsp_result_t. */
int mnsp_solve(const int32_t *positions, int grid,
               mnsp_move_t *out_moves, int capacity, uint64_t max_states);

/* Weighted A*: the search key is g + weight * h instead of g + h.
 *
 * weight == 1 is exact (same result as mnsp_solve). Larger weights bias the search
 * towards the goal, which turns an intractable board into a tractable one at the cost
 * of a possibly longer path. Measured on freshly shuffled boards: 5x5 drops from 275
 * moves (undo replay) to 65, 8x8 from 704 to 292, all in about a second.
 *
 * Returns the move count, or a negative mnsp_result_t. */
int mnsp_solve_weighted(const int32_t *positions, int grid,
                        mnsp_move_t *out_moves, int capacity, uint64_t max_states,
                        double weight);

/* Weighted A* over a descending ladder of biases, returning the shortest path any of
 * them finds. This is what the CLI uses above 4x4.
 *
 * A single weight is unreliable on large boards: a bias that solves one board can
 * exhaust the budget on another, and path length is not monotone in the weight. The
 * ladder starts at the bias that almost always finishes in seconds and only descends
 * when needed, so a typical board costs one search. `max_states` is shared across
 * rounds rather than granted to each, which bounds the worst case.
 *
 * Boards of 4x4 and below are solved exactly. Returns the move count or a negative
 * mnsp_result_t. */
int mnsp_solve_best(const int32_t *positions, int grid,
                    mnsp_move_t *out_moves, int capacity, uint64_t max_states);

/* Recommended weight and search budget for a board size: exact below 5x5, biased
 * above it. Both are what the CLI uses for its "auto" effort level. */
double mnsp_default_weight(int grid);
uint64_t mnsp_default_budget(int grid);

/* 1 when the permutation is solvable, 0 otherwise. */
int mnsp_solvable(const int32_t *positions, int grid);

/* 1 when positions is a permutation of 0..n*n-1 with exactly one -1. */
int mnsp_positions_valid(const int32_t *positions, int grid);

/* Apply moves to a copy of positions. Both arrays hold grid*grid values. */
mnsp_result_t mnsp_apply_moves(const int32_t *positions, int grid,
                               const mnsp_move_t *moves, int count, int32_t *out_positions);

/* Filled positions of the solved board: 0..n*n-2 then -1. */
void mnsp_goal_positions(int grid, int32_t *out_positions);

/* ------------------------------------------------------- screenshot reader */

/* Read a whole-screen screenshot, recover the board and return its tilePositions.
 *
 * jacket_path points at the song's cover art (the game slices it N x N and shows
 * each tile at its own correct position, so the screenshot alone determines the
 * permutation). out_positions holds grid*grid values with -1 for the blank.
 *
 * `out_confident` (optional, grid*grid) reports 1 for cells whose match was
 * unambiguous; a 0 there means the cell should be confirmed by a human instead of
 * trusted. `out_total_score` (optional) sums the per-cell correlations.
 *
 * Returns MNSP_ERR_IMAGE when the board or a cell cannot be located.
 */
typedef struct mnsp_read_options {
    int grid;              /* 0 = detect from the board rectangle's aspect, else 3..8 */
    float min_score;       /* per-cell NCC floor, e.g. 0.30f */
    float min_margin;      /* best-minus-runner-up floor, e.g. 0.01f */
    double cell_gap_percent; /* inset applied to each cell to drop tile separators */
} mnsp_read_options_t;

mnsp_result_t mnsp_read_screenshot(const char *screenshot_path, const char *jacket_path,
                                   const mnsp_read_options_t *options,
                                   int32_t *out_positions, uint8_t *out_confident,
                                   float *out_total_score);

/* ------------------------------------------------------------ jacket library */

/* Number of songs with cover art in the built-in table. */
int mnsp_jacket_count(void);

/* Song id and jacket asset name ("jkt_001_100001") for a built-in entry.
 * Returns 0 when `index` is out of range. */
int mnsp_jacket_at(int index, int32_t *out_music_id, const char **out_asset_name);

/* Jacket asset name for a song id, or NULL when unknown. */
const char *mnsp_jacket_asset_name(int32_t music_id);

/* Cache directory used for downloaded jackets (created on demand by fetch). */
int mnsp_jacket_cache_dir(char *out, size_t capacity);

/* Resolve a song id to a local PNG, fetching it first when `allow_fetch` is non-zero.
 *
 * The URL layout is <base_url>/<locale>/Image/Jacket/<name>/<name>.png; base_url may
 * be NULL to use the public mirror. `fetcher` names the download helper program
 * (default "curl"); it is invoked as `curl -fsSL -o <destination> <url>` and, for any
 * other program, as `<fetcher> <url> <destination>`.
 *
 * Returns MNSP_OK on a usable cached file.
 */
typedef struct mnsp_jacket_options {
    const char *region;    /* cache namespace, default "tw" */
    const char *locale;    /* CDN locale path, default "zh-Hant" */
    const char *base_url;  /* NULL for https://assets.bdon.moe */
    const char *fetcher;   /* NULL for "curl" */
    int allow_fetch;       /* 0 = cache only */
} mnsp_jacket_options_t;

mnsp_result_t mnsp_jacket_resolve(int32_t music_id, const mnsp_jacket_options_t *options,
                                  char *out_path, size_t capacity);

/* Blank cell detection helper: index (in tilePositions order) of the flattest cell,
 * or -1 when the board has no obviously empty slot. */
mnsp_result_t mnsp_find_blank_cell(const char *screenshot_path, int grid, int *out_cell);

#ifdef __cplusplus
}
#endif

#endif /* MOENOTES_SLIDEPUZZLE_H */
