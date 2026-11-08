# moenotes-slidepuzzle

C17 library and CLI for the in-game sliding puzzle (`SlidePuzzle`) of
`com.bilibili.sirius`. It reproduces the board the game builds for a
`(musicId, grid)` pair, solves boards, recovers a board from a phone screenshot, and
batch-exports complete step-by-step solutions. Version 0.1.0. No LICENSE file.

## How the game builds a board

Boards are generated locally by the game, so they are reproducible offline: the shuffle
seed is the song's master id; the Random is a port of .NET `System.Random(int)` including
the 32-bit wraparound near `INT32_MIN`/`INT32_MAX`; the board starts at the solved layout
and the game performs `11 * N * N` legal slides, each chosen with
`Random.Next(0, legalMoveCount)` over an up/down/left/right scan (`src/sp_puzzle.c`,
mirroring `Vector2Int..cctor @ 0xBEE96E4`). Every generated board is solvable by
construction; `mnsp_shuffle_steps(N)` is `11*N*N` (99/176/275 for 3/4/5).

**Coordinates.** Unity origin is bottom-left, `y` grows upwards, the blank starts at
`(N-1, N-1)`, and `tilePositions[i]` holds the tile's *correct* position index with
`index = y*N + x`, `-1` for the blank. A tile's value is the position it belongs in, not
a tile number, so `mnsp_goal_positions(4)` yields `12,13,14,15, 8,9,10,11, 4,5,6,7,
0,1,2,-1`. `mnsp_solvable` therefore does not use the classic inversions-plus-blank-row
parity rule: it runs a bounded 2M-state search and returns 1 when that search is
inconclusive, so a 0 on a large board means "not decided", not "proved unsolvable".
Moves are the blank's displacement; the CLI prints `U/D/L/R` for the blank's travel
direction on screen.

## Build

CMake >= 3.16 and a C17 compiler. zlib is needed **only by the CLI** (gzip output of
`export`); `-DMOENOTES_BUILD_CLI=OFF -DMOENOTES_BUILD_TESTS=OFF` compiles with no zlib
present. `third_party/stb_image.h` (public domain, v2.30) is compiled into the library
for PNG/JPEG/BMP/GIF decoding.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
ctest --test-dir build --output-on-failure
```

`MOENOTES_BUILD_CLI` (ON) builds the `moenotes_slidepuzzle` executable (target
`moenotes_slidepuzzle_cli`); `MOENOTES_BUILD_TESTS` (ON) registers the tests. Targets are
`moenotes_slidepuzzle` (static), alias `moenotes::slidepuzzle`, and the CLI.
`install(TARGETS ...)` installs the archive, header and CLI, but there is no
`install(EXPORT ...)` or package config, so the alias only works inside this build tree.
Non-MSVC builds use `-Wall -Wextra -Wpedantic -ffp-contract=off` and link `m`; MSVC gets
`_CRT_SECURE_NO_WARNINGS`.

## CLI

Exit codes: 0 success, 1 processing failure, 2 invalid arguments.

| Command | Behaviour |
|---|---|
| `gen <musicId> <grid>` | Prints `solvable=`, a board view and the `tilePositions` line for the board the game would build. |
| `solve <musicId> <grid> [--strategy auto\|search\|undo] [--max-states N] [--quality] [--quality-weight W] [--quality-nodes N]` | `auto` searches then replays the shuffle backwards; `search` fails instead of falling back; `undo` skips search. `--max-states` caps distinct boards explored (`mnsp_default_budget` when omitted). `--quality*` tries the PDB first (weight default 1.5, nodes default 200000000) and notes-then-continues where no PDB exists. It calls `mnsp_pdb_create`, not the cached entry point, so each run pays the ~3 min table build. **An unrecognised `--strategy` value is accepted silently and behaves as `auto`.** |
| `solve-state <grid> <p0,p1,...>` | Solves an arbitrary board; rejects a non-permutation, a board without exactly one `-1`, and a board `mnsp_solvable` calls unsolvable. |
| `read <screenshot> <jacket> <grid> [--solve]` | Recovers `tilePositions`; prints `totalScore`, prints low-confidence cells to stderr, and fails when more than a quarter of cells are low-confidence. `--solve` only takes effect as the **last** argument. |
| `read-music <screenshot> <musicId> <grid> [--solve] [--offline]` | Resolves the jacket from the cache (fetching unless `--offline`), then runs `read`. |
| `fetch-jackets [--region tw] [--locale zh-Hant] [--base URL] [--limit N] [--offline]` | Downloads cover art into the cache. `--offline` is parsed but absent from the help text; the reported `fetched` count means "a cached file exists after the resolve attempt", not "a download happened". |
| `list-jackets` | Prints the built-in `musicId assetName` table (84 rows). |
| `export --out <dir> [options]` | Batch exporter, below. |
| `--help`, `--version` | |

Cache directory: `$MOENOTES_SLIDEPUZZLE_CACHE`, else
`$XDG_CACHE_HOME/moenotes-slidepuzzle`, else `$HOME/.cache/moenotes-slidepuzzle`, else
`./.cache/moenotes-slidepuzzle`; jackets land under
`<cache>/jackets/<region>/<locale>/<musicId>_<asset>.png`. Jacket fetching shells out -
`curl` as `curl -fsSL --max-time 30 -o <dest> <url>`, any other fetcher as
`<fetcher> <url> <dest>` - from
`https://assets.bdon.moe/<locale>/Image/Jacket/<name>/<name>.png` by default.

