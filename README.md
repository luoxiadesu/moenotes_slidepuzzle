# moenotes-slidepuzzle

A standalone C17 tool for the in-game sliding puzzle (`SlidePuzzle`) of
`com.bilibili.sirius` 1.1.0. It rebuilds any board offline, reads one out of a
screenshot, and solves it.

> Status: **verified in game** — 3x3 (21 moves) and 4x4 (34 moves) were both solved
> from a device screenshot and cleared by hand. Board generation, screenshot
> recognition, jacket fetching and the CLI are complete; rotation handling and
> stronger solvers for large freshly-shuffled boards are not.

## Why the boards are reproducible

The game builds each puzzle locally:

- `SlidePuzzleBoard.Setup` (`0x6B43268`) sets `_shuffleCount = 11 * N * N`;
- `BuildBoard` (`0x6B43344`) starts from the solved board with the empty slot at
  `(N-1, N-1)`;
- `ShuffleBoard` (`0x6B43820`) drives `System.Random(MasterLiveMusic.MasterId)` and
  applies `11 * N * N` random legal slides.

So a board is a pure function of `(musicId, grid)`, and because every slide is
legal the result is always solvable. Note the solved layout is **not** the identity
permutation: a tile's id counts y from the bottom while cells are listed top-down,
so a cleared 3x3 reads `6 7 8 / 3 4 5 / 0 1 2`. This was confirmed on a device. Full evidence lives in
`work/126_slide-puzzle-analysis/docs/ANALYSIS.md`.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Requires CMake >= 3.16 and a C17 compiler. No third-party runtime dependencies.

## Usage

```bash
./build/moenotes_slidepuzzle gen   100001 4    # board the game will show
./build/moenotes_slidepuzzle solve 100001 4    # shortest-known solution for a fresh board
./build/moenotes_slidepuzzle solve-state 3 '7,2,6,0,3,-1,5,4,1'

# Full pipeline: screenshot + the song's cover art -> board -> moves
./build/moenotes_slidepuzzle read screenshot.png jacket_100001.png 4 --solve

# Same, but fetch/locate the cover art by song id (cached under ~/.cache)
./build/moenotes_slidepuzzle read-music screenshot.png 100001 4 --solve

# Pre-download the whole cover-art library
./build/moenotes_slidepuzzle fetch-jackets --region tw --locale zh-Hant
./build/moenotes_slidepuzzle list-jackets          # 84 built-in song -> jacket rows
```

`read` locates the board in a whole-screen screenshot, slices it N×N, matches each
cell against the N×N slices of the cover art, and solves the assignment so that every
tile is used exactly once. Cells whose match is ambiguous are reported on stderr as
low-confidence rather than silently guessed.

Arrays are in **screen order**: index `i = row * grid + col` with row 0 at the top,
matching what the game draws and what `SlidePuzzleSaveData._tilePositions` records.
`-1` marks the empty slot. A tile's *value* is the position it belongs in, counted
with y from the bottom, which is why the cleared layout is not `0,1,2,...`.

Moves describe the **blank** as seen on screen: `U` moves it up, which is the player
tapping the tile above it.

### Two solvers

Both commands search first and only fall back to replaying the shuffle when the
board is out of reach.

Measured over **all 84 built-in songs** at every size (504 boards):

| Grid | Moves (avg / worst) | Time (avg / worst) | Undo replay | Optimal? |
|---|---|---|---|---|
| 3x3 | 17 / 27 | <0.01 s | 99 | **yes** |
| 4x4 | 36 / 50 | 0.03 / 0.5 s | 176 | **yes** |
| 5x5 | 78 / 109 | 0.08 / 0.8 s | 275 | no |
| 6x6 | 156 / 220 | 0.4 / 9.2 s | 396 | no |
| 7x7 | 280 / 377 | 0.9 / 11 s | 539 | no |
| 8x8 | 410 / 566 | 12.7 / 39 s | 704 | no |

Only 3x3 and 4x4 are solved **optimally** (exact A*, weight 1). Above that the
search key becomes `g + weight*h`, which trades optimality for reachability — the
reachable state count is 7.8e24 at 5x5 and 6.3e88 at 8x8, so proving a path shortest
is not an engineering problem but an impossible one. The measured paths land at
2.2–2.6x the Manhattan-plus-linear-conflict lower bound.

A single weight does not work above 6x6: the bias that makes one board trivial lets
another exhaust its budget, and path length is not monotone in the weight (one 7x7
board needs 289 moves at w=6 but 305 at w=5, found 100x slower). `mnsp_solve_best`
runs a short ladder of biases and keeps the shortest path any round finds. Round 0 is
deliberately over-biased — it must finish, and *cheaply* — and each later round is
pruned at generation time by the incumbent length (`g + h >= best` is dropped), so
refinement costs less than the round that produced the bound.

8x8 is the size where the bias matters most, and it is the one size where **two
mutually exclusive weights are required**. Over all 84 songs no single weight covers
the set: w=10 solves 78, w=14 and w=16 solve 83 (all failing on 100404), and the
outliers are opposite — 100404 solves instantly at w=8 and fails at every w >= 10,
while 100021 fails at w <= 10 and needs w >= 12. Path length is not monotone in the
weight, so neither direction is "safer". The shipped pair `{8, 14}` covers all 84
boards (average 408 moves).

`solve <musicId> <grid> --strategy search|undo|auto` picks the path; `auto` (the
default) searches and only replays the shuffle backwards, with a note on stderr, when
the budget is exceeded.

### Optional: pattern-database search (`--quality`)

A disjoint pattern database (four groups of six tiles, summed) gives a much stronger
admissible heuristic than Manhattan plus linear conflict, which makes weighted IDA*
return shorter paths. Measured over all 84 built-in songs at 5x5:

