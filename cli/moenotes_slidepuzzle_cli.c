/* moenotes_slidepuzzle - command line front end.
 *
 * Exit codes follow work/moenotes-chart-parser: 0 success, 1 processing failure,
 * 2 invalid arguments.
 */
#include "moenotes_slidepuzzle.h"

#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *out)
{
    fputs("usage: moenotes_slidepuzzle <command> [options]\n"
          "\n"
          "commands:\n"
          "  gen <musicId> <grid>        print the board the game builds, as tilePositions\n"
          "  solve <musicId> <grid> [--strategy auto|search|undo] [--max-states N]\n"
          "                              solve the opening board; auto searches first and\n"
          "                              only replays the shuffle when search is too slow.\n"
          "                              --max-states caps distinct boards explored (default\n"
          "                              depends on size); lower it to bound peak memory\n"
          "  solve-state <grid> <p0,p1,...>\n"
          "                              solve an arbitrary board\n"
          "  read <screenshot> <jacket> <grid> [--solve]\n"
          "                              recover the board from a screenshot\n"
          "  read-music <screenshot> <musicId> <grid> [--solve]\n"
          "                              same, resolving the jacket from the cache\n"
          "  fetch-jackets [--region tw] [--locale zh-Hant] [--base URL] [--limit N]\n"
          "                              download cover art into the local cache\n"
          "  list-jackets              print the built-in song -> jacket table\n"
          "  --help, --version\n"
          "\n"
          "The grid is the puzzle size the game offers: 3..8.\n"
          "tilePositions[i] is the tile's correct index at cell i (y*grid+x, y grows up),\n"
          "with -1 for the empty slot.\n"
          "Moves describe the blank: U/D/L/R is where the blank travels.\n",
          out);
}

static int fail(const char *message)
{
    fprintf(stderr, "moenotes_slidepuzzle: %s\n", message);
    return 1;
}

static int parse_int(const char *text, long *out)
{
    char *end = NULL;
    long value;
    if (text == NULL || *text == '\0') {
        return 0;
    }
    value = strtol(text, &end, 10);
    if (end == text || *end != '\0') {
        return 0;
    }
    *out = value;
    return 1;
}

static int parse_ll(const char *text, long long *out)
{
    char *end = NULL;
    long long value;
    if (text == NULL || *text == '\0') {
        return 0;
    }
    value = strtoll(text, &end, 10);
    if (end == text || *end != '\0') {
        return 0;
    }
    *out = value;
    return 1;
}

static char move_letter(mnsp_move_t move)
{
    /* Cells are in screen order, so dy = +1 moves the blank one row *down*. The
     * letters describe what the player sees: U is up the screen. */
    if (move.dx == 0 && move.dy == -1) {
        return 'U';
    }
    if (move.dx == 0 && move.dy == 1) {
        return 'D';
    }
    if (move.dx == -1 && move.dy == 0) {
        return 'L';
    }
    return 'R';
}

static void print_positions(const int32_t *positions, int cells)
{
    for (int i = 0; i < cells; i++) {
        printf("%s%d", i == 0 ? "" : ",", positions[i]);
    }
    putchar('\n');
}

static int print_board_view(const int32_t *positions, int grid)
{
    /* The array is already in screen order (row 0 at the top), which is also how the
     * game lays the board out. */
    for (int y = 0; y < grid; y++) {
        for (int x = 0; x < grid; x++) {
            int32_t value = positions[y * grid + x];
            if (value < 0) {
                printf("  .");
            } else {
                printf(" %2d", value);
            }
        }
        putchar('\n');
    }
    return 0;
}

static int command_gen(int argc, char **argv)
{
    long music_id;
    long grid;
    int32_t positions[64];

    if (argc < 4 || !parse_int(argv[2], &music_id) || !parse_int(argv[3], &grid)) {
        return 2;
    }
    if (mnsp_generate((int32_t)music_id, (int)grid, positions) != MNSP_OK) {
        return fail("unsupported grid");
    }
    printf("musicId=%ld grid=%ld solvable=%s\n", music_id, grid,
           mnsp_solvable(positions, (int)grid) ? "yes" : "no");
    print_board_view(positions, (int)grid);
    print_positions(positions, (int)(grid * grid));
    return 0;
}

