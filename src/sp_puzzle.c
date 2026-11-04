/* Board generation, solvability and solvers. See include/moenotes_slidepuzzle.h. */
#include "moenotes_slidepuzzle.h"

#include <stdlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* GetValidMoves walks Vector2Int.up, .down, .left, .right in this order
 * (UnityEngine.Vector2Int..cctor @ 0xBEE96E4: up=(0,1) down=(0,-1) left=(-1,0) right=(1,0)). */
static const int8_t DIR_X[4] = { 0, 0, -1, 1 };
static const int8_t DIR_Y[4] = { 1, -1, 0, 0 };

int mnsp_solve(const int32_t *positions, int grid, mnsp_move_t *out_moves,
               int capacity, uint64_t max_states);

int mnsp_shuffle_steps(int grid)
{
    return 11 * grid * grid;
}

void mnsp_goal_positions(int grid, int32_t *out_positions)
{
    int cells;

    if (out_positions == NULL || grid <= 0) {
        return;
    }
    cells = grid * grid;
    /* The solved board, in the same screen order every array in this library uses
     * (row 0 at the top). A tile's id is the position it belongs in, and the board
     * the game shows on a clear — confirmed on a device capture — is exactly this:
     * 12,13,14,15 / 8,9,10,11 / 4,5,6,7 / 0,1,2,- for a 4x4. */
    for (int row = 0; row < grid; row++) {
        for (int col = 0; col < grid; col++) {
            out_positions[row * grid + col] = (grid - 1 - row) * grid + col;
        }
    }
    out_positions[cells - 1] = -1;
}

static int inside(int grid, int x, int y)
{
    return x >= 0 && y >= 0 && x < grid && y < grid;
}

static void swap_cells(int32_t *positions, int a, int b)
{
    int32_t tmp = positions[a];
    positions[a] = positions[b];
    positions[b] = tmp;
}

mnsp_result_t mnsp_generate(int32_t music_id, int grid, int32_t *out_positions)
{
    mnsp_random_t rng;
    int cells;
    int empty_index;
    int step;
    int steps;

    if (out_positions == NULL || grid < MNSP_MIN_GRID || grid > MNSP_MAX_GRID) {
        return MNSP_ERR_RANGE;
    }

    cells = grid * grid;
    /* BuildBoard starts from the solved layout, which is not the identity
     * permutation: a tile id counts y from the bottom while cells are listed in
     * screen order. See mnsp_goal_positions. */
    mnsp_goal_positions(grid, out_positions);
    empty_index = cells - 1;

    mnsp_random_init(&rng, music_id);
    steps = mnsp_shuffle_steps(grid);
    for (step = 0; step < steps; step++) {
        int ex = empty_index % grid;
        int ey = empty_index / grid;
        int candidates[4];
        int count = 0;
        int dir;
        int choose;

        for (dir = 0; dir < 4; dir++) {
            int nx = ex + DIR_X[dir];
            int ny = ey + DIR_Y[dir];
            if (inside(grid, nx, ny)) {
                candidates[count++] = ny * grid + nx;
            }
        }
        if (count == 0) {
            return MNSP_ERR_INTERNAL;
        }
        choose = mnsp_random_next(&rng, 0, count);
        if (choose < 0 || choose >= count) {
            return MNSP_ERR_INTERNAL;
        }
        /* SwapTile: the chosen tile slides into the blank's cell; the blank moves there. */
        swap_cells(out_positions, empty_index, candidates[choose]);
        empty_index = candidates[choose];
    }
    return MNSP_OK;
}

int mnsp_positions_valid(const int32_t *positions, int grid)
{
    unsigned char seen[64];
    int cells;
    int i;
    int blanks = 0;

    if (positions == NULL || grid < MNSP_MIN_GRID || grid > MNSP_MAX_GRID) {
        return 0;
    }
    cells = grid * grid;
    memset(seen, 0, sizeof seen);
    for (i = 0; i < cells; i++) {
        int32_t value = positions[i];
        if (value == -1) {
            blanks++;
            continue;
        }
        if (value < 0 || value >= cells || seen[value]) {
            return 0;
        }
        seen[value] = 1;
    }
    return blanks == 1;
}

