/* Disjoint pattern databases for the 5x5 board -- Korf & Felner form.
 *
 * WHY THE BLANK IS NOT IN THE PATTERN
 * -----------------------------------
 * Felner, Korf & Hanan, "Additive Pattern Database Heuristics", JAIR 22 (2004),
 * Theorem 1: if the state variables are partitioned so that no operator affects
 * variables in more than one subset, the sum of the pattern costs is a lower bound.
 * For sliding-tile puzzles they state the consequence directly:
 *
 *   "We need to partition the physical tiles into disjoint subsets. We can't include
 *    the blank in any of these subsets, because every operator affects the blank
 *    position."
 *
 * An earlier version used five tiles *plus the blank* per group and charged 1 for
 * moving the blank into a cell holding a non-pattern tile. That makes each single
 * table admissible, but the four-way sum over-counts: one real move moves exactly one
 * tile, yet every group whose blank is out of place charges that same move again.
 * Measured exhaustively on 3x3 (lab/model_check.c, all 181,440 reachable states):
 * 179,211 states violated the bound, worst case +25. That is why the old heuristic
 * returned 110 on a board whose optimum is 59.
 *
 * The fix is not a cost tweak: it is to drop the blank from the index entirely.
 *
 * NO GLOBAL STATE
 * ---------------
 * Tables live in an mnsp_pdb_t owned by the caller. The public header promises that
 * every entry point is safe to call concurrently as long as each call uses its own
 * objects, and a single static table would break that -- and would also make a WASM
 * build unable to host more than one independent instance. See mnsp_pdb_create /
 * mnsp_pdb_destroy in moenotes_slidepuzzle.h.
 *
 * THE GROUPS
 * ----------
 * The board carries 24 tiles, not 25: mnsp_goal_positions lays out
 * (grid-1-row)*grid+col and then overwrites the last cell with the blank, so tile id 4
 * never appears. Four disjoint groups of six cover all 24, which means the Manhattan
 * term for uncovered tiles disappears -- the PDB alone is the heuristic.
 *
 * The table size is unchanged from the 5-tile-plus-blank design: 25P6 = 127,512,000
 * either way, because "five tiles plus a blank" and "six tiles" both choose 6 symbols
 * from 25 cells. Four groups is about 486 MiB at one byte per entry.
 *
 * WHY PLAIN BFS IS ENOUGH HERE
 * ----------------------------
 * With the blank out of the index every transition costs exactly 1, so a FIFO queue
 * computes exact distances. The zero-cost edge only appears if the blank is modelled
 * with a cost; that variant needs a deque-based 0-1 BFS, and using a FIFO there
 * silently leaves some entries too high. Measured (lab/fifo_vs_01.c, 5x5, a three-tile
 * pattern plus the blank, all 303,600 entries): FIFO disagreed with an exact 0-1 BFS
 * on 31.62% of reachable states, overestimating by up to 6.
 */
#include "moenotes_slidepuzzle.h"
#include "sp_pdb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PDB_GRID 5
#define PDB_CELLS 25
#define PDB_TILES 6
#define PDB_GROUPS 4
#define PDB_ENTRIES (25u * 24u * 23u * 22u * 21u * 20u) /* 127,512,000 */
#define PDB_UNVISITED 0xFFu

/* One byte per entry is enough: measured maxima per group are 39 / 32 / 31 / 37
 * (lab/range2), so six bits would do losslessly, but unpacking costs more than the
 * 25% memory it saves on a desktop. A WASM build that needs the smaller tables should
 * use a smaller pattern (25P5 = 6,375,600 entries, about 6 MiB per group) rather than
 * bit-packing, because a 5-tile group also means a cheaper rank and no unpack step. */
typedef struct pdb_group {
    uint8_t *dist;            /* PDB_ENTRIES bytes, PDB_UNVISITED = not reached */
    int32_t tiles[PDB_TILES]; /* tile ids, in symbol order */
} pdb_group_t;

struct mnsp_pdb {
    pdb_group_t groups[PDB_GROUPS];
    uint32_t divisor[PDB_TILES];
    int ready;
};

/* Four disjoint groups of six covering every tile a 5x5 board carries (all ids 0..24
 * except 4, which the goal layout replaces with the blank). Tiles that sit near each
 * other in the solved layout interfere more, so grouping by rows of the goal board
 * gives a slightly stronger bound than an arbitrary split. */
static const int8_t PDB_GROUP_TILES[PDB_GROUPS][PDB_TILES] = {
    { 20, 21, 22, 23, 24, 15 },
    { 16, 17, 18, 19, 10, 11 },
    { 12, 13, 14, 5, 6, 7 },
    { 8, 9, 0, 1, 2, 3 }
};

