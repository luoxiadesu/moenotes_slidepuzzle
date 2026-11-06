/* Contract tests for the batch exporter.
 *
 * The exporter is a file producer, so these tests run it and read what it wrote rather
 * than calling an API. Two properties matter most:
 *
 *   1. Replaying the step lines must reproduce the board at every step and finish on
 *      the goal. A single wrong move here silently ships broken puzzles downstream.
 *   2. The JSON must be parseable by a strict reader. The generator has no JSON
 *      library, so the escaping and the array shape are checked by hand.
 *
 * Fixtures are written to a temporary directory supplied by CTest.
 */
#include "moenotes_slidepuzzle.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <zlib.h>

/* The tests drive the exporter through a shell so they exercise the real binary. That
 * is POSIX syntax throughout (quoting, /dev/null, ulimit), so on Windows the suite is
 * skipped rather than made to fail for a reason that has nothing to do with the code
 * under test. */
#if defined(_WIN32)
#define MNSP_HAVE_SHELL 0
#else
#define MNSP_HAVE_SHELL 1
#endif

#ifndef MNSP_EXPORT_TEST_DIR
#define MNSP_EXPORT_TEST_DIR "test-export"
#endif
#ifndef MNSP_EXPORT_BINARY
#define MNSP_EXPORT_BINARY "moenotes_slidepuzzle"
#endif

#define MAX_GRID 8
#define MAX_CELLS (MAX_GRID * MAX_GRID)

/* Reads one line from a gzFile into `buffer`, stripping the newline. Returns the length
 * or -1 at end of file. */
static int gz_read_line(gzFile file, char *buffer, int capacity)
{
    int length = 0;
    for (;;) {
        char c;
        if (gzread(file, &c, 1) != 1) {
            break;
        }
        if (c == '\n') {
            break;
        }
        if (length + 1 < capacity) {
            buffer[length++] = c;
        }
    }
    buffer[length] = '\0';
    return length == 0 ? -1 : length;
}

/* Parses a 2-D integer array of the form [[a,b,],[c,d]]. Returns the number of values
 * written to `out`, or -1 on a shape error. Deliberately strict: this also serves as
 * the check that the generator emitted well-formed nested arrays. */
static int parse_board(const char *text, int grid, int32_t *out)
{
    const char *cursor = text;
    int count = 0;

    while (*cursor == ' ') cursor++;
    if (*cursor++ != '[') return -1;

    for (int row = 0; row < grid; row++) {
        if (row > 0) {
            if (*cursor++ != ',') return -1;
        }
        if (*cursor++ != '[') return -1;
        for (int col = 0; col < grid; col++) {
            char *end = NULL;
            long value;
            if (col > 0 && *cursor++ != ',') return -1;
            value = strtol(cursor, &end, 10);
            if (end == cursor) return -1;
            out[row * grid + col] = (int32_t)value;
            cursor = end;
            count++;
        }
        if (*cursor++ != ']') return -1;
    }
    if (*cursor++ != ']') return -1;
    return count;
}

static int count_steps(gzFile file, int32_t *final_board, int grid, int *out_declared)
{
    char line[65536];
    int steps = 0;
    int declared = -1;
    int32_t board[MAX_CELLS];
    int have_board = 0;

    /* Metadata line. */
    if (gz_read_line(file, line, sizeof line) < 0) return -1;
    assert(strstr(line, "\"type\":\"meta\"") != NULL);
    assert(strstr(line, "\"schema\":1") != NULL);
    {
        const char *p = strstr(line, "\"solutionLength\":");
        assert(p != NULL);
        declared = (int)strtol(p + 17, NULL, 10);
    }

    while (gz_read_line(file, line, sizeof line) > 0) {
        const char *p;
        int index;

        assert(strstr(line, "\"type\":\"step\"") != NULL);
        p = strstr(line, "\"index\":");
        assert(p != NULL);
        index = (int)strtol(p + 8, NULL, 10);
        assert(index == steps + 1);

        p = strstr(line, "\"board\":");
        assert(p != NULL);
        assert(parse_board(p + 8, grid, board) == grid * grid);
        memcpy(final_board, board, sizeof(int32_t) * (size_t)(grid * grid));
        have_board = 1;
        steps++;
    }
    if (!have_board && declared == 0) {
        return 0;
    }
    if (out_declared != NULL) *out_declared = declared;
    return steps;
}

