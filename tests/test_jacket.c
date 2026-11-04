/* Jacket library regression: table lookup, URL layout, cache paths.
 * No network access: resolving is exercised against a scratch cache directory. */
#include "moenotes_slidepuzzle.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_table(void)
{
    int32_t music_id = 0;
    const char *name = NULL;

    assert(mnsp_jacket_count() >= 84);
    assert(mnsp_jacket_at(0, &music_id, &name));
    assert(music_id > 0 && name != NULL);
    assert(strncmp(name, "jkt_", 4) == 0);

    assert(mnsp_jacket_asset_name(100001) != NULL);
    assert(strcmp(mnsp_jacket_asset_name(100001), "jkt_001_100001") == 0);
    assert(mnsp_jacket_asset_name(999999) == NULL);
    assert(!mnsp_jacket_at(-1, NULL, NULL));
    assert(!mnsp_jacket_at(mnsp_jacket_count(), NULL, NULL));

    /* Every built-in id maps to a distinct name. */
    for (int i = 0; i < mnsp_jacket_count(); i++) {
        int32_t other = 0;
        const char *other_name = NULL;
        assert(mnsp_jacket_at(i, &other, &other_name));
        for (int j = i + 1; j < mnsp_jacket_count(); j++) {
            int32_t candidate = 0;
            const char *candidate_name = NULL;
            assert(mnsp_jacket_at(j, &candidate, &candidate_name));
            assert(other != candidate);
            assert(strcmp(other_name, candidate_name) != 0);
        }
    }
}

static void test_cache_dir(void)
{
    char directory[1024];

    assert(mnsp_jacket_cache_dir(directory, sizeof directory));
    assert(directory[0] != '\0');
    /* The directory is either the explicit override or a *.cache/moenotes-slidepuzzle
     * default, so only assert that a path came back and that it is absolute or
     * explicitly relative. */
    assert(strchr(directory, '/') != NULL || strchr(directory, '\\') != NULL);
}

static void test_resolve_cache_only(void)
{
    char path[1024];
    mnsp_jacket_options_t options;

    options.region = "tw";
    options.locale = "zh-Hant";
    options.base_url = NULL;
    options.fetcher = NULL;
    options.allow_fetch = 0; /* cache only: must never touch the network */

    /* Unknown id is rejected before any I/O. */
    assert(mnsp_jacket_resolve(999999, &options, path, sizeof path) == MNSP_ERR_RANGE);

    /* A known id with no cached file reports IO, not a crash. */
    {
        mnsp_result_t result = mnsp_jacket_resolve(100001, &options, path, sizeof path);
        assert(result == MNSP_OK || result == MNSP_ERR_IO);
    }
}

int main(void)
{
    test_table();
    test_cache_dir();
    test_resolve_cache_only();
    puts("test_jacket: ok");
    return 0;
}