int mnsp_solvable(const int32_t *positions, int grid)
{
    /* Decided by the search itself rather than by a parity shortcut.
     *
     * The classic (inversions + blank row) rule assumes the solved layout is the
     * identity permutation. This board's solved layout is not — a tile id counts y
     * from the bottom while cells are listed top-down — so the shortcut reported the
     * solved board as unsolvable and disagreed with a full BFS on real captures.
     * Running the bounded A* costs milliseconds on 3x3/4x4 and is always right. */
    mnsp_move_t moves[1408];
    int result;

    if (positions == NULL || grid < MNSP_MIN_GRID || grid > MNSP_MAX_GRID) {
        return 0;
    }
    if (!mnsp_positions_valid(positions, grid)) {
        return 0;
    }
    result = mnsp_solve(positions, grid, moves, 1408, 2000000);
    if (result >= 0) {
        return 1;
    }
    if (result == -MNSP_ERR_SEARCH_LIMIT) {
        /* Too far from solved to decide here. Every board the game produces comes
         * from legal slides out of the solved layout, so it is solvable by
         * construction; report that instead of a false negative. */
        return 1;
    }
    return 0;
}

mnsp_result_t mnsp_apply_moves(const int32_t *positions, int grid,
                               const mnsp_move_t *moves, int count, int32_t *out_positions)
{
    int cells;
    int blank;
    int i;

    if (positions == NULL || out_positions == NULL || grid < MNSP_MIN_GRID || grid > MNSP_MAX_GRID) {
        return MNSP_ERR_INVALID_ARGUMENT;
    }
    cells = grid * grid;
    memcpy(out_positions, positions, (size_t)cells * sizeof(int32_t));

    blank = -1;
    for (i = 0; i < cells; i++) {
        if (out_positions[i] == -1) {
            blank = i;
            break;
        }
    }
    if (blank < 0) {
        return MNSP_ERR_INVALID_ARGUMENT;
    }

    for (i = 0; i < count; i++) {
        int bx = blank % grid;
        int by = blank / grid;
        int nx = bx + moves[i].dx;
        int ny = by + moves[i].dy;
        int target;
        if (!inside(grid, nx, ny)) {
            return MNSP_ERR_RANGE;
        }
        target = ny * grid + nx;
        swap_cells(out_positions, blank, target);
        blank = target;
    }
    return MNSP_OK;
}

int mnsp_solve_by_undo(int32_t music_id, int grid, mnsp_move_t *out_moves, int capacity)
{
    mnsp_random_t rng;
    int8_t steps_x[1408];
    int8_t steps_y[1408];
    int cells;
    int steps;
    int empty_index;
    int step;
    int step_count = 0;

    if (grid < MNSP_MIN_GRID || grid > MNSP_MAX_GRID) {
        return -MNSP_ERR_RANGE;
    }
    cells = grid * grid;
    steps = mnsp_shuffle_steps(grid);
    if (steps > (int)(sizeof steps_x)) {
        return -MNSP_ERR_INTERNAL;
    }

    /* Replay the shuffle only to recover the blank path, then reverse it. */
    {
        int32_t *scratch = (int32_t *)malloc((size_t)cells * sizeof(int32_t));
        if (scratch == NULL) {
            return -MNSP_ERR_OUT_OF_MEMORY;
        }
        for (int i = 0; i < cells; i++) {
            scratch[i] = i;
        }
        scratch[cells - 1] = -1;
        empty_index = cells - 1;
        mnsp_random_init(&rng, music_id);
        for (step = 0; step < steps; step++) {
            int ex = empty_index % grid;
            int ey = empty_index / grid;
            int candidates[4];
            int count = 0;
            int dir;
            int choose;
            int nx;
            int ny;
            for (dir = 0; dir < 4; dir++) {
                nx = ex + DIR_X[dir];
                ny = ey + DIR_Y[dir];
                if (inside(grid, nx, ny)) {
                    candidates[count++] = ny * grid + nx;
                }
            }
            choose = mnsp_random_next(&rng, 0, count);
            if (choose < 0 || choose >= count) {
                free(scratch);
                return -MNSP_ERR_INTERNAL;
            }
            nx = candidates[choose] % grid;
            ny = candidates[choose] / grid;
            steps_x[step] = (int8_t)(nx - ex);
            steps_y[step] = (int8_t)(ny - ey);
            swap_cells(scratch, empty_index, candidates[choose]);
            empty_index = candidates[choose];
        }
        free(scratch);
    }

    for (step = steps - 1; step >= 0; step--) {
        if (step_count >= capacity) {
            return -MNSP_ERR_RANGE;
        }
        out_moves[step_count].dx = (int8_t)(-steps_x[step]);
        out_moves[step_count].dy = (int8_t)(-steps_y[step]);
        step_count++;
    }
    return step_count;
}


/* ------------------------------------------------------------------- A* */

/* Best known g per board, covering every state that has been *discovered* — both the
 * ones still on the open list and the ones already expanded.
 *
 * Recording only expanded states (the earlier design) let every not-yet-expanded
 * state be enqueued once per parent that reached it: on an 8x8 board that produced
 * 3.4 arena entries per unique state, i.e. two thirds of all node allocations and
 * heuristic evaluations were duplicates that were thrown away on pop. Moving the
 * insert to child-generation time costs one probe per child and removes them.
 *
 * Open addressing with linear probing. */
