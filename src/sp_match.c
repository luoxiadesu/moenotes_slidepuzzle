/* Template matching and tile-to-cell assignment. */
#include "sp_match.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

mnsp_result_t sp_tiles_from_jacket(const sp_image_t *jacket, int grid, sp_tile_set_t *out)
{
    size_t mosaic_floats;
    int count;

    if (jacket == NULL || jacket->rgb == NULL || out == NULL) {
        return MNSP_ERR_INVALID_ARGUMENT;
    }
    if (grid < MNSP_MIN_GRID || grid > MNSP_MAX_GRID) {
        return MNSP_ERR_RANGE;
    }
    memset(out, 0, sizeof *out);
    /* Every cell of the cover art can appear on the board. The blank hides a *cell*,
     * not a tile id: whichever tile is currently under the blank is still one of the
     * N*N slices, and every other slice stays visible. So the template set has to be
     * the full grid*grid. Confirmed on a real screenshot: a cell matched image
     * (row 0, col 2) with NCC 0.982, which is exactly the slice a skip-one scheme
     * would have dropped. */
    count = grid * grid;
    mosaic_floats = (size_t)count * SP_MOSAIC_SIZE * SP_MOSAIC_SIZE * 3u;
    out->mosaic = (float *)malloc(mosaic_floats * sizeof(float));
    out->flat = (uint8_t *)calloc((size_t)count, sizeof(uint8_t));
    out->tile_variance = (double *)calloc((size_t)count, sizeof(double));
    if (out->mosaic == NULL || out->flat == NULL || out->tile_variance == NULL) {
        sp_tiles_free(out);
        return MNSP_ERR_OUT_OF_MEMORY;
    }
    out->grid = grid;
    out->count = count;

    for (int ty = 0; ty < grid; ty++) {
        for (int tx = 0; tx < grid; tx++) {
            int index = (grid - 1 - ty) * grid + tx;
            /* Tiles are stored in tilePositions order: index j = y * grid + x with y
             * counted from the bottom. Image row `ty` (from the top) is therefore
             * tile row grid - 1 - ty, which is also where TileUVSlicer samples the
             * jacket in-game (correctPos.y is a bottom-up coordinate). */
            int x0 = (int)((long)jacket->width * tx / grid);
            int x1 = (int)((long)jacket->width * (tx + 1) / grid);
            int y0 = (int)((long)jacket->height * ty / grid);
            int y1 = (int)((long)jacket->height * (ty + 1) / grid);
            float *dst = out->mosaic + (size_t)index * SP_MOSAIC_SIZE * SP_MOSAIC_SIZE * 3u;
            mnsp_result_t rc = sp_image_sample_box(jacket, x0, y0, x1 - x0, y1 - y0,
                                                   SP_MOSAIC_SIZE, SP_MOSAIC_SIZE, dst);
            if (rc != MNSP_OK) {
                sp_tiles_free(out);
                return rc;
            }
            /* A slice that is essentially one colour carries no information: its NCC
             * is 0 against everything, so it must never win a match. Real jackets do
             * have blank cells (white margins, flat skies). */
            {
                const size_t tile_floats = (size_t)SP_MOSAIC_SIZE * SP_MOSAIC_SIZE * 3u;
                double sum = 0.0;
                double sum_sq = 0.0;
                for (size_t i = 0; i < tile_floats; i++) {
                    sum += dst[i];
                    sum_sq += (double)dst[i] * dst[i];
                }
                {
                    double mean = sum / (double)tile_floats;
                    double variance = sum_sq / (double)tile_floats - mean * mean;
                    out->tile_variance[index] = variance;
                }
            }
        }
    }
    /* Mark the slices that carry no usable detail. A fixed variance threshold is too
     * brittle (it flipped with a 1 px inset change), so compare against the median:
     * a slice is flat when it is essentially uniform *and* far flatter than a typical
     * slice of this artwork. */
    {
        double *sorted = (double *)malloc((size_t)count * sizeof(double));
        if (sorted != NULL) {
            memcpy(sorted, out->tile_variance, (size_t)count * sizeof(double));
            for (int i = 0; i < count; i++) {
                for (int j = i + 1; j < count; j++) {
                    if (sorted[j] < sorted[i]) {
                        double swap = sorted[i];
                        sorted[i] = sorted[j];
                        sorted[j] = swap;
                    }
                }
            }
            {
                double median = sorted[count / 2];
                double limit = median * 0.05;
                if (limit > 2e-2) {
                    limit = 2e-2;
                }
                for (int i = 0; i < count; i++) {
                    /* "Uniform" measured absolutely, or vanishingly flat relative to
                     * the rest of this artwork. */
                    out->flat[i] = out->tile_variance[i] <= limit ? 1 : 0;
                }
            }
            free(sorted);
        }
    }
    return MNSP_OK;
}