## `export` and the published `dist/`

`export` solves every `(song, size)` board once and writes the whole path, so a consumer
replays instead of re-solving.

| Flag | Meaning |
|---|---|
| `--out <dir>` | Output directory (default `./puzzles`) |
| `--grid <N>` / `--music <id>` | Restrict, repeatable (defaults: all 3..8 / all built-in songs). Unknown ids, non-numeric values, values outside int32 and duplicates are rejected or de-duplicated |
| `--strategy auto\|exact\|pdb\|ladder` | Strategy override (table below) |
| `--resume` | Skip boards whose output file is already complete |
| `--no-compress` | Plain `.jsonl` instead of `.jsonl.gz` |
| `--deterministic` | Write `0` for `elapsedMs` so repeated runs are byte-identical |
| `--index` | Maintain `<out>/index.jsonl[.gz]` beside the boards |

| Size | Strategy | `solver` | `quality` |
|---|---|---|---|
| 3x3, 4x4 | exact A*, always - `--strategy` cannot override | `exact-astar` | `optimal` |
| 5x5, `auto` / `pdb` | PDB weighted IDA*, w=1.5, 200M nodes | `pdb-w1.5` | `near-optimal` |
| 5x5, `exact` | PDB exact IDA*, w=1, 3G nodes | `pdb-exact` | `optimal` |
| 5x5+, `ladder`; all 6x6-8x8 | `mnsp_solve_best` weighted ladder | `ladder` | `heuristic` |

5x5 PDB paths fall back to the ladder when the node budget runs out, so every board is
covered. `quality` is the guarantee level, not a measured ratio: only `optimal` proves a
shortest path.

One file per board, `<out>/<musicId>_<grid>.jsonl[.gz]`: a metadata line then one line
per step, so `solutionLength + 1` lines. `board` is **screen order** (row 0 = top row as
drawn) and each value is a jacket slice index; `-1` is the blank. `move` is the blank's
direction, while `from`/`to` are the **tile's** `[row, col]` in the same screen order,
i.e. the reverse of `move`.

```json
{"type":"meta","schema":1,"musicId":100001,"grid":5,"jacket":"jkt_001_100001","initial":[[20,23,11,19,22],[10,21,6,17,18],[1,5,15,8,14],[7,24,-1,16,12],[0,13,2,3,9]],"goal":[[20,21,22,23,24],[15,16,17,18,19],[10,11,12,13,14],[5,6,7,8,9],[0,1,2,3,-1]],"solutionLength":67,"solver":"pdb-w1.5","quality":"near-optimal","elapsedMs":0}
{"type":"step","index":1,"move":"L","tile":24,"from":[3,1],"to":[3,2],"board":[[20,23,11,19,22],[10,21,6,17,18],[1,5,15,8,14],[7,-1,24,16,12],[0,13,2,3,9]]}
```

`--resume` treats a file as complete only if it opens, decompresses to EOF with a valid
gzip trailer (plain files must end with a newline), and its meta line's
`schema`/`musicId`/`grid`/`solutionLength` match the current task.

