/* Public wrappers for the jacket library. Kept apart from sp_jacket.c so the
 * internal header stays out of the installed include tree. */
#include "moenotes_slidepuzzle.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "sp_jacket.h"

int mnsp_jacket_count(void) { return sp_jacket_builtin_count(); }

int mnsp_jacket_at(int index, int32_t *out_music_id, const char **out_asset_name)
{
    return sp_jacket_builtin_at(index, out_music_id, out_asset_name);
}

const char *mnsp_jacket_asset_name(int32_t music_id)
{
    return sp_jacket_asset_name(music_id);
}

int mnsp_jacket_cache_dir(char *out, size_t capacity) { return sp_cache_dir(out, capacity); }

/* Create every missing directory along `path` (mkdir -p). */
static int make_directories(const char *path)
{
    char buffer[1024];
    size_t length = strlen(path);

    if (length >= sizeof buffer) {
        return 0;
    }
    memcpy(buffer, path, length + 1);
    for (size_t i = 1; i < length; i++) {
        if (buffer[i] != '/') {
            continue;
        }
        buffer[i] = '\0';
        if (mkdir(buffer, 0755) != 0 && errno != EEXIST) {
            return 0;
        }
        buffer[i] = '/';
    }
    if (mkdir(buffer, 0755) != 0 && errno != EEXIST) {
        return 0;
    }
    return 1;
}

mnsp_result_t mnsp_jacket_resolve(int32_t music_id, const mnsp_jacket_options_t *options,
                                  char *out_path, size_t capacity)
{
    mnsp_jacket_options_t config;
    char path[1024];
    char url[1024];
    char directory[1024];
    const char *name;
    struct stat info;

    if (out_path == NULL || capacity == 0) {
        return MNSP_ERR_INVALID_ARGUMENT;
    }
    config.region = options != NULL && options->region != NULL ? options->region : "tw";
    config.locale = options != NULL && options->locale != NULL ? options->locale : "zh-Hant";
    config.base_url = options != NULL ? options->base_url : NULL;
    config.fetcher = options != NULL && options->fetcher != NULL ? options->fetcher : "curl";
    config.allow_fetch = options != NULL ? options->allow_fetch : 1;

    name = sp_jacket_asset_name(music_id);
    if (name == NULL) {
        return MNSP_ERR_RANGE;
    }
    if (!sp_jacket_cache_path(path, sizeof path, config.region, config.locale, music_id)) {
        return MNSP_ERR_RANGE;
    }
    if (stat(path, &info) == 0 && info.st_size > 0) {
        snprintf(out_path, capacity, "%s", path);
        return MNSP_OK;
    }
    if (!config.allow_fetch) {
        return MNSP_ERR_IO;
    }
    if (!sp_jacket_url(url, sizeof url, config.base_url, config.locale, name)) {
        return MNSP_ERR_RANGE;
    }

    /* Create the cache tree down to the locale directory. */
    snprintf(directory, sizeof directory, "%s", path);
    {
        char *slash = strrchr(directory, '/');
        if (slash == NULL) {
            return MNSP_ERR_INTERNAL;
        }
        *slash = '\0';
    }
    if (!make_directories(directory)) {
        return MNSP_ERR_IO;
    }

    {
        mnsp_result_t result = sp_fetch_to_file(config.fetcher, url, path);
        if (result != MNSP_OK) {
            return result;
        }
    }
    snprintf(out_path, capacity, "%s", path);
    return MNSP_OK;
}