void sp_tiles_free(sp_tile_set_t *set)
{
    if (set == NULL) {
        return;
    }
    free(set->mosaic);
    free(set->flat);
    set->mosaic = NULL;
    set->flat = NULL;
    set->grid = 0;
    set->count = 0;
}

/* Luma mean/variance over a rectangle, subsampled 2:1. */
static int cell_stats_for_match(const sp_image_t *image, const sp_rect_t *rect,
                                double *out_mean, double *out_variance)
{
    double sum = 0.0;
    double sum_sq = 0.0;
    long n = 0;

    for (int y = rect->y; y < rect->y + rect->h; y += 2) {
        const float *row = image->rgb + ((size_t)y * image->width + rect->x) * 3u;
        for (int x = 0; x < rect->w; x += 2) {
            double value = 0.299 * row[0] + 0.587 * row[1] + 0.114 * row[2];
            sum += value;
            sum_sq += value * value;
            n++;
            row += 6;
        }
    }
    if (n < 16) {
        return 0;
    }
    *out_mean = sum / (double)n;
    *out_variance = sum_sq / (double)n - (*out_mean) * (*out_mean);
    return 1;
}

/* Zero-mean normalised cross correlation over the whole mosaic (all channels). */
static float ncc(const float *a, const float *b, size_t n)
{
    double mean_a = 0.0;
    double mean_b = 0.0;
    double num = 0.0;
    double den_a = 0.0;
    double den_b = 0.0;

    for (size_t i = 0; i < n; i++) {
        mean_a += a[i];
        mean_b += b[i];
    }
    mean_a /= (double)n;
    mean_b /= (double)n;
    for (size_t i = 0; i < n; i++) {
        double da = a[i] - mean_a;
        double db = b[i] - mean_b;
        num += da * db;
        den_a += da * da;
        den_b += db * db;
    }
    if (den_a <= 1e-12 || den_b <= 1e-12) {
        return 0.0f;
    }
    return (float)(num / sqrt(den_a * den_b));
}

/* Hungarian algorithm on a size x size cost matrix (minimisation), 1-indexed
 * internally. O(size^3); size is at most 64. */
static void hungarian(const double *cost, int size, int *assignment)
{
    int n = size;
    double *u = (double *)calloc((size_t)n + 1, sizeof(double));
    double *v = (double *)calloc((size_t)n + 1, sizeof(double));
    int *p = (int *)calloc((size_t)n + 1, sizeof(int));
    int *way = (int *)calloc((size_t)n + 1, sizeof(int));
    double *minv = (double *)calloc((size_t)n + 1, sizeof(double));
    unsigned char *used = (unsigned char *)calloc((size_t)n + 1, sizeof(unsigned char));

    if (u == NULL || v == NULL || p == NULL || way == NULL || minv == NULL || used == NULL) {
        for (int i = 0; i < size; i++) {
            assignment[i] = i;
        }
        free(u); free(v); free(p); free(way); free(minv); free(used);
        return;
    }

    for (int i = 1; i <= n; i++) {
        p[0] = i;
        int j0 = 0;
        for (int j = 0; j <= n; j++) {
            minv[j] = 1e18;
            used[j] = 0;
        }
        do {
            used[j0] = 1;
            int i0 = p[j0];
            int j1 = 0;
            double delta = 1e18;
            for (int j = 1; j <= n; j++) {
                if (used[j]) {
                    continue;
                }
                double current = cost[(size_t)(i0 - 1) * size + (j - 1)] - u[i0] - v[j];
                if (current < minv[j]) {
                    minv[j] = current;
                    way[j] = j0;
                }
                if (minv[j] < delta) {
                    delta = minv[j];
                    j1 = j;
                }
            }
            for (int j = 0; j <= n; j++) {
                if (used[j]) {
                    u[p[j]] += delta;
                    v[j] -= delta;
                } else {
                    minv[j] -= delta;
                }
            }
            j0 = j1;
        } while (p[j0] != 0);
        do {
            int j1 = way[j0];
            p[j0] = p[j1];
            j0 = j1;
        } while (j0 != 0);
    }

    for (int j = 1; j <= n; j++) {
        if (p[j] > 0) {
            assignment[p[j] - 1] = j - 1;
        }
    }
    free(u); free(v); free(p); free(way); free(minv); free(used);
}