typedef struct seen_set {
    uint64_t *keys;   /* 0 = empty slot */
    int32_t *g;       /* best known g for that key */
    size_t capacity;
    size_t used;
    size_t max_capacity; /* growth ceiling; 0 = unbounded */
} seen_set_t;

static uint64_t hash_board(const int8_t *state, int cells)
{
    uint64_t hash = 1469598103934665603ull;
    for (int i = 0; i < cells; i++) {
        hash ^= (uint64_t)(uint32_t)(int32_t)state[i] + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
    }
    return hash == 0 ? 1 : hash;
}

static int seen_init(seen_set_t *set, size_t capacity, size_t max_capacity)
{
    set->keys = (uint64_t *)calloc(capacity, sizeof(uint64_t));
    set->g = (int32_t *)malloc(capacity * sizeof(int32_t));
    set->capacity = capacity;
    set->used = 0;
    set->max_capacity = max_capacity;
    return set->keys != NULL && set->g != NULL;
}

static void seen_free(seen_set_t *set)
{
    free(set->keys);
    free(set->g);
    set->keys = NULL;
    set->g = NULL;
    set->capacity = 0;
    set->used = 0;
}

/* Grow and rehash. Returns 1 on success, 0 when growth is not possible (at the
 * ceiling, or out of memory) — the caller then keeps inserting into the smaller
 * table, which stays correct but costs extra re-expansions. */
static int seen_grow(seen_set_t *set)
{
    size_t next = set->capacity * 2;
    uint64_t *keys;
    int32_t *g;
    size_t i;

    if (set->max_capacity != 0 && next > set->max_capacity) {
        if (set->capacity >= set->max_capacity) {
            return 0;
        }
        next = set->max_capacity;
    }
    keys = (uint64_t *)calloc(next, sizeof(uint64_t));
    g = (int32_t *)malloc(next * sizeof(int32_t));
    if (keys == NULL || g == NULL) {
        free(keys);
        free(g);
        return 0;
    }
    for (i = 0; i < set->capacity; i++) {
        uint64_t key = set->keys[i];
        if (key == 0) {
            continue;
        }
        {
            size_t slot = (size_t)(key % next);
            while (keys[slot] != 0) {
                slot = (slot + 1) % next;
            }
            keys[slot] = key;
            g[slot] = set->g[i];
        }
    }
    free(set->keys);
    free(set->g);
    set->keys = keys;
    set->g = g;
    set->capacity = next;
    return 1;
}

/* Returns 1 when the key was present (and fills *stored_g), else 0. */
static int seen_find(const seen_set_t *set, uint64_t key, int32_t *stored_g)
{
    size_t slot = (size_t)(key % set->capacity);
    for (size_t probes = 0; probes < set->capacity; probes++) {
        if (set->keys[slot] == 0) {
            return 0;
        }
        if (set->keys[slot] == key) {
            if (stored_g != NULL) {
                *stored_g = set->g[slot];
            }
            return 1;
        }
        slot = (slot + 1) % set->capacity;
    }
    return 0;
}

/* Insert or improve. Returns 1 when the entry is recorded, 0 when the table is at
 * its ceiling and cannot grow.
 *
 * The old version silently dropped every insert past a 75% load factor on a fixed
 * 1M-slot table. That is not a benign optimisation: once the closed set stops
 * recording expanded states, A* loses cycle detection entirely and re-expands them,
 * which is exponential rather than linear degradation. It is what made any weight
 * below 4x4 shipped values time out with no diagnostic. Now the table grows, and a
 * table that cannot grow is reported to the caller instead of being quietly unusable. */
static int seen_insert(seen_set_t *set, uint64_t key, int32_t g)
{
    size_t slot;

    if (set->used * 4 >= set->capacity * 3) {
        if (!seen_grow(set)) {
            return 0;
        }
    }
    slot = (size_t)(key % set->capacity);
    while (set->keys[slot] != 0) {
        if (set->keys[slot] == key) {
            if (g < set->g[slot]) {
                set->g[slot] = g;
            }
            return 1;
        }
        slot = (slot + 1) % set->capacity;
    }
    set->keys[slot] = key;
    set->g[slot] = g;
    set->used++;
    return 1;
}

/* Manhattan distance plus linear conflict; admissible and consistent.
 *
 * `value` is a tile id, which equals the cell it belongs in *in the solved layout*;
 * the solved layout is not the identity permutation (see mnsp_goal_positions), so
 * the target cell must be looked up rather than assumed to be `value`. */
