/* Image loading, cropping and resampling. Internal to the library. */
#ifndef SP_IMAGE_H
#define SP_IMAGE_H

#include <stdint.h>

#include "moenotes_slidepuzzle.h"

typedef struct sp_image {
    int width;
    int height;
    float *rgb; /* width * height * 3, interleaved, nominally 0..1 */
} sp_image_t;

/* Load a PNG/JPEG/etc. via stb_image and convert to float RGB. */
mnsp_result_t sp_image_load(const char *path, sp_image_t *out);
void sp_image_free(sp_image_t *image);

/* Average-box resample of a rectangle into out_w * out_h RGB floats. */
mnsp_result_t sp_image_sample_box(const sp_image_t *image, int x, int y, int w, int h,
                                  int out_w, int out_h, float *out);

/* Luma copy of the whole image (caller frees). */
mnsp_result_t sp_image_luma(const sp_image_t *image, float **out, int *out_w, int *out_h);

#endif /* SP_IMAGE_H */