**`dist/` is committed**, not generated at build time: 504 board files (84 songs x 3-8)
plus `index.jsonl`, ~19.5 MB of plain JSONL, produced with
`export --out dist --deterministic --no-compress --index` (every `elapsedMs` is 0). By
`solver`: 168 `exact-astar`, 84 `pdb-w1.5`, 252 `ladder`. Longest solution 566 moves
(100052, 8x8); largest file 160,018 bytes; `tools/verify_export.py dist` reports
`505 files checked, 0 with problems`.

`tools/update_board.py <dir> <musicId> <grid> [--strategy S] [--dry-run] [--binary PATH]`
re-solves one board with the real exporter, refuses to replace a file with a longer
solution, and rewrites `index.jsonl` changing only that row; it is idempotent when the
solution is unchanged. The binary comes from `--binary`, then `$MOENOTES_SLIDEPUZZLE`,
then `PATH`.

## Library API

All 30 symbols declared in `include/moenotes_slidepuzzle.h` are present in the static
library. Every function borrows its inputs, writes into caller buffers, and uses **no
global mutable state**, so independent calls may run concurrently provided each uses its
own objects. The PDB takes a handle for the same reason (`src/sp_pdb.c` documents the
earlier `static g_pdb` design as a contract violation).

| Purpose | Functions |
|---|---|
| Random | `mnsp_random_init`, `mnsp_random_next` (`mnsp_random_t` is caller-owned) |
| Board | `mnsp_shuffle_steps`, `mnsp_generate`, `mnsp_goal_positions`, `mnsp_positions_valid`, `mnsp_solvable`, `mnsp_apply_moves` |
| Solving | `mnsp_solve_by_undo`, `mnsp_solve` (exact A*), `mnsp_solve_weighted`, `mnsp_solve_best` (ladder), `mnsp_default_weight`, `mnsp_default_budget` |
| PDB (5x5) | `mnsp_pdb_supported`, `mnsp_pdb_create`, `mnsp_pdb_create_cached`, `mnsp_pdb_destroy`, `mnsp_pdb_ready`, `mnsp_pdb_heuristic`, `mnsp_solve_pdb`, `mnsp_solve_optimal` |
| Screenshot | `mnsp_read_screenshot`, `mnsp_find_blank_cell` (`mnsp_read_options_t`) |
| Jackets | `mnsp_jacket_count`, `mnsp_jacket_at`, `mnsp_jacket_asset_name`, `mnsp_jacket_cache_dir`, `mnsp_jacket_resolve` (`mnsp_jacket_options_t`) |
| Misc | `mnsp_result_string` |

Solve functions return the move count or a negative `mnsp_result_t`:
`MNSP_ERR_SEARCH_LIMIT` when `max_states` is exhausted, `MNSP_ERR_RANGE` when the path
does not fit in `capacity`. `mnsp_solve_best` ignores `mnsp_default_weight`; its ladders
are 5: `{2.5, 2.0}`, 6: `{4.5, 3.5}`, 7: `{6.0}`, 8: `{8.0, 14.0}`, sharing one
`max_states` budget across rounds. `mnsp_default_weight`/`mnsp_default_budget` give
3-4 -> 1.0/4M, 5 -> 1.5/8M, 6 -> 3.0, 7 -> 4.0, 8 -> 5.0, all 8M.
`mnsp_read_screenshot` requires a non-NULL `options` with `grid` in 3..8: the header
comment says `grid = 0` detects the size, but the implementation returns
`MNSP_ERR_INVALID_ARGUMENT` for 0. Zeroed fields mean `min_score` 0.30, `min_margin`
0.005, `cell_gap_percent` 0 - and 0 does not mean "no inset"; `sp_solver.c` substitutes
an 8-pixel inset.

## Pattern database (5x5 only)

Four disjoint groups of six tiles covering all 24 real 5x5 tiles; the blank is out of the
index, which is what makes the four tables additively admissible (`src/sp_pdb.c`
documents the earlier blank-costed design, which violated the bound on 179,211 of
181,440 reachable 3x3 states). Each table is `25P6 = 127,512,000` bytes, ~486 MiB total.
Per-group maxima measured 39/32/31/37, so 4-bit packing is lossy. Build ~217 s, cached
load ~0.05 s; `mnsp_pdb_create_cached` keys a `meta.bin` header to the group definitions
and goal layout and rebuilds on mismatch. The exporter caches at `<cache>/pdb-5`. The PDB
makes 5x5 *optimal* solving possible but not fast: `mnsp_solve_optimal` measured 0.8-77 s
per board on seven songs, and the all-84-song w=1 sweep ran 98 minutes without finishing.

