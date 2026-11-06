/* Batch export of every board the game can produce, as step-by-step JSONL.
 *
 * The point of this command is to turn the solver into a one-off generator: solve
 * every (song, size) pair once, write the whole path to disk, and let downstream
 * consumers read the answer instead of re-solving. That inverts the cost model in a
 * useful way -- the generator may spend minutes on a board, because the result is
 * cached forever afterwards.
 *
 * Output is one gzip'd JSONL file per board:
 *
 *   <out>/<musicId>_<grid>.jsonl.gz
 *
 * The first line is metadata, every later line is one step:
 *
 *   {"type":"meta","schema":1,"musicId":100050,"grid":5,...,
 *    "initial":[[...],[...]],"goal":[[...]],"solutionLength":68,
 *    "solver":"pdb-w1.5","quality":"near-optimal","elapsedMs":245}
 *   {"type":"step","index":1,"move":"U","tile":13,"from":[3,2],"to":[2,2],
 *    "board":[[...],[...]]}
 *
 * `board` is a screen-order 2-D array: row 0 is the top row as drawn, and each value
 * is the tile's correct position (the same convention the rest of the library uses),
 * with -1 for the empty slot. A reader can render it directly without knowing
 * anything about the internal coordinate flips.
 *
 * `move` is the direction the *blank* travels, matching the CLI's solve output.
 * `from` and `to` are [row, col] in the same screen order.
 */
/* clock_gettime is POSIX, and the project builds with -std=c17 and
 * CMAKE_C_EXTENSIONS OFF, so the feature macro has to be requested explicitly. */
#define _POSIX_C_SOURCE 200809L

#include "moenotes_slidepuzzle.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <zlib.h>

#define EXPORT_SCHEMA 1
#define EXPORT_MAX_CELLS 64
#define EXPORT_MAX_MOVES 4096

typedef enum export_strategy {
    EXPORT_AUTO = 0,
    EXPORT_EXACT,
    EXPORT_PDB,
    EXPORT_LADDER
} export_strategy_t;

typedef struct export_options {
    const char *out_dir;
    const char *index_path;
    int grids[MNSP_MAX_GRID + 1];   /* 1 = enabled */
    int any_grid;                   /* 0 = all supported sizes */
    int32_t music[256];
    int music_count;
    int resume;
    int compress;
    int want_index;
    export_strategy_t strategy;
} export_options_t;

/* What the solver actually did, so the file records its own provenance. */
typedef struct export_result {
    const char *solver;
    const char *quality;
    double elapsed_ms;
    int moves;
} export_result_t;

static double export_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* ------------------------------------------------------------------ output sink */

/* Writing through one type keeps the step/meta code identical for compressed and plain
 * output. An earlier version always called gzprintf and only varied the file suffix,
 * which silently wrote gzip bytes into a file named .jsonl. */
typedef struct export_sink {
    gzFile gz;
    FILE *plain;
} export_sink_t;

static export_sink_t *sink_open(const char *path, int compress)
{
    export_sink_t *sink = (export_sink_t *)calloc(1, sizeof *sink);
    if (sink == NULL) {
        return NULL;
    }
    if (compress) {
        sink->gz = gzopen(path, "wb6");
        if (sink->gz == NULL) {
            free(sink);
            return NULL;
        }
    } else {
        sink->plain = fopen(path, "wb");
        if (sink->plain == NULL) {
            free(sink);
            return NULL;
        }
    }
    return sink;
}

static void sink_printf(export_sink_t *sink, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    if (sink->gz != NULL) {
        gzvprintf(sink->gz, format, args);
    } else {
        vfprintf(sink->plain, format, args);
    }
    va_end(args);
}

/* Returns 0 on success. */
static int sink_close(export_sink_t *sink)
{
    int status = 0;
    if (sink->gz != NULL) {
        status = gzclose(sink->gz) == Z_OK ? 0 : -1;
    } else if (sink->plain != NULL) {
        status = fclose(sink->plain) == 0 ? 0 : -1;
    }
    free(sink);
    return status;
}

/* -------------------------------------------------------------- JSON output */

/* Writes a screen-order 2-D array. The array itself is already top-down, so this is a
 * straight dump; the only subtlety is that -1 prints as -1 rather than being hidden,
 * because a consumer replaying the steps needs to know where the blank is. */