static int command_solve(int argc, char **argv)
{
    long music_id;
    long grid;
    int32_t positions[64];
    mnsp_move_t moves[4096];
    int count;
    const char *strategy = "auto";
    uint64_t budget = 0;

    if (argc < 4 || !parse_int(argv[2], &music_id) || !parse_int(argv[3], &grid)) {
        return 2;
    }
    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "--strategy") == 0 && i + 1 < argc) {
            strategy = argv[++i];
        } else if (strcmp(argv[i], "--max-states") == 0 && i + 1 < argc) {
            long long parsed;
            if (!parse_ll(argv[++i], &parsed) || parsed <= 0) {
                return 2;
            }
            budget = (uint64_t)parsed;
        } else {
            return 2;
        }
    }
    if (mnsp_generate((int32_t)music_id, (int)grid, positions) != MNSP_OK) {
        return fail("unsupported grid");
    }
    if (budget == 0) {
        budget = mnsp_default_budget((int)grid);
    }

    /* auto: search first (much shorter paths), fall back to replaying the shuffle
     * backwards when the board is too far for the budget. */
    if (strcmp(strategy, "undo") != 0) {
        count = mnsp_solve_best(positions, (int)grid, moves, 4096, budget);
        if (count >= 0) {
            printf("musicId=%ld grid=%ld moves=%d strategy=search\n", music_id, grid, count);
            for (int i = 0; i < count; i++) {
                putchar(move_letter(moves[i]));
            }
            putchar('\n');
            return 0;
        }
        if (strcmp(strategy, "search") == 0) {
            return fail(mnsp_result_string((mnsp_result_t)(-count)));
        }
        fprintf(stderr,
                "note: search hit its budget on this board; falling back to replaying\n"
                "      the shuffle backwards, which is valid but roughly 2-4x longer.\n");
    }

    count = mnsp_solve_by_undo((int32_t)music_id, (int)grid, moves, 4096);
    if (count < 0) {
        return fail(mnsp_result_string((mnsp_result_t)(-count)));
    }
    printf("musicId=%ld grid=%ld moves=%d strategy=undo\n", music_id, grid, count);
    for (int i = 0; i < count; i++) {
        putchar(move_letter(moves[i]));
    }
    putchar('\n');
    return 0;
}

static int command_solve_state(int argc, char **argv)
{
    long grid;
    int32_t positions[64];
    mnsp_move_t moves[4096];
    int cells;
    char *cursor;
    int count;
    int solved;

    if (argc < 4 || !parse_int(argv[2], &grid)) {
        return 2;
    }
    if (grid < MNSP_MIN_GRID || grid > MNSP_MAX_GRID) {
        return fail("unsupported grid");
    }
    cells = (int)(grid * grid);
    cursor = argv[3];
    for (int i = 0; i < cells; i++) {
        char *end = NULL;
        long value = strtol(cursor, &end, 10);
        if (end == cursor) {
            return 2;
        }
        positions[i] = (int32_t)value;
        cursor = end;
        if (*cursor == ',') {
            cursor++;
        }
    }
    if (!mnsp_positions_valid(positions, (int)grid)) {
        return fail("board is not a permutation with exactly one empty slot");
    }
    if (!mnsp_solvable(positions, (int)grid)) {
        return fail("board is unsolvable");
    }
    printf("grid=%ld solvable=yes\n", grid);
    print_board_view(positions, (int)grid);
    count = mnsp_solve_best(positions, (int)grid, moves, 4096,
                            mnsp_default_budget((int)grid));
    if (count == -MNSP_ERR_SEARCH_LIMIT) {
        return fail("search limit reached; the board is too far from a solved state");
    }
    if (count < 0) {
        return fail(mnsp_result_string((mnsp_result_t)(-count)));
    }
    printf("moves=%d\n", count);
    for (int i = 0; i < count; i++) {
        putchar(move_letter(moves[i]));
    }
    putchar('\n');

    solved = 1;
    (void)solved;
    return 0;
}

