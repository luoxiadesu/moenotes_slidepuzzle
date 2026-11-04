/* Random port regression: compares against dotnet's own System.Random output.
 * Fixture: tests/fixtures/dotnet-random-reference.txt, one line per seed:
 *   <seed>:<v0>,<v1>,...   where vi = new Random(seed).Next(0, 2 + i % 3)
 */
#include "moenotes_slidepuzzle.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef MNSP_FIXTURE_DIR
#define MNSP_FIXTURE_DIR "tests/fixtures"
#endif

static void test_seed_sequence(void)
{
    char path[512];
    FILE *file;
    char line[2048];
    int lines = 0;

    snprintf(path, sizeof path, "%s/dotnet-random-reference.txt", MNSP_FIXTURE_DIR);
    file = fopen(path, "r");
    assert(file != NULL);

    while (fgets(line, (int)sizeof line, file) != NULL) {
        char *colon = strchr(line, ':');
        char *cursor;
        long seed;
        int index = 0;
        mnsp_random_t rng;

        if (colon == NULL) {
            continue;
        }
        *colon = '\0';
        seed = strtol(line, NULL, 10);
        mnsp_random_init(&rng, (int32_t)seed);
        cursor = colon + 1;
        while (*cursor != '\0' && *cursor != '\n') {
            long expected = strtol(cursor, &cursor, 10);
            int32_t actual = mnsp_random_next(&rng, 0, 2 + index % 3);
            if (actual != (int32_t)expected) {
                fprintf(stderr, "seed %ld draw %d: got %d want %ld\n", seed, index, actual, expected);
                abort();
            }
            index++;
            if (*cursor == ',') {
                cursor++;
            }
        }
        assert(index >= 40);
        lines++;
    }
    fclose(file);
    assert(lines >= 8);
}

static void test_seed_extremes_are_stable(void)
{
    /* Guards the 32-bit wraparound: these seeds overflow the state table. */
    mnsp_random_t a;
    mnsp_random_t b;
    mnsp_random_init(&a, INT32_MAX);
    mnsp_random_init(&b, INT32_MIN);
    assert(a.state[55] == b.state[55]);
    for (int i = 0; i < 20; i++) {
        assert(mnsp_random_next(&a, 0, 4) == mnsp_random_next(&b, 0, 4));
    }
}

int main(void)
{
    test_seed_sequence();
    test_seed_extremes_are_stable();
    puts("test_rng: ok");
    return 0;
}