mnsp_result_t sp_match_board(const sp_image_t *image, const sp_rect_t *cells, int grid,
                             const sp_tile_set_t *tiles, int empty_cell,
                             float min_score, float min_margin, sp_match_result_t *out)
{
    const size_t mosaic_floats = (size_t)SP_MOSAIC_SIZE * SP_MOSAIC_SIZE * 3u;
    int count;
    int tile_count;/* tiles in the jacket set: count - 1 */
    float *mosaic = NULL;
    double *score = NULL;
    double *cost = NULL;
    int *assignment = NULL;

    if (image == NULL || cells == NULL || tiles == NULL || out == NULL) {
        return MNSP_ERR_INVALID_ARGUMENT;
    }
    if (grid < MNSP_MIN_GRID || grid > MNSP_MAX_GRID || tiles->grid != grid) {
        return MNSP_ERR_RANGE;
    }
    if (tiles->count != grid * grid) {
        return MNSP_ERR_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof *out);
    count = grid * grid;
    tile_count = tiles->count;

    /* `empty_cell`, cells[] and every result array share screen order, so the index
     * is used as given. */

    mosaic = (float *)malloc(mosaic_floats * sizeof(float));
    score = (double *)calloc((size_t)count * (size_t)tile_count, sizeof(double));
    cost = (double *)malloc((size_t)count * (size_t)tile_count * sizeof(double));
    assignment = (int *)malloc((size_t)count * sizeof(int));
    out->assignment = (int32_t *)malloc((size_t)count * sizeof(int32_t));
    out->score = (float *)calloc((size_t)count, sizeof(float));
    out->margin = (float *)calloc((size_t)count, sizeof(float));
    out->confident = (uint8_t *)calloc((size_t)count, sizeof(uint8_t));
    if (mosaic == NULL || score == NULL || cost == NULL || assignment == NULL ||
        out->assignment == NULL || out->score == NULL || out->margin == NULL ||
        out->confident == NULL) {
        free(mosaic); free(score); free(cost); free(assignment);
        sp_match_free(out);
        return MNSP_ERR_OUT_OF_MEMORY;
    }

    {
        double *cell_variance = (double *)calloc((size_t)count, sizeof(double));
        double *sorted_variance = (double *)calloc((size_t)count, sizeof(double));
        double flat_limit = 0.0;
        int have_cell_stats = cell_variance != NULL && sorted_variance != NULL;

        if (have_cell_stats) {
            for (int index = 0; index < count; index++) {
                double mean = 0.0;
                double variance = 0.0;
                if (cell_stats_for_match(image, &cells[index], &mean, &variance)) {
                    cell_variance[index] = variance;
                }
                sorted_variance[index] = cell_variance[index];
            }
            for (int i = 0; i < count; i++) {
                for (int j = i + 1; j < count; j++) {
                    if (sorted_variance[j] < sorted_variance[i]) {
                        double swap = sorted_variance[i];
                        sorted_variance[i] = sorted_variance[j];
                        sorted_variance[j] = swap;
                    }
                }
            }
            /* A slice is flat when it is far flatter than a typical slice of this
             * artwork. 5 % of the median separates genuine blank margins (measured
             * 0.002 against a 0.187 median) from detailed slices without being
             * sensitive to a one-pixel change in the inset. */
            flat_limit = sorted_variance[count / 2] * 0.05;
            if (flat_limit > 2e-2) {
                flat_limit = 2e-2;
            }

        }

    for (int screen_cell = 0; screen_cell < count; screen_cell++) {
        const sp_rect_t *rect = &cells[screen_cell];
        /* assignment[] must end up in the same order as the cells (screen order), so
         * no row flip here; the blank slot is identified in screen order too. */
        const int cell = screen_cell;
        mnsp_result_t rc = sp_image_sample_box(image, rect->x, rect->y, rect->w, rect->h,
                                 SP_MOSAIC_SIZE, SP_MOSAIC_SIZE, mosaic);
        if (rc != MNSP_OK) {
            free(mosaic); free(score); free(cost); free(assignment);
            sp_match_free(out);
            return rc;
        }
        {
            double cell_variance = 0.0;
            {
                double sum = 0.0;
                double sum_sq = 0.0;
                for (size_t i = 0; i < mosaic_floats; i++) {
                    sum += mosaic[i];
                    sum_sq += (double)mosaic[i] * mosaic[i];
                }
                double mean = sum / (double)mosaic_floats;
                cell_variance = sum_sq / (double)mosaic_floats - mean * mean;
            }
            (void)0;
            for (int tile = 0; tile < tile_count; tile++) {
                const float *candidate = tiles->mosaic + (size_t)tile * mosaic_floats;
                /* Featureless slices (white margins, flat skies) have no NCC signal
                 * against anything, so they are matched by *kind* instead: a flat cell
                 * only accepts a flat template, and neither side may win against a
                 * detailed counterpart. Without this the solver hands a detailed tile
                 * to the blank-looking cell and the flat tile is left over. */
                {
                    int cell_flat = have_cell_stats && cell_variance <= flat_limit;
                    if (cell_flat && tiles->flat[tile]) {
                        score[(size_t)cell * tile_count + tile] = 1.0;
                    } else if (cell_flat || tiles->flat[tile]) {
                        score[(size_t)cell * tile_count + tile] = -2.0;
                    } else {
                        score[(size_t)cell * tile_count + tile] =
                            (double)ncc(mosaic, candidate, mosaic_floats);
                    }
                }
            }
        }
        {
            double best = -2.0;
            double second = -2.0;
            for (int tile = 0; tile < tile_count; tile++) {
                double value = score[(size_t)cell * tile_count + tile];
                if (value > best) {
                    second = best;
                    best = value;
                } else if (value > second) {
                    second = value;
                }
            }
            out->score[cell] = (float)best;
            out->margin[cell] = (float)(best - second);
        }
    }

    if (empty_cell >= 0 && empty_cell < count) {
        out->assignment[empty_cell] = -1;
        out->score[empty_cell] = 0.0f;
        out->margin[empty_cell] = 0.0f;
        out->confident[empty_cell] = 1;
    }

    /* Assignment: (count-1) live cells against `count` tiles. The one tile hidden
     * under the blank has no cell, so pad the matrix with a dummy row whose cost is
     * the same constant everywhere; whichever column it takes is simply the unused
     * tile. This keeps the matrix square, which the Hungarian solver requires. */
    {
        const double dummy_cost = 1000.0;
        int row = 0;
        for (int cell = 0; cell < count; cell++) {
            if (cell == empty_cell) {
                continue;
            }
            for (int tile = 0; tile < tile_count; tile++) {
                cost[(size_t)row * tile_count + tile] = -score[(size_t)cell * tile_count + tile];
            }
            row++;
        }
        for (int tile = 0; tile < tile_count; tile++) {
            cost[(size_t)row * tile_count + tile] = dummy_cost;
        }
    }
    hungarian(cost, count, assignment);
    {
        int row = 0;
        for (int cell = 0; cell < count; cell++) {
            if (cell == empty_cell) {
                continue;
            }
            out->assignment[cell] = assignment[row++];
        }
    }

    free(cell_variance);
    free(sorted_variance);
    }

    for (int cell = 0; cell < count; cell++) {
        if (cell == empty_cell) {
            continue;
        }
        out->total_score += out->score[cell];
        if (out->assignment[cell] < 0 || out->assignment[cell] >= tile_count) {
            out->confident[cell] = 0;
        } else if (tiles->flat[out->assignment[cell]]) {
            /* The assigned slice is featureless; the match is arbitrary by nature. */
            out->confident[cell] = 0;
        } else {
            out->confident[cell] =
                (uint8_t)(out->score[cell] >= min_score && out->margin[cell] >= min_margin);
        }
    }

    free(mosaic);
    free(score);
    free(cost);
    free(assignment);
    return MNSP_OK;
}

