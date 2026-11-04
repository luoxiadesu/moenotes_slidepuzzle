/* Image loading and resampling. Internal to the library. */
#include "sp_image.h"

#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#include "stb_image.h"

mnsp_result_t sp_image_load(const char *path, sp_image_t *out)
{
    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char *pixels;
    size_t count;

    if (path == NULL || out == NULL) {
        return MNSP_ERR_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof *out);

    pixels = stbi_load(path, &width, &height, &channels, 3);
    if (pixels == NULL) {
        return MNSP_ERR_IMAGE;
    }
    if (width <= 0 || height <= 0) {
        stbi_image_free(pixels);
        return MNSP_ERR_IMAGE;
    }

    count = (size_t)width * (size_t)height * 3u;
    out->rgb = (float *)malloc(count * sizeof(float));
    if (out->rgb == NULL) {
        stbi_image_free(pixels);
        return MNSP_ERR_OUT_OF_MEMORY;
    }
    for (size_t i = 0; i < count; i++) {
        out->rgb[i] = (float)pixels[i] * (1.0f / 255.0f);
    }
    out->width = width;
    out->height = height;
    stbi_image_free(pixels);
    return MNSP_OK;
}

void sp_image_free(sp_image_t *image)
{
    if (image == NULL) {
        return;
    }
    free(image->rgb);
    image->rgb = NULL;
    image->width = 0;
    image->height = 0;
}

mnsp_result_t sp_image_sample_box(const sp_image_t *image, int x, int y, int w, int h,
                                  int out_w, int out_h, float *out)
{
    if (image == NULL || image->rgb == NULL || out == NULL) {
        return MNSP_ERR_INVALID_ARGUMENT;
    }
    if (w <= 0 || h <= 0 || out_w <= 0 || out_h <= 0) {
        return MNSP_ERR_RANGE;
    }
    if (x < 0 || y < 0 || x + w > image->width || y + h > image->height) {
        return MNSP_ERR_RANGE;
    }

    for (int oy = 0; oy < out_h; oy++) {
        int sy0 = y + (int)((long)oy * h / out_h);
        int sy1 = y + (int)((long)(oy + 1) * h / out_h);
        if (sy1 <= sy0) {
            sy1 = sy0 + 1;
        }
        for (int ox = 0; ox < out_w; ox++) {
            int sx0 = x + (int)((long)ox * w / out_w);
            int sx1 = x + (int)((long)(ox + 1) * w / out_w);
            double sum[3] = { 0.0, 0.0, 0.0 };
            long n = 0;
            if (sx1 <= sx0) {
                sx1 = sx0 + 1;
            }
            for (int sy = sy0; sy < sy1; sy++) {
                const float *row = image->rgb + ((size_t)sy * image->width + sx0) * 3u;
                for (int sx = sx0; sx < sx1; sx++) {
                    sum[0] += row[0];
                    sum[1] += row[1];
                    sum[2] += row[2];
                    row += 3;
                    n++;
                }
            }
            float *dst = out + ((size_t)oy * out_w + ox) * 3u;
            dst[0] = (float)(sum[0] / (double)n);
            dst[1] = (float)(sum[1] / (double)n);
            dst[2] = (float)(sum[2] / (double)n);
        }
    }
    return MNSP_OK;
}

mnsp_result_t sp_image_luma(const sp_image_t *image, float **out, int *out_w, int *out_h)
{
    size_t count;

    if (image == NULL || image->rgb == NULL || out == NULL || out_w == NULL || out_h == NULL) {
        return MNSP_ERR_INVALID_ARGUMENT;
    }
    count = (size_t)image->width * (size_t)image->height;
    *out = (float *)malloc(count * sizeof(float));
    if (*out == NULL) {
        return MNSP_ERR_OUT_OF_MEMORY;
    }
    for (size_t i = 0; i < count; i++) {
        const float *px = image->rgb + i * 3u;
        (*out)[i] = 0.299f * px[0] + 0.587f * px[1] + 0.114f * px[2];
    }
    *out_w = image->width;
    *out_h = image->height;
    return MNSP_OK;
}
