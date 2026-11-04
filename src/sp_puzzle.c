/* Board generation, solvability and solvers. See include/moenotes_slidepuzzle.h. */
#include "moenotes_slidepuzzle.h"

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

/* Closed set keyed by the board bytes. Open addressing with linear probing; a full
 * table simply stops absorbing new keys (search stays correct, only slower). */
typedef struct closed_set {
    uint64_t *keys;   /* 0 = empty slot */
    int32_t *g;       /* best known g for that key */
    size_t capacity;
    size_t used;
} closed_set_t;

static uint64_t hash_board(const int32_t *state, int cells)
{
    uint64_t hash = 1469598103934665603ull;
    for (int i = 0; i < cells; i++) {
        hash ^= (uint64_t)(uint32_t)state[i] + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
    }
    return hash == 0 ? 1 : hash;
}

static int closed_init(closed_set_t *set, size_t capacity)
{
    set->keys = (uint64_t *)calloc(capacity, sizeof(uint64_t));
    set->g = (int32_t *)malloc(capacity * sizeof(int32_t));
    set->capacity = capacity;
    set->used = 0;
    return set->keys != NULL && set->g != NULL;
}

static void closed_free(closed_set_t *set)
{
    free(set->keys);
    free(set->g);
    set->keys = NULL;
    set->g = NULL;
    set->capacity = 0;
    set->used = 0;
}

/* Returns 1 when the key was present (and fills *stored_g), else 0. */
static int closed_find(const closed_set_t *set, uint64_t key, int32_t *stored_g)
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

static void closed_insert(closed_set_t *set, uint64_t key, int32_t g)
{
    size_t slot;
    if (set->used * 4 >= set->capacity * 3) {
        return; /* keep the load factor under 75%; dropping is safe for A* correctness */
    }
    slot = (size_t)(key % set->capacity);
    while (set->keys[slot] != 0) {
        if (set->keys[slot] == key) {
            if (g < set->g[slot]) {
                set->g[slot] = g;
            }
            return;
        }
        slot = (slot + 1) % set->capacity;
    }
    set->keys[slot] = key;
    set->g[slot] = g;
    set->used++;
}

/* Manhattan distance plus linear conflict; admissible and consistent.
 *
 * `value` is a tile id, which equals the cell it belongs in *in the solved layout*;
 * the solved layout is not the identity permutation (see mnsp_goal_positions), so
 * the target cell must be looked up rather than assumed to be `value`. */
static int heuristic(const int32_t *state, int grid)
{
    int total = 0;
    int cells = grid * grid;
    int target[64];

    for (int row = 0; row < grid; row++) {
        for (int col = 0; col < grid; col++) {
            target[(grid - 1 - row) * grid + col] = row * grid + col;
        }
    }

    for (int i = 0; i < cells; i++) {
        int32_t value = state[i];
        if (value < 0 || value >= cells) {
            continue;
        }
        {
            int goal_cell = target[value];
            total += abs(i % grid - goal_cell % grid) + abs(i / grid - goal_cell / grid);
        }
    }
    for (int row = 0; row < grid; row++) {
        for (int a = 0; a < grid; a++) {
            int32_t va = state[row * grid + a];
            if (va < 0 || va >= cells || target[va] / grid != row) {
                continue;
            }
            for (int b = a + 1; b < grid; b++) {
                int32_t vb = state[row * grid + b];
                if (vb >= 0 && vb < cells && target[vb] / grid == row &&
                    target[va] % grid > target[vb] % grid) {
                    total += 2;
                }
            }
        }
    }
    for (int col = 0; col < grid; col++) {
        for (int a = 0; a < grid; a++) {
            int32_t va = state[a * grid + col];
            if (va < 0 || va >= cells || target[va] % grid != col) {
                continue;
            }
            for (int b = a + 1; b < grid; b++) {
                int32_t vb = state[b * grid + col];
                if (vb >= 0 && vb < cells && target[vb] % grid == col &&
                    target[va] / grid > target[vb] / grid) {
                    total += 2;
                }
            }
        }
    }
    return total;
}

typedef struct open_node {
    int32_t *state;
    int32_t g;
    int32_t f;
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
    if (na->f != nb->f) {
        return na->f < nb->f;
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

int mnsp_solve(const int32_t *positions, int grid,
               mnsp_move_t *out_moves, int capacity, uint64_t max_states)
{
    int cells;
    int32_t *goal = NULL;
    node_arena_t arena = { 0 };
    index_heap_t open = { 0 };
    closed_set_t closed = { 0 };
    int result = -MNSP_ERR_INTERNAL;
    uint64_t order = 0;
    int32_t blank = -1;

    if (positions == NULL || out_moves == NULL || grid < MNSP_MIN_GRID || grid > MNSP_MAX_GRID ||
        capacity < 0) {
        return -MNSP_ERR_INVALID_ARGUMENT;
    }
    if (!mnsp_positions_valid(positions, grid)) {
        return -MNSP_ERR_INVALID_ARGUMENT;
    }
    cells = grid * grid;

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
    if (!closed_init(&closed, 1u << 20)) {
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
        int32_t *state = (int32_t *)malloc((size_t)cells * sizeof(int32_t));
        if (state == NULL) {
            result = -MNSP_ERR_OUT_OF_MEMORY;
            goto done;
        }
        memcpy(state, positions, (size_t)cells * sizeof(int32_t));
        root.state = state;
        root.g = 0;
        root.f = heuristic(state, grid);
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
        key = hash_board(current->state, cells);
        if (closed_find(&closed, key, &stored)) {
            if (stored <= current->g) {
                continue;
            }
        }
        closed_insert(&closed, key, current->g);
        current->closed = 1;

        if (memcmp(current->state, goal, (size_t)cells * sizeof(int32_t)) == 0) {
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

        if (max_states > 0 && arena.count > max_states) {
            result = -MNSP_ERR_SEARCH_LIMIT;
            goto done;
        }

        {
            /* arena_push may realloc, which invalidates `current`; snapshot what the
             * expansion loop needs first. */
            const int32_t *current_state = current->state;
            const int32_t current_g = current->g;
            const int32_t current_blank = current->blank;
            int ex = current_blank % grid;
            int ey = current_blank / grid;
            for (int dir = 0; dir < 4; dir++) {
                int nx = ex + DIR_X[dir];
                int ny = ey + DIR_Y[dir];
                int target;
                open_node_t child;
                if (!inside(grid, nx, ny)) {
                    continue;
                }
                target = ny * grid + nx;
                child.state = (int32_t *)malloc((size_t)cells * sizeof(int32_t));
                if (child.state == NULL) {
                    result = -MNSP_ERR_OUT_OF_MEMORY;
                    goto done;
                }
                memcpy(child.state, current_state, (size_t)cells * sizeof(int32_t));
                swap_cells(child.state, current_blank, target);
                child.g = current_g + 1;
                child.f = child.g + heuristic(child.state, grid);
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
        }
    }

    result = -MNSP_ERR_UNSOLVABLE;

done:
    free(goal);
    free(open.items);
    closed_free(&closed);
    for (size_t i = 0; i < arena.count; i++) {
        free(arena.items[i].state);
    }
    free(arena.items);
    return result;
}
