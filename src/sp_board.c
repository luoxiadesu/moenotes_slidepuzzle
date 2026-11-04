/* Board detection inside a whole-screen screenshot.
 *
 * The game draws the board as an N x N grid of jacket slices separated by dark
 * gutters, so the most reliable signal is the regular lattice of strong edges rather
 * than overall luma variance (a blurred illustration background also has high
 * variance). We therefore:
 *   1. build mean absolute-gradient profiles along rows and columns,
 *   2. extract peaks (edge positions) with an adaptive threshold,
 *   3. fit an equally spaced run of peaks — that run is the board's grid,
 *   4. fall back to a variance window only when no lattice is found.
 */
#include "sp_board.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SP_MAX_PEAKS 64
#define SP_MAX_GRID_LINES (MNSP_MAX_GRID + 1)

typedef struct peak_list {
    int count;
    int position[SP_MAX_PEAKS];
    double strength[SP_MAX_PEAKS];
} peak_list_t;

/* Collect local maxima of `profile` above `ratio * max`, merging runs of adjacent
 * samples and dropping peaks closer together than `min_gap`. */
static void find_peaks(const double *profile, int length, double ratio, int min_gap,
                       peak_list_t *out)
{
    double peak = 0.0;
    int index;
    int group_start = -1;

    out->count = 0;
    for (index = 0; index < length; index++) {
        if (profile[index] > peak) {
            peak = profile[index];
        }
    }
    if (peak <= 0.0) {
        return;
    }

    for (index = 0; index <= length; index++) {
        int active = index < length && profile[index] > peak * ratio;
        if (active && group_start < 0) {
            group_start = index;
        } else if (!active && group_start >= 0) {
            /* Weighted centre of the group. */
            double weight = 0.0;
            double weighted = 0.0;
            for (int i = group_start; i < index; i++) {
                weight += profile[i];
                weighted += profile[i] * (double)i;
            }
            if (weight > 0.0 && out->count < SP_MAX_PEAKS) {
                int centre = (int)(weighted / weight + 0.5);
                int strength = (int)(profile[centre] * 1000.0);
                if (out->count == 0 || centre - out->position[out->count - 1] >= min_gap) {
                    out->position[out->count] = centre;
                    out->strength[out->count] = strength;
                    out->count++;
                } else if (strength > out->strength[out->count - 1]) {
                    out->position[out->count - 1] = centre;
                    out->strength[out->count - 1] = strength;
                }
            }
            group_start = -1;
        }
    }
}

/* Fit the longest equally spaced run of peaks with at least 4 entries (3 cells).
 * Writes the first line, spacing and line count on success. */
static int fit_lattice(const peak_list_t *peaks, int min_lines, int *out_first,
                       double *out_step, int *out_lines)
{
    int best_lines = 0;
    double best_error = 0.0;
    int best_first = 0;
    double best_step = 0.0;

    for (int start = 0; start < peaks->count; start++) {
        for (int end = start + min_lines - 1; end < peaks->count; end++) {
            int spans = end - start;
            double step = (double)(peaks->position[end] - peaks->position[start]) / (double)spans;
            double error = 0.0;

            if (spans < 3 || step < 24.0) {
                continue;
            }
            /* Every interior line must land on a real peak. */
            for (int k = 1; k < spans; k++) {
                double expected = peaks->position[start] + step * (double)k;
                double nearest = 1e9;
                for (int j = 0; j < peaks->count; j++) {
                    double distance = fabs((double)peaks->position[j] - expected);
                    if (distance < nearest) {
                        nearest = distance;
                    }
                }
                error += nearest;
            }
            error /= (double)spans;
            if (error > step * 0.06) {
                continue;
            }
            if (spans > best_lines || (spans == best_lines && error < best_error)) {
                best_lines = spans;
                best_error = error;
                best_first = peaks->position[start];
                best_step = step;
            }
        }
    }
    if (best_lines + 1 < min_lines) {
        return 0;
    }
    *out_first = best_first;
    *out_step = best_step;
    *out_lines = best_lines + 1;
    return 1;
}

/* Fallback: pick the square window with the highest interior luma spread and the
 * flattest surroundings. Used only when no edge lattice exists. */
