/* Screenshot -> tilePositions pipeline. Ties the image, board and match modules
 * together behind the public API declared in moenotes_slidepuzzle.h. */
#include "moenotes_slidepuzzle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sp_board.h"
#include "sp_image.h"
#include "sp_match.h"

/* Mean and variance of luma over a rectangle, subsampled 2:1. */
static int cell_stats(const sp_image_t *image, const sp_rect_t *rect,
                      double *out_mean, double *out_variance)
{
    double sum = 0.0;
    double sum_sq = 0.0;
    long n = 0;

    for (int y = rect->y; y < rect->y + rect->h; y += 2) {
        const float *row = image->rgb + ((size_t)y * image->width + rect->x) * 3u;
        for (int x = 0; x < rect->w; x += 2) {
            double v = 0.299 * row[0] + 0.587 * row[1] + 0.114 * row[2];
            sum += v;
            sum_sq += v * v;
            n++;
            row += 6; /* two pixels, three channels */
        }
    }
    if (n < 16) {
        return 0;
    }
    *out_mean = sum / (double)n;
    *out_variance = sum_sq / (double)n - (*out_mean) * (*out_mean);
    return 1;
}

/* Find the empty slot. Two signals separate it from a real tile:
 *   - a jacket slice that happens to be flat is *bright* (white margin, flat sky),
 *   - the empty slot shows the board's dark backing plate, so it is flat AND dark.
 * Verified on a device capture: real blank (std 0.002, mean 0.12) versus a pure
 * white slice (std 0.000, mean 1.00). */
static int detect_blank(const sp_image_t *image, const sp_rect_t *cells, int grid,
                        double *out_variance)
{
    int best = -1;
    double lowest = 0.0;

    for (int index = 0; index < grid * grid; index++) {
        double mean = 0.0;
        double variance = 0.0;
        if (!cell_stats(image, &cells[index], &mean, &variance)) {
            continue;
        }
        if (variance > 0.0025 || mean > 0.45) {
            continue;
        }
        if (best < 0 || variance < lowest) {
            lowest = variance;
            best = index;
        }
    }
    if (out_variance != NULL) {
        *out_variance = lowest;
    }
    return best;
}

mnsp_result_t mnsp_read_screenshot(const char *screenshot_path, const char *jacket_path,
                                   const mnsp_read_options_t *options,
                                   int32_t *out_positions, uint8_t *out_confident,
                                   float *out_total_score)
{
    mnsp_read_options_t config;
    sp_image_t screen;
    sp_image_t jacket;
    sp_rect_t board;
    sp_rect_t cells[64];
    sp_tile_set_t tiles;
    sp_match_result_t match;
    int grid;
    int blank_screen;
    mnsp_result_t result;

    if (screenshot_path == NULL || jacket_path == NULL || out_positions == NULL) {
        return MNSP_ERR_INVALID_ARGUMENT;
    }
    config.grid = options != NULL ? options->grid : 0;
    config.min_score = options != NULL ? options->min_score : 0.30f;
    config.min_margin = options != NULL ? options->min_margin : 0.005f;
    config.cell_gap_percent = options != NULL ? options->cell_gap_percent : 0.0;

    if (config.grid != 0 && (config.grid < MNSP_MIN_GRID || config.grid > MNSP_MAX_GRID)) {
        return MNSP_ERR_RANGE;
    }
    if (config.min_margin < 0.0f) {
        config.min_margin = 0.0f;
    }
    if (config.cell_gap_percent < 0.0) {
        config.cell_gap_percent = 0.0;
    }
    if (config.cell_gap_percent > 20.0) {
        config.cell_gap_percent = 20.0;
    }
    if (config.grid == 0) {
        /* Board detection alone cannot infer N; the caller must state it. */
        return MNSP_ERR_INVALID_ARGUMENT;
    }
    grid = config.grid;

    memset(&screen, 0, sizeof screen);
    memset(&jacket, 0, sizeof jacket);
    memset(&tiles, 0, sizeof tiles);
    memset(&match, 0, sizeof match);

    result = sp_image_load(screenshot_path, &screen);
    if (result != MNSP_OK) {
        return result;
    }
    result = sp_board_locate_grid(&screen, grid, &board, NULL);
    if (result != MNSP_OK) {
        sp_image_free(&screen);
        return result;
    }
    /* The board lattice includes the dark gutter between tiles. It is a fixed number
     * of device pixels (6-8 px on every capture measured), so inset by pixels rather
     * than by a percentage: a percentage would over-trim large grids and under-trim
     * small ones. 8 px clears the gutter plus its anti-aliased fringe. */
    {
        int inset_px = (int)(config.cell_gap_percent * (double)(board.w / grid) / 100.0);
        if (config.cell_gap_percent <= 0.0) {
            inset_px = 8;
        }
        if (inset_px < 0) {
            inset_px = 0;
        }
        if (inset_px * 4 > (board.w / grid)) {
            inset_px = (board.w / grid) / 4;
        }
        if (getenv("SP_DEBUG_GAP")) {
            fprintf(stderr, "board w=%d grid=%d pitch=%.1f inset=%dpx\n",
                    board.w, grid, (double)board.w / grid, inset_px);
        }
        result = sp_board_cells_inset(&board, grid, inset_px, cells);
    }
    if (result != MNSP_OK) {
        sp_image_free(&screen);
        return result;
    }
    blank_screen = detect_blank(&screen, cells, grid, NULL);

    result = sp_image_load(jacket_path, &jacket);
    if (result != MNSP_OK) {
        sp_image_free(&screen);
        return result;
    }
    result = sp_tiles_from_jacket(&jacket, grid, &tiles);
    if (result != MNSP_OK) {
        sp_image_free(&screen);
        sp_image_free(&jacket);
        return result;
    }

    result = sp_match_board(&screen, cells, grid, &tiles, blank_screen,
                            config.min_score, config.min_margin, &match);
    if (result == MNSP_OK) {
        result = sp_match_positions(&match, grid, blank_screen, out_positions);
        if (result == MNSP_OK && out_confident != NULL) {
            memcpy(out_confident, match.confident, (size_t)(grid * grid));
        }
        if (result == MNSP_OK && out_total_score != NULL) {
            *out_total_score = match.total_score;
        }
    }

    sp_match_free(&match);
    sp_tiles_free(&tiles);
    sp_image_free(&screen);
    sp_image_free(&jacket);
    return result;
}

mnsp_result_t mnsp_find_blank_cell(const char *screenshot_path, int grid, int *out_cell)
{
    sp_image_t screen;
    sp_rect_t board;
    sp_rect_t cells[64];
    int blank;
    mnsp_result_t result;

    if (screenshot_path == NULL || out_cell == NULL || grid < MNSP_MIN_GRID ||
        grid > MNSP_MAX_GRID) {
        return MNSP_ERR_INVALID_ARGUMENT;
    }
    result = sp_image_load(screenshot_path, &screen);
    if (result != MNSP_OK) {
        return result;
    }
    result = sp_board_locate_grid(&screen, grid, &board, NULL);
    if (result == MNSP_OK) {
        result = sp_board_cells(&board, grid, 0.0, cells);
    }
    if (result == MNSP_OK) {
        blank = detect_blank(&screen, cells, grid, NULL);
        if (blank < 0) {
            result = MNSP_ERR_IMAGE;
        } else {
            /* detect_blank indexes cells in screen order; report tilePositions order. */
            *out_cell = (grid - 1 - blank / grid) * grid + blank % grid;
        }
    }
    sp_image_free(&screen);
    return result;
}