| | standard search | `--quality` (w=1.5) |
|---|---|---|
| mean moves | 78.5 | **68.2 (-13.1%)** |
| worst moves | 109 | **89 (-18.3%)** |
| better / equal / worse | - | **82 / 2 / 0** |
| total time | 9.7 s | 166.8 s |

It is opt-in because the tables cost **486 MiB of RAM and about three minutes to
build**, and on 17 of the 84 boards the search exceeds a one-second budget. With
`--quality` a node limit applies (`--quality-nodes`, default 200M); when it runs out
the command falls back to the standard search automatically.

```bash
./build/moenotes_slidepuzzle solve 100001 5 --quality
./build/moenotes_slidepuzzle solve 100001 5 --quality --quality-nodes 50000000
```

`mnsp_solve_optimal` (C API only) goes further and returns a provably shortest path,
but takes 0.8-77 s per board on real songs and does not finish on the hardest ones.
That is not an implementation gap: 5x5 optimal solving is a minutes-to-hours problem
(see `docs/110_slide-puzzle-optimal-pdb.md`).

## API

```c
#include "moenotes_slidepuzzle.h"

int32_t positions[16];
mnsp_generate(100001, 4, positions);          /* the board the game shows */

mnsp_move_t moves[1408];
int count = mnsp_solve_by_undo(100001, 4, moves, 1408);
```

All functions borrow their inputs and write into caller buffers; there is no global
state. See `include/moenotes_slidepuzzle.h` for the full contract.

## How `read` works

1. **Board location** — a luma-variance profile over rows and columns finds the single
   contiguous square of busy content; a local sweep then refines the rectangle.
   Tested to ±4 px on 1080×2400 screenshots.
2. **Tile templates** — the cover art is resampled into `grid*grid - 1` mosaics of
   96×96. Index `y*grid + x` follows the game's bottom-up coordinates, and the last
   slice (the blank's home) is never drawn, so it is not a template.
3. **Matching** — zero-mean normalised cross correlation between each cell mosaic and
   every template, then a Hungarian assignment over the (N²−1)×(N²−1) matrix. Solving
   it globally rather than taking per-cell argmax is what fixes the flat-cover cases.
4. **Blank** — the cell with the smallest luma variance is the empty slot; it is
   excluded from the assignment and written as `-1`.

Measured on synthesized 1080×2400 screenshots (JPEG, rescaled, mildly blurred):

| Grid | Cells correct |
|---|---|
| 3×3 … 6×6 | 100 % (every fixture) |

## Cover-art library

`fetch-jackets` downloads each song's cover art into
`$MOENOTES_SLIDEPUZZLE_CACHE` (default `$XDG_CACHE_HOME/moenotes-slidepuzzle`, else
`~/.cache/moenotes-slidepuzzle`) as
`jackets/<region>/<locale>/<musicId>_<assetName>.png`.

The URL is `<base>/<locale>/Image/Jacket/<name>/<name>.png`, where the asset name
(`jkt_<band>_<musicId>`) comes from `MasterLiveMusic._jacketAssetName`. The band
prefix cannot be derived from the song id, so the 84-row mapping is generated into
`src/sp_jackets_builtin.h` by `tools/gen_jacket_table.py <MasterLiveMusic.json>`.

Downloads shell out to `curl` (override with the `fetcher` option) rather than
linking an HTTP client; nothing else in the tool touches the network.

## Verification

- `tests/test_rng.c` replays 8 seeds × 80 draws of real `dotnet 10
  System.Random(seed).Next(0, n)` and requires a bit-exact match, including the
  `INT32_MIN` / `INT32_MAX` wraparound cases.
- `tests/test_puzzle.c` checks 72 generated boards (grid 3..8, 12 music ids) and 30
  undo solutions against the Python reference implementation, plus solvability,
  move application and A* round-trips.
- `tests/test_read.c` runs the whole screenshot pipeline on four synthesized
  screenshots (3×3 … 6×6) and requires an exact permutation match.
- `tests/test_jacket.c` checks the built-in table is a bijection and exercises the
  cache-only resolve path with the network disabled.
  `tools/make_fixture_screens.py <jacket.png> <musicId> <grid> <out.png>` regenerates
  the fixtures; they are synthetic, not captured from the game.

## Known limits

1. Recognition is verified on one device (2410CRP4CC, Android 16, hk-tw-mo, 1.1.0);
   other resolutions, aspect ratios and UI layouts are untested.
2. Flat, mostly-uniform cover art still confuses matching at grid >= 5; those cells
   show up as low-confidence.
3. Rotated or perspective-distorted screenshots are not handled (only straight,
   axis-aligned captures).
4. The built-in jacket table is a snapshot; a new song needs a regenerated table
   (`tools/gen_jacket_table.py`) before `read-music` can find its cover art.
5. Boards above 4x4 are solved approximately: the shipped ladders produce valid paths
   that are typically 2.2-2.6x the Manhattan-plus-linear-conflict lower bound, not
   provably shortest. Every one of the 84 built-in songs solves at every size, but
   that is an empirical result, not a guarantee — a board the budget cannot reach
   falls back to undo replay with a note on stderr. 8x8 is the fragile size: its
   weight (10.0) is the only one of five tried that covered all 84 boards, and both
   higher and lower values failed on boards it handles. Peak memory is dominated by
   the search budget (~210 MiB on the hardest 7x7 with the default 8M-state cap);
   lower `--max-states` trades length for memory.
6. The tool never reads or writes game saves and never touches the game or its
   servers; it only computes boards and move lists.

## License

The library and CLI are original code. `third_party/stb_image.h` (v2.30, public
domain / MIT) is bundled for the upcoming screenshot stage.
