/* Board detection inside a whole-screen screenshot. Internal to the library. */
#ifndef SP_BOARD_H
#define SP_BOARD_H

#include <stdint.h>

#include "moenotes_slidepuzzle.h"
#include "sp_image.h"

typedef struct sp_rect {
    int x;
    int y;
    int w;
    int h;
} sp_rect_t;

/* Find the puzzle board (the N x N grid of jacket slices) in a full screenshot.
 *
 * The game draws the board as a dense, roughly square block of high-variance
 * content over a dim background, so we score sliding windows by the standard
 * deviation of their luma and prefer square windows with a dark border. On
 * failure the caller may fall back to sp_board_cells_rect (user gave the rect) or
 * to treating the whole image as the board.
 */
mnsp_result_t sp_board_locate(const sp_image_t *image, sp_rect_t *out);

/* Same, but tells the locator how many cells the board has (0 = unknown) and reports
 * the number of cells the detected edge lattice implies. */
mnsp_result_t sp_board_locate_grid(const sp_image_t *image, int expected_grid,
                                   sp_rect_t *out, int *out_detected_grid);

/* Divide an already-known board rectangle into grid x grid cells, inset by
 * `gap_percent` of a cell to drop the separator between tiles.
 * cells[] receives grid*grid rectangles in *screen* order: index = row * grid + col
 * with row 0 at the top of the image. Callers that need tilePositions order (row 0
 * at the bottom) must flip, which sp_match_board does for them. */
mnsp_result_t sp_board_cells(const sp_rect_t *board, int grid, double gap_percent,
                             sp_rect_t *cells);

/* Same, but insets by an absolute number of pixels instead of a percentage. This is
 * the right unit for the tile gutter, which is a fixed device-pixel width regardless
 * of how many cells the board has. */
mnsp_result_t sp_board_cells_inset(const sp_rect_t *board, int grid, int inset_px,
                                   sp_rect_t *cells);

#endif /* SP_BOARD_H */