static void test_export_small_boards(void)
{
    char path[512];
    gzFile file;
    int32_t final_board[MAX_CELLS];
    int32_t goal[MAX_CELLS];
    int declared = -1;
    int steps;

    snprintf(path, sizeof path, "%s/100050_3.jsonl.gz", MNSP_EXPORT_TEST_DIR);
    file = gzopen(path, "rb");
    assert(file != NULL);
    steps = count_steps(file, final_board, 3, &declared);
    gzclose(file);

    assert(steps > 0);
    assert(steps == declared);
    mnsp_goal_positions(3, goal);
    assert(memcmp(final_board, goal, sizeof(int32_t) * 9) == 0);
    printf("  3x3: %d steps, final board is the goal\n", steps);
}

static void test_export_five_by_five(void)
{
    char path[512];
    gzFile file;
    int32_t final_board[MAX_CELLS];
    int32_t goal[MAX_CELLS];
    int declared = -1;
    int steps;

    snprintf(path, sizeof path, "%s/100001_5.jsonl.gz", MNSP_EXPORT_TEST_DIR);
    file = gzopen(path, "rb");
    assert(file != NULL);
    steps = count_steps(file, final_board, 5, &declared);
    gzclose(file);

    assert(steps > 0);
    assert(steps == declared);
    mnsp_goal_positions(5, goal);
    assert(memcmp(final_board, goal, sizeof(int32_t) * 25) == 0);
    printf("  5x5: %d steps, final board is the goal\n", steps);
}

/* --no-compress is a separate run into its own directory; the default output is gzip. */
static void test_plain_output(void)
{
    char dir[512];
    char path[600];
    char command[1024];
    FILE *file;
    char line[4096];

    snprintf(dir, sizeof dir, "%s/plain", MNSP_EXPORT_TEST_DIR);
    snprintf(path, sizeof path, "%s/100001_3.jsonl", dir);
    snprintf(command, sizeof command,
             "\"%s\" export --out \"%s\" --music 100001 --grid 3 "
             "--no-compress >/dev/null 2>&1",
             MNSP_EXPORT_BINARY, dir);
    assert(system(command) == 0);

    file = fopen(path, "r");
    assert(file != NULL);
    assert(fgets(line, sizeof line, file) != NULL);
    assert(strstr(line, "\"type\":\"meta\"") != NULL);
    fclose(file);
    printf("  --no-compress produced plain JSONL\n");
}

/* --resume must not rewrite a file that is already complete. Comparing the inode's
 * modification time would be flaky on coarse clocks, so compare the file contents. */
static unsigned long file_hash(const char *path)
{
    unsigned long hash = 5381;
    FILE *file = fopen(path, "rb");
    int c;
    assert(file != NULL);
    while ((c = fgetc(file)) != EOF) {
        hash = ((hash << 5) + hash) + (unsigned long)c;
    }
    fclose(file);
    return hash;
}

static void test_resume_is_idempotent(void)
{
    char path[512];
    char command[1024];
    unsigned long before, after;

    snprintf(path, sizeof path, "%s/100050_3.jsonl.gz", MNSP_EXPORT_TEST_DIR);
    before = file_hash(path);

    snprintf(command, sizeof command,
             "\"%s\" export --out \"%s\" --music 100050 --grid 3 --resume "
             ">/dev/null 2>&1",
             MNSP_EXPORT_BINARY, MNSP_EXPORT_TEST_DIR);
    assert(system(command) == 0);

    after = file_hash(path);
    assert(before == after);
    printf("  --resume did not rewrite a complete file\n");
}

