/* Weighted IDA* driven by the 5x5 pattern database.
 *
 * Why IDA* here and not A*
 * ------------------------
 * A* keeps every discovered board in a hash table, which is what bounds mnsp_solve_best
 * at 8M states and ~200 MiB. IDA* keeps only the current path, so its memory is
 * O(solution length) -- a handful of kilobytes -- and the work is bounded by a node
 * count instead. That trades a repeated-search cost for a memory ceiling, which is the
 * right trade for 5x5: the pattern database is already 486 MiB, and doubling that with
 * a closed set is what makes the whole thing unusable on small machines.
 *
 * Move ordering matters more than usual. With the four successors sorted by g + w*h,
 * the measured node counts on real songs drop from tens of millions to tens of
 * thousands on easy boards. Without ordering the same threshold sequence re-explores
 * almost the entire tree every iteration.
 *
 * Weights
 * -------
 * w = 1 is exact but takes 0.8-77 s per board on real songs, and one board (100010)
 * was not solved in 30 billion nodes. w = 1.5 returns paths about 10% longer than
 * optimal but finishes within a second on most boards. w = 2.0 and above return
 * substantially longer paths than mnsp_solve_best already produces, so 1.5 is the
 * only useful non-exact setting. See docs/110 for the full sweep.
 */
#include "moenotes_slidepuzzle.h"
#include "sp_pdb.h"

#include <stdlib.h>
#include <string.h>

#define IDA_MAX_GRID 5
#define IDA_CELLS (IDA_MAX_GRID * IDA_MAX_GRID)
#define IDA_MAX_MOVES 1024

typedef struct ida_ctx {
    const mnsp_pdb_t *pdb;
    int32_t goal[IDA_CELLS];
    int8_t board[IDA_CELLS];
    int pos[IDA_CELLS + 1]; /* pos[0] is the blank, pos[value+1] is a tile */
    int8_t path[IDA_MAX_MOVES];
    double weight;
    uint64_t nodes;
    uint64_t node_limit;
    int aborted;
} ida_ctx_t;

static const int8_t IDA_DX[4] = { 0, 0, -1, 1 };
static const int8_t IDA_DY[4] = { 1, -1, 0, 0 };

static int ida_heuristic(const ida_ctx_t *ctx)
{
    int32_t positions[IDA_CELLS];
    for (int i = 0; i < IDA_CELLS; i++) {
        positions[i] = ctx->board[i];
    }
    return mnsp_pdb_heuristic(ctx->pdb, positions, IDA_MAX_GRID);
}

/* Returns the new threshold, or a negative value when the goal was reached (the
 * negated move count is left in *out_len). */
static int ida_dfs(ida_ctx_t *ctx, int g, int bound, int prev_dir, int depth)
{
    int h = ida_heuristic(ctx);
    int f;
    int bx, by;
    int order[4];
    int keys[4];
    int min_next;

    if (h < 0) {
        ctx->aborted = 1;
        return 1 << 29;
    }
    /* f = g + w*h. The weight is used as-is (no fixed-point scaling): the search only
     * compares f values against the threshold and never needs them to be exact, and
     * the move ordering only needs a consistent ordering within one node. */
    f = g + (int)(ctx->weight * (double)h);
    if (f > bound) {
        return f;
    }
    if (h == 0) {
        return -g;
    }

    ctx->nodes++;
    if (ctx->node_limit != 0 && ctx->nodes > ctx->node_limit) {
        ctx->aborted = 1;
        return 1 << 29;
    }
    /* The path buffer bounds the search depth. A 5x5 solution is at most a few hundred
     * moves, but a weighted search with a large bias can wander; stop rather than
     * writing past the buffer. */
    if (depth + 1 >= IDA_MAX_MOVES) {
        ctx->aborted = 1;
        return 1 << 29;
    }

    bx = ctx->pos[0] % IDA_MAX_GRID;
    by = ctx->pos[0] / IDA_MAX_GRID;

    /* Order the four moves by the resulting f. This is what makes the iteration
     * scheme affordable; see the file comment. */
    for (int d = 0; d < 4; d++) {
        int nx = bx + IDA_DX[d];
        int ny = by + IDA_DY[d];
        order[d] = d;
        keys[d] = 1 << 29;
        if (nx < 0 || ny < 0 || nx >= IDA_MAX_GRID || ny >= IDA_MAX_GRID) {
            continue;
        }
        if ((d ^ 1) == prev_dir) {
            continue; /* undo the move that produced this node */
        }
        {
            int to = ny * IDA_MAX_GRID + nx;
            int bc = ctx->pos[0];
            int val = ctx->board[to];
            int child_h;
            ctx->board[bc] = (int8_t)val;
            ctx->board[to] = -1;
            ctx->pos[val + 1] = bc;
            ctx->pos[0] = to;
            child_h = ida_heuristic(ctx);
            ctx->board[to] = (int8_t)val;
            ctx->board[bc] = -1;
            ctx->pos[val + 1] = to;
            ctx->pos[0] = bc;
            if (child_h < 0) {
                ctx->aborted = 1;
                return 1 << 29;
            }
            keys[d] = g + 1 + (int)(ctx->weight * (double)child_h);
        }
    }
    for (int i = 0; i < 4; i++) {
        for (int j = i + 1; j < 4; j++) {
            if (keys[j] < keys[i]) {
                int t = keys[i]; keys[i] = keys[j]; keys[j] = t;
                t = order[i]; order[i] = order[j]; order[j] = t;
            }
        }
    }

    min_next = 1 << 29;
    for (int k = 0; k < 4; k++) {
        int d = order[k];
        int nx, ny, to, val, t;
        if (keys[k] >= (1 << 29)) {
            continue;
        }
        nx = bx + IDA_DX[d];
        ny = by + IDA_DY[d];
        to = ny * IDA_MAX_GRID + nx;
        val = ctx->board[to];
        {
            int bc = ctx->pos[0];
            ctx->board[bc] = (int8_t)val;
            ctx->board[to] = -1;
            ctx->pos[val + 1] = bc;
            ctx->pos[0] = to;
            ctx->path[depth] = (int8_t)d;
            t = ida_dfs(ctx, g + 1, bound, d, depth + 1);
            if (t < 0) {
                return t;
            }
            ctx->board[to] = (int8_t)val;
            ctx->board[bc] = -1;
            ctx->pos[val + 1] = to;
            ctx->pos[0] = bc;
            if (ctx->aborted) {
                return 1 << 29;
            }
            if (t < min_next) {
                min_next = t;
            }
        }
    }
    return min_next;
}