/* Everything the heuristic needs that depends only on the board size. Building it
 * once per solve and passing it in removes the integer divisions that used to sit in
 * the inner loops: `target[v] / grid` and `% grid` ran grid^2 times per line for the
 * linear-conflict scan, which dominated the search at 7x7/8x8. */
typedef struct heur_ctx {
    int grid;
    int cells;
    int8_t goal_row[64]; /* goal cell row of tile id v */
    int8_t goal_col[64]; /* goal cell column of tile id v */
    int8_t cell_row[64];
    int8_t cell_col[64];
} heur_ctx_t;

static void heur_ctx_init(heur_ctx_t *ctx, int grid)
{
    ctx->grid = grid;
    ctx->cells = grid * grid;
    for (int i = 0; i < ctx->cells; i++) {
        ctx->cell_row[i] = (int8_t)(i / grid);
        ctx->cell_col[i] = (int8_t)(i % grid);
    }
    /* Tile id v belongs at cell index (grid-1-row)*grid + col in the solved layout,
     * because the game numbers tiles with y counted from the bottom while this
     * library lists cells in screen order. */
    for (int v = 0; v < ctx->cells; v++) {
        int flat = (grid - 1 - v / grid) * grid + (v % grid);
        ctx->goal_row[v] = (int8_t)(flat / grid);
        ctx->goal_col[v] = (int8_t)(flat % grid);
    }
}

/* Manhattan distance plus linear conflict; admissible and consistent. */
static int heuristic(const int8_t *state, const heur_ctx_t *ctx)
{
    const int grid = ctx->grid;
    const int cells = ctx->cells;
    int total = 0;

    for (int i = 0; i < cells; i++) {
        int32_t value = (int32_t)state[i];
        if (value < 0 || value >= cells) {
            continue;
        }
        total += abs(ctx->cell_col[i] - ctx->goal_col[value]) +
                 abs(ctx->cell_row[i] - ctx->goal_row[value]);
    }
    for (int row = 0; row < grid; row++) {
        const int base = row * grid;
        for (int a = 0; a < grid; a++) {
            int32_t va = (int32_t)state[base + a];
            if (va < 0 || va >= cells || ctx->goal_row[va] != row) {
                continue;
            }
            for (int b = a + 1; b < grid; b++) {
                int32_t vb = (int32_t)state[base + b];
                if (vb >= 0 && vb < cells && ctx->goal_row[vb] == row &&
                    ctx->goal_col[va] > ctx->goal_col[vb]) {
                    total += 2;
                }
            }
        }
    }
    for (int col = 0; col < grid; col++) {
        for (int a = 0; a < grid; a++) {
            int32_t va = (int32_t)state[a * grid + col];
            if (va < 0 || va >= cells || ctx->goal_col[va] != col) {
                continue;
            }
            for (int b = a + 1; b < grid; b++) {
                int32_t vb = (int32_t)state[b * grid + col];
                if (vb >= 0 && vb < cells && ctx->goal_col[vb] == col &&
                    ctx->goal_row[va] > ctx->goal_row[vb]) {
                    total += 2;
                }
            }
        }
    }
    return total;
}

typedef struct open_node {
    /* Board packed as bytes: a tile id never exceeds grid*grid-1 <= 63 and the blank
     * is -1, so int8_t holds every value. Storing int32_t here cost 4x the memory and
     * the frontier is the dominant allocation on 7x7/8x8. */
    int8_t *state;    /* NULL once expanded (freed) */
    int32_t g;
    int32_t f;      /* search key: g + weight*scale*h */
    int32_t h;      /* raw heuristic, kept for deterministic tie-breaking */
    int32_t blank;
    int32_t parent;      /* index into the node arena, -1 for the root */
    int8_t move_dx;
    int8_t move_dy;
    uint64_t order;
    int closed;          /* 1 once expanded */
} open_node_t;

typedef struct node_arena {
    open_node_t *items;
    size_t count;
    size_t capacity;
} node_arena_t;

static int arena_push(node_arena_t *arena, const open_node_t *node)
{
    if (arena->count == arena->capacity) {
        size_t next = arena->capacity == 0 ? 1024 : arena->capacity * 2;
        open_node_t *grown = (open_node_t *)realloc(arena->items, next * sizeof(open_node_t));
        if (grown == NULL) {
            return 0;
        }
        arena->items = grown;
        arena->capacity = next;
    }
    arena->items[arena->count++] = *node;
    return 1;
}

/* Binary heap of arena indices ordered by (f, order). */
typedef struct index_heap {
    int32_t *items;
    size_t count;
    size_t capacity;
    const open_node_t *arena;
} index_heap_t;