## Testing

`ctest` registers nine tests; the two Python-driven ones are registered only when an
interpreter is found, and `export`/`update_board` report themselves skipped (exit 77,
`SKIP_RETURN_CODE 77`) without a POSIX shell.

| Test | Exercises |
|---|---|
| `rng` | 8 seeds x >=40 draws against `tests/fixtures/dotnet-random-reference.txt`, plus `INT32_MIN`/`INT32_MAX` wrap equivalence |
| `puzzle` | Generation and undo replay against 72 board / 30 RLE fixture lines; goal layout; `mnsp_apply_moves` bounds; validity and solvability; exact A* on 3x3 and near-solved 4x4/5x5; a tiny-budget limit; weighted search for 3..8; `mnsp_solve_best` for 3..8 plus a mid-game board |
| `read` | `mnsp_read_screenshot` on three synthesized screenshots (3x3, 4x4, 5x5), exact permutation match; 6x6 deliberately not asserted |
| `jacket` | 84-entry table with unique ids and names, cache-dir shape, cache-only resolve for unknown and known ids; no network |
| `pdb_api` | PDB contract: `mnsp_pdb_supported`, NULL handles, invalid grids and boards; re-checks the handle-free solver. The admissibility run is opt-in (`MNSP_SLOW_PDB_TESTS=1`, ~3 min) and checks h <= optimum plus exact solve for 100001/100002/100050/100100 |
| `cli_version` | `--version` matches `moenotes_slidepuzzle 0\.1\.0` |
| `export` | Drives the real binary: replays steps to the goal; plain output and index; `--resume` idempotence plus rebuild of a truncated gzip and a wrong-schema file; malformed index rows dropped while good rows survive; a single gzip member; `--deterministic` byte equality; strict numeric parsing; over-long `--out`; write failure via `ulimit -f` |
| `verify_export` | Runs `tools/verify_export.py` over the `export` test's output |
| `update_board` | Runs `tests/test_update_board.py`: only the target file changes, the index stays honest, re-run is idempotent, a longer solution is refused |

A clean Release build passes 9/9 (~32 s), and the same suite passes 9/9 under
`-fsanitize=address,undefined` (~112 s) with the fixtures as they stand.

```bash
python3 tools/verify_export.py dist            # also --quiet, --no-index-check
python3 -I tests/test_update_board.py <python> tools/update_board.py \
    tools/verify_export.py <exporter-binary> <workdir>
```

`verify_export.py` replays every step, checks `tile`/`from`/`to` against the board, checks
the final board against `goal`, cross-checks the index against the files on disk, and
reports a suffix-vs-gzip mismatch explicitly.

**Measured numbers** (from `docs/PDB-INTEGRATION.md` unless noted; one machine, not
guarantees). Full 504-board export: 2104 s wall (2091 s solving), peak RSS 2.6 GiB;
solving time per size 0.0 / 4.1 / 393.5 / 49.7 / 109.0 / 1534.4 s and mean path lengths
17.7 / 36.9 / 68.2 / 156.8 / 280.6 / 410.2 for 3x3 / 4x4 / 5x5 / 6x6 / 7x7 / 8x8. 420
boards (3x3-7x7, `auto`, PDB cached): 355 s, 2.1 MiB gzipped, 0 failed; table build 217 s
on the first run, 0.184 s for a later one-board run. Peak RSS on each size's slowest
board: 5x5 108 MiB, 6x6 589 MiB, 7x7 1036 MiB, 8x8 2071 MiB. 5x5 PDB at w=1.5: mean 68.2
moves versus 78.5 for `mnsp_solve_best` over all 84 songs (82 better, 2 equal, 0 worse),
167 s versus 9.7 s, slowest single board 70.5 s. `--max-states` is the linear memory
lever: 5x5 board 100104 at 8M -> 111 MiB / 87 moves, at 200k -> 28 MiB / 91 moves.
Regenerating 100001_3 and 100002_4 at 0.1.0 with `--deterministic` reproduced the
committed `dist/` bytes exactly.

