/* Template matching and tile assignment. Internal to the library. */
#ifndef SP_MATCH_H
#define SP_MATCH_H

#include <stdint.h>

#include "moenotes_slidepuzzle.h"
#include "sp_board.h"
#include "sp_image.h"

/* Side of the normalised mosaic every tile is resampled to before comparing.
 * 96 keeps full accuracy in tests while staying cheap (a 180px tile squares to
 * 96x96 across ~27k samples). */
#define SP_MOSAIC_SIZE 96

typedef struct sp_tile_set {
    int grid;
    int count;              /* grid * grid */
    float *mosaic;          /* count * SP_MOSAIC_SIZE^2 * 3 */
    uint8_t *flat;          /* count: 1 when the slice is uniform (no features) */
    double *tile_variance;  /* count: luma variance of each slice */
} sp_tile_set_t;

/* Precompute mosaics for the grid*grid slices of a jacket image. */
mnsp_result_t sp_tiles_from_jacket(const sp_image_t *jacket, int grid, sp_tile_set_t *out);
void sp_tiles_free(sp_tile_set_t *set);

typedef struct sp_match_result {
    int32_t *assignment;    /* grid*grid: mosaic index chosen for each cell */
    float *score;           /* grid*grid: NCC of the chosen candidate */
    float *margin;          /* grid*grid: score difference to the runner-up */
    uint8_t *confident;     /* grid*grid: 1 when the cell may be trusted */
    float total_score;
} sp_match_result_t;

/* Recover the board permutation from a screenshot.
 *
 * cells[] and `empty_cell` use *screen* order (row 0 at the top), matching
 * sp_board_cells. Both assignment[] and the resulting tilePositions are expressed in
 * tilePositions order (row 0 at the bottom), so screen cell `r` maps to output index
 * (grid - 1 - r) * grid + c.
 *
 * A cell whose best score is below `min_score` or whose margin is below
 * `min_margin` is marked not confident instead of being guessed.
 */
mnsp_result_t sp_match_board(const sp_image_t *image, const sp_rect_t *cells, int grid,
                             const sp_tile_set_t *tiles, int empty_cell,
                             float min_score, float min_margin, sp_match_result_t *out);

void sp_match_free(sp_match_result_t *result);

/* Turn the assignment into tilePositions (row-major, y grows up) with -1 for the
 * empty cell. Fails when the assignment is not a permutation. */
mnsp_result_t sp_match_positions(const sp_match_result_t *result, int grid, int empty_cell,
                                 int32_t *out_positions);

#endif /* SP_MATCH_H */