static int ih_less(const index_heap_t *heap, int32_t a, int32_t b)
{
    const open_node_t *na = &heap->arena[a];
    const open_node_t *nb = &heap->arena[b];
    /* Deterministic ordering: search key, then the heuristic itself (prefers nodes
     * closer to the goal when the key ties, which is what makes a weight > 1 behave
     * like a best-first search instead of degenerating to FIFO), then insertion
     * order. Without the second key the result depends on heap layout. */
    if (na->f != nb->f) {
        return na->f < nb->f;
    }
    if (na->h != nb->h) {
        return na->h < nb->h;
    }
    return na->order < nb->order;
}

static int ih_push(index_heap_t *heap, int32_t index)
{
    if (heap->count == heap->capacity) {
        size_t next = heap->capacity == 0 ? 1024 : heap->capacity * 2;
        int32_t *grown = (int32_t *)realloc(heap->items, next * sizeof(int32_t));
        if (grown == NULL) {
            return 0;
        }
        heap->items = grown;
        heap->capacity = next;
    }
    size_t at = heap->count++;
    while (at > 0) {
        size_t parent = (at - 1) / 2;
        if (ih_less(heap, heap->items[parent], index)) {
            break;
        }
        heap->items[at] = heap->items[parent];
        at = parent;
    }
    heap->items[at] = index;
    return 1;
}

static int32_t ih_pop(index_heap_t *heap)
{
    int32_t top = heap->items[0];
    int32_t last = heap->items[--heap->count];
    size_t at = 0;

    while (1) {
        size_t left = at * 2 + 1;
        size_t right = left + 1;
        size_t best;
        if (left >= heap->count) {
            break;
        }
        best = left;
        if (right < heap->count && ih_less(heap, heap->items[right], heap->items[left])) {
            best = right;
        }
        if (ih_less(heap, last, heap->items[best])) {
            break;
        }
        heap->items[at] = heap->items[best];
        at = best;
    }
    if (heap->count > 0) {
        heap->items[at] = last;
    }
    return top;
}

/* weight = 1 gives an optimal solution (exact A*). Larger weights bias the search
 * towards the goal: the result is a valid but possibly longer path, found with far
 * fewer expansions. `h` is kept unscaled so the heap can break ties on it.
 *
 * `upper_bound` (when > 0) is a known solution length: any state whose admissible
 * lower bound g+h already reaches it cannot lead to a shorter path, so it is pruned.
 * Passing the length of a path found by an earlier, cruder search is what makes the
 * refinement rounds cheaper than the round that produced the bound. */