static int ida_solve(const mnsp_pdb_t *pdb, const int32_t *positions, int grid,
                     mnsp_move_t *out_moves, int capacity, double weight,
                     uint64_t max_nodes)
{
    ida_ctx_t ctx;
    int bound;
    int depth = 0;

    if (pdb == NULL || positions == NULL || out_moves == NULL) {
        return -MNSP_ERR_INVALID_ARGUMENT;
    }
    if (grid != IDA_MAX_GRID) {
        return -MNSP_ERR_RANGE;
    }
    if (capacity < 0) {
        return -MNSP_ERR_INVALID_ARGUMENT;
    }
    if (!(weight >= 1.0)) {
        weight = 1.0; /* also catches NaN */
    }
    if (weight > 100.0) {
        weight = 100.0;
    }
    if (!mnsp_pdb_ready(pdb)) {
        return -MNSP_ERR_INVALID_ARGUMENT;
    }
    if (!mnsp_positions_valid(positions, grid)) {
        return -MNSP_ERR_INVALID_ARGUMENT;
    }

    memset(&ctx, 0, sizeof ctx);
    ctx.pdb = pdb;
    ctx.weight = weight;
    ctx.node_limit = max_nodes;
    mnsp_goal_positions(grid, ctx.goal);
    for (int i = 0; i < IDA_CELLS; i++) {
        ctx.board[i] = (int8_t)positions[i];
        ctx.pos[positions[i] < 0 ? 0 : positions[i] + 1] = i;
    }

    {
        int h0 = ida_heuristic(&ctx);
        if (h0 < 0) {
            return -MNSP_ERR_INVALID_ARGUMENT;
        }
        if (h0 == 0) {
            return 0;
        }
        bound = (int)(weight * (double)h0);
    }

    for (;;) {
        int result;
        ctx.path[0] = 0;
        result = ida_dfs(&ctx, 0, bound, -1, 0);
        if (result < 0) {
            depth = -result;
            break;
        }
        if (ctx.aborted) {
            return -MNSP_ERR_SEARCH_LIMIT;
        }
        if (result >= (1 << 29)) {
            return -MNSP_ERR_UNSOLVABLE;
        }
        bound = result;
    }

    if (depth > capacity) {
        return -MNSP_ERR_RANGE;
    }
    for (int i = 0; i < depth; i++) {
        out_moves[i].dx = IDA_DX[(int)ctx.path[i]];
        out_moves[i].dy = IDA_DY[(int)ctx.path[i]];
    }
    return depth;
}

int mnsp_solve_pdb(const mnsp_pdb_t *pdb, const int32_t *positions, int grid,
                   mnsp_move_t *out_moves, int capacity,
                   double weight, uint64_t max_nodes)
{
    return ida_solve(pdb, positions, grid, out_moves, capacity, weight, max_nodes);
}

int mnsp_solve_optimal(const mnsp_pdb_t *pdb, const int32_t *positions, int grid,
                       mnsp_move_t *out_moves, int capacity, uint64_t max_nodes)
{
    return ida_solve(pdb, positions, grid, out_moves, capacity, 1.0, max_nodes);
}