## Limitations

- **Optimality.** 3x3 and 4x4 use exact A* and record `optimal`. 5x5 is provably optimal
  only via `mnsp_solve_optimal` / `--strategy exact`, which is slow and does not finish
  every board in practice. The default 5x5 export and everything 6x6+ are heuristic.
- **PDB.** 5x5 only; `mnsp_pdb_create` returns NULL for any other grid and every other
  PDB entry point returns -1/0. `src/sp_pdb.h` says a 5-tile split (25P5, ~24 MiB) is not
  implemented, and `docs/PDB-INTEGRATION.md` records it as 2.5x slower than the 6-tile
  tables on a 20-song sample (19/20 solved).
- **Memory.** 6x6+ is not browser-friendly: 589 / 1036 / 2071 MiB peak for 6x6 / 7x7 /
  8x8. The PDB adds a fixed ~486 MiB resident (552 MiB peak including the build queue)
  regardless of difficulty. No WASM build exists or was attempted.
- **Platform.** Everything was built and run on Linux (GCC 13.3.0, CMake 3.28.3). No MSVC
  or Windows toolchain was available, so no MSVC build is verified. Static inspection
  shows the exporter's `_WIN32` branches cover only `_mkdir` and `_getpid`:
  `export_now_ms` calls `clock_gettime(CLOCK_MONOTONIC, ...)` with just a
  `_POSIX_C_SOURCE` define, `S_ISDIR` is used unguarded, and `src/sp_jacket.c` includes
  `<unistd.h>`/`<sys/wait.h>` and calls `fork`/`execlp`/`waitpid`.
  `notes/2026-11-07_0803_slidepuzzle-exporter-merged.md` records the same gap (checked
  against header symbols, not a real toolchain), and `test_export` skips on Windows.
- **Screenshot reader.** Tested against `tools/make_fixture_screens.py` output and one 4x4
  device capture
  (`work/126_slide-puzzle-analysis/cases/20261104-device/board_4x4_fresh.png`), which
  reads back identically to `mnsp_generate(100001, 4)` and solves to the same 34-move
  path in `notes/2026-11-04_1913_slide-puzzle-device-validation.md`. 3x3 and 4x4 were
  cleared by hand from device captures; nothing larger was. The reader needs a dark, flat
  blank cell: on a cleared board, or when the stated grid does not match the screenshot,
  blank detection returns -1 and `mnsp_read_screenshot` returns `MNSP_ERR_IMAGE`. 6x6
  recognition is not asserted: one jacket slice is pure white next to a near-white one.
- **Fixture orientation.** Fixed on 2026-11-09. `tools/make_fixture_screens.py` used to
  flip the board once for drawing and again for the sidecar, so the synthetic screenshots
  were vertical mirrors of `mnsp_generate`'s output and the sidecar matched the mirrored
  reading. The fixtures were regenerated and `read` now returns exactly what
  `mnsp_generate` produces (verified for 3x3, 4x4 and 5x5). Ground truth for the
  orientation is the device capture: `read` on it gives `3,8,7,-1,4,6,0,5,1`, identical
  to `gen 100001 3`.
- **Jackets.** Shells out to `curl`; there is no HTTPS stack in the library. Only the 84
  built-in songs are known, from one `MasterLiveMusic` snapshot (`hk-tw-mo`) via
  `tools/gen_jacket_table.py`.
- **Docs vs `dist/`.** `docs/EXPORT-CONSUMER.md` quotes gzip figures (~0.9-3 MiB total,
  0.5-54 KiB per file) that do not describe the committed plain-text directory (~19.5 MB,
  up to 160 KB per file); its own header notes the switch to plain JSON on 2026-11-09.
- **Not implemented.** No `--jobs` parallelism, no stale-lock cleanup (`.export.lock`
  must be removed by hand after `kill -9`), no `--stdout`, no CMake package config.
  `docs/EXPORT-SELF-REVIEW.md` lists these. Undocumented: `read`/`read-music` accept
  `--offline`, and the library reads `SP_DEBUG_COUNTS`, `SP_DEBUG_GAP` and
  `SP_PDB_VERBOSE` for diagnostics.