void sp_match_free(sp_match_result_t *result)
{
    if (result == NULL) {
        return;
    }
    free(result->assignment);
    free(result->score);
    free(result->margin);
    free(result->confident);
    result->assignment = NULL;
    result->score = NULL;
    result->margin = NULL;
    result->confident = NULL;
}

mnsp_result_t sp_match_positions(const sp_match_result_t *result, int grid, int empty_cell,
                                 int32_t *out_positions)
{
    int count;
    unsigned char seen[64];

    if (result == NULL || out_positions == NULL || grid < MNSP_MIN_GRID || grid > MNSP_MAX_GRID) {
        return MNSP_ERR_INVALID_ARGUMENT;
    }
    count = grid * grid;
    memset(seen, 0, sizeof seen);
    /* Everything in this library uses screen order (row 0 at the top): cells[],
     * assignment[], mnsp_goal_positions output and mnsp_solve. Nothing is re-indexed. */
    for (int screen_cell = 0; screen_cell < count; screen_cell++) {
        const int cell = screen_cell;
        int32_t tile = result->assignment[screen_cell];
        if (screen_cell == empty_cell) {
            out_positions[cell] = -1;
            continue;
        }
        if (tile < 0 || tile >= count || seen[tile]) {
            return MNSP_ERR_INVALID_ARGUMENT;
        }
        seen[tile] = 1;
        out_positions[cell] = tile;
    }
    return MNSP_OK;
}
