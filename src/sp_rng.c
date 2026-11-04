/* .NET System.Random(int) port. See include/moenotes_slidepuzzle.h. */
#include "moenotes_slidepuzzle.h"

#include <limits.h>

#define MBIG 2147483647
#define MSEED 161803398

/* Two's complement 32-bit wrap, matching the C# int arithmetic. */
static int32_t wrap32(int32_t value) { return value; }

static int32_t wrap_add(int32_t a, int32_t b)
{
    return (int32_t)((uint32_t)a + (uint32_t)b);
}

static int32_t wrap_sub(int32_t a, int32_t b)
{
    return (int32_t)((uint32_t)a - (uint32_t)b);
}

void mnsp_random_init(mnsp_random_t *rng, int32_t seed)
{
    int32_t subtraction;
    int32_t mj;
    int32_t mk;
    int i;
    int k;
    int ii;

    if (rng == NULL) {
        return;
    }

    /* C#: subtraction = Seed == int.MinValue ? int.MaxValue : Math.Abs(Seed) */
    subtraction = seed == INT32_MIN ? INT32_MAX : (seed < 0 ? wrap32(-seed) : seed);

    mj = wrap_sub(MSEED, subtraction);
    for (i = 0; i < 56; i++) {
        rng->state[i] = 0;
    }
    rng->state[55] = mj;

    mk = 1;
    for (i = 1; i < 55; i++) {
        ii = (21 * i) % 55;
        rng->state[ii] = mk;
        mk = wrap_sub(mj, mk);
        if (mk < 0) {
            mk = wrap_add(mk, MBIG);
        }
        mj = rng->state[ii];
    }
    for (k = 1; k < 5; k++) {
        for (i = 1; i < 56; i++) {
            int32_t value = wrap_sub(rng->state[i], rng->state[1 + (i + 30) % 55]);
            if (value < 0) {
                value = wrap_add(value, MBIG);
            }
            rng->state[i] = value;
        }
    }

    rng->inext = 0;
    rng->inextp = 21;
}

static int32_t internal_sample(mnsp_random_t *rng)
{
    int32_t value;

    rng->inext += 1;
    if (rng->inext >= 56) {
        rng->inext = 1;
    }
    rng->inextp += 1;
    if (rng->inextp >= 56) {
        rng->inextp = 1;
    }

    value = wrap_sub(rng->state[rng->inext], rng->state[rng->inextp]);
    if (value == MBIG) {
        value -= 1;
    }
    if (value < 0) {
        value = wrap_add(value, MBIG);
    }
    rng->state[rng->inext] = value;
    return value;
}

int32_t mnsp_random_next(mnsp_random_t *rng, int32_t min_value, int32_t max_value)
{
    double sample;
    int32_t range;

    if (rng == NULL || max_value <= min_value) {
        return min_value;
    }

    /* Sample() is InternalSample() * (1.0 / MBIG). */
    sample = (double)internal_sample(rng) * (1.0 / (double)MBIG);
    range = (int32_t)((uint32_t)max_value - (uint32_t)min_value);
    return min_value + (int32_t)(sample * (double)range);
}

const char *mnsp_result_string(mnsp_result_t result)
{
    switch (result) {
    case MNSP_OK: return "ok";
    case MNSP_ERR_INVALID_ARGUMENT: return "invalid argument";
    case MNSP_ERR_OUT_OF_MEMORY: return "out of memory";
    case MNSP_ERR_IO: return "io error";
    case MNSP_ERR_IMAGE: return "image error";
    case MNSP_ERR_RANGE: return "out of range";
    case MNSP_ERR_UNSOLVABLE: return "unsolvable";
    case MNSP_ERR_SEARCH_LIMIT: return "search limit reached";
    case MNSP_ERR_INTERNAL: return "internal error";
    default: return "internal error";
    }
}