static const int8_t DIR_X[4] = { 0, 0, -1, 1 };
static const int8_t DIR_Y[4] = { 1, -1, 0, 0 };

/* -------------------------------------------------------------------- ranking */

/* popcount is spelled out for portability: the project's CMakeLists has an MSVC branch
 * and __builtin_popcount is a GCC/Clang builtin. The compiler still emits a single
 * POPCNT where the target has one. */
static int popcount32(uint32_t value)
{
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_popcount(value);
#else
    /* Hacker's Delight, figure 5-2. */
    value = value - ((value >> 1) & 0x55555555u);
    value = (value & 0x33333333u) + ((value >> 2) & 0x33333333u);
    value = (value + (value >> 4)) & 0x0F0F0F0Fu;
    return (int)((value * 0x01010101u) >> 24);
#endif
}

/* Mixed-radix index of a placement: symbol i takes the ordinal of its cell among the
 * cells still free after symbols 0..i-1 have been placed. A bijection onto
 * [0, PDB_ENTRIES).
 *
 * The ordinal is a popcount of the free mask below the chosen cell rather than a scan.
 * The scan version was O(25^2) per lookup and dominated both the build and every
 * heuristic evaluation; this is O(6). */
static uint32_t pdb_rank(const int32_t *sym_cell)
{
    uint32_t rank = 0;
    uint32_t free_mask = (1u << PDB_CELLS) - 1u;

    for (int i = 0; i < PDB_TILES; i++) {
        uint32_t cell = (uint32_t)sym_cell[i];
        uint32_t below = free_mask & ((1u << cell) - 1u);
        rank = rank * (uint32_t)(PDB_CELLS - i) + (uint32_t)popcount32(below);
        free_mask &= ~(1u << cell);
    }
    return rank;
}

static void pdb_unrank(const uint32_t *divisor, uint32_t rank, int32_t *sym_cell)
{
    uint32_t free_mask = (1u << PDB_CELLS) - 1u;

    for (int i = 0; i < PDB_TILES; i++) {
        uint32_t ordinal = (rank / divisor[i]) % (uint32_t)(PDB_CELLS - i);
        uint32_t seen = 0;
        for (uint32_t cell = 0; cell < PDB_CELLS; cell++) {
            if (!(free_mask & (1u << cell))) {
                continue;
            }
            if (seen == ordinal) {
                sym_cell[i] = (int32_t)cell;
                free_mask &= ~(1u << cell);
                break;
            }
            seen++;
        }
    }
}

/* ---------------------------------------------------------------------- build */

typedef struct rank_queue {
    uint32_t *items;
    size_t capacity;
    size_t head;
    size_t tail;
} rank_queue_t;

static int queue_init(rank_queue_t *q, size_t capacity)
{
    q->items = (uint32_t *)malloc(capacity * sizeof(uint32_t));
    q->capacity = capacity;
    q->head = 0;
    q->tail = 0;
    return q->items != NULL;
}

static void queue_free(rank_queue_t *q)
{
    free(q->items);
    q->items = NULL;
}

static int queue_push(rank_queue_t *q, uint32_t value)
{
    if (q->tail == q->capacity) {
        if (q->head > 0) {
            memmove(q->items, q->items + q->head, (q->tail - q->head) * sizeof(uint32_t));
            q->tail -= q->head;
            q->head = 0;
        }
    }
    if (q->tail == q->capacity) {
        size_t next = q->capacity * 2;
        uint32_t *grown = (uint32_t *)realloc(q->items, next * sizeof(uint32_t));
        if (grown == NULL) {
            return 0;
        }
        q->items = grown;
        q->capacity = next;
    }
    q->items[q->tail++] = value;
    return 1;
}

/* Backwards BFS from the goal pattern. Returns 1 on success.
 *
 * Transitions: any group tile may slide into an adjacent cell that holds no other
 * group tile. The blank is a don't-care -- whatever it takes to bring it next to a
 * tile is not charged, which is exactly what makes the four tables summable. */
