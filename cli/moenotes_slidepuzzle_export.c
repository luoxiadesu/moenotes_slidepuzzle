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
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <signal.h>
#include <time.h>
#if defined(_WIN32)
#include <direct.h>
#include <process.h>
#else
#include <unistd.h>
#endif
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
    char index_path[1024];
    char index_tmp_path[1024];
    int index_path_used;
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
    int failed;   /* sticky: set once any write reports an error */
} export_sink_t;

static export_sink_t *sink_open_mode(const char *path, int compress, const char *mode)
{
    export_sink_t *sink = (export_sink_t *)calloc(1, sizeof *sink);
    if (sink == NULL) {
        return NULL;
    }
    if (compress) {
        sink->gz = gzopen(path, mode);
        if (sink->gz == NULL) {
            free(sink);
            return NULL;
        }
    } else {
        /* gzopen's "a6" maps to fopen("ab"); the plain path must match. */
        sink->plain = fopen(path, strchr(mode, 'a') != NULL ? "ab" : "wb");
        if (sink->plain == NULL) {
            free(sink);
            return NULL;
        }
    }
    return sink;
}

static export_sink_t *sink_open(const char *path, int compress)
{
    return sink_open_mode(path, compress, "wb6");
}

static export_sink_t *sink_open_append(const char *path, int compress)
{
    return sink_open_mode(path, compress, "ab6");
}

/* A short write here means the file is wrong, not merely slow: gzip streams cannot be
 * resumed, and a truncated JSONL parses up to the cut and then stops, which is the kind
 * of failure a consumer notices only when replaying the last few steps. The error is
 * sticky so a single failed line is not lost among thousands of successful ones. */
static void sink_printf(export_sink_t *sink, const char *format, ...)
{
    va_list args;
    int written;

    if (sink->failed) {
        return;
    }
    va_start(args, format);
    if (sink->gz != NULL) {
        written = gzvprintf(sink->gz, format, args);
    } else {
        written = vfprintf(sink->plain, format, args);
    }
    va_end(args);
    if (written < 0) {
        sink->failed = 1;
    }
}

/* 1 when any write to this sink has failed. */
static int sink_failed(const export_sink_t *sink)
{
    return sink->failed;
}

/* Returns 0 on success. A write error anywhere in the stream is reported here even if
 * the close itself succeeds -- gzclose flushes and may return Z_OK after a failed
 * intermediate write on some zlib builds. */