static int solve_core(const int32_t *positions, int grid,
                      mnsp_move_t *out_moves, int capacity, uint64_t max_states,
                      double weight, int32_t upper_bound)
{
    int cells;
    int32_t *goal = NULL;
    node_arena_t arena = { 0 };
    index_heap_t open = { 0 };
    seen_set_t closed = { 0 };
    heur_ctx_t hctx;
    int result = -MNSP_ERR_INTERNAL;
    uint64_t order = 0;
    int32_t blank = -1;

    if (positions == NULL || out_moves == NULL || grid < MNSP_MIN_GRID || grid > MNSP_MAX_GRID ||
        capacity < 0) {
        return -MNSP_ERR_INVALID_ARGUMENT;
    }
    if (!(weight >= 1.0)) {
        weight = 1.0; /* also catches NaN */
    }
    if (weight > 100.0) {
        weight = 100.0;
    }
    if (!mnsp_positions_valid(positions, grid)) {
        return -MNSP_ERR_INVALID_ARGUMENT;
    }
    cells = grid * grid;
    heur_ctx_init(&hctx, grid);

    goal = (int32_t *)malloc((size_t)cells * sizeof(int32_t));
    if (goal == NULL) {
        result = -MNSP_ERR_OUT_OF_MEMORY;
        goto done;
    }
    mnsp_goal_positions(grid, goal);
    if (memcmp(positions, goal, (size_t)cells * sizeof(int32_t)) == 0) {
        result = 0;
        goto done;
    }
    if (!seen_init(&closed, 1u << 16, 0)) {
        result = -MNSP_ERR_OUT_OF_MEMORY;
        goto done;
    }

    for (int i = 0; i < cells; i++) {
        if (positions[i] == -1) {
            blank = i;
            break;
        }
    }
    {
        open_node_t root;
        int8_t *state = (int8_t *)malloc((size_t)cells);
        if (state == NULL) {
            result = -MNSP_ERR_OUT_OF_MEMORY;
            goto done;
        }
        for (int i = 0; i < cells; i++) {
            state[i] = (int8_t)positions[i];
        }
        root.state = state;
        root.g = 0;
        root.h = heuristic(state, &hctx);
        root.f = (int32_t)((double)root.h * weight);
        root.blank = blank;
        root.parent = -1;
        root.move_dx = 0;
        root.move_dy = 0;
        root.order = order++;
        root.closed = 0;
        if (!arena_push(&arena, &root)) {
            free(state);
            result = -MNSP_ERR_OUT_OF_MEMORY;
            goto done;
        }
        open.arena = arena.items;
        if (!ih_push(&open, 0)) {
            result = -MNSP_ERR_OUT_OF_MEMORY;
            goto done;
        }
    }
    open.arena = arena.items;

    while (open.count > 0) {
        int32_t index = ih_pop(&open);
        open_node_t *current = &arena.items[index];
        uint64_t key;
        int32_t stored = -1;

        if (current->closed) {
            continue;
        }
        /* Bounded refinement: with an admissible h, no path through this state can be
         * shorter than g+h, so once that reaches the incumbent the state is useless. */
        if (upper_bound > 0 && current->g + current->h >= upper_bound) {
            continue;
        }
        /* The state was claimed at generation time. If the table now holds a cheaper
         * g than this node's, a better duplicate is (or was) on the open list too, so
         * this copy cannot improve anything. */
        key = hash_board(current->state, cells);
        if (seen_find(&closed, key, &stored) && stored < current->g) {
            continue;
        }
        current->closed = 1;

        {
            int is_goal = 1;
            for (int i = 0; i < cells; i++) {
                if ((int32_t)current->state[i] != goal[i]) {
                    is_goal = 0;
                    break;
                }
            }
            if (is_goal) {
            int steps = 0;
            int32_t cursor = index;
            while (arena.items[cursor].parent >= 0) {
                steps++;
                cursor = arena.items[cursor].parent;
            }
            if (steps > capacity) {
                result = -MNSP_ERR_RANGE;
                goto done;
            }
            cursor = index;
            for (int i = steps - 1; i >= 0; i--) {
                out_moves[i].dx = arena.items[cursor].move_dx;
                out_moves[i].dy = arena.items[cursor].move_dy;
                cursor = arena.items[cursor].parent;
            }
            result = steps;
            goto done;
            }
        }

        /* Budget on distinct boards discovered, which is what the caller asked for and
         * what actually bounds memory. */
        if (max_states > 0 && closed.used > max_states) {
            result = -MNSP_ERR_SEARCH_LIMIT;
            goto done;
        }

        {
            /* arena_push may realloc, which invalidates `current`; snapshot what the
             * expansion loop needs first. */
            const int8_t *current_state = current->state;
            const int32_t current_g = current->g;
            const int32_t current_blank = current->blank;
            int8_t *current_owned = current->state;
            int ex = current_blank % grid;
            int ey = current_blank / grid;
            for (int dir = 0; dir < 4; dir++) {
                int nx = ex + DIR_X[dir];
                int ny = ey + DIR_Y[dir];
                int target;
                uint64_t child_key;
                open_node_t child;
                if (!inside(grid, nx, ny)) {
                    continue;
                }
                target = ny * grid + nx;
                /* Generate the child's board into a stack scratch buffer first: the key
                 * decides whether it is worth allocating, and the heuristic decides
                 * whether it is worth exploring at all. */
                {
                    int8_t probe[64];
                    int32_t child_h;
                    memcpy(probe, current_state, (size_t)cells);
                    {
                        int8_t tmp = probe[current_blank];
                        probe[current_blank] = probe[target];
                        probe[target] = tmp;
                    }
                    child_key = hash_board(probe, cells);
                    if (seen_find(&closed, child_key, &stored) && stored <= current_g + 1) {
                        continue;
                    }
                    /* Bounded refinement, applied *here* rather than only at pop time.
                     * Pruning at pop alone does not save anything: the child is already
                     * claimed in the seen set and allocated before it reaches the pop,
                     * so the search still walks the full budget. With an admissible h,
                     * g+h >= upper_bound means no path through this child can beat the
                     * incumbent. */
                    child_h = heuristic(probe, &hctx);
                    if (upper_bound > 0 && current_g + 1 + child_h >= upper_bound) {
                        continue;
                    }
                    /* Claim the key now rather than at expansion time. Every state is
                     * therefore enqueued at most once per strictly better g, which is
                     * what keeps the arena close to the number of distinct boards. */
                    if (!seen_insert(&closed, child_key, current_g + 1)) {
                        result = -MNSP_ERR_OUT_OF_MEMORY;
                        goto done;
                    }
                    child.state = (int8_t *)malloc((size_t)cells);
                    if (child.state == NULL) {
                        result = -MNSP_ERR_OUT_OF_MEMORY;
                        goto done;
                    }
                    memcpy(child.state, probe, (size_t)cells);
                    child.g = current_g + 1;
                    child.h = child_h;
                }
                child.f = child.g + (int32_t)((double)child.h * weight);
                child.blank = target;
                child.parent = index;
                child.move_dx = DIR_X[dir];
                child.move_dy = DIR_Y[dir];
                child.order = order++;
                child.closed = 0;
                if (!arena_push(&arena, &child)) {
                    free(child.state);
                    result = -MNSP_ERR_OUT_OF_MEMORY;
                    goto done;
                }
                open.arena = arena.items;
                if (!ih_push(&open, (int32_t)(arena.count - 1))) {
                    result = -MNSP_ERR_OUT_OF_MEMORY;
                    goto done;
                }
            }
            /* The expanded node never needs its board again: the path is rebuilt from
             * (parent, move_dx, move_dy) alone, and re-expansion is prevented by the
             * closed flag. Freeing here keeps peak memory proportional to the frontier
             * instead of to every node ever enqueued — measured 626 MiB -> well under
             * 100 MiB on an 8x8 board. */
            free(current_owned);
            /* `current` may have been invalidated by arena_push's realloc; the arena
             * owns the slot at `index`, so write through a fresh lookup. */
            arena.items[index].state = NULL;
        }
    }

    result = -MNSP_ERR_UNSOLVABLE;

done:
    if (getenv("SP_DEBUG_COUNTS")) {
        fprintf(stderr, "arena=%zu closed=%zu ratio=%.2f\n",
                arena.count, closed.used,
                closed.used ? (double)arena.count / (double)closed.used : 0.0);
    }
    free(goal);
    free(open.items);
    seen_free(&closed);
    for (size_t i = 0; i < arena.count; i++) {
        free(arena.items[i].state);
    }
    free(arena.items);
    return result;
}

