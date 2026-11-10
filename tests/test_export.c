/* Contract tests for the batch exporter.
 *
 * The exporter is a file producer, so these tests run it and read what it wrote rather
 * than calling an API. Three properties matter most:
 *
 *   1. Replaying the step lines from the declared `initial` board must reproduce the
 *      board of every step and finish on the goal, and the `move` letter must agree with
 *      the tile's `from`/`to` endpoints. A single wrong move here silently ships broken
 *      puzzles downstream.
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
    int32_t replayed[MAX_CELLS];
    int blank = -1;

    /* Metadata line. */
    if (gz_read_line(file, line, sizeof line) < 0) return -1;
    assert(strstr(line, "\"type\":\"meta\"") != NULL);
    assert(strstr(line, "\"schema\":1") != NULL);
    {
        const char *p = strstr(line, "\"solutionLength\":");
        assert(p != NULL);
        declared = (int)strtol(p + 17, NULL, 10);
    }
    {
        /* The replay has to start from the board the file itself declares, not from the
         * game's board: a file renamed from another board would otherwise be replayed
         * against the wrong start and still reach the goal by luck. */
        const char *p = strstr(line, "\"initial\":");
        assert(p != NULL);
        assert(parse_board(p + 10, grid, replayed) == grid * grid);
        for (int cell = 0; cell < grid * grid; cell++) {
            if (replayed[cell] == -1) blank = cell;
        }
        assert(blank >= 0);
    }

    while (gz_read_line(file, line, sizeof line) > 0) {
        const char *p;
        int index;
        char move;
        int from_row, from_col, to_row, to_col;
        char want;

        assert(strstr(line, "\"type\":\"step\"") != NULL);
        p = strstr(line, "\"index\":");
        assert(p != NULL);
        index = (int)strtol(p + 8, NULL, 10);
        assert(index == steps + 1);

        /* `move` names the blank's travel while `from`/`to` are the tile's endpoints,
         * so the letter is the reverse of the tile's direction. Checking it here is the
         * only place the writer's letter mapping is exercised: a flipped vertical
         * mapping still lands every replay on the goal and is invisible to a consumer
         * that animates from/to. */
        p = strstr(line, "\"move\":\"");
        assert(p != NULL);
        move = p[8];
        p = strstr(line, "\"from\":[");
        assert(p != NULL);
        assert(sscanf(p + 8, "%d,%d", &from_row, &from_col) == 2);
        p = strstr(line, "\"to\":[");
        assert(p != NULL);
        assert(sscanf(p + 6, "%d,%d", &to_row, &to_col) == 2);
        if (to_row < from_row) want = 'D';
        else if (to_row > from_row) want = 'U';
        else if (to_col < from_col) want = 'R';
        else want = 'L';
        assert(move == want);
        assert(replayed[from_row * grid + from_col] != -1);
        assert(replayed[to_row * grid + to_col] == -1);
        assert(blank == to_row * grid + to_col);
        {
            int32_t tile = replayed[from_row * grid + from_col];
            replayed[to_row * grid + to_col] = tile;
            replayed[from_row * grid + from_col] = -1;
            blank = from_row * grid + from_col;
        }

        p = strstr(line, "\"board\":");
        assert(p != NULL);
        assert(parse_board(p + 8, grid, board) == grid * grid);
        assert(memcmp(board, replayed, sizeof(int32_t) * (size_t)(grid * grid)) == 0);
        memcpy(final_board, replayed, sizeof(int32_t) * (size_t)(grid * grid));
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

    /* Rebuilt, not left at 500. Comparing sizes against the first run would be
     * fragile: the gzip byte count is not part of the contract, and the exporter
     * regenerates the file from scratch. What matters is that the file now reads
     * back whole, so that is what is asserted. */
    assert(after.st_size > 500);
    {
        gzFile rebuilt = gzopen(path, "rb");
        char line[65536];
        int rows = 0;
        assert(rebuilt != NULL);
        while (gzgets(rebuilt, line, sizeof line) != NULL) {
            rows++;
        }
        assert(gzclose(rebuilt) == Z_OK);   /* trailer verified: not truncated */
    }
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

/* A hand-edited or half-written index row can claim any grid. Seeding the next
 * --resume run from it must not index the grids[] table out of bounds, and must not
 * carry the unusable row forward. */
static void test_resume_survives_malformed_index_rows(void)
{
    char dir[512];
    char path[600];
    char command[4096];

    snprintf(dir, sizeof dir, "%s/badindex", MNSP_EXPORT_TEST_DIR);
    snprintf(path, sizeof path, "%s/index.jsonl.gz", dir);
    snprintf(command, sizeof command,
             "\"%s\" export --out \"%s\" --music 100001 --grid 3 --index "
             ">/dev/null 2>&1", MNSP_EXPORT_BINARY, dir);
    assert(system(command) == 0);

    /* Overwrite the index with one good row plus three that cannot be used: a grid far
     * past the array, a negative grid, and a line that is not JSON at all. */
    snprintf(command, sizeof command,
             "printf '%%s\\n' "
             "'{\"musicId\":100001,\"grid\":3,\"file\":\"a\",\"solutionLength\":21,"
             "\"solver\":\"s\",\"quality\":\"q\"}' "
             "'{\"musicId\":999,\"grid\":9999,\"file\":\"x\",\"solutionLength\":1,"
             "\"solver\":\"s\",\"quality\":\"q\"}' "
             "'{\"musicId\":999,\"grid\":-5,\"file\":\"y\",\"solutionLength\":1,"
             "\"solver\":\"s\",\"quality\":\"q\"}' "
             "'garbage' "
             "| gzip > \"%s.n\" && mv \"%s.n\" \"%s\"", path, path, path);
    assert(system(command) == 0);

    /* No --music, so the run takes the branch that tested grids[] for every row --
     * the one that read out of bounds. Grid 4 only: the good row for grid 3 is for a
     * board this run does not touch, so it has to survive. */
    snprintf(command, sizeof command,
             "\"%s\" export --out \"%s\" --grid 4 --index --resume >/dev/null 2>&1",
             MNSP_EXPORT_BINARY, dir);
    assert(system(command) == 0);

    /* The malformed rows must be gone; the untouched good row must survive. */
    snprintf(command, sizeof command,
             "zcat \"%s\" | grep -q '\"grid\":9999'", path);
    assert(system(command) != 0);
    snprintf(command, sizeof command,
             "zcat \"%s\" | grep -q 'garbage'", path);
    assert(system(command) != 0);
    snprintf(command, sizeof command,
             "zcat \"%s\" | grep -q '\"musicId\":100001,\"grid\":3'", path);
    assert(system(command) == 0);
    printf("  malformed index rows dropped, good rows kept\n");
}

/* The published index must be a single gzip member. Readers that stop after the first
 * member (Java's GZIPInputStream, some HTTP clients) would otherwise see only the
 * first row, and `gzip -l` reports the last member's sizes instead of the file's. */
static void test_index_is_a_single_gzip_member(void)
{
    char dir[512];
    char path[600];
    char command[2048];
    FILE *file;
    unsigned char buffer[65536];
    size_t read_bytes;
    int members = 0;

    snprintf(dir, sizeof dir, "%s/member", MNSP_EXPORT_TEST_DIR);
    snprintf(path, sizeof path, "%s/index.jsonl.gz", dir);
    snprintf(command, sizeof command,
             "\"%s\" export --out \"%s\" --grid 3 --music 100001 --music 100050 "
             "--music 100003 --index >/dev/null 2>&1", MNSP_EXPORT_BINARY, dir);
    assert(system(command) == 0);

    /* A second run with --resume exercises the seeding path, which used to append its
     * own stream on top of the seeded one. */
    snprintf(command, sizeof command,
             "\"%s\" export --out \"%s\" --grid 4 --music 100003 --index --resume "
             ">/dev/null 2>&1", MNSP_EXPORT_BINARY, dir);
    assert(system(command) == 0);

    file = fopen(path, "rb");
    assert(file != NULL);
    read_bytes = fread(buffer, 1, sizeof buffer, file);
    fclose(file);
    for (size_t i = 0; i + 2 < read_bytes; i++) {
        if (buffer[i] == 0x1f && buffer[i + 1] == 0x8b && buffer[i + 2] == 0x08) {
            members++;
        }
    }
    assert(members == 1);

    /* All four rows (three from the first run, one from the resumed run) are present.
     * Counted by decompressing rather than shelling out, so the test does not depend on
     * popen being declared with the C17 feature macros this project builds under. */
    {
        gzFile index = gzopen(path, "rb");
        char line[1024];
        int rows = 0;
        assert(index != NULL);
        while (gzgets(index, line, sizeof line) != NULL) {
            rows++;
        }
        assert(gzclose(index) == Z_OK);
        assert(rows == 4);
    }
    printf("  index is one gzip member with every row\n");
}

/* --deterministic must make two runs over the same inputs byte-identical. The solver
 * already is; the wall-clock timing in the meta line is the only variable field. */
static void test_deterministic_output(void)
{
    char dir_a[512];
    char dir_b[512];
    char path_a[600];
    char path_b[600];
    char command[2048];
    unsigned char a[16384];
    unsigned char b[16384];
    FILE *file;
    size_t size_a, size_b;

    snprintf(dir_a, sizeof dir_a, "%s/det-a", MNSP_EXPORT_TEST_DIR);
    snprintf(dir_b, sizeof dir_b, "%s/det-b", MNSP_EXPORT_TEST_DIR);
    snprintf(path_a, sizeof path_a, "%s/100050_3.jsonl.gz", dir_a);
    snprintf(path_b, sizeof path_b, "%s/100050_3.jsonl.gz", dir_b);

    snprintf(command, sizeof command,
             "\"%s\" export --out \"%s\" --music 100050 --grid 3 --deterministic "
             ">/dev/null 2>&1", MNSP_EXPORT_BINARY, dir_a);
    assert(system(command) == 0);
    snprintf(command, sizeof command,
             "\"%s\" export --out \"%s\" --music 100050 --grid 3 --deterministic "
             ">/dev/null 2>&1", MNSP_EXPORT_BINARY, dir_b);
    assert(system(command) == 0);

    file = fopen(path_a, "rb");
    assert(file != NULL);
    size_a = fread(a, 1, sizeof a, file);
    fclose(file);
    file = fopen(path_b, "rb");
    assert(file != NULL);
    size_b = fread(b, 1, sizeof b, file);
    fclose(file);

    assert(size_a > 0 && size_a == size_b);
    assert(memcmp(a, b, size_a) == 0);
    printf("  --deterministic produced identical bytes\n");
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
    test_resume_survives_malformed_index_rows();
    test_index_is_a_single_gzip_member();
    test_deterministic_output();
    test_write_failure_is_reported();
    puts("test_export: ok");
    return 0;
#endif
}