static int sink_close(export_sink_t *sink)
{
    int status = sink->failed ? -1 : 0;
    if (sink->gz != NULL) {
        if (gzclose(sink->gz) != Z_OK) {
            status = -1;
        }
    } else if (sink->plain != NULL) {
        if (fclose(sink->plain) != 0) {
            status = -1;
        }
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

/* Writes s as the inside of a JSON string. The current jacket table only holds
 * [0-9a-z_], but it is generated from master data, so escaping is cheaper than
 * trusting the generator's character set forever. */
static void json_string(export_sink_t *out, const char *s)
{
    for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; p++) {
        switch (*p) {
        case '"':  sink_printf(out, "\\\""); break;
        case '\\': sink_printf(out, "\\\\"); break;
        case '\b': sink_printf(out, "\\b"); break;
        case '\f': sink_printf(out, "\\f"); break;
        case '\n': sink_printf(out, "\\n"); break;
        case '\r': sink_printf(out, "\\r"); break;
        case '\t': sink_printf(out, "\\t"); break;
        default:
            if (*p < 0x20) {
                sink_printf(out, "\\u%04x", (unsigned)*p);
            } else {
                sink_printf(out, "%c", (char)*p);
            }
            break;
        }
    }
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
        if (sink_failed(out)) {
            return -1; /* stop early; the caller removes the temporary file */
        }
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
        /* Exact A* is both faster and provably optimal here, so --strategy cannot
         * improve on it. It is not silently ignored: the help says ladder applies
         * "from 5x5 up". */
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
#if defined(_WIN32)
    if (_mkdir(path) != 0 && errno != EEXIST) {
        return -1;
    }
#else
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
#endif
    return 0;
}

/* Returns 0 when the whole path fit. A truncated path is not a harmless short name:
 * it is still a valid path, so open/rename succeed and several boards silently
 * overwrite each other while the summary reports success. */
static int build_path(char *buffer, size_t capacity, const char *dir,
                      int32_t music_id, int grid, const char *suffix)
{
    int written = snprintf(buffer, capacity, "%s/%d_%d.jsonl%s", dir,
                           (int)music_id, grid, suffix);
    return (written > 0 && (size_t)written < capacity) ? 0 : -1;
}

/* --resume: a finished file is one that exists, is non-empty, and carries the schema
 * marker we are writing now. A truncated or older file is rebuilt. */
/* Parses the four fields that identify a complete export of one board.
 * Returns 1 when the line is well formed and matches the expected identity. */
static int meta_matches(const char *line, int32_t music_id, int grid)
{
    long schema = -1, found_music = -1, found_grid = -1, length = -1;
    const char *p;

    p = strstr(line, "\"schema\":");
    if (p == NULL) return 0;
    schema = strtol(p + 9, NULL, 10);

    p = strstr(line, "\"musicId\":");
    if (p == NULL) return 0;
    found_music = strtol(p + 10, NULL, 10);

    p = strstr(line, "\"grid\":");
    if (p == NULL) return 0;
    found_grid = strtol(p + 7, NULL, 10);

    p = strstr(line, "\"solutionLength\":");
    if (p == NULL) return 0;
    length = strtol(p + 17, NULL, 10);

    /* Only a substring test was done before, so a file stamped schema 99, or one that
     * was renamed from another board, passed as complete. */
    return schema == EXPORT_SCHEMA && found_music == (long)music_id &&
           found_grid == grid && length >= 0;
}

static int cache_is_valid(const char *path, int compress, int32_t music_id, int grid)
{
    char line[8192];
    int ok = 0;

    if (compress) {
        gzFile file = gzopen(path, "rb");
        if (file == NULL) {
            return 0;
        }
        /* A truncation test is not enough: a .gz cut in half still starts with the
         * magic bytes, and --resume would then skip a file no reader can open.
         * Reading to EOF makes zlib verify the trailer. */
        if (gzgets(file, line, sizeof line) != NULL && meta_matches(line, music_id, grid)) {
            char scratch[8192];
            while (gzgets(file, scratch, sizeof scratch) != NULL) {
                /* drain */
            }
            {
                int error_code = 0;
                (void)gzerror(file, &error_code);
                if (gzeof(file) && error_code == Z_OK) {
                    ok = 1;
                }
            }
        }
        gzclose(file);
        return ok;
    }
    {
        FILE *file = fopen(path, "rb");
        long last;
        if (file == NULL) {
            return 0;
        }
        if (fgets(line, sizeof line, file) != NULL && meta_matches(line, music_id, grid)) {
            char scratch[8192];
            while (fgets(scratch, sizeof scratch, file) != NULL) {
                /* drain */
            }
            ok = ferror(file) == 0;
        }
        /* A truncated text file still reaches EOF without setting an error, so the
         * last byte has to be checked: every complete record ends with a newline. */
        if (ok && fseek(file, -1, SEEK_END) == 0) {
            last = fgetc(file);
            ok = last == '\n';
        } else {
            ok = 0;
        }
        fclose(file);
        return ok;
    }
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

    if (build_path(path, sizeof path, options->out_dir, music_id, grid, suffix) != 0 ||
        build_path(temp, sizeof temp, options->out_dir, music_id, grid, ".tmp") != 0) {
        snprintf(error, error_size,
                 "output path too long for the %d-byte buffer; use a shorter --out",
                 (int)sizeof path);
        return -1;
    }
    if (options->resume && cache_is_valid(path, options->compress, music_id, grid)) {
        /* The file is already complete, but the index still needs a row. Read the
         * metadata line back so the reported length and solver are the real ones
         * rather than placeholders. */
        /* Read the provenance back rather than stamping "cached": the index is how a
         * consumer filters by quality, and overwriting every row with "cached" on a
         * resume run destroys that information. */
        out_result->moves = -1;
        out_result->solver = "cached";
        out_result->quality = "cached";
        {
            char line[8192];
            int have_line = 0;
            if (options->compress) {
                gzFile existing = gzopen(path, "rb");
                if (existing != NULL) {
                    have_line = gzgets(existing, line, sizeof line) != NULL;
                    gzclose(existing);
                }
            } else {
                FILE *existing = fopen(path, "rb");
                if (existing != NULL) {
                    have_line = fgets(line, sizeof line, existing) != NULL;
                    fclose(existing);
                }
            }
            if (have_line) {
                const char *p = strstr(line, "\"solutionLength\":");
                if (p != NULL) {
                    out_result->moves = (int)strtol(p + 17, NULL, 10);
                }
                p = strstr(line, "\"solver\":\"");
                if (p != NULL) {
                    static char solver_buf[64];
                    size_t n = 0;
                    p += 10;
                    while (*p != '"' && *p != '\0' && n + 1 < sizeof solver_buf) {
                        solver_buf[n++] = *p++;
                    }
                    solver_buf[n] = '\0';
                    if (n > 0) {
                        out_result->solver = solver_buf;
                    }
                }
                p = strstr(line, "\"quality\":\"");
                if (p != NULL) {
                    static char quality_buf[64];
                    size_t n = 0;
                    p += 11;
                    while (*p != '"' && *p != '\0' && n + 1 < sizeof quality_buf) {
                        quality_buf[n++] = *p++;
                    }
                    quality_buf[n] = '\0';
                    if (n > 0) {
                        out_result->quality = quality_buf;
                    }
                }
            }
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
    out = sink_open(temp, options->compress);
    if (out == NULL) {
        snprintf(error, error_size, "cannot open the temporary output file");
        return -1;
    }

    sink_printf(out,
             "{\"type\":\"meta\",\"schema\":%d,\"musicId\":%d,\"grid\":%d,"
             "\"jacket\":\"",
             EXPORT_SCHEMA, (int)music_id, grid);
    {
        const char *jacket = mnsp_jacket_asset_name(music_id);
        json_string(out, jacket != NULL ? jacket : "");
    }
    sink_printf(out, "\",\"initial\":");
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
    /* stderr is unbuffered on a terminal but block-buffered when redirected to a file,
     * so a long batch appears to hang without this. */
    fflush(stderr);
    *out_result = result;
    return 1; /* 1 = written, 0 = skipped, -1 = failed */
}

/* -------------------------------------------------------------------- index */

/* Copies the current index into the temp file, dropping any row for a board that this
 * run is about to regenerate. Without this a selective `--resume --music X` would
 * leave the index describing only X. */
static void seed_index_from_existing(const export_options_t *options)
{
    char line[4096];
    export_sink_t *out;
    int have_source = 0;

    if (!options->index_path_used) {
        return;
    }
    out = sink_open_append(options->index_tmp_path, options->compress);
    if (out == NULL) {
        return;
    }
    {
        FILE *source = NULL;
        gzFile gz_source = NULL;
        if (options->compress) {
            gz_source = gzopen(options->index_path, "rb");
            have_source = gz_source != NULL;
        } else {
            source = fopen(options->index_path, "rb");
            have_source = source != NULL;
        }
        if (have_source) {
            while ((options->compress ? gzgets(gz_source, line, sizeof line)
                                      : fgets(line, sizeof line, source)) != NULL) {
                long music = -1, grid = -1;
                const char *p = strstr(line, "\"musicId\":");
                int regenerate = 0;
                if (p != NULL) music = strtol(p + 10, NULL, 10);
                p = strstr(line, "\"grid\":");
                if (p != NULL) grid = strtol(p + 7, NULL, 10);
                /* Only keep rows this run is not going to rewrite. */
                for (int k = 0; k < options->music_count && !regenerate; k++) {
                    if (options->music[k] == (int32_t)music &&
                        options->grids[(int)grid]) {
                        regenerate = 1;
                    }
                }
                if (options->music_count == 0 && options->grids[(int)grid]) {
                    regenerate = 1;
                }
                if (!regenerate) {
                    sink_printf(out, "%s", line);
                }
            }
        }
        if (gz_source != NULL) gzclose(gz_source);
        if (source != NULL) fclose(source);
    }
    sink_close(out);
}

/* Appends one row per board.
 *
 * This open-per-row design is deliberate: the index is written as boards complete, so a
 * run that is interrupted still leaves an index covering everything written so far.
 * Opening in "ab" mode appends atomically enough for a single process; the run lock
 * keeps a second process out.
 *
 * The sink is used rather than a bare gzFile so --no-compress produces a plain text
 * index. It previously always compressed, which produced a file named index.jsonl
 * containing gzip bytes. */
static int write_index(const export_options_t *options, int32_t music_id, int grid,
                       int solution_length, const char *solver, const char *quality)
{
    export_sink_t *index;
    int status;

    if (options->index_path_used == 0) {
        return 0;
    }
    /* Appended to a temporary file; renamed into place only after the whole run
     * succeeds. An index written directly would be left truncated by a crash or a
     * full disk, and a truncated gzip index parses right up to the cut. */
    index = sink_open_append(options->index_tmp_path, options->compress);
    if (index == NULL) {
        return -1;
    }
    sink_printf(index,
                "{\"musicId\":%d,\"grid\":%d,\"file\":\"%d_%d.jsonl%s\","
                "\"solutionLength\":%d,\"solver\":\"%s\",\"quality\":\"%s\"}\n",
                (int)music_id, grid, (int)music_id, grid,
                options->compress ? ".gz" : "", solution_length, solver, quality);
    status = sink_close(index);
    return status;
}

/* --------------------------------------------------------------------- main */

/* A run lock, so two exports writing into the same directory fail loudly instead of
 * interleaving. The rename-based write is atomic per file, but two runs would still
 * duplicate gzip index appends and waste the PDB build. */
static char g_lock_path[1024];

static int acquire_lock(const char *dir)
{
    FILE *file;
    {
        int written = snprintf(g_lock_path, sizeof g_lock_path, "%s/.export.lock", dir);
        if (written <= 0 || (size_t)written >= sizeof g_lock_path) {
            g_lock_path[0] = '\0';
            return -1;   /* path too long; distinct from "lock held" */
        }
    }
    /* "wx" fails when the file already exists, which is exactly the check we want. */
    file = fopen(g_lock_path, "wx");
    if (file == NULL) {
        return 0;
    }
#if defined(_WIN32)
    fprintf(file, "%d\n", _getpid());
#else
    fprintf(file, "%ld\n", (long)getpid());
#endif
    fclose(file);
    return 1;
}

static void release_lock(void)
{
    if (g_lock_path[0] != '\0') {
        remove(g_lock_path);
        g_lock_path[0] = '\0';
    }
}

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
          "                       auto:  exact below 5x5, PDB at 5x5, ladder above\n"
          "                       exact: try a provably shortest path at 5x5 (may take\n"
          "                              minutes per board, falls back when it gives up)\n"
          "                       pdb:   force the 5x5 pattern-database search\n"
          "                       ladder: force the shipped weighted search (5x5 and up;\n"
          "                               below that exact A* is already optimal and faster)\n"
          "                     every strategy falls back rather than failing, so the\n"
          "                     export always covers all boards\n"
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
            char *end = NULL;
            long value = strtol(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' ||
                value < MNSP_MIN_GRID || value > MNSP_MAX_GRID) {
                fprintf(stderr,
                        "moenotes_slidepuzzle: --grid needs a number %d..%d, got \"%s\"\n",
                        MNSP_MIN_GRID, MNSP_MAX_GRID, argv[i]);
                return -2;
            }
            options->grids[value] = 1;
            options->any_grid = 1;
        } else if (strcmp(argv[i], "--music") == 0 && i + 1 < argc) {
            char *end = NULL;
            long value;
            int duplicate = 0;
            /* strtol alone accepts "100001x" as 100001, silently exporting a board
             * for a number the user did not type. Require the whole argument. */
            /* strtol saturates silently on overflow and the value is narrowed to
             * int32 below, so 2^32+100001 would wrap to 100001 and export a board
             * for a song the user never named. Reject anything outside int32. */
            errno = 0;
            value = strtol(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || value <= 0 || errno == ERANGE ||
                value > INT32_MAX || options->music_count >= 256) {
                fprintf(stderr,
                        "moenotes_slidepuzzle: --music needs a numeric song id, got \"%s\"\n",
                        argv[i]);
                return -2;
            }
            /* A song id the game does not ship would still generate a board -- the id
             * only seeds the shuffle -- but the result is a puzzle nobody can see, with
             * an empty jacket name. Refuse rather than emit it. */
            if (mnsp_jacket_asset_name((int32_t)value) == NULL) {
                fprintf(stderr,
                        "moenotes_slidepuzzle: %ld is not a built-in song; "
                        "run `list-jackets` for the ids this build knows\n", value);
                return -2; /* bad value, but the syntax was fine: no usage dump */
            }
            for (int k = 0; k < options->music_count; k++) {
                if (options->music[k] == (int32_t)value) {
                    duplicate = 1;
                    break;
                }
            }
            if (!duplicate) {
                options->music[options->music_count++] = (int32_t)value;
            }
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
        int written = snprintf(options->index_path, sizeof options->index_path,
                               "%s/index.jsonl%s", options->out_dir,
                               options->compress ? ".gz" : "");
        if (written <= 0 || (size_t)written >= sizeof options->index_path) {
            fprintf(stderr,
                    "moenotes_slidepuzzle: --out is too long to place index.jsonl "
                    "beside the boards; use a shorter path\n");
            return -2;
        }
        written = snprintf(options->index_tmp_path, sizeof options->index_tmp_path,
                           "%s.tmp", options->index_path);
        if (written <= 0 || (size_t)written >= sizeof options->index_tmp_path) {
            return -2;
        }
        options->index_path_used = 1;
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
    if (parsed == -2) {
        return 2;
    }
    if (parsed < 0) {
        export_usage(stderr);
        return 2;
    }
    if (ensure_dir(options.out_dir) != 0) {
        fprintf(stderr, "moenotes_slidepuzzle: cannot create %s\n", options.out_dir);
        return 1;
    }
    {
        int lock_status = acquire_lock(options.out_dir);
        if (lock_status < 0) {
            fprintf(stderr,
                    "moenotes_slidepuzzle: --out is too long to place a lock file "
                    "inside it; use a shorter path\n");
            return 2;
        }
        if (lock_status == 0) {
            fprintf(stderr,
                    "moenotes_slidepuzzle: another export is already writing to %s\n"
                    "  (remove %s/.export.lock if that is stale)\n",
                    options.out_dir, options.out_dir);
            return 1;
        }
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
    if (options.index_path_used) {
        remove(options.index_tmp_path);
        /* --resume means "continue", so the rows for boards this run did not touch
         * have to survive. Seed the temp file with the existing index first, then the
         * rows written below replace their own (musicId, grid) entries. */
        if (options.resume) {
            seed_index_from_existing(&options);
        }
    }

    /* On Linux exceeding a file-size rlimit raises SIGXFSZ, whose default action is to
     * kill the process -- the write never returns, so the sink never sees an error and
     * a truncated .tmp (plus the lock) is left behind. Ignoring the signal makes write()
     * return EFBIG instead, which the sink reports as an ordinary failure. */
#ifdef SIGXFSZ
    signal(SIGXFSZ, SIG_IGN);
#endif

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
                if (outcome >= 0) {
                    if (outcome > 0) {
                        written++;
                    } else {
                        skipped++;
                    }
                    /* A board that exported but could not be indexed is still a
                     * failure: the index is how a consumer discovers the files. */
                    if (write_index(&options, music_id, grid, result.moves,
                                    result.solver, result.quality) != 0) {
                        failed++;
                        fprintf(stderr,
                                "  %d %dx%d  index write failed\n",
                                (int)music_id, grid, grid);
                        fflush(stderr);
                    }
                } else {
                    failed++;
                    fprintf(stderr, "  %d %dx%d  FAILED: %s\n",
                            (int)music_id, grid, grid, error);
                    fflush(stderr);
                }
            }
        }
    }

    fprintf(stderr,
            "\nexport: %d planned, %d written, %d skipped, %d failed in %.1f s\n",
            planned, written, skipped, failed,
            (export_now_ms() - started) / 1000.0);

    /* Publish the index only now: every board has been attempted and each row in the
     * temp file corresponds to a board that really exists. */
    if (options.index_path_used && failed == 0) {
        if (rename(options.index_tmp_path, options.index_path) != 0) {
            fprintf(stderr, "moenotes_slidepuzzle: cannot publish the index\n");
            failed++;
        }
    } else if (options.index_path_used) {
        remove(options.index_tmp_path);
        fprintf(stderr,
                "note: the index was discarded because some boards failed\n");
    }

    mnsp_pdb_destroy(pdb);
    release_lock();
    return failed == 0 ? 0 : 1;
}