static int build_group(const uint32_t *divisor, pdb_group_t *group)
{
    int32_t sym_cell[PDB_TILES];
    rank_queue_t queue = { 0 };
    size_t reached = 1;

    group->dist = (uint8_t *)malloc(PDB_ENTRIES);
    if (group->dist == NULL) {
        return 0;
    }
    memset(group->dist, PDB_UNVISITED, PDB_ENTRIES);

    /* Goal placement: the cell where each tile sits in the solved layout. */
    for (int s = 0; s < PDB_TILES; s++) {
        int32_t value = group->tiles[s];
        sym_cell[s] = (PDB_GRID - 1 - value / PDB_GRID) * PDB_GRID + value % PDB_GRID;
    }

    if (!queue_init(&queue, 1u << 16)) {
        return 0;
    }
    group->dist[pdb_rank(sym_cell)] = 0;
    if (!queue_push(&queue, pdb_rank(sym_cell))) {
        queue_free(&queue);
        return 0;
    }

    while (queue.head < queue.tail) {
        uint32_t rank = queue.items[queue.head++];
        uint8_t depth = group->dist[rank];
        int32_t cells[PDB_TILES];
        int32_t scratch[PDB_TILES];
        uint32_t occupied = 0;

        pdb_unrank(divisor, rank, cells);
        for (int s = 0; s < PDB_TILES; s++) {
            occupied |= 1u << cells[s];
        }

        memcpy(scratch, cells, sizeof scratch);
        for (int s = 0; s < PDB_TILES; s++) {
            int cx = cells[s] % PDB_GRID;
            int cy = cells[s] / PDB_GRID;
            for (int dir = 0; dir < 4; dir++) {
                int nx = cx + DIR_X[dir];
                int ny = cy + DIR_Y[dir];
                int target;
                uint32_t next_rank;

                if (nx < 0 || ny < 0 || nx >= PDB_GRID || ny >= PDB_GRID) {
                    continue;
                }
                target = ny * PDB_GRID + nx;
                if (occupied & (1u << target)) {
                    continue;
                }
                scratch[s] = target;
                next_rank = pdb_rank(scratch);
                scratch[s] = cells[s];
                if (group->dist[next_rank] != PDB_UNVISITED) {
                    continue;
                }
                group->dist[next_rank] = (uint8_t)(depth + 1);
                reached++;
                if (!queue_push(&queue, next_rank)) {
                    queue_free(&queue);
                    return 0;
                }
            }
        }
    }

    queue_free(&queue);
    if (getenv("SP_PDB_VERBOSE")) {
        fprintf(stderr, "pdb: group reached %zu of %u\n", reached, (unsigned)PDB_ENTRIES);
    }
    return 1;
}

/* --------------------------------------------------------------------- public */

int mnsp_pdb_supported(int grid)
{
    return grid == PDB_GRID;
}

mnsp_pdb_t *mnsp_pdb_create(int grid)
{
    mnsp_pdb_t *pdb;

    if (grid != PDB_GRID) {
        return NULL;
    }
    pdb = (mnsp_pdb_t *)calloc(1, sizeof *pdb);
    if (pdb == NULL) {
        return NULL;
    }
    {
        uint32_t product = 1;
        for (int i = PDB_TILES - 1; i >= 0; i--) {
            pdb->divisor[i] = product;
            product *= (uint32_t)(PDB_CELLS - i);
        }
    }
    for (int g = 0; g < PDB_GROUPS; g++) {
        for (int s = 0; s < PDB_TILES; s++) {
            pdb->groups[g].tiles[s] = PDB_GROUP_TILES[g][s];
        }
        if (!build_group(pdb->divisor, &pdb->groups[g])) {
            mnsp_pdb_destroy(pdb);
            return NULL;
        }
    }
    pdb->ready = 1;
    return pdb;
}

void mnsp_pdb_destroy(mnsp_pdb_t *pdb)
{
    if (pdb == NULL) {
        return;
    }
    for (int g = 0; g < PDB_GROUPS; g++) {
        free(pdb->groups[g].dist);
        pdb->groups[g].dist = NULL;
    }
    free(pdb);
}

int mnsp_pdb_ready(const mnsp_pdb_t *pdb)
{
    return pdb != NULL && pdb->ready;
}

int mnsp_pdb_heuristic(const mnsp_pdb_t *pdb, const int32_t *positions, int grid)
{
    int32_t cell_of_tile[64];
    int total = 0;

    if (!mnsp_pdb_ready(pdb) || positions == NULL || grid != PDB_GRID) {
        return -1;
    }
    for (int i = 0; i < PDB_CELLS; i++) {
        int32_t value = positions[i];
        if (value < -1 || value >= 64) {
            return -1;
        }
        cell_of_tile[value + 1] = i; /* +1 so the blank (-1) lands at 0 */
    }

    for (int g = 0; g < PDB_GROUPS; g++) {
        const pdb_group_t *group = &pdb->groups[g];
        int32_t sym_cell[PDB_TILES];
        uint8_t value;

        for (int s = 0; s < PDB_TILES; s++) {
            sym_cell[s] = cell_of_tile[group->tiles[s] + 1];
        }
        value = group->dist[pdb_rank(sym_cell)];
        if (value == PDB_UNVISITED) {
            return -1; /* impossible pattern: the board itself must be invalid */
        }
        total += value;
    }
    /* Every real tile is covered by exactly one group, so there is no Manhattan
     * residual. That is a property of the 5x5 split, not an approximation. */
    return total;
}