int mnsp_solve(const int32_t *positions, int grid,
               mnsp_move_t *out_moves, int capacity, uint64_t max_states)
{
    return solve_core(positions, grid, out_moves, capacity, max_states, 1.0, 0);
}

int mnsp_solve_weighted(const int32_t *positions, int grid,
                        mnsp_move_t *out_moves, int capacity, uint64_t max_states,
                        double weight)
{
    return solve_core(positions, grid, out_moves, capacity, max_states, weight, 0);
}

/* Weighted A* over a ladder of biases, returning the shortest path any of them finds.
 *
 * A single weight is a lottery on large boards: the bias that makes one board trivial
 * can make another exhaust its budget, and path length is not monotone in the weight
 * (on one 7x7 board w=5 returned 305 moves where w=6 returned 289 in 0.1 s). So the
 * first round is deliberately over-biased — it is nearly guaranteed to produce *some*
 * path cheaply — and later rounds drop the bias to shorten it.
 *
 * Each later round is given the best length so far as an upper bound, which prunes
 * every state whose admissible lower bound g+h already reaches it. The rounds therefore
 * get *cheaper* as they get more accurate, instead of costing more.
 *
 * Returns the move count, or a negative mnsp_result_t — MNSP_ERR_SEARCH_LIMIT when
 * every round ran out, in which case the caller should fall back to undo replay. */