static double window_spread(const float *luma, int width, int height, int x, int y, int size)
{
    const int step = 3;
    double sum = 0.0;
    double sum_sq = 0.0;
    long n = 0;

    if (size < 16 || x < 0 || y < 0 || x + size > width || y + size > height) {
        return -1.0;
    }
    for (int sy = y; sy < y + size; sy += step) {
        const float *row = luma + (size_t)sy * width;
        for (int sx = x; sx < x + size; sx += step) {
            double v = row[sx];
            sum += v;
            sum_sq += v * v;
            n++;
        }
    }
    if (n < 32) {
        return -1.0;
    }
    double mean = sum / (double)n;
    double variance = sum_sq / (double)n - mean * mean;
    return variance > 0.0 ? sqrt(variance) : -1.0;
}

static int locate_by_variance(const float *luma, int width, int height, sp_rect_t *out,
                              int expected_grid)
{
    int min_side = (width < height ? width : height) * 20 / 100;
    int max_side = (width < height ? width : height) * 96 / 100;
    int best_size = 0;
    int best_x = 0;
    int best_y = 0;
    double best = -1.0;

    if (min_side < 64) {
        min_side = 64;
    }
    for (int size = min_side; size <= max_side; size = size + size / 8 + 8) {
        int stride = size / 6 > 8 ? size / 6 : 8;
        for (int y = 0; y + size <= height; y += stride) {
            for (int x = 0; x + size <= width; x += stride) {
                double score = window_spread(luma, width, height, x, y, size);
                if (score > best) {
                    best = score;
                    best_size = size;
                    best_x = x;
                    best_y = y;
                }
            }
        }
    }
    if (best_size == 0) {
        return 0;
    }
    /* If the grid is known, snap the side to an integer number of cells. */
    if (expected_grid >= MNSP_MIN_GRID && expected_grid <= MNSP_MAX_GRID) {
        int cell = best_size / expected_grid;
        if (cell > 8) {
            best_size = cell * expected_grid;
            best_x += (best_size - cell * expected_grid) / 2;
        }
    }
    out->x = best_x;
    out->y = best_y;
    out->w = best_size;
    out->h = best_size;
    return 1;
}

mnsp_result_t sp_board_locate(const sp_image_t *image, sp_rect_t *out)
{
    return sp_board_locate_grid(image, 0, out, NULL);
}

mnsp_result_t sp_board_locate_grid(const sp_image_t *image, int expected_grid,
                                   sp_rect_t *out, int *out_detected_grid)
{
    float *luma = NULL;
    double *col_profile = NULL;
    double *row_profile = NULL;
    int width;
    int height;
    peak_list_t col_peaks;
    peak_list_t row_peaks;
    int col_first = 0;
    int row_first = 0;
    double col_step = 0.0;
    double row_step = 0.0;
    int col_lines = 0;
    int row_lines = 0;
    mnsp_result_t result = MNSP_ERR_IMAGE;

    if (image == NULL || image->rgb == NULL || out == NULL) {
        return MNSP_ERR_INVALID_ARGUMENT;
    }
    if (sp_image_luma(image, &luma, &width, &height) != MNSP_OK) {
        return MNSP_ERR_OUT_OF_MEMORY;
    }
    col_profile = (double *)calloc((size_t)width, sizeof(double));
    row_profile = (double *)calloc((size_t)height, sizeof(double));
    if (col_profile == NULL || row_profile == NULL) {
        result = MNSP_ERR_OUT_OF_MEMORY;
        goto done;
    }

    /* Mean absolute horizontal / vertical gradient, one sample per pixel column/row. */
    for (int y = 0; y < height; y++) {
        const float *row = luma + (size_t)y * width;
        for (int x = 0; x + 1 < width; x++) {
            col_profile[x] += fabs((double)row[x + 1] - row[x]);
        }
    }
    for (int x = 0; x < width; x++) {
        for (int y = 0; y + 1 < height; y++) {
            row_profile[y] += fabs((double)luma[(size_t)(y + 1) * width + x] -
                                   luma[(size_t)y * width + x]);
        }
    }
    for (int x = 0; x < width; x++) {
        col_profile[x] /= (double)height;
    }
    for (int y = 0; y < height; y++) {
        row_profile[y] /= (double)width;
    }

    /* Peaks: gutters between tiles are 1-4 px, so merge tightly and keep peaks apart
     * by at least a tenth of the expected cell size. */
    find_peaks(col_profile, width, 0.25, 24, &col_peaks);
    find_peaks(row_profile, height, 0.25, 24, &row_peaks);

    {
        int min_lines = 4; /* 3 cells is the smallest board the game offers */
        if (expected_grid >= MNSP_MIN_GRID && expected_grid <= MNSP_MAX_GRID) {
            min_lines = expected_grid + 1;
        }
        if (!fit_lattice(&col_peaks, min_lines, &col_first, &col_step, &col_lines) ||
            !fit_lattice(&row_peaks, min_lines, &row_first, &row_step, &row_lines)) {
            if (!locate_by_variance(luma, width, height, out, expected_grid)) {
                result = MNSP_ERR_IMAGE;
                goto done;
            }
            result = MNSP_OK;
            goto done;
        }
    }

    out->x = col_first;
    out->y = row_first;
    out->w = (int)(col_step * (double)(col_lines - 1) + 0.5);
    out->h = (int)(row_step * (double)(row_lines - 1) + 0.5);
    if (out->w <= 0 || out->h <= 0) {
        result = MNSP_ERR_IMAGE;
        goto done;
    }
    if (out_detected_grid != NULL) {
        *out_detected_grid = col_lines - 1;
    }
    result = MNSP_OK;

done:
    free(luma);
    free(col_profile);
    free(row_profile);
    return result;
}

