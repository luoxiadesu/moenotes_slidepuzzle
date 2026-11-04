/* Jacket library: name lookup, cache paths and fetching. */
#include "sp_jacket.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "sp_jackets_builtin.h"

/* Default public mirror. Overridable through the option struct / env in the CLI. */
#define SP_DEFAULT_ASSET_BASE "https://assets.bdon.moe"

const char *sp_jacket_asset_name(int32_t music_id)
{
    for (int i = 0; i < SP_BUILTIN_JACKET_COUNT; i++) {
        if (SP_BUILTIN_JACKETS[i].music_id == music_id) {
            return SP_BUILTIN_JACKETS[i].asset_name;
        }
    }
    return NULL;
}

int sp_jacket_builtin_count(void) { return SP_BUILTIN_JACKET_COUNT; }

int sp_jacket_builtin_at(int index, int32_t *out_music_id, const char **out_asset_name)
{
    if (index < 0 || index >= SP_BUILTIN_JACKET_COUNT) {
        return 0;
    }
    if (out_music_id != NULL) {
        *out_music_id = SP_BUILTIN_JACKETS[index].music_id;
    }
    if (out_asset_name != NULL) {
        *out_asset_name = SP_BUILTIN_JACKETS[index].asset_name;
    }
    return 1;
}

static int append(char *out, size_t capacity, size_t *used, const char *text)
{
    size_t length = strlen(text);
    if (*used + length + 1 > capacity) {
        return 0;
    }
    memcpy(out + *used, text, length + 1);
    *used += length;
    return 1;
}

int sp_cache_dir(char *out, size_t capacity)
{
    const char *explicit_dir;
    const char *xdg;
    const char *home;
    size_t used = 0;

    if (out == NULL || capacity == 0) {
        return 0;
    }
    explicit_dir = getenv("MOENOTES_SLIDEPUZZLE_CACHE");
    if (explicit_dir != NULL && *explicit_dir != '\0') {
        out[0] = '\0';
        return append(out, capacity, &used, explicit_dir);
    }
    xdg = getenv("XDG_CACHE_HOME");
    if (xdg != NULL && *xdg != '\0') {
        out[0] = '\0';
        if (!append(out, capacity, &used, xdg)) {
            return 0;
        }
        return append(out, capacity, &used, "/moenotes-slidepuzzle");
    }
    home = getenv("HOME");
    if (home != NULL && *home != '\0') {
        out[0] = '\0';
        if (!append(out, capacity, &used, home)) {
            return 0;
        }
        return append(out, capacity, &used, "/.cache/moenotes-slidepuzzle");
    }
    out[0] = '\0';
    return append(out, capacity, &used, ".cache/moenotes-slidepuzzle");
}

int sp_jacket_cache_path(char *out, size_t capacity, const char *region,
                         const char *locale, int32_t music_id)
{
    char base[1024];
    const char *name;
    size_t used = 0;

    if (out == NULL || region == NULL || locale == NULL) {
        return 0;
    }
    name = sp_jacket_asset_name(music_id);
    if (name == NULL) {
        return 0;
    }
    if (!sp_cache_dir(base, sizeof base)) {
        return 0;
    }
    out[0] = '\0';
    if (!append(out, capacity, &used, base) ||
        !append(out, capacity, &used, "/jackets/") ||
        !append(out, capacity, &used, region) ||
        !append(out, capacity, &used, "/") ||
        !append(out, capacity, &used, locale) ||
        !append(out, capacity, &used, "/")) {
        return 0;
    }
    /* File name carries the song id so the directory stays greppable. */
    {
        char file[128];
        snprintf(file, sizeof file, "%d_%s.png", (int)music_id, name);
        if (!append(out, capacity, &used, file)) {
            return 0;
        }
    }
    return 1;
}

int sp_jacket_url(char *out, size_t capacity, const char *base_url, const char *locale,
                  const char *asset_name)
{
    size_t used = 0;

    if (out == NULL || locale == NULL || asset_name == NULL) {
        return 0;
    }
    if (base_url == NULL || *base_url == '\0') {
        base_url = SP_DEFAULT_ASSET_BASE;
    }
    out[0] = '\0';
    /* Trim a trailing slash so the join never doubles it. */
    {
        char trimmed[512];
        size_t length = strlen(base_url);
        if (length >= sizeof trimmed) {
            return 0;
        }
        memcpy(trimmed, base_url, length + 1);
        while (length > 0 && trimmed[length - 1] == '/') {
            trimmed[--length] = '\0';
        }
        if (!append(out, capacity, &used, trimmed)) {
            return 0;
        }
    }
    if (!append(out, capacity, &used, "/") ||
        !append(out, capacity, &used, locale) ||
        !append(out, capacity, &used, "/Image/Jacket/") ||
        !append(out, capacity, &used, asset_name) ||
        !append(out, capacity, &used, "/") ||
        !append(out, capacity, &used, asset_name) ||
        !append(out, capacity, &used, ".png")) {
        return 0;
    }
    return 1;
}

mnsp_result_t sp_fetch_to_file(const char *fetcher, const char *url, const char *destination)
{
    pid_t pid;
    int status = 0;
    FILE *probe;
    long size;

    if (fetcher == NULL || url == NULL || destination == NULL) {
        return MNSP_ERR_INVALID_ARGUMENT;
    }

    pid = fork();
    if (pid < 0) {
        return MNSP_ERR_IO;
    }
    if (pid == 0) {
        /* Child: exec the helper with stderr suppressed so a failing mirror does not
         * spray the caller's terminal; the exit status carries the verdict. */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, 2);
            close(devnull);
        }
        if (strcmp(fetcher, "curl") == 0) {
            execlp(fetcher, fetcher, "-fsSL", "--max-time", "30", "-o", destination, url,
                   (char *)NULL);
        } else {
            execlp(fetcher, fetcher, url, destination, (char *)NULL);
        }
        _exit(127);
    }
    if (waitpid(pid, &status, 0) < 0) {
        return MNSP_ERR_IO;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        remove(destination);
        return MNSP_ERR_IO;
    }

    probe = fopen(destination, "rb");
    if (probe == NULL) {
        return MNSP_ERR_IO;
    }
    if (fseek(probe, 0, SEEK_END) != 0) {
        fclose(probe);
        remove(destination);
        return MNSP_ERR_IO;
    }
    size = ftell(probe);
    fclose(probe);
    if (size <= 0) {
        remove(destination);
        return MNSP_ERR_IO;
    }
    return MNSP_OK;
}