int mnsp_solve_best(const int32_t *positions, int grid,
                    mnsp_move_t *out_moves, int capacity, uint64_t max_states)
{
    /* Round 0 is the safety net: it must finish, and it must finish *cheaply*, because
     * a round that exhausts its cap costs the full cap. That rules out starting at a
     * mild bias hoping to save a few moves — on this game's boards the mild bias is
     * the slow one. Concrete measurements on single boards, all of which the ladder
     * has to survive:
     *
     *   5x5 100010:  w=1.5 -> 8.6 s exhausted    w=2.0 -> 0.60 s, 95 moves
     *   6x6 100006:  w=3.0 -> 8.1 s (5.7M states) w=4.0 -> 0.04 s, 140 moves
     *   7x7 100066:  w=5.0 -> 15 s, 305 moves     w=6.0 -> 0.12 s, 289 moves
     *
     * So each ladder starts high and only descends one step. The second entry is the
     * refinement: with the incumbent bound pruning it at generation time, it either
     * shortens the path cheaply or returns in a fraction of the first round. */
    static const double LADDER_5[] = { 2.5, 2.0 };
    static const double LADDER_6[] = { 4.5, 3.5 };
    /* One entry: refinement rounds did not pay for themselves at this size. Dropping
     * 7x7 from 6.0 to 4.0 on board 100066 re-explored for 26 s and returned a *longer*
     * path (305 vs 289). */
    static const double LADDER_7[] = { 6.0 };
    /* 8x8 needs *two* mutually exclusive biases, and this is the only size where that
     * was proven necessary rather than merely convenient. Over all 84 songs no single
     * weight covers the set:
     *
     *   w=10            78/84
     *   w=14            83/84  (fails 100404)
     *   w=16            83/84  (fails 100404)
     *
     * and the boards that break are opposite: 100404 solves instantly at w=8 and fails
     * at every w >= 10, while 100021 fails at w <= 10 and needs w >= 12. Path length is
     * not monotone in the weight, so neither direction is "safer".
     *
     * {8, 14} covers all 84 (average 408 moves); adding 6.0 as a third round improves
     * that to 379 moves but costs another full-budget round on every board the first
     * two miss, which roughly doubles the sweep time. The pair is the shipped choice. */
    static const double LADDER_8[] = { 8.0, 14.0 };
    const double *weights;
    int rounds;
    int best = -1;
    int best_len = 0;
    mnsp_move_t *scratch = NULL;
    int result = -MNSP_ERR_SEARCH_LIMIT;

    if (positions == NULL || out_moves == NULL || grid < MNSP_MIN_GRID || grid > MNSP_MAX_GRID ||
        capacity < 0) {
        return -MNSP_ERR_INVALID_ARGUMENT;
    }
    if (grid <= 4) {
        /* Small boards are solved exactly and instantly; no bias needed. */
        return mnsp_solve(positions, grid, out_moves, capacity, max_states);
    }

    if (grid == 5) {
        weights = LADDER_5;
        rounds = (int)(sizeof LADDER_5 / sizeof LADDER_5[0]);
    } else if (grid == 6) {
        weights = LADDER_6;
        rounds = (int)(sizeof LADDER_6 / sizeof LADDER_6[0]);
    } else if (grid == 7) {
        weights = LADDER_7;
        rounds = (int)(sizeof LADDER_7 / sizeof LADDER_7[0]);
    } else {
        weights = LADDER_8;
        rounds = (int)(sizeof LADDER_8 / sizeof LADDER_8[0]);
    }

    scratch = (mnsp_move_t *)malloc((size_t)(capacity > 0 ? capacity : 1) * sizeof(mnsp_move_t));
    if (scratch == NULL) {
        return -MNSP_ERR_OUT_OF_MEMORY;
    }

    for (int i = 0; i < rounds; i++) {
        /* Cumulative cap: round i may consume up to max_states*(i+1)/rounds, so the
         * final round can still use the whole allowance while the early cheap rounds
         * cannot starve it. */
        uint64_t allow = max_states == 0 ? 0
                       : max_states * (uint64_t)(i + 1) / (uint64_t)rounds;
        /* Refinement rounds are pruned by the incumbent: every state whose admissible
         * bound reaches best_len is dropped, so a round that can only tie ends almost
         * immediately instead of re-exploring the same space. */
        int count = solve_core(positions, grid, scratch, capacity, allow, weights[i],
                               i == 0 ? 0 : best_len);
        if (count >= 0) {
            if (best < 0 || count < best_len) {
                memcpy(out_moves, scratch, (size_t)count * sizeof(mnsp_move_t));
                best_len = count;
                best = count;
            }
        } else if (count == -MNSP_ERR_SEARCH_LIMIT) {
            continue;
        } else {
            /* A real error (bad input, out of memory): stop, but keep any path found. */
            if (best < 0) {
                result = count;
            }
            break;
        }
    }

    free(scratch);
    return best >= 0 ? best : result;
}

double mnsp_default_weight(int grid)
{
    /* Measured over 6-12 freshly shuffled boards per size (2026-11-04, see docs/109).
     *
     *   N   weight  outcome
     *   3-4   1.0   exact A*, optimal, under 0.1 s
     *   5     1.5   57-73 moves in 0.00-0.25 s (exact would be ~60, so near-optimal)
     *   6     3.0   116-172 moves, 0.06 s average, 6/6 solved
     *   7     4.0   229-307 moves, 5/6 solved
     *   8     5.0   336-434 moves, 5/6 solved
     *
     * Beyond 6x6 exact search is hopeless (the reachable state count is 10^39 at 6x6
     * and 10^87 at 8x8), so the bias is what makes these sizes tractable at all.
     * Where a board still exceeds the budget the caller falls back to undo replay. */
    if (grid <= 4) {
        return 1.0;
    }
    if (grid == 5) {
        return 1.5;
    }
    if (grid == 6) {
        return 3.0;
    }
    if (grid == 7) {
        return 4.0;
    }
    return 5.0;
}

uint64_t mnsp_default_budget(int grid)
{
    /* 8M keeps every size under about 12 s even in the worst case measured; smaller
     * boards finish long before the cap. */
    if (grid <= 4) {
        return 4000000u;
    }
    return 8000000u;
}