mnsp_result_t sp_board_cells(const sp_rect_t *board, int grid, double gap_percent,
                             sp_rect_t *cells)
{
    if (board == NULL || cells == NULL || grid < MNSP_MIN_GRID || grid > MNSP_MAX_GRID) {
        return MNSP_ERR_INVALID_ARGUMENT;
    }
    if (board->w <= 0 || board->h <= 0) {
        return MNSP_ERR_RANGE;
    }
    if (gap_percent < 0.0) {
        gap_percent = 0.0;
    }
    if (gap_percent > 30.0) {
        gap_percent = 30.0;
    }

    for (int row = 0; row < grid; row++) {
        for (int col = 0; col < grid; col++) {
            /* Integer edges derived from the full board extent so rounding never
             * leaves a 1px sliver or drifts across the grid. */
            int x0 = board->x + (int)((long)board->w * col / grid);
            int x1 = board->x + (int)((long)board->w * (col + 1) / grid);
            int y0 = board->y + (int)((long)board->h * row / grid);
            int y1 = board->y + (int)((long)board->h * (row + 1) / grid);
            int inset_x = (int)((double)(x1 - x0) * gap_percent / 100.0);
            int inset_y = (int)((double)(y1 - y0) * gap_percent / 100.0);
            sp_rect_t *cell = &cells[row * grid + col];
            cell->x = x0 + inset_x;
            cell->y = y0 + inset_y;
            cell->w = (x1 - x0) - 2 * inset_x;
            cell->h = (y1 - y0) - 2 * inset_y;
            if (cell->w <= 0 || cell->h <= 0) {
                return MNSP_ERR_RANGE;
            }
        }
    }
    return MNSP_OK;
}

mnsp_result_t sp_board_cells_inset(const sp_rect_t *board, int grid, int inset_px,
                                   sp_rect_t *cells)
{
    if (board == NULL || cells == NULL || grid < MNSP_MIN_GRID || grid > MNSP_MAX_GRID) {
        return MNSP_ERR_INVALID_ARGUMENT;
    }
    if (board->w <= 0 || board->h <= 0) {
        return MNSP_ERR_RANGE;
    }
    if (inset_px < 0) {
        inset_px = 0;
    }

    for (int row = 0; row < grid; row++) {
        for (int col = 0; col < grid; col++) {
            int x0 = board->x + (int)((long)board->w * col / grid) + inset_px;
            int x1 = board->x + (int)((long)board->w * (col + 1) / grid) - inset_px;
            int y0 = board->y + (int)((long)board->h * row / grid) + inset_px;
            int y1 = board->y + (int)((long)board->h * (row + 1) / grid) - inset_px;
            sp_rect_t *cell = &cells[row * grid + col];
            cell->x = x0;
            cell->y = y0;
            cell->w = x1 - x0;
            cell->h = y1 - y0;
            if (cell->w <= 0 || cell->h <= 0) {
                return MNSP_ERR_RANGE;
            }
        }
    }
    return MNSP_OK;
}