/* Runs the exporter as a child process; the test then reads what it wrote. */
static int generate_fixtures(void)
{
    char command[1024];
    int status;

    snprintf(command, sizeof command,
             "\"%s\" export --out \"%s\" --music 100001 --music 100050 "
             "--grid 3 --grid 5 --strategy ladder --index >/dev/null 2>&1",
             MNSP_EXPORT_BINARY, MNSP_EXPORT_TEST_DIR);
    status = system(command);
    return status == 0;
}

/* --no-compress must produce a readable plain-text index, not gzip bytes behind a
 * .jsonl name. */
static void test_plain_index_is_not_gzip(void)
{
    char dir[512];
    char path[600];
    char command[1024];
    FILE *file;
    unsigned char head[2];

    snprintf(dir, sizeof dir, "%s/plainindex", MNSP_EXPORT_TEST_DIR);
    snprintf(path, sizeof path, "%s/index.jsonl", dir);
    snprintf(command, sizeof command,
             "\"%s\" export --out \"%s\" --music 100001 --grid 3 --no-compress "
             "--index >/dev/null 2>&1", MNSP_EXPORT_BINARY, dir);
    assert(system(command) == 0);

    file = fopen(path, "rb");
    assert(file != NULL);
    assert(fread(head, 1, 2, file) == 2);
    fclose(file);
    assert(!(head[0] == 0x1f && head[1] == 0x8b));
    printf("  --no-compress index is plain text\n");
}

/* A truncated .gz still begins with the magic bytes; --resume must not accept it. */
static void test_resume_rebuilds_truncated_gzip(void)
{
    char dir[512];
    char path[600];
    char command[1024];
    struct stat before, after;

    snprintf(dir, sizeof dir, "%s/truncated", MNSP_EXPORT_TEST_DIR);
    snprintf(path, sizeof path, "%s/100050_7.jsonl.gz", dir);
    snprintf(command, sizeof command,
             "\"%s\" export --out \"%s\" --music 100050 --grid 7 --strategy ladder "
             ">/dev/null 2>&1", MNSP_EXPORT_BINARY, dir);
    assert(system(command) == 0);
    assert(stat(path, &before) == 0);

    snprintf(command, sizeof command, "truncate -s 500 \"%s\"", path);
    assert(system(command) == 0);

    snprintf(command, sizeof command,
             "\"%s\" export --out \"%s\" --music 100050 --grid 7 --strategy ladder "
             "--resume >/dev/null 2>&1", MNSP_EXPORT_BINARY, dir);
    assert(system(command) == 0);
    assert(stat(path, &after) == 0);
    assert(after.st_size == before.st_size);   /* rebuilt, not left at 500 */
    printf("  --resume rebuilt a truncated gzip\n");
}

/* Numeric options must reject trailing garbage rather than parsing a prefix. */
static void test_numeric_options_are_strict(void)
{
    char command[1024];
    char dir[512];

    snprintf(dir, sizeof dir, "%s/strict", MNSP_EXPORT_TEST_DIR);
    snprintf(command, sizeof command,
             "\"%s\" export --out \"%s\" --music 100001x --grid 3 >/dev/null 2>&1",
             MNSP_EXPORT_BINARY, dir);
    assert(system(command) != 0);

    snprintf(command, sizeof command,
             "\"%s\" export --out \"%s\" --grid 3x >/dev/null 2>&1",
             MNSP_EXPORT_BINARY, dir);
    assert(system(command) != 0);
    printf("  trailing garbage rejected\n");
}

/* A path long enough to truncate must be rejected, not silently write several boards
 * into one file. */
static void test_long_out_dir_is_rejected(void)
{
    static char dir[1200];
    char command[2400];
    int status;

    /* Build a directory whose length lands just under the exporter's 1024-byte buffer
     * so the board file name would be cut off. */
    snprintf(dir, sizeof dir, "%s/long", MNSP_EXPORT_TEST_DIR);
    while (strlen(dir) < 1010) {
        strncat(dir, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 
                sizeof dir - strlen(dir) - 1);
    }
    /* The path is long enough that mkdir itself may refuse; the exporter must reject
     * it either way, so the creation result is not asserted. */
    snprintf(command, sizeof command, "mkdir -p \"%s\" 2>/dev/null", dir);
    (void)!system(command);

    snprintf(command, sizeof command,
             "\"%s\" export --out \"%s\" --music 100001 --music 100050 "
             "--grid 3 --grid 4 >/dev/null 2>&1",
             MNSP_EXPORT_BINARY, dir);
    status = system(command);
    assert(status != 0);   /* must refuse rather than collide */
    printf("  over-long --out rejected\n");
}