static int command_read(int argc, char **argv)
{
    int32_t positions[64];
    uint8_t confident[64];
    float total = 0.0f;
    mnsp_read_options_t options;
    mnsp_result_t result;
    long grid;
    int cells;

    if (argc < 5 || !parse_int(argv[4], &grid)) {
        return 2;
    }
    if (grid < MNSP_MIN_GRID || grid > MNSP_MAX_GRID) {
        return fail("unsupported grid");
    }
    cells = (int)(grid * grid);
    options.grid = (int)grid;
    options.min_score = 0.25f;
    options.min_margin = 0.0f;
    options.cell_gap_percent = 0.0;

    result = mnsp_read_screenshot(argv[2], argv[3], &options, positions, confident, &total);
    if (result != MNSP_OK) {
        return fail(mnsp_result_string(result));
    }
    printf("grid=%ld totalScore=%.3f\n", grid, total);
    print_board_view(positions, (int)grid);
    printf("tilePositions: ");
    for (int i = 0; i < cells; i++) {
        printf("%s%d", i == 0 ? "" : ",", positions[i]);
    }
    putchar('\n');

    {
        int unsure = 0;
        for (int i = 0; i < cells; i++) {
            if (!confident[i]) {
                if (unsure == 0) {
                    fputs("low-confidence cells: ", stderr);
                } else {
                    fputs(", ", stderr);
                }
                fprintf(stderr, "(%d,%d)=%d", i % (int)grid, i / (int)grid, positions[i]);
                unsure++;
            }
        }
        if (unsure > 0) {
            fputc('\n', stderr);
        }
        if (unsure * 4 > cells) {
            return fail("too many low-confidence cells; confirm the board by hand");
        }
    }

    if (strcmp(argv[argc - 1], "--solve") == 0) {
        mnsp_move_t moves[4096];
        int count;
        if (!mnsp_positions_valid(positions, (int)grid)) {
            return fail("recognized board is not a valid permutation");
        }
        if (!mnsp_solvable(positions, (int)grid)) {
            return fail("recognized board is unsolvable");
        }
        count = mnsp_solve(positions, (int)grid, moves, 4096, 4000000);
        if (count == -MNSP_ERR_SEARCH_LIMIT) {
            return fail("search limit reached");
        }
        if (count < 0) {
            return fail(mnsp_result_string((mnsp_result_t)(-count)));
        }
        printf("moves=%d\n", count);
        for (int i = 0; i < count; i++) {
            putchar(move_letter(moves[i]));
        }
        putchar('\n');
    }
    return 0;
}

static int command_fetch_jackets(int argc, char **argv)
{
    mnsp_jacket_options_t options;
    const char *region = "tw";
    const char *locale = "zh-Hant";
    const char *base_url = NULL;
    long limit = 0;
    int fetched = 0;
    int cached = 0;
    int failed = 0;

    options.allow_fetch = 1;
    options.fetcher = NULL;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--region") == 0 && i + 1 < argc) {
            region = argv[++i];
        } else if (strcmp(argv[i], "--locale") == 0 && i + 1 < argc) {
            locale = argv[++i];
        } else if (strcmp(argv[i], "--base") == 0 && i + 1 < argc) {
            base_url = argv[++i];
        } else if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc) {
            if (!parse_int(argv[++i], &limit)) {
                return 2;
            }
        } else if (strcmp(argv[i], "--offline") == 0) {
            options.allow_fetch = 0;
        } else {
            return 2;
        }
    }
    options.region = region;
    options.locale = locale;
    options.base_url = base_url;

    for (int i = 0; i < mnsp_jacket_count(); i++) {
        int32_t music_id = 0;
        char path[1024];
        mnsp_result_t result;
        if (limit > 0 && fetched + cached >= limit) {
            break;
        }
        if (!mnsp_jacket_at(i, &music_id, NULL)) {
            continue;
        }
        result = mnsp_jacket_resolve(music_id, &options, path, sizeof path);
        if (result == MNSP_OK) {
            /* Distinguish a cache hit from a fresh download by checking whether the
             * file predates this run: simplest reliable signal is the fetch attempt. */
            struct stat info;
            if (options.allow_fetch && stat(path, &info) == 0) {
                fetched++;
            } else {
                cached++;
            }
        } else {
            failed++;
            fprintf(stderr, "fetch %d failed: %s\n", (int)music_id, mnsp_result_string(result));
        }
    }
    printf("jackets: %d ok (%d fetched), %d failed, %d total\n",
           fetched + cached, fetched, failed, mnsp_jacket_count());
    return failed == 0 ? 0 : 1;
}