static void json_board(export_sink_t *out, const int32_t *positions, int grid)
{
    sink_printf(out, "[");
    for (int row = 0; row < grid; row++) {
        if (row > 0) {
            sink_printf(out, ",");
        }
        sink_printf(out, "[");
        for (int col = 0; col < grid; col++) {
            if (col > 0) {
                sink_printf(out, ",");
            }
            sink_printf(out, "%d", (int)positions[row * grid + col]);
        }
        sink_printf(out, "]");
    }
    sink_printf(out, "]");
}

static char move_letter(mnsp_move_t move)
{
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

static int find_blank(const int32_t *positions, int cells)
{
    for (int i = 0; i < cells; i++) {
        if (positions[i] < 0) {
            return i;
        }
    }
    return -1;
}

/* ------------------------------------------------------------ path building */

/* Applies the first `count` moves and writes a step line for each one. Returns the
 * number of steps written, or -1 on a malformed move. */
static int write_steps(export_sink_t *out, const int32_t *initial, int grid,
                       const mnsp_move_t *moves, int count)
{
    int cells = grid * grid;
    int32_t current[EXPORT_MAX_CELLS];
    int blank;

    memcpy(current, initial, (size_t)cells * sizeof(int32_t));
    blank = find_blank(current, cells);
    if (blank < 0) {
        return -1;
    }

    for (int step = 0; step < count; step++) {
        int from_row = blank / grid;
        int from_col = blank % grid;
        int to_row = from_row + moves[step].dy;
        int to_col = from_col + moves[step].dx;
        int target, tile;

        if (to_row < 0 || to_row >= grid || to_col < 0 || to_col >= grid) {
            return -1;
        }
        target = to_row * grid + to_col;
        tile = (int)current[target];

        /* The move slides the tile at `target` into the blank's cell. */
        current[blank] = current[target];
        current[target] = -1;
        blank = target;

        /* `tile` travels from the cell it occupies into the blank's cell. Reporting the
         * tile's own endpoints (rather than the blank's) is what a consumer animating
         * the move needs; the blank's path is the reverse and can be derived. */
        sink_printf(out,
                 "{\"type\":\"step\",\"index\":%d,\"move\":\"%c\",\"tile\":%d,"
                 "\"from\":[%d,%d],\"to\":[%d,%d],\"board\":",
                 step + 1, move_letter(moves[step]), tile,
                 to_row, to_col, from_row, from_col);
        json_board(out, current, grid);
        sink_printf(out, "}\n");
    }
    return count;
}

/* ---------------------------------------------------------------- solving */

/* Chooses a strategy for one size and records what was actually used.
 *
 * 3x3 and 4x4 are solved exactly and instantly by the shipped solver, so the PDB adds
 * nothing there. 5x5 is the only size where the PDB helps. Above that no pattern
 * database fits, so the weighted ladder is the only option. */
static int solve_one(mnsp_pdb_t *pdb, const int32_t *board, int grid,
                     mnsp_move_t *moves, int capacity, export_strategy_t strategy,
                     export_result_t *result)
{
    double start = export_now_ms();
    int count = -MNSP_ERR_INTERNAL;

    result->solver = "ladder";
    result->quality = "heuristic";

    if (grid <= 4) {
        count = mnsp_solve_best(board, grid, moves, capacity, mnsp_default_budget(grid));
        result->solver = "exact-astar";
        result->quality = "optimal";
    } else if (strategy == EXPORT_LADDER) {
        count = mnsp_solve_best(board, grid, moves, capacity, mnsp_default_budget(grid));
        result->solver = "ladder";
        result->quality = "heuristic";
    } else if (grid == 5 && pdb != NULL && strategy != EXPORT_LADDER) {
        if (strategy == EXPORT_EXACT) {
            count = mnsp_solve_optimal(pdb, board, grid, moves, capacity, 3000000000ull);
            if (count >= 0) {
                result->solver = "pdb-exact";
                result->quality = "optimal";
            }
        } else {
            count = mnsp_solve_pdb(pdb, board, grid, moves, capacity, 1.5, 200000000ull);
            if (count >= 0) {
                result->solver = "pdb-w1.5";
                result->quality = "near-optimal";
            }
        }
        /* Fall back rather than fail: the generator must cover every board. */
        if (count < 0) {
            count = mnsp_solve_best(board, grid, moves, capacity, mnsp_default_budget(grid));
            result->solver = "ladder";
            result->quality = "heuristic";
        }
    } else {
        count = mnsp_solve_best(board, grid, moves, capacity, mnsp_default_budget(grid));
    }

    result->elapsed_ms = export_now_ms() - start;
    result->moves = count;
    return count;
}

/* ------------------------------------------------------------------ files */

static int ensure_dir(const char *path)
{
    struct stat info;

    if (stat(path, &info) == 0) {
        return S_ISDIR(info.st_mode) ? 0 : -1;
    }
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

static void build_path(char *buffer, size_t capacity, const char *dir,
                       int32_t music_id, int grid, const char *suffix)
{
    snprintf(buffer, capacity, "%s/%d_%d.jsonl%s", dir, (int)music_id, grid, suffix);
}

/* --resume: a finished file is one that exists, is non-empty, and carries the schema
 * marker we are writing now. A truncated or older file is rebuilt. */
static int cache_is_valid(const char *path, int compress)
{
    char line[512];
    FILE *file;
    int ok = 0;

    file = fopen(path, "rb");
    if (file == NULL) {
        return 0;
    }
    if (compress) {
        /* Cheap check: gzip magic plus a non-trivial size. A full validation would mean
         * decompressing the file, which costs as much as rewriting it. */
        unsigned char magic[2];
        fclose(file);
        file = fopen(path, "rb");
        if (file != NULL && fread(magic, 1, 2, file) == 2) {
            struct stat info;
            ok = magic[0] == 0x1f && magic[1] == 0x8b &&
                 stat(path, &info) == 0 && info.st_size > 64;
        }
        if (file != NULL) {
            fclose(file);
        }
        return ok;
    }
    if (fgets(line, sizeof line, file) != NULL) {
        ok = strstr(line, "\"schema\":") != NULL;
    }
    fclose(file);
    return ok;
}

/* ------------------------------------------------------------------ export */

static int export_board(const export_options_t *options, mnsp_pdb_t *pdb,
                        int32_t music_id, int grid, export_result_t *out_result,
                        char *error, size_t error_size)
{
    int32_t board[EXPORT_MAX_CELLS];
    int32_t goal[EXPORT_MAX_CELLS];
    mnsp_move_t moves[EXPORT_MAX_MOVES];
    export_result_t result;
    char path[1024];
    char temp[1024];
    const char *suffix = options->compress ? ".gz" : "";
    export_sink_t *out;
    int count;

    build_path(path, sizeof path, options->out_dir, music_id, grid, suffix);
    if (options->resume && cache_is_valid(path, options->compress)) {
        /* The file is already complete, but the index still needs a row. Read the
         * metadata line back so the reported length and solver are the real ones
         * rather than placeholders. */
        gzFile existing = gzopen(path, "rb");
        out_result->moves = -1;
        out_result->solver = "cached";
        out_result->quality = "cached";
        if (existing != NULL) {
            char line[8192];
            if (gzgets(existing, line, sizeof line) != NULL) {
                const char *p = strstr(line, "\"solutionLength\":");
                if (p != NULL) {
                    out_result->moves = (int)strtol(p + 17, NULL, 10);
                }
            }
            gzclose(existing);
        }
        return 0;
    }

    if (mnsp_generate(music_id, grid, board) != MNSP_OK) {
        snprintf(error, error_size, "board generation failed");
        return -1;
    }
    mnsp_goal_positions(grid, goal);

    count = solve_one(pdb, board, grid, moves, EXPORT_MAX_MOVES, options->strategy, &result);
    if (count < 0) {
        snprintf(error, error_size, "solve failed (%d)", count);
        return -1;
    }

    /* Write to a temporary name and rename at the end, so an interrupted run never
     * leaves a half-written file that --resume would mistake for complete. */
    build_path(temp, sizeof temp, options->out_dir, music_id, grid, ".tmp");
    out = sink_open(temp, options->compress);
    if (out == NULL) {
        snprintf(error, error_size, "cannot open the temporary output file");
        return -1;
    }

    sink_printf(out,
             "{\"type\":\"meta\",\"schema\":%d,\"musicId\":%d,\"grid\":%d,"
             "\"jacket\":\"%s\",\"initial\":",
             EXPORT_SCHEMA, (int)music_id, grid,
             mnsp_jacket_asset_name(music_id) ? mnsp_jacket_asset_name(music_id) : "");
    json_board(out, board, grid);
    sink_printf(out, ",\"goal\":");
    json_board(out, goal, grid);
    sink_printf(out,
             ",\"solutionLength\":%d,\"solver\":\"%s\",\"quality\":\"%s\","
             "\"elapsedMs\":%.0f}\n",
             count, result.solver, result.quality, result.elapsed_ms);

    if (write_steps(out, board, grid, moves, count) != count) {
        sink_close(out);
        remove(temp);
        snprintf(error, error_size, "step serialisation failed");
        return -1;
    }

    if (sink_close(out) != 0) {
        remove(temp);
        snprintf(error, error_size, "closing the output file failed");
        return -1;
    }
    if (rename(temp, path) != 0) {
        remove(temp);
        snprintf(error, error_size, "cannot rename the temporary output file");
        return -1;
    }

    fprintf(stderr, "  %d %dx%d  %d moves  %s (%s, %.0f ms)\n",
            (int)music_id, grid, grid, count, result.solver, result.quality,
            result.elapsed_ms);
    *out_result = result;
    return 1; /* 1 = written, 0 = skipped, -1 = failed */
}

/* -------------------------------------------------------------------- index */

static int write_index(const export_options_t *options, int32_t music_id, int grid,
                       int solution_length, const char *solver, const char *quality)
{
    gzFile index;

    if (options->index_path == NULL) {
        return 0;
    }
    index = gzopen(options->index_path, "ab6");
    if (index == NULL) {
        return -1;
    }
    gzprintf(index,
             "{\"musicId\":%d,\"grid\":%d,\"file\":\"%d_%d.jsonl%s\","
             "\"solutionLength\":%d,\"solver\":\"%s\",\"quality\":\"%s\"}\n",
             (int)music_id, grid, (int)music_id, grid,
             options->compress ? ".gz" : "", solution_length, solver, quality);
    gzclose(index);
    return 0;
}

/* --------------------------------------------------------------------- main */

static void export_usage(FILE *out)
{
    fputs("usage: moenotes_slidepuzzle export --out <dir> [options]\n"
          "\n"
          "Solve every board the game can produce and write the full path to disk,\n"
          "one gzip'd JSONL file per (song, size). Each file has a metadata line\n"
          "followed by one line per step, including the complete board after the move,\n"
          "so a consumer can render or replay the solution without re-solving.\n"
          "\n"
          "options:\n"
          "  --out <dir>        output directory (default ./puzzles)\n"
          "  --grid <N>         only this size; repeatable (default 3..8)\n"
          "  --music <id>       only this song; repeatable (default every built-in song)\n"
          "  --strategy <s>     auto (default) | exact | pdb | ladder\n"
          "                       auto: exact below 5x5, PDB at 5x5, ladder above\n"
          "                       exact: try a provably shortest path at 5x5 (slow)\n"
          "  --resume           skip boards whose output file is already complete\n"
          "  --no-compress      write plain .jsonl instead of .jsonl.gz\n"
          "  --index            also maintain <out>/index.jsonl.gz\n"
          "\n"
          "The 5x5 pattern database is built once per run (about 217 s) and shared by\n"
          "every 5x5 board; expect about 5 minutes for 3x3..7x7 and 39 for 8x8.\n",
          out);
}

static int parse_args(int argc, char **argv, export_options_t *options)
{
    memset(options, 0, sizeof *options);
    options->out_dir = "puzzles";
    options->compress = 1;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            options->out_dir = argv[++i];
        } else if (strcmp(argv[i], "--grid") == 0 && i + 1 < argc) {
            long value = strtol(argv[++i], NULL, 10);
            if (value < MNSP_MIN_GRID || value > MNSP_MAX_GRID) {
                return -1;
            }
            options->grids[value] = 1;
            options->any_grid = 1;
        } else if (strcmp(argv[i], "--music") == 0 && i + 1 < argc) {
            long value = strtol(argv[++i], NULL, 10);
            if (value <= 0 || options->music_count >= 256) {
                return -1;
            }
            options->music[options->music_count++] = (int32_t)value;
        } else if (strcmp(argv[i], "--strategy") == 0 && i + 1 < argc) {
            const char *name = argv[++i];
            if (strcmp(name, "auto") == 0) {
                options->strategy = EXPORT_AUTO;
            } else if (strcmp(name, "exact") == 0) {
                options->strategy = EXPORT_EXACT;
            } else if (strcmp(name, "pdb") == 0) {
                options->strategy = EXPORT_PDB;
            } else if (strcmp(name, "ladder") == 0) {
                options->strategy = EXPORT_LADDER;
            } else {
                return -1;
            }
        } else if (strcmp(argv[i], "--resume") == 0) {
            options->resume = 1;
        } else if (strcmp(argv[i], "--no-compress") == 0) {
            options->compress = 0;
        } else if (strcmp(argv[i], "--index") == 0) {
            options->want_index = 1;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            export_usage(stdout);
            return 1;
        } else {
            return -1;
        }
    }
    if (!options->any_grid) {
        for (int g = MNSP_MIN_GRID; g <= MNSP_MAX_GRID; g++) {
            options->grids[g] = 1;
        }
    }
    if (options->want_index) {
        static char index_path[1024];
        snprintf(index_path, sizeof index_path, "%s/index.jsonl%s",
                 options->out_dir, options->compress ? ".gz" : "");
        options->index_path = index_path;
    }
    return 0;
}