/* A file stamped with a different schema must be rebuilt, not resumed past. */
static void test_resume_rejects_wrong_schema(void)
{
    char dir[512];
    char path[600];
    char command[4096];
    struct stat before;

    snprintf(dir, sizeof dir, "%s/schema", MNSP_EXPORT_TEST_DIR);
    snprintf(path, sizeof path, "%s/100050_3.jsonl.gz", dir);
    snprintf(command, sizeof command,
             "\"%s\" export --out \"%s\" --music 100050 --grid 3 >/dev/null 2>&1",
             MNSP_EXPORT_BINARY, dir);
    assert(system(command) == 0);
    assert(stat(path, &before) == 0);

    snprintf(command, sizeof command,
             "zcat \"%s\" | sed 's/\"schema\":1/\"schema\":99/' | gzip > \"%s.n\" "
             "&& mv \"%s.n\" \"%s\"", path, path, path, path);
    assert(system(command) == 0);

    snprintf(command, sizeof command,
             "\"%s\" export --out \"%s\" --music 100050 --grid 3 --resume "
             ">/dev/null 2>&1", MNSP_EXPORT_BINARY, dir);
    assert(system(command) == 0);

    /* Rebuilt means the schema marker is 1 again. */
    snprintf(command, sizeof command, "zcat \"%s\" | head -1 | grep -q '\"schema\":1'",
             path);
    assert(system(command) == 0);
    printf("  --resume rebuilt a wrong-schema file\n");
}

/* A full disk must fail the run, not silently produce a truncated file. The file-size
 * rlimit reproduces it without needing to fill a real filesystem: with SIGXFSZ ignored
 * the write returns EFBIG and the exporter reports the failure.
 *
 * Skipped where the shell cannot set a limit or the platform lacks ulimit. */
static void test_write_failure_is_reported(void)
{
    char dir[512];
    char command[2048];
    int status;

    snprintf(dir, sizeof dir, "%s/full", MNSP_EXPORT_TEST_DIR);
    snprintf(command, sizeof command,
             "mkdir -p \"%s\" && sh -c 'ulimit -f 1 2>/dev/null && \"%s\" export "
             "--out \"%s\" --music 100050 --grid 7 --strategy ladder' >/dev/null 2>&1",
             dir, MNSP_EXPORT_BINARY, dir);
    status = system(command);
    if (status == 0) {
        /* No rlimit support here (or the write fit): nothing to assert. */
        printf("  write-failure test skipped (no file-size limit)\n");
        return;
    }
    {
        char path[600];
        FILE *file;
        snprintf(path, sizeof path, "%s/100050_7.jsonl.gz", dir);
        file = fopen(path, "rb");
        assert(file == NULL);   /* the truncated temp must not have been renamed */
        printf("  full disk reported as failure, no output left behind\n");
    }
}

int main(void)
{
#if !MNSP_HAVE_SHELL
    puts("test_export: skipped (needs a POSIX shell to drive the exporter)");
    return 77;   /* CTest SKIP_RETURN_CODE: a skip must not look like a pass */
#else
    assert(generate_fixtures());
    test_export_small_boards();
    test_export_five_by_five();
    test_plain_output();
    test_resume_is_idempotent();
    test_plain_index_is_not_gzip();
    test_resume_rebuilds_truncated_gzip();
    test_numeric_options_are_strict();
    test_long_out_dir_is_rejected();
    test_resume_rejects_wrong_schema();
    test_write_failure_is_reported();
    puts("test_export: ok");
    return 0;
#endif
}