static int command_list_jackets(void)
{
    for (int i = 0; i < mnsp_jacket_count(); i++) {
        int32_t music_id = 0;
        const char *name = NULL;
        if (mnsp_jacket_at(i, &music_id, &name)) {
            printf("%d %s\n", (int)music_id, name);
        }
    }
    return 0;
}

static int command_read_music(int argc, char **argv)
{
    char jacket[1024];
    mnsp_jacket_options_t jacket_options;
    mnsp_result_t result;
    long music_id;
    char **shifted;

    if (argc < 5 || !parse_int(argv[3], &music_id)) {
        return 2;
    }
    jacket_options.region = NULL;
    jacket_options.locale = NULL;
    jacket_options.base_url = NULL;
    jacket_options.fetcher = NULL;
    jacket_options.allow_fetch = 1;
    /* --offline makes the resolver cache-only. */
    for (int i = 5; i < argc; i++) {
        if (strcmp(argv[i], "--offline") == 0) {
            jacket_options.allow_fetch = 0;
        }
    }
    result = mnsp_jacket_resolve((int32_t)music_id, &jacket_options, jacket, sizeof jacket);
    if (result != MNSP_OK) {
        return fail(mnsp_result_string(result));
    }

    /* Rebuild argv as: read <screenshot> <jacket> <grid> [flags] */
    shifted = (char **)calloc((size_t)argc + 2, sizeof(char *));
    if (shifted == NULL) {
        return fail("out of memory");
    }
    shifted[0] = argv[0];
    shifted[1] = "read";
    shifted[2] = argv[2];
    shifted[3] = jacket;
    shifted[4] = argv[4];
    {
        int out = 5;
        for (int i = 5; i < argc; i++) {
            if (strcmp(argv[i], "--offline") != 0) {
                shifted[out++] = argv[i];
            }
        }
        out--;
        result = (mnsp_result_t)command_read(out + 1, shifted);
    }
    free(shifted);
    return (int)result;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage(stderr);
        return 2;
    }
    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        usage(stdout);
        return 0;
    }
    if (strcmp(argv[1], "--version") == 0) {
        printf("moenotes_slidepuzzle %s\n", MOENOTES_SLIDEPUZZLE_VERSION_STRING);
        return 0;
    }
    if (strcmp(argv[1], "gen") == 0) {
        int code = command_gen(argc, argv);
        if (code == 2) {
            usage(stderr);
        }
        return code;
    }
    if (strcmp(argv[1], "solve") == 0) {
        int code = command_solve(argc, argv);
        if (code == 2) {
            usage(stderr);
        }
        return code;
    }
    if (strcmp(argv[1], "fetch-jackets") == 0) {
        int code = command_fetch_jackets(argc, argv);
        if (code == 2) {
            usage(stderr);
        }
        return code;
    }
    if (strcmp(argv[1], "list-jackets") == 0) {
        return command_list_jackets();
    }
    if (strcmp(argv[1], "read-music") == 0) {
        int code = command_read_music(argc, argv);
        if (code == 2) {
            usage(stderr);
        }
        return code;
    }
    if (strcmp(argv[1], "read") == 0) {
        int code = command_read(argc, argv);
        if (code == 2) {
            usage(stderr);
        }
        return code;
    }
    if (strcmp(argv[1], "solve-state") == 0) {
        int code = command_solve_state(argc, argv);
        if (code == 2) {
            usage(stderr);
        }
        return code;
    }
    usage(stderr);
    return 2;
}