int mnsp_export_command(int argc, char **argv)
{
    export_options_t options;
    mnsp_pdb_t *pdb = NULL;
    int parsed = parse_args(argc, argv, &options);
    int planned = 0, written = 0, skipped = 0, failed = 0;
    int need_pdb;
    double started;

    if (parsed == 1) {
        return 0;
    }
    if (parsed < 0) {
        export_usage(stderr);
        return 2;
    }
    if (ensure_dir(options.out_dir) != 0) {
        fprintf(stderr, "moenotes_slidepuzzle: cannot create %s\n", options.out_dir);
        return 1;
    }

    /* The PDB is only worth building when a 5x5 board will actually use it. */
    need_pdb = options.grids[5] && options.strategy != EXPORT_LADDER;
    if (need_pdb) {
        /* The tables live in the same cache tree the jacket downloader uses, not in the
         * output directory: they are build artefacts, and mixing them with the exported
         * puzzles would both dirty the output and confuse --resume. */
        char base[900];
        char cache_dir[1024];
        int have_cache = mnsp_jacket_cache_dir(base, sizeof base) != 0;
        if (have_cache) {
            snprintf(cache_dir, sizeof cache_dir, "%s/pdb-5", base);
            have_cache = ensure_dir(base) == 0 && ensure_dir(cache_dir) == 0;
        }
        if (have_cache) {
            fprintf(stderr, "loading or building the 5x5 pattern database (%s) ...\n",
                    cache_dir);
            pdb = mnsp_pdb_create_cached(5, cache_dir);
        } else {
            fprintf(stderr, "building the 5x5 pattern database (no cache directory) ...\n");
            pdb = mnsp_pdb_create(5);
        }
        if (pdb != NULL) {
            fprintf(stderr, "pattern database ready\n");
        }
        if (pdb == NULL) {
            fprintf(stderr, "note: pattern database unavailable; 5x5 will use the "
                            "weighted ladder\n");
        }
    }

    /* The index is appended to as boards complete, so clear it first; otherwise a
     * second run would duplicate every row. */
    if (options.index_path != NULL) {
        remove(options.index_path);
    }

    started = export_now_ms();
    {
        int song_count = options.music_count > 0 ? options.music_count
                                                 : mnsp_jacket_count();
        for (int s = 0; s < song_count; s++) {
            int32_t music_id;
            if (options.music_count > 0) {
                music_id = options.music[s];
            } else if (!mnsp_jacket_at(s, &music_id, NULL)) {
                continue;
            }
            for (int grid = MNSP_MIN_GRID; grid <= MNSP_MAX_GRID; grid++) {
                char error[256];
                export_result_t result;
                int outcome;
                if (!options.grids[grid]) {
                    continue;
                }
                planned++;
                error[0] = '\0';
                memset(&result, 0, sizeof result);
                outcome = export_board(&options, pdb, music_id, grid, &result,
                                       error, sizeof error);
                if (outcome > 0) {
                    written++;
                    write_index(&options, music_id, grid, result.moves,
                                result.solver, result.quality);
                } else if (outcome == 0) {
                    skipped++;
                    write_index(&options, music_id, grid, result.moves,
                                result.solver, result.quality);
                } else {
                    failed++;
                    fprintf(stderr, "  %d %dx%d  FAILED: %s\n",
                            (int)music_id, grid, grid, error);
                }
            }
        }
    }

    fprintf(stderr,
            "\nexport: %d planned, %d written, %d skipped, %d failed in %.1f s\n",
            planned, written, skipped, failed,
            (export_now_ms() - started) / 1000.0);

    mnsp_pdb_destroy(pdb);
    return failed == 0 ? 0 : 1;
}
